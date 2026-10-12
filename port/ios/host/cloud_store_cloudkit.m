/* The cloud saves store (port/runtime/cloud_saves.h) on CloudKit: the zone Saves of the
   container's private database, one SaveSlot record per unit or copy, its files as one asset.
   Every call is synchronous, bounded, and made from the cloud saves queue (host_cloud_saves.m),
   never the game's thread.

   The operations and the list of errors no retry fixes follow port/tvos/host_cloud_saves.m in
   bmanturner's halo-ce-universal (apple-v0.3.0, CC0). */
#import <CloudKit/CloudKit.h>
#import <Foundation/Foundation.h>
#include "host.h"
#include "host_cloud_saves.h"
#include "cloud_saves.h"

#define CLOUD_ZONE @"Saves"
#define CLOUD_TYPE @"SaveSlot"
#define CLOUD_CALL_SECONDS 45

static CKContainer *cloud_container;
static CKDatabase *cloud_database;
static CKRecordZoneID *cloud_zone;
static NSString *cloud_user;              /* the account's record name, until the account changes */
static NSDate *cloud_not_before;          /* the server asked to be left alone until then */

/* a partial failure carries the item's own error; an item that only failed with its batch is not it */
static NSError *cloud_cause(NSError *error) {
    if (![error.domain isEqualToString:CKErrorDomain] || error.code != CKErrorPartialFailure) return error;
    NSError *other = nil;
    for (NSError *item in [error.userInfo[CKPartialErrorsByItemIDKey] allValues]) {
        if (![item.domain isEqualToString:CKErrorDomain] || item.code != CKErrorBatchRequestFailed) return item;
        other = item;
    }
    return other ?: error;
}

static enum cloud_status cloud_status_of(NSError *error, const char *what) {
    if (!error) return CLOUD_OK;
    NSError *cause = cloud_cause(error);
    enum cloud_status status = CLOUD_RETRY;
    if ([cause.domain isEqualToString:CKErrorDomain]) {
        NSNumber *after = cause.userInfo[CKErrorRetryAfterKey];
        if (after) cloud_not_before = [NSDate dateWithTimeIntervalSinceNow:after.doubleValue];
        switch (cause.code) {
        case CKErrorServerRecordChanged: status = CLOUD_CHANGED; break;
        case CKErrorUnknownItem: status = CLOUD_NOT_FOUND; break;
        case CKErrorZoneNotFound: status = CLOUD_NO_ZONE; break;
        case CKErrorUserDeletedZone: status = CLOUD_ZONE_DELETED; break;
        case CKErrorQuotaExceeded: status = CLOUD_QUOTA; break;
        case CKErrorNotAuthenticated: status = CLOUD_NO_ACCOUNT; break;
        /* the container, entitlement or account is wrong: no retry in this run fixes it */
        case CKErrorPermissionFailure: case CKErrorBadContainer: case CKErrorMissingEntitlement:
        case CKErrorIncompatibleVersion: case CKErrorManagedAccountRestricted:
            status = CLOUD_FAILED; break;
        default: break;   /* network, busy, rate limited, cancelled, temporarily unavailable: later */
        }
    }
    if (status != CLOUD_CHANGED && status != CLOUD_NOT_FOUND && status != CLOUD_NO_ZONE)
        host_logf(HOST_LOG_WARN, "cloud saves: %s: %s (%s %ld)", what, cause.localizedDescription.UTF8String,
            cause.domain.UTF8String, (long)cause.code);
    return status;
}

/* runs one operation and waits for finish to be signaled; NO when it took too long (and is cancelled) */
static BOOL cloud_run(CKDatabaseOperation *operation, dispatch_semaphore_t finished) {
    operation.qualityOfService = NSQualityOfServiceUserInitiated;
    operation.configuration.timeoutIntervalForRequest = 20;
    operation.configuration.timeoutIntervalForResource = CLOUD_CALL_SECONDS;
    [cloud_database addOperation:operation];
    if (dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, (CLOUD_CALL_SECONDS + 5) * NSEC_PER_SEC)) == 0)
        return YES;
    [operation cancel];
    return NO;
}

static BOOL cloud_backing_off(void) {
    return cloud_not_before && cloud_not_before.timeIntervalSinceNow > 0;
}

static void cloud_text(NSString *text, char *out, size_t size) {
    snprintf(out, size, "%s", [text isKindOfClass:NSString.class] ? text.UTF8String : "");
}

static void cloud_record_out(CKRecord *record, struct cloud_record *out) {
    memset(out, 0, sizeof(*out));
    cloud_text(record.recordID.recordName, out->name, sizeof(out->name));
    cloud_text(record[@"unit"], out->unit, sizeof(out->unit));
    out->copy = strcmp(out->name, out->unit) != 0;
    out->format = [record[@"format"] isKindOfClass:NSNumber.class] ? [record[@"format"] longLongValue] : 0;
    /* a record without the field is not one this build may use */
    out->min_format = [record[@"minFormat"] isKindOfClass:NSNumber.class] ? [record[@"minFormat"] longLongValue] : 1LL << 30;
    cloud_text(record[@"contentHash"], out->hash, sizeof(out->hash));
    cloud_text((NSString *)record.encryptedValues[@"meta"], out->meta, sizeof(out->meta));
    cloud_text(record.recordChangeTag, out->tag, sizeof(out->tag));
}

static CKRecordID *cloud_record_id(const char *name) {
    return [[CKRecordID alloc] initWithRecordName:@(name) zoneID:cloud_zone];
}

static enum cloud_status cloud_account(void *context, char *identifier, size_t size) {
    (void)context;
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);
    __block CKAccountStatus status = CKAccountStatusCouldNotDetermine;
    __block NSError *failure = nil;
    [cloud_container accountStatusWithCompletionHandler:^(CKAccountStatus s, NSError *error) {
        status = s; failure = error;
        dispatch_semaphore_signal(finished);
    }];
    if (dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC)) != 0) return CLOUD_RETRY;
    if (status == CKAccountStatusNoAccount || status == CKAccountStatusRestricted) return CLOUD_NO_ACCOUNT;
    /* not yet known, or temporarily unavailable (common just after a boot or a sign-in): not signed out */
    if (status != CKAccountStatusAvailable) return failure ? cloud_status_of(failure, "account") : CLOUD_RETRY;
    NSString *user;
    @synchronized (cloud_container) { user = cloud_user; }
    if (!user) {
        __block NSString *fetched = nil;
        [cloud_container fetchUserRecordIDWithCompletionHandler:^(CKRecordID *record, NSError *error) {
            fetched = record.recordName; failure = error;
            dispatch_semaphore_signal(finished);
        }];
        if (dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC)) != 0) return CLOUD_RETRY;
        if (!fetched) return failure ? cloud_status_of(failure, "account") : CLOUD_RETRY;
        user = fetched;
        @synchronized (cloud_container) { cloud_user = user; }
    }
    cloud_text(user, identifier, size);
    return CLOUD_OK;
}

/* Every record of the zone, without the payloads: the whole zone from the start each time, so
   there is no change token to keep or to expire. */
static enum cloud_status cloud_list(void *context, struct cloud_record **records, int *count) {
    (void)context;
    if (cloud_backing_off()) return CLOUD_RETRY;
    NSMutableArray<CKRecord *> *found = [NSMutableArray array];
    __block CKServerChangeToken *token = nil;
    __block NSError *failure = nil;
    BOOL more = YES;
    while (more) {
        dispatch_semaphore_t finished = dispatch_semaphore_create(0);
        CKFetchRecordZoneChangesConfiguration *configuration = [CKFetchRecordZoneChangesConfiguration new];
        configuration.previousServerChangeToken = token;
        configuration.desiredKeys = @[@"format", @"minFormat", @"unit", @"contentHash", @"meta"];
        CKFetchRecordZoneChangesOperation *operation = [[CKFetchRecordZoneChangesOperation alloc]
            initWithRecordZoneIDs:@[cloud_zone] configurationsByRecordZoneID:@{cloud_zone: configuration}];
        __block BOOL more_coming = NO;
        operation.recordWasChangedBlock = ^(CKRecordID *identifier, CKRecord *record, NSError *error) {
            (void)identifier; (void)error;
            if (record && [record.recordType isEqualToString:CLOUD_TYPE]) @synchronized (found) { [found addObject:record]; }
        };
        operation.recordZoneFetchCompletionBlock = ^(CKRecordZoneID *zone, CKServerChangeToken *next, NSData *client,
                BOOL coming, NSError *error) {
            (void)zone; (void)client;
            token = next; more_coming = coming;
            if (error) failure = error;
        };
        operation.fetchRecordZoneChangesCompletionBlock = ^(NSError *error) {
            if (error && !failure) failure = error;
            dispatch_semaphore_signal(finished);
        };
        if (!cloud_run(operation, finished)) return CLOUD_RETRY;
        if (failure) return cloud_status_of(failure, "listing the saves");
        more = more_coming && token;
    }
    *records = calloc(found.count + 1, sizeof(**records));
    if (!*records) return CLOUD_RETRY;
    *count = 0;
    for (CKRecord *record in found) cloud_record_out(record, &(*records)[(*count)++]);
    return CLOUD_OK;
}

static enum cloud_status cloud_create_zone(void *context) {
    (void)context;
    if (cloud_backing_off()) return CLOUD_RETRY;
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);
    __block NSError *failure = nil;
    CKModifyRecordZonesOperation *operation = [[CKModifyRecordZonesOperation alloc]
        initWithRecordZonesToSave:@[[[CKRecordZone alloc] initWithZoneID:cloud_zone]] recordZoneIDsToDelete:nil];
    operation.modifyRecordZonesCompletionBlock = ^(NSArray *saved, NSArray *deleted, NSError *error) {
        (void)saved; (void)deleted;
        failure = error;
        dispatch_semaphore_signal(finished);
    };
    if (!cloud_run(operation, finished)) return CLOUD_RETRY;
    return cloud_status_of(failure, "making the zone");
}

static enum cloud_status cloud_delete_zone(void *context) {
    (void)context;
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);
    __block NSError *failure = nil;
    CKModifyRecordZonesOperation *operation = [[CKModifyRecordZonesOperation alloc]
        initWithRecordZonesToSave:nil recordZoneIDsToDelete:@[cloud_zone]];
    operation.modifyRecordZonesCompletionBlock = ^(NSArray *saved, NSArray *deleted, NSError *error) {
        (void)saved; (void)deleted;
        failure = error;
        dispatch_semaphore_signal(finished);
    };
    if (!cloud_run(operation, finished)) return CLOUD_RETRY;
    return cloud_status_of(failure, "deleting the zone");
}

/* one record as the server has it; with_payload NO leaves its asset on the server */
static enum cloud_status cloud_fetch_record(const char *name, BOOL with_payload, CKRecord **record, NSData **payload) {
    if (cloud_backing_off()) return CLOUD_RETRY;
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);
    CKRecordID *identifier = cloud_record_id(name);
    CKFetchRecordsOperation *operation = [[CKFetchRecordsOperation alloc] initWithRecordIDs:@[identifier]];
    __block CKRecord *fetched = nil;
    __block NSData *bytes = nil;
    __block NSError *failure = nil;
    if (!with_payload) operation.desiredKeys = @[@"format", @"minFormat", @"unit", @"contentHash", @"meta"];
    operation.fetchRecordsCompletionBlock = ^(NSDictionary<CKRecordID *, CKRecord *> *records, NSError *error) {
        fetched = records[identifier];
        failure = error;
        /* the asset's file is CloudKit's and may go once this block returns */
        CKAsset *asset = fetched[@"payload"];
        if ([asset isKindOfClass:CKAsset.class] && asset.fileURL) bytes = [NSData dataWithContentsOfURL:asset.fileURL];
        dispatch_semaphore_signal(finished);
    };
    if (!cloud_run(operation, finished)) return CLOUD_RETRY;
    if (failure) return cloud_status_of(failure, "fetching a save");
    if (!fetched || ![fetched.recordType isEqualToString:CLOUD_TYPE]) return CLOUD_NOT_FOUND;
    *record = fetched;
    if (payload) *payload = bytes;
    return CLOUD_OK;
}

static enum cloud_status cloud_fetch(void *context, const char *name, struct cloud_record *record, uint8_t **payload,
        size_t *size) {
    (void)context;
    CKRecord *fetched = nil;
    NSData *bytes = nil;
    enum cloud_status status = cloud_fetch_record(name, YES, &fetched, &bytes);
    if (status != CLOUD_OK) return status;
    if (!bytes.length) return CLOUD_RETRY;   /* the record came without its asset: ask again */
    *payload = malloc(bytes.length);
    if (!*payload) return CLOUD_RETRY;
    memcpy(*payload, bytes.bytes, bytes.length);
    *size = bytes.length;
    cloud_record_out(fetched, record);
    return CLOUD_OK;
}

/* A replacement changes the record just fetched and saves it only if the server's is still that
   one; a new record is refused by the server when the name is taken. Either refusal is "changed". */
static enum cloud_status cloud_save(void *context, const struct cloud_record *record, const uint8_t *payload, size_t size,
        const char *expected_tag, char *new_tag, size_t tag_size) {
    (void)context;
    CKRecord *target = nil;
    if (expected_tag[0]) {
        enum cloud_status status = cloud_fetch_record(record->name, NO, &target, NULL);
        if (status == CLOUD_NOT_FOUND) return CLOUD_CHANGED;
        if (status != CLOUD_OK) return status;
        if (![target.recordChangeTag isEqualToString:@(expected_tag)]) return CLOUD_CHANGED;
    } else {
        if (cloud_backing_off()) return CLOUD_RETRY;
        target = [[CKRecord alloc] initWithRecordType:CLOUD_TYPE recordID:cloud_record_id(record->name)];
    }
    NSURL *file = nil;
    if (payload) {
        file = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:
            [@"visr-cloud-" stringByAppendingString:NSUUID.UUID.UUIDString]]];
        if (![[NSData dataWithBytesNoCopy:(void *)payload length:size freeWhenDone:NO] writeToURL:file atomically:NO]) {
            host_logf(HOST_LOG_WARN, "cloud saves: cannot write the upload's temporary file");
            return CLOUD_RETRY;
        }
        target[@"payload"] = [[CKAsset alloc] initWithFileURL:file];
    }
    target[@"format"] = @(record->format);
    target[@"minFormat"] = @(record->min_format);
    target[@"unit"] = @(record->unit);
    target[@"contentHash"] = @(record->hash);
    target.encryptedValues[@"meta"] = @(record->meta);
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);
    __block NSError *failure = nil;
    __block NSString *tag = nil;
    CKModifyRecordsOperation *operation = [[CKModifyRecordsOperation alloc] initWithRecordsToSave:@[target] recordIDsToDelete:nil];
    operation.savePolicy = CKRecordSaveIfServerRecordUnchanged;
    operation.modifyRecordsCompletionBlock = ^(NSArray<CKRecord *> *saved, NSArray *deleted, NSError *error) {
        (void)deleted;
        failure = error;
        tag = saved.firstObject.recordChangeTag;
        dispatch_semaphore_signal(finished);
    };
    BOOL ran = cloud_run(operation, finished);
    if (file) [NSFileManager.defaultManager removeItemAtURL:file error:nil];
    if (!ran) return CLOUD_RETRY;
    if (failure) return cloud_status_of(failure, "saving");
    if (!tag) return CLOUD_RETRY;
    cloud_text(tag, new_tag, tag_size);
    return CLOUD_OK;
}

static enum cloud_status cloud_remove(void *context, const char *name) {
    (void)context;
    if (cloud_backing_off()) return CLOUD_RETRY;
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);
    __block NSError *failure = nil;
    CKModifyRecordsOperation *operation = [[CKModifyRecordsOperation alloc] initWithRecordsToSave:nil
        recordIDsToDelete:@[cloud_record_id(name)]];
    operation.modifyRecordsCompletionBlock = ^(NSArray *saved, NSArray *deleted, NSError *error) {
        (void)saved; (void)deleted;
        failure = error;
        dispatch_semaphore_signal(finished);
    };
    if (!cloud_run(operation, finished)) return CLOUD_RETRY;
    return cloud_status_of(failure, "removing a copy");
}

int host_cloudkit_store(const char *container, struct cloud_store *store) {
    /* CloudKit raises an exception, not an error, when the signature lacks the container */
    @try {
        cloud_container = [CKContainer containerWithIdentifier:@(container)];
        cloud_database = cloud_container.privateCloudDatabase;
    } @catch (NSException *exception) {
        host_logf(HOST_LOG_WARN, "cloud saves: the container %s cannot be opened: %s", container,
            exception.reason.UTF8String ?: exception.name.UTF8String);
        cloud_container = nil;
        return 0;
    }
    if (!cloud_database) return 0;
    cloud_zone = [[CKRecordZoneID alloc] initWithZoneName:CLOUD_ZONE ownerName:CKCurrentUserDefaultName];
    [NSNotificationCenter.defaultCenter addObserverForName:CKAccountChangedNotification object:nil queue:nil
        usingBlock:^(NSNotification *note) {
            (void)note;
            @synchronized (cloud_container) { cloud_user = nil; }
        }];
    *store = (struct cloud_store){ NULL, cloud_account, cloud_list, cloud_create_zone, cloud_fetch, cloud_save,
        cloud_remove, cloud_delete_zone };
    return 1;
}
