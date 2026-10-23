/* Automatic benchmark delivery. Immutable outbox files and content-derived record IDs
   make a timed-out or offline attempt safe to retry without duplicate records. */
#import <CloudKit/CloudKit.h>
#import <CommonCrypto/CommonDigest.h>
#include "ios_host.h"
#include "host_benchmark.h"
#import "host_benchmark_outbox.h"
#import "host_benchmark_snapshot.h"
#include <zlib.h>
#define RECORD_TYPE @"BenchmarkReport"
#define SEND_SECONDS 12

static NSData *gzipped(NSData *data) {
    NSMutableData *out = [NSMutableData dataWithLength:compressBound((uLong)data.length) + 32];
    z_stream stream = { 0 };

    if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return nil;
    stream.next_in = (Bytef *)data.bytes; stream.avail_in = (uInt)data.length;
    stream.next_out = out.mutableBytes; stream.avail_out = (uInt)out.length;
    if (deflate(&stream, Z_FINISH) != Z_STREAM_END) { deflateEnd(&stream); return nil; }
    out.length = stream.total_out;
    deflateEnd(&stream);
    return out;
}

#define SUMMARY_MAXIMUM_BYTES (512 * 1024)
static NSString *bounded_text(id value, NSUInteger maximum) {
    if(![value isKindOfClass:NSString.class])return @"";
    return [value length]>maximum ? [value substringToIndex:maximum] : value;
}

/* Searchable record fields share CloudKit's 1 MiB limit. The asset and outbox
   retain every original byte; only this separate summary omits trace payloads. */
static NSString *summary_json(NSData *report) {
    id parsed=report ? [NSJSONSerialization JSONObjectWithData:report options:NSJSONReadingMutableContainers error:nil] : nil;
    if(![parsed isKindOfClass:NSMutableDictionary.class] || ![parsed[@"runs"] isKindOfClass:NSArray.class])return @"";
    NSMutableDictionary *whole=parsed;
    NSMutableArray *runs=[whole[@"runs"] mutableCopy];
    if([whole[@"shader_output_intervals"] isKindOfClass:NSArray.class])
        for(id prior in whole[@"shader_output_intervals"])
            if([prior isKindOfClass:NSDictionary.class] && [prior[@"runs"] isKindOfClass:NSArray.class])
                [runs addObjectsFromArray:prior[@"runs"]];
    /* A display-variant suite's last report carries the earlier passes whole; the summary keeps their identity,
       flags and runs, without what the asset alone needs. */
    if([whole[@"variant_reports"] isKindOfClass:NSArray.class])
        for(id prior in whole[@"variant_reports"])
            if([prior isKindOfClass:NSMutableDictionary.class]) {
                [prior removeObjectsForKeys:@[@"shader_dependency_candidates",@"shader_output_intervals",@"data_updates",
                    @"recording_snapshot",@"output_reports"]];
                if([prior[@"runs"] isKindOfClass:NSArray.class])[runs addObjectsFromArray:prior[@"runs"]];
            }
    for(id value in runs) {
        if(![value isKindOfClass:NSMutableDictionary.class])return @"";
        NSMutableDictionary *run=value;
        [run removeObjectsForKeys:@[@"frame_interval",@"frame_cpu",@"frame_gpu"]];
        id presentation=run[@"presentation"];
        id trace=[presentation isKindOfClass:NSDictionary.class] ? presentation[@"trace"] : nil;
        if(![trace isKindOfClass:NSMutableDictionary.class] || ![trace[@"data"] isKindOfClass:NSString.class])continue;
        NSData *payload=[trace[@"data"] dataUsingEncoding:NSUTF8StringEncoding];
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256(payload.bytes,(CC_LONG)payload.length,digest);
        NSMutableString *hash=[NSMutableString stringWithCapacity:64];
        for(size_t i=0;i<sizeof(digest);i++)[hash appendFormat:@"%02x",digest[i]];
        trace[@"encoded_bytes"]=@(payload.length);
        trace[@"encoded_sha256"]=hash;
        trace[@"payload_location"]=@"report_asset";
        trace[@"source_status"]=trace[@"status"] ?: @"unknown";
        trace[@"status"]=@"omitted_from_searchable_summary";
        [trace removeObjectForKey:@"data"];
    }
    host_benchmark_shader_summarize(whole);
    NSData *text=[NSJSONSerialization dataWithJSONObject:whole options:NSJSONWritingSortedKeys error:nil];
    if(text.length>SUMMARY_MAXIMUM_BYTES) {
        NSMutableArray *statuses=[NSMutableArray array];
        for(NSDictionary *run in whole[@"runs"]) {
            if(statuses.count==64)break;
            [statuses addObject:@{ @"index":[run[@"index"] isKindOfClass:NSNumber.class] ? run[@"index"] : @(statuses.count),
                @"scene":bounded_text(run[@"scene"],48), @"status":bounded_text(run[@"status"],32) }];
        }
        /* Even malformed or unexpectedly large metadata cannot make another
           oversized record. Identity also remains in CloudKit's own columns. */
        NSArray *all_flags=[whole[@"flags"] isKindOfClass:NSArray.class] ? whole[@"flags"] : @[];
        NSMutableArray *compact_flags=[NSMutableArray array];
        for(id flag in all_flags) {
            if(compact_flags.count==64)break;
            if([flag isKindOfClass:NSString.class])[compact_flags addObject:bounded_text(flag,128)];
        }
        /* a display-variant suite: each pass's variant, code and run statuses, a few bytes each */
        NSMutableArray *passes=[NSMutableArray array];
        NSArray *prior=[whole[@"variant_reports"] isKindOfClass:NSArray.class] ? whole[@"variant_reports"] : @[];
        for(id report in [prior arrayByAddingObject:whole]) {
            if(passes.count==8 || ![report isKindOfClass:NSDictionary.class] ||
                ![report[@"variant"] isKindOfClass:NSDictionary.class])continue;
            NSMutableArray *pass_statuses=[NSMutableArray array];
            NSArray *pass_runs=[report[@"runs"] isKindOfClass:NSArray.class] ? report[@"runs"] : @[];
            for(id run in pass_runs) {
                if(pass_statuses.count==32)break;
                if([run isKindOfClass:NSDictionary.class])[pass_statuses addObject:@{
                    @"scene":bounded_text(run[@"scene"],48), @"status":bounded_text(run[@"status"],32) }];
            }
            [passes addObject:@{ @"variant":bounded_text(report[@"variant"][@"id"],48), @"code":bounded_text(report[@"code"],32),
                @"run_count":@(pass_runs.count), @"runs":pass_statuses }];
        }
        NSMutableDictionary *compact=[@{ @"summary_status":@"compact", @"code":bounded_text(whole[@"code"],32),
            @"mode":bounded_text(whole[@"mode"],64), @"run_count":@([whole[@"runs"] count]),
            @"runs":statuses, @"statuses_omitted":@([whole[@"runs"] count]-statuses.count),
            @"flags":compact_flags, @"flags_omitted":@(all_flags.count-compact_flags.count),
            @"completion":[all_flags containsObject:@"finished_early"] ? @"finished_early" : @"see_report_asset",
            @"detail_asset_field":@"report", @"original_summary_bytes":@(text.length) } mutableCopy];
        if(passes.count)compact[@"variant_passes"]=passes;
        text=[NSJSONSerialization dataWithJSONObject:compact options:NSJSONWritingSortedKeys error:nil];
    }
    if(!text || text.length>SUMMARY_MAXIMUM_BYTES)
        return @"{\"detail_asset_field\":\"report\",\"summary_status\":\"serialization_failed\"}";
    return [[NSString alloc] initWithData:text encoding:NSUTF8StringEncoding];
}

static BOOL wait_for(dispatch_semaphore_t done, CFAbsoluteTime deadline) {
    while(CFAbsoluteTimeGetCurrent()<deadline) {
        if(dispatch_semaphore_wait(done,DISPATCH_TIME_NOW)==0)return YES;
        @autoreleasepool { CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.05,true); }
    }
    return dispatch_semaphore_wait(done,DISPATCH_TIME_NOW)==0;
}

/* saves the record; 1 sent, 0 not, with a sentence for the screen either way */
static int upload(NSData *report, NSString *record_name, double seconds, NSString **message) {
    NSDictionary *whole = [NSJSONSerialization JSONObjectWithData:report options:0 error:nil];
    NSDictionary *device = whole[@"device"], *app = whole[@"app"];
    CKContainer *container = [CKContainer containerWithIdentifier:@HALO_BENCHMARK_CLOUD];
    NSURL *file = [[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:
        [NSString stringWithFormat:@"visr-benchmark-%@.json.gz", record_name]];
    NSDateFormatter *minutes = [NSDateFormatter new];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block CKAccountStatus account = CKAccountStatusCouldNotDetermine;
    __block NSError *failure = nil;
    CFAbsoluteTime until = CFAbsoluteTimeGetCurrent() + seconds;
    CKRecord *record;

    if (![whole isKindOfClass:NSDictionary.class] || ![gzipped(report) writeToURL:file atomically:YES]) {
        *message = @"The report could not be prepared for sending.";
        return 0;
    }
    [container accountStatusWithCompletionHandler:^(CKAccountStatus status, NSError *error) {
        (void)error;
        account = status;
        dispatch_semaphore_signal(done);
    }];
    BOOL account_answered=wait_for(done,until);
    if (!account_answered || account != CKAccountStatusAvailable) {
        *message = @"This device is not signed in to iCloud, so the report cannot be sent this way.";
        host_logf(HOST_LOG_INFO, "benchmark: not sent: iCloud account status %ld", (long)account);
        return 0;
    }
    minutes.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    minutes.timeZone = [NSTimeZone timeZoneForSecondsFromGMT:0];
    minutes.dateFormat = @"yyyy-MM-dd'T'HH:mm'Z'";
    record = [[CKRecord alloc] initWithRecordType:RECORD_TYPE recordID:
        [[CKRecordID alloc] initWithRecordName:record_name]];
    record[@"code"] = whole[@"code"];
    record[@"schema"] = whole[@"format"];
    record[@"suite"] = whole[@"suite"];
    record[@"mode"] = whole[@"mode"];
    record[@"model"] = device[@"model"];
    record[@"platform"] = device[@"platform"];
    record[@"osVersion"] = device[@"os"];
    record[@"appVersion"] = app[@"version"];
    record[@"appBuild"] = app[@"build"];
    record[@"started"] = [minutes dateFromString:whole[@"started"] ?: @""];
    record[@"runs"] = @([whole[@"runs"] count]);
    if ([whole[@"flags"] count]) record[@"flags"] = whole[@"flags"];
    record[@"summary"] = summary_json(report);
    record[@"report"] = [[CKAsset alloc] initWithFileURL:file];
    CKModifyRecordsOperation *operation = [[CKModifyRecordsOperation alloc] initWithRecordsToSave:@[record] recordIDsToDelete:nil];
    operation.savePolicy = CKRecordSaveAllKeys;
    operation.atomic = YES;
    operation.configuration.timeoutIntervalForRequest = seconds;
    operation.configuration.timeoutIntervalForResource = seconds;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    operation.modifyRecordsCompletionBlock = ^(NSArray<CKRecord *> *saved, NSArray<CKRecordID *> *deleted, NSError *error) {
        (void)saved; (void)deleted;
        failure = error;
        dispatch_semaphore_signal(done);
    };
#pragma clang diagnostic pop
    [container.publicCloudDatabase addOperation:operation];
    if(!wait_for(done,until)) {
        [operation cancel];
        *message=@"Sending took too long. The report is still on this device.";
        host_logf(HOST_LOG_INFO,"benchmark: upload pending after bounded attempt");
        return 0;
    }
    [NSFileManager.defaultManager removeItemAtURL:file error:nil];
    if (failure) {
        *message = @"The report could not be sent. It is still on this device.";
        host_logf(HOST_LOG_INFO, "benchmark: not sent: %s (%s %ld)", failure.localizedDescription.UTF8String,
            failure.domain.UTF8String, (long)failure.code);
        return 0;
    }
    *message = [NSString stringWithFormat:@"Sent. Thank you. The report's code is %@.", whole[@"code"]];
    host_logf(HOST_LOG_INFO, "benchmark: report %s sent as record %s", [whole[@"code"] UTF8String], record.recordID.recordName.UTF8String);
    return 1;
}


int host_benchmark_send(const char *report_path, const char *summary, char *message, unsigned long size) {
    (void)summary;
    NSString *folder=@"benchmark/outbox";
    NSData *report=report_path ? [NSData dataWithContentsOfFile:@(report_path)] : nil;
    NSError *error=nil;
    NSString *current=report ? host_benchmark_outbox_store(folder,report,&error) : nil;
    if(report_path && !current) {
        snprintf(message,size,"Could not queue this report. Keep this screen open; the original report is retained.");
        host_logf(HOST_LOG_INFO,"benchmark: outbox could not preserve report: %s",error.localizedDescription.UTF8String ?: "unknown error");
        return 0;
    }
    NSMutableArray *pending=[host_benchmark_outbox_pending(folder) mutableCopy];
    if(current && [pending containsObject:current]) { [pending removeObject:current]; [pending insertObject:current atIndex:0]; }
    if(!current && !pending.count) { if(size)message[0]=0; return 1; }
    CFAbsoluteTime deadline=CFAbsoluteTimeGetCurrent()+SEND_SECONDS;
    BOOL current_sent=!current || ![pending containsObject:current];
    for(NSString *path in pending) {
        double left=deadline-CFAbsoluteTimeGetCurrent();
        if(left<=0)break;
        NSData *data=[NSData dataWithContentsOfFile:path];
        NSString *result=nil;
        if(!data || !upload(data,path.lastPathComponent.stringByDeletingPathExtension,left,&result))break;
        if(!host_benchmark_outbox_acknowledge(path,&error))break;
        if([path isEqual:current])current_sent=YES;
    }
    NSUInteger count=host_benchmark_outbox_pending(folder).count;
    snprintf(message,size,"%s",count ? [[NSString stringWithFormat:@"%lu report%@ saved on this device. Upload will retry automatically.",
        (unsigned long)count,count==1 ? @"" : @"s"] UTF8String] : "Benchmark data uploaded.");
    return current_sent;
}
