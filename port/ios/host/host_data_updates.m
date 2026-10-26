/* Recordings and shaders stay fixed for a launch; verified profiles may be offered at gameplay breaks. */
#import "host_data_updates.h"
#import "host_data_pack.h"
#import <CommonCrypto/CommonDigest.h>
#include "host.h"
#include "host_profile.h"
#include "host_shader_pack.h"
#include "host_recording_pack.h"
#import "host_benchmark_catalog.h"
#ifndef HALO_GUEST_SHA256
#include "guest_image.h"
#endif
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#import <Network/Network.h>

static HostDataPackStore *profiles, *recordings, *shaders;
static NSString *profileFolder, *recordingFolder;
static NSDictionary *profileIdentity, *recordingIdentity, *shaderIdentity, *shaderLists;
static dispatch_queue_t profileRefreshQueue;
static dispatch_source_t profileRefreshTimer, profileDirectoryWatch, profileFileWatch;
static NSString *profileDropIn;
static unsigned profileWatchGeneration;
static void (^profileVisitor)(NSString *, NSDictionary *);

static void offerProfileSnapshot(void)
{
    if (profileVisitor) [profiles visitPendingSnapshot:profileVisitor];
}

static void watchProfileFile(void);
static void profileDropInChanged(void)
{
    unsigned generation = ++profileWatchGeneration;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 4), profileRefreshQueue, ^{
        @autoreleasepool {
            if (generation != profileWatchGeneration) return;
            watchProfileFile();
            offerProfileSnapshot();
        }
    });
}
static dispatch_source_t profileVnode(NSString *path)
{
    int descriptor = open(path.fileSystemRepresentation, O_EVTONLY | O_CLOEXEC);
    if (descriptor < 0) return nil;
    dispatch_source_t source = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, descriptor,
        DISPATCH_VNODE_WRITE | DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME, profileRefreshQueue);
    if (!source) { close(descriptor); return nil; }
    dispatch_source_set_event_handler(source, ^{ profileDropInChanged(); });
    dispatch_source_set_cancel_handler(source, ^{ close(descriptor); });
    dispatch_resume(source);
    return source;
}
static void watchProfileFile(void)
{
    if (profileFileWatch) dispatch_source_cancel(profileFileWatch);
    profileFileWatch = profileVnode(profileDropIn);
}
static void ensureProfileRefreshQueue(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        profileRefreshQueue = dispatch_queue_create("visr.profile-refresh",
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    });
}
/* Both the legacy launch and the launch menu offer newly staged profile generations at gameplay breaks. */
static void observeProfileStaging(void)
{
    static dispatch_once_t once;
    ensureProfileRefreshQueue();
    dispatch_once(&once, ^{
        [NSNotificationCenter.defaultCenter addObserverForName:HostDataPackSnapshotStagedNotification object:profiles queue:nil usingBlock:^(NSNotification *note) {
            (void)note;
            dispatch_async(profileRefreshQueue, ^{ @autoreleasepool { offerProfileSnapshot(); } });
        }];
    });
}
/* Match launch's conditional request and signature checks, with a bounded play cadence. */
static void startProfileRefreshTimer(NSURL *targetURL)
{
    static dispatch_once_t once;
    if (!targetURL) return;
    ensureProfileRefreshQueue();
    dispatch_once(&once, ^{
        profileRefreshTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, profileRefreshQueue);
        dispatch_source_set_timer(profileRefreshTimer, dispatch_time(DISPATCH_TIME_NOW, 15 * 60 * NSEC_PER_SEC),
            15 * 60 * NSEC_PER_SEC, 30 * NSEC_PER_SEC);
        dispatch_source_set_event_handler(profileRefreshTimer, ^{ @autoreleasepool { [profiles checkForUpdateAtURL:targetURL]; } });
        dispatch_resume(profileRefreshTimer);
    });
}
static NSString *shaderSnapshotFolder;

static NSString *digest(NSData *data)
{
    unsigned char bytes[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, bytes);
    NSMutableString *result = [NSMutableString string];
    for (NSUInteger i = 0; i < sizeof(bytes); i++) [result appendFormat:@"%02x", bytes[i]];
    return result;
}

static NSDictionary *snapshotIdentity(NSDictionary *snapshot);
/* Bundled data also needs a content identity: app versions may contain changed files. */
static NSDictionary *identity(HostDataPackStore *store, NSString *folder)
{
    return snapshotIdentity(@{@"metadata": [store metadata], @"folder": folder});
}

static NSData *publicKey(NSString *text)
{
    if (![text isKindOfClass:NSString.class] || text.length != 64) return [NSData data];
    NSMutableData *key = [NSMutableData data];
    for (NSUInteger i = 0; i < 64; i += 2) {
        NSString *pair = [text substringWithRange:NSMakeRange(i, 2)];
        if ([pair rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdefABCDEF"] invertedSet]].location != NSNotFound) return [NSData data];
        unsigned value = 0;
        [[NSScanner scannerWithString:pair] scanHexInt:&value];
        uint8_t byte = value;
        [key appendBytes:&byte length:1];
    }
    return key;
}

/* Guest updates must not retain an unbounded set of old recording generations. */
static void pruneCompatibilityRoots(NSString *root)
{
    NSString *parent = root.stringByDeletingLastPathComponent;
    NSMutableArray<NSDictionary *> *old = [NSMutableArray array];
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:parent error:NULL]) {
        if (name.length != 64 || [name rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location != NSNotFound) continue;
        NSString *path = [parent stringByAppendingPathComponent:name];
        struct stat info;
        if ([path isEqual:root] || lstat(path.fileSystemRepresentation, &info) || !S_ISDIR(info.st_mode)) continue;
        [old addObject:@{@"path": path, @"modified": @(info.st_mtime)}];
    }
    [old sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) { return [b[@"modified"] compare:a[@"modified"]]; }];
    for (NSUInteger i = 1; i < old.count; i++) [NSFileManager.defaultManager removeItemAtPath:old[i][@"path"] error:NULL];
}

/* Probes point these at a temporary folder and their own key; the app leaves them nil. */
static NSString *dataPackCacheOverride, *dataPackChannelOverride;
static NSData *dataPackKeyOverride;
static HostDataPackStore *store(NSString *kind, NSString *bundled, NSString *compatibility, HostPackValidator validator)
{
    NSBundle *bundle = NSBundle.mainBundle;
    NSString *channel = dataPackChannelOverride ?: [bundle objectForInfoDictionaryKey:@"HaloDataPackChannel"];
    if (![channel isEqual:@"tester"] && ![channel isEqual:@"stable"]) channel = @"stable";
    NSData *key = dataPackKeyOverride ?: publicKey([bundle objectForInfoDictionaryKey:@"HaloDataPackPublicKey"]);
    NSString *cache = dataPackCacheOverride ?: NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
    NSString *root = [[[[cache stringByAppendingPathComponent:@"visr-data-packs"] stringByAppendingPathComponent:kind] stringByAppendingPathComponent:channel] stringByAppendingPathComponent:digest([compatibility dataUsingEncoding:NSUTF8StringEncoding])];
    pruneCompatibilityRoots(root);
    return [[HostDataPackStore alloc] initWithRoot:root bundled:bundled kind:kind channel:channel publicKey:key compatibility:compatibility validator:validator];
}

/* This generation's profile store for device (docs/data-packs.md, "Profile feed generations"). Its shards bind to
   HOST_PROFILE_FEED_DOMAIN, so a cached shard of another generation (the device-profiles-1 shards 0.4.2 to 0.5.1
   read) lives under another root and is never selected. A shard older than the bundled profiles is refused when it
   arrives and when a cached one is loaded, so older profiles never mask the bundled ones: a lower revision, or the
   same revision stamped from other sources. Without a bundled revision every shard counts as older. */
static BOOL profileShardOlder(long long revision, const char *sha256, long long bundled, NSString *bundledSHA256)
{
    return bundled <= 0 || revision < bundled || (revision == bundled && ![@(sha256) isEqualToString:bundledSHA256]);
}
static HostDataPackStore *profileStore(NSString *bundled, struct host_device device)
{
    char key[192];
    BOOL identified = host_profile_device_key(&device, key, sizeof(key));
    NSString *compatibility = [@HOST_PROFILE_FEED_DOMAIN ":" stringByAppendingString:identified ? @(key) : @"unidentified"];
    char bundledText[65];
    long long bundledRevision = host_profile_revision_read(bundled.fileSystemRepresentation, bundledText);
    NSString *bundledSHA256 = @(bundledText);
    if (bundledRevision <= 0)
        host_logf(HOST_LOG_WARN, "data-packs: the bundled profiles have no valid revision; profile downloads stay off");
    HostDataPackStore *result = store(@"profiles", bundled, compatibility, ^BOOL(NSString *folder, NSError **error) {
        char sha256[65];
        BOOL valid = identified && host_profile_validate_feed_folder(folder.fileSystemRepresentation, &device, HOST_PROFILE_FEED_GENERATION, NULL);
        long long revision = valid ? host_profile_revision_read(folder.fileSystemRepresentation, sha256) : 0;
        if (valid && profileShardOlder(revision, sha256, bundledRevision, bundledSHA256)) {
            host_logf(HOST_LOG_INFO, "data-packs: cached profiles (revision %lld) are older than the bundled ones (revision %lld); using the bundled profiles", revision, bundledRevision);
            valid = NO;
        }
        if (!valid && error) *error = [NSError errorWithDomain:@"VISRDataUpdates" code:1 userInfo:@{NSLocalizedDescriptionKey: @"Invalid profile snapshot"}];
        return valid;
    });
    result.supersededCheck = ^BOOL(NSDictionary<NSString *, NSData *> *files) {
        NSData *file = files[@"hardware/revision.toml"]; char sha256[65];
        long long revision = file ? host_profile_revision_parse(file.bytes, file.length, sha256) : 0;
        // A missing or malformed revision is the validator's to refuse, unless nothing can be newer than the bundle.
        BOOL older = (revision > 0 || bundledRevision <= 0) && profileShardOlder(revision, sha256, bundledRevision, bundledSHA256);
        if (older) host_logf(HOST_LOG_INFO, "data-packs: the served profiles (revision %lld) are older than the bundled ones (revision %lld); using the bundled profiles", revision, bundledRevision);
        return older;
    };
    return result;
}

static NSURL *profileFeedURL(NSString *base, const struct host_device *device)
{
    char key[192];
    if (![base isKindOfClass:NSString.class] || !host_profile_device_key(device,key,sizeof(key))) return nil;
    NSURL *url = [NSURL URLWithString:base];
    if (![url.scheme.lowercaseString isEqual:@"https"] || !url.host.length || url.user || url.password || url.query || url.fragment) return nil;
    NSArray *parts = [@(key) componentsSeparatedByString:@"/"];
    for (NSString *part in parts) url = [url URLByAppendingPathComponent:part];
    return [url URLByAppendingPathExtension:@"json"];
}

/* Read selected additions into immutable, bounded map bytes before starting a background check.
   Both size/read calls from the guest therefore observe exactly the same union for the launch. */
static NSDictionary *snapshotShaderLists(NSString *folder, NSString *bundled)
{
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    size_t total = 0;
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:folder error:NULL]) {
        if (result.count == HOST_SHADER_PACK_MAX_MAPS || ![name.pathExtension isEqual:@"txt"] ||
            !host_shader_map_name_valid(name.stringByDeletingPathExtension.UTF8String)) return nil;
        size_t size = 0, baseSize = 0, mergedSize = 0; char *merged = NULL;
        char *bytes = host_shader_list_read_file([folder stringByAppendingPathComponent:name].fileSystemRepresentation,&size);
        char *base = host_shader_list_read_file([bundled stringByAppendingPathComponent:name].fileSystemRepresentation,&baseSize);
        BOOL valid = bytes && size <= HOST_SHADER_PACK_MAX_TOTAL_BYTES-total &&
            host_shader_list_merge(base,baseSize,bytes,size,&merged,&mergedSize);
        free(bytes); free(base);
        if (!valid) return nil;
        total += size;
        result[name.stringByDeletingPathExtension] = [NSData dataWithBytesNoCopy:merged length:mergedSize freeWhenDone:YES];
    }
    return result.count ? [result copy] : nil;
}

static void initializeLegacy(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *resources = NSBundle.mainBundle.resourcePath;
        NSString *bundledProfiles = [resources stringByAppendingPathComponent:@"profiles"];
        NSString *bundledRecordings = [resources stringByAppendingPathComponent:@"benchmark-recordings"];
        struct host_device device; char deviceKey[192];
        host_device_identify(&device);
        BOOL identified = host_profile_device_key(&device,deviceKey,sizeof(deviceKey));
        profiles = profileStore(bundledProfiles, device);
        recordings = store(@"recordings", bundledRecordings, @"benchmark-catalog-1:" HALO_GUEST_SHA256, ^BOOL(NSString *folder, NSError **error) {
            BOOL valid = host_recording_pack_validate(folder.fileSystemRepresentation, NULL) &&
                host_benchmark_catalog_validate_folder(folder, error);
            if (!valid && error && !*error) *error = [NSError errorWithDomain:@"VISRDataUpdates" code:2 userInfo:@{NSLocalizedDescriptionKey: @"Invalid recording snapshot"}];
            return valid;
        });
        NSString *bundledShaders = [resources stringByAppendingPathComponent:@"shader-lists"];
        shaders = store(@"shaders", bundledShaders, @HOST_SHADER_PACK_COMPATIBILITY, ^BOOL(NSString *folder, NSError **error) {
            BOOL valid = host_shader_pack_validate(folder.fileSystemRepresentation,bundledShaders.fileSystemRepresentation,NULL);
            if (!valid && error) *error = [NSError errorWithDomain:@"VISRDataUpdates" code:3 userInfo:@{NSLocalizedDescriptionKey: @"Invalid shader snapshot"}];
            return valid;
        });
        observeProfileStaging();
        profileFolder = [profiles beginLaunch];
        recordingFolder = [recordings beginLaunch];
        profileIdentity = identity(profiles, profileFolder);
        recordingIdentity = identity(recordings, recordingFolder);
        NSString *shaderFolder = [shaders beginLaunch];
        shaderIdentity = identity(shaders,shaderFolder);
        if (![shaderIdentity[@"bundled"] boolValue]) {
            shaderLists = snapshotShaderLists(shaderFolder,bundledShaders);
            if (!shaderLists) {
                NSMutableDictionary *fallback = [shaderIdentity mutableCopy];
                fallback[@"effective_bundled_fallback"] = @YES; shaderIdentity = [fallback copy];
            }
        }
        if (publicKey([NSBundle.mainBundle objectForInfoDictionaryKey:@"HaloDataPackPublicKey"]).length == 32) {
            NSString *profileURL = [NSBundle.mainBundle objectForInfoDictionaryKey:@"HaloProfileFeedURL"];
            NSString *recordingURL = [NSBundle.mainBundle objectForInfoDictionaryKey:@"HaloRecordingFeedURL"];
            NSURL *targetURL = identified ? profileFeedURL(profileURL,&device) : nil;
            if (targetURL) {
                [profiles checkForUpdateAtURL:targetURL];
                startProfileRefreshTimer(targetURL);
            }
            NSString *shaderURL = [NSBundle.mainBundle objectForInfoDictionaryKey:@"HaloShaderFeedURL"];
            if (shaderURL.length) [shaders checkForUpdateAtURL:[NSURL URLWithString:shaderURL]];
            if (recordingURL.length) [recordings checkForUpdateAtURL:[NSURL URLWithString:recordingURL]];
        }
    });
}
/* Presentation only holds this lock while copying memory. Validators and stores never take it. */
static NSObject *selectionLock(void)
{
    static NSObject *lock; static dispatch_once_t once;
    dispatch_once(&once, ^{ lock = [NSObject new]; });
    return lock;
}
static dispatch_queue_t updatesQueue(void)
{
    static dispatch_queue_t queue; static dispatch_once_t once;
    dispatch_once(&once, ^{ queue = dispatch_queue_create("org.visr.data-updates", DISPATCH_QUEUE_SERIAL); });
    return queue;
}
static BOOL menuRequested, menuFrozen, menuVisible, menuInitialized;
static BOOL profileReady, recordingReady, profileLoading, recordingLoading;
static BOOL profileChecking, recordingChecking, profileRetryPending, recordingRetryPending;
static NSURL *menuProfileURL, *menuRecordingURL;
static NSDictionary *menuState;
/* While the menu is up, a failed check is tried again quietly (the failure text stays until the data loads):
   after 5, 15 and 30 s, then every minute, and at once when the network path changes. The retry state belongs to
   the updates queue. */
static const double menuRetryDelays[] = {5, 15, 30, 60};
static unsigned menuRetryAttempt, menuRetryGeneration;
static BOOL menuRetryScheduled, profileAttempted, recordingAttempted, profileUnpublishedLogged;
static void refreshMenuKinds(BOOL profile, BOOL recording);

static void publishMenuState(void)
{
    @synchronized(selectionLock()) {
        BOOL ready = profileReady && recordingReady;
        BOOL failed = menuInitialized && ((!profileReady && !profileLoading) || (!recordingReady && !recordingLoading));
        BOOL loading = !ready && !failed && (!menuInitialized || profileLoading || recordingLoading);
        menuState = @{@"ready": @(ready), @"loading": @(loading),
            @"profileFinished": @(menuInitialized && !profileLoading),
            @"reason": ready || loading ? @"" : !profileReady && !profileLoading ? @"Can't reach the profile server. Benchmarking disabled." : @"Benchmark recordings are unavailable"};
    }
}
static NSString *bundledFolder(NSString *name)
{
    return [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:name];
}
static NSDictionary *snapshotIdentity(NSDictionary *snapshot)
{
    NSMutableDictionary *result = [snapshot[@"metadata"] mutableCopy];
    if ([result[@"bundled"] boolValue]) {
        NSMutableData *bytes = [NSMutableData data];
        NSString *folder = snapshot[@"folder"];
        for (NSString *path in [[NSFileManager.defaultManager subpathsAtPath:folder] sortedArrayUsingSelector:@selector(compare:)]) {
            NSData *file = [NSData dataWithContentsOfFile:[folder stringByAppendingPathComponent:path]];
            if (!file) continue;
            [bytes appendData:[path dataUsingEncoding:NSUTF8StringEncoding]];
            [bytes appendBytes:"\0" length:1];
            [bytes appendData:[digest(file) dataUsingEncoding:NSUTF8StringEncoding]];
        }
        result[@"sha256"] = digest(bytes);
    }
    return result;
}
static NSString *publishMenuSnapshot(BOOL profile, NSDictionary *snapshot)
{
    NSString *bundled = bundledFolder(profile ? @"profiles" : @"benchmark-recordings");
    @synchronized(selectionLock()) {
        NSString *selected = profile ? profileFolder : recordingFolder;
        if (menuFrozen && ![snapshot[@"folder"] isEqual:selected ?: bundled] && (!selected || [selected isEqual:bundled])) {
            NSMutableDictionary *metadata = [snapshot[@"metadata"] mutableCopy];
            metadata[@"bundled"] = @YES; metadata[@"sequence"] = @0; metadata[@"sha256"] = @"";
            snapshot = @{@"folder": bundled, @"metadata": metadata};
        }
    }
    NSDictionary *metadata = snapshotIdentity(snapshot);
    NSString *folder = snapshot[@"folder"];
    BOOL needsFallback = NO;
    NSString *selectedFolder;
    @synchronized(selectionLock()) {
        NSString *selected = profile ? profileFolder : recordingFolder;
        if (!menuFrozen) {
            if (profile) { profileFolder = folder; profileIdentity = metadata; }
            else { recordingFolder = folder; recordingIdentity = metadata; }
        } else if ([folder isEqual:selected ?: bundled]) {
            // A fallback selected before hashing completed may acquire its content identity, never a new folder.
            if (profile) profileIdentity = metadata; else recordingIdentity = metadata;
        } else needsFallback = !selected || [selected isEqual:bundled];
        selectedFolder = (profile ? profileFolder : recordingFolder) ?: bundled;
    }
    // Play may have frozen a fallback during hashing. One worker-only second pass hashes that exact folder.
    // The freeze is permanent, so the second pass cannot need another retry.
    return needsFallback ? publishMenuSnapshot(profile, snapshot) : selectedFolder;
}
static BOOL selectionFrozen(void)
{
    @synchronized(selectionLock()) { return menuFrozen; }
}
static BOOL menuShowing(void)
{
    @synchronized(selectionLock()) { return menuVisible && !menuFrozen; }
}
static void scheduleMenuRetry(void)
{
    BOOL needed;
    @synchronized(selectionLock()) { needed = (menuProfileURL && !profileReady) || (menuRecordingURL && !recordingReady); }
    if (!needed) { menuRetryAttempt = 0; return; }
    if (menuRetryScheduled || profileChecking || recordingChecking || !menuShowing()) return;
    unsigned count = sizeof(menuRetryDelays) / sizeof(menuRetryDelays[0]);
    double delay = menuRetryDelays[menuRetryAttempt < count ? menuRetryAttempt : count - 1];
    if (menuRetryAttempt < count) menuRetryAttempt++;
    unsigned generation = ++menuRetryGeneration;
    menuRetryScheduled = YES;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)), updatesQueue(), ^{
        if (generation != menuRetryGeneration) return;
        menuRetryScheduled = NO;
        if (!menuShowing()) return;
        BOOL profile, recording;
        @synchronized(selectionLock()) { profile = !profileReady; recording = !recordingReady; }
        refreshMenuKinds(profile, recording);
    });
}
/* Cancels a waiting retry; the next failure starts the delays over. */
static void resetMenuRetry(void)
{
    menuRetryGeneration++; menuRetryScheduled = NO; menuRetryAttempt = 0;
}
static void refreshMenuKinds(BOOL profile, BOOL recording)
{
    if (!profiles || !recordings) return;
    @synchronized(selectionLock()) { if (!menuVisible) return; }
    if (profile && menuProfileURL && !profileChecking) {
        profileChecking = YES;
        // Only the first check shows "Downloading"; a retry leaves the failure text until the data arrives.
        @synchronized(selectionLock()) { if (!menuFrozen && !profileAttempted) profileLoading = YES; }
        profileAttempted = YES;
        [profiles checkForUpdateAtURL:menuProfileURL completion:^(BOOL success) {
            dispatch_async(updatesQueue(), ^{
                profileChecking = NO;
                if (!selectionFrozen()) {
                    NSDictionary *snapshot = [profiles menuSnapshot];
                    NSString *selected = publishMenuSnapshot(YES, snapshot);
                    [profiles retainMenuFolder:selected];
                    // The feed has no profile for this model (the publisher shards by model): its bundled profiles
                    // are the best there is, so they do not hold the benchmark back.
                    BOOL unpublished = profiles.lastFeedStatus == 404;
                    // The feed's profiles are older than the bundled ones, which are then the best there is too.
                    BOOL superseded = profiles.lastFeedSuperseded;
                    BOOL bundled;
                    @synchronized(selectionLock()) {
                        bundled = !profileIdentity || [profileIdentity[@"bundled"] boolValue];
                        profileReady = !bundled || unpublished || superseded;
                        profileLoading = NO;
                    }
                    if (bundled && unpublished && !profileUnpublishedLogged) {
                        profileUnpublishedLogged = YES;
                        host_logf(HOST_LOG_INFO, "data-packs: no profile is published for this device; the benchmark uses the bundled profiles");
                    }
                    (void)success;
                }
                @synchronized(selectionLock()) { profileLoading = NO; }
                publishMenuState();
                if (profileRetryPending) { profileRetryPending = NO; refreshMenuKinds(YES, NO); }
                scheduleMenuRetry();
            });
        }];
    }
    if (recording && menuRecordingURL && !recordingChecking) {
        recordingChecking = YES;
        @synchronized(selectionLock()) { if (!menuFrozen && !recordingAttempted) recordingLoading = YES; }
        recordingAttempted = YES;
        [recordings checkForUpdateAtURL:menuRecordingURL completion:^(BOOL success) {
            dispatch_async(updatesQueue(), ^{
                recordingChecking = NO;
                if (!selectionFrozen()) {
                    NSDictionary *snapshot = [recordings menuSnapshot];
                    NSString *selected = publishMenuSnapshot(NO, snapshot);
                    [recordings retainMenuFolder:selected];
                    BOOL valid = host_benchmark_catalog_validate_folder(selected, NULL);
                    @synchronized(selectionLock()) { recordingReady = valid; recordingLoading = NO; }
                    (void)success;
                }
                @synchronized(selectionLock()) { recordingLoading = NO; }
                publishMenuState();
                if (recordingRetryPending) { recordingRetryPending = NO; refreshMenuKinds(NO, YES); }
                scheduleMenuRetry();
            });
        }];
    }
    publishMenuState();
}
static void refreshMenu(void) { refreshMenuKinds(YES, YES); }
static void requestMenuRefresh(void)
{
    resetMenuRetry();
    if (profileChecking) profileRetryPending = YES;
    if (recordingChecking) recordingRetryPending = YES;
    refreshMenu();
}
static void commitMenuSelection(void)
{
    NSString *profile, *recording, *shader;
    @synchronized(selectionLock()) {
        profile = profileFolder ?: bundledFolder(@"profiles");
        recording = recordingFolder ?: bundledFolder(@"benchmark-recordings");
        shader = shaderSnapshotFolder ?: bundledFolder(@"shader-lists");
    }
    [profiles selectMenuFolder:profile]; [recordings selectMenuFolder:recording]; [shaders selectMenuFolder:shader];
    if (!profiles) return;
    ensureProfileRefreshQueue();
    // Play has frozen the launch selection; later verified generations wait for a gameplay break.
    startProfileRefreshTimer(menuProfileURL);
    dispatch_async(profileRefreshQueue, ^{ @autoreleasepool { offerProfileSnapshot(); } });
}
static void initializeMenu(struct host_device device)
{
    NSString *resources = NSBundle.mainBundle.resourcePath;
    NSString *bundledProfiles = [resources stringByAppendingPathComponent:@"profiles"];
    NSString *bundledRecordings = [resources stringByAppendingPathComponent:@"benchmark-recordings"];
    char deviceKey[192]; BOOL identified = host_profile_device_key(&device, deviceKey, sizeof(deviceKey));
    NSData *key = publicKey([NSBundle.mainBundle objectForInfoDictionaryKey:@"HaloDataPackPublicKey"]);
    menuProfileURL = key.length == 32 && identified ? profileFeedURL([NSBundle.mainBundle objectForInfoDictionaryKey:@"HaloProfileFeedURL"], &device) : nil;
    NSString *recordingURL = [NSBundle.mainBundle objectForInfoDictionaryKey:@"HaloRecordingFeedURL"];
    menuRecordingURL = key.length == 32 && recordingURL.length ? [NSURL URLWithString:recordingURL] : nil;
    profiles = profileStore(bundledProfiles, device);
    observeProfileStaging();
    NSDictionary *profile = [profiles menuSnapshot];
    NSString *selectedProfile = publishMenuSnapshot(YES, profile);
    [profiles retainMenuFolder:selectedProfile];
    @synchronized(selectionLock()) {
        profileReady = !menuProfileURL || (profileIdentity && ![profileIdentity[@"bundled"] boolValue]);
        profileLoading = menuProfileURL != nil;
    }
    recordings = store(@"recordings", bundledRecordings, @"benchmark-catalog-1:" HALO_GUEST_SHA256, ^BOOL(NSString *folder, NSError **error) {
        return host_recording_pack_validate(folder.fileSystemRepresentation, NULL) && host_benchmark_catalog_validate_folder(folder, error);
    });
    NSDictionary *recording = [recordings menuSnapshot];
    NSString *selectedRecording = publishMenuSnapshot(NO, recording);
    [recordings retainMenuFolder:selectedRecording];
    BOOL valid = host_benchmark_catalog_validate_folder(selectedRecording, NULL);
    @synchronized(selectionLock()) {
        recordingReady = valid; recordingLoading = menuRecordingURL != nil; menuInitialized = YES;
    }
    publishMenuState();
    if (selectionFrozen()) commitMenuSelection();
    refreshMenu();
    // Shader validation/hashing never delays profile availability or benchmark readiness.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSString *bundled = [resources stringByAppendingPathComponent:@"shader-lists"];
        HostDataPackStore *shaderStore = store(@"shaders", bundled, @HOST_SHADER_PACK_COMPATIBILITY, ^BOOL(NSString *folder, NSError **error) {
            (void)error; return host_shader_pack_validate(folder.fileSystemRepresentation, bundled.fileSystemRepresentation, NULL);
        });
        NSDictionary *snapshot = [shaderStore menuSnapshot];
        NSDictionary *metadata = snapshotIdentity(snapshot);
        NSDictionary *lists = [metadata[@"bundled"] boolValue] ? nil : snapshotShaderLists(snapshot[@"folder"], bundled);
        if (![metadata[@"bundled"] boolValue] && !lists) {
            NSMutableDictionary *fallback = [metadata mutableCopy]; fallback[@"effective_bundled_fallback"] = @YES; metadata = fallback;
        }
        dispatch_async(updatesQueue(), ^{
            shaders = shaderStore;
            BOOL published = NO;
            @synchronized(selectionLock()) {
                if (!menuFrozen) { shaderIdentity = metadata; shaderLists = lists; shaderSnapshotFolder = snapshot[@"folder"]; published = YES; }
            }
            if (!published) {
                [shaders selectMenuFolder:bundled];
                NSDictionary *fallback = snapshotIdentity(@{@"folder": bundled, @"metadata": [shaders metadata]});
                @synchronized(selectionLock()) { if (!shaderIdentity) shaderIdentity = fallback; }
            }
            NSString *url = [NSBundle.mainBundle objectForInfoDictionaryKey:@"HaloShaderFeedURL"];
            if (key.length == 32 && url.length) [shaders checkForUpdateAtURL:[NSURL URLWithString:url]];
        });
    });
    static nw_path_monitor_t monitor;
    monitor = nw_path_monitor_create();
    __block nw_path_t previous;
    nw_path_monitor_set_queue(monitor, updatesQueue());
    nw_path_monitor_set_update_handler(monitor, ^(nw_path_t path) {
        BOOL changed = previous && !nw_path_is_equal(previous, path);
        previous = path;
        if (changed) requestMenuRefresh();
    });
    nw_path_monitor_start(monitor);
}
void host_data_updates_menu_start(void)
{
    @synchronized(selectionLock()) { menuRequested = YES; menuVisible = YES; }
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        // host_device_identify reads UIScreen; capture it without waiting for the presentation thread.
        dispatch_async(dispatch_get_main_queue(), ^{
            struct host_device device; host_device_identify(&device);
            dispatch_async(updatesQueue(), ^{ initializeMenu(device); });
        });
    });
    dispatch_async(updatesQueue(), ^{ requestMenuRefresh(); });
}
NSDictionary *host_data_updates_menu_state(void)
{
    @synchronized(selectionLock()) {
        return menuState ?: @{@"ready": @NO, @"loading": @YES, @"reason": @""};
    }
}
void host_data_updates_menu_finish(void)
{
    @synchronized(selectionLock()) {
        if (menuFrozen) return;
        menuRequested = YES; menuFrozen = YES; menuVisible = NO;
    }
    dispatch_async(updatesQueue(), ^{ commitMenuSelection(); });
}
static BOOL usesMenu(void)
{
    @synchronized(selectionLock()) { return menuRequested; }
}
void host_profile_updates_watch(NSString *dropIn, void (^visitor)(NSString *, NSDictionary *))
{
    if (!usesMenu()) initializeLegacy();
    ensureProfileRefreshQueue();
    void (^watch)(void) = ^{
        dispatch_async(profileRefreshQueue, ^{ @autoreleasepool {
            profileVisitor = [visitor copy];
            profileDropIn = [dropIn copy];
            if (profileDirectoryWatch) dispatch_source_cancel(profileDirectoryWatch);
            profileDirectoryWatch = profileVnode(profileDropIn.stringByDeletingLastPathComponent);
            watchProfileFile();
            offerProfileSnapshot();
        } });
    };
    // The menu's store creation and Play selection run on the updates queue; offer after both.
    if (usesMenu()) dispatch_async(updatesQueue(), watch); else watch();
}
NSString *host_profile_update_folder(NSString *bundled)
{
    if (!usesMenu()) initializeLegacy();
    @synchronized(selectionLock()) { return profileFolder ?: bundled; }
}
NSString *host_recording_update_folder(NSString *bundled)
{
    if (!usesMenu()) initializeLegacy();
    @synchronized(selectionLock()) { return recordingFolder ?: bundled; }
}
NSData *host_shader_update_list(const char *name)
{
    if (!usesMenu()) initializeLegacy();
    @synchronized(selectionLock()) { return name && host_shader_map_name_valid(name) ? shaderLists[@(name)] : nil; }
}
NSDictionary *host_data_updates_metadata(void)
{
    if (!usesMenu()) initializeLegacy();
    @synchronized(selectionLock()) {
        NSDictionary *bundled = @{@"bundled": @YES, @"sequence": @0, @"sha256": @""};
        return @{@"profiles": profileIdentity ?: bundled, @"recordings": recordingIdentity ?: bundled, @"shaders": shaderIdentity ?: bundled};
    }
}
void host_data_updates_healthy(void)
{
    dispatch_async(updatesQueue(), ^{
        @autoreleasepool {
            if (!usesMenu()) initializeLegacy();
            [profiles markHealthy]; [recordings markHealthy]; if (shaderLists) [shaders markHealthy];
        }
    });
}
