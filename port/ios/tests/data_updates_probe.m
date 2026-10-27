/* Exercise manager identity and retention without writing to the app cache. */
#import "../host/host_data_updates.m"
#include <assert.h>
#include <unistd.h>
#import <objc/runtime.h>
extern NSDictionary *host_data_updates_menu_state(void);
extern void host_data_updates_menu_finish(void);
void host_logf(int priority, const char *format, ...) { (void)priority; (void)format; }
void host_device_identify(struct host_device *d) { memset(d,0,sizeof(*d)); strcpy(d->platform,"ios"); strcpy(d->model,"iPhone18,1"); }
static NSString *blockedPath;
static dispatch_semaphore_t readEntered, resumeRead;
static IMP originalRead;
static NSData *blockedRead(id object, SEL selector, NSString *path)
{
    if ([path isEqual:blockedPath]) {
        dispatch_semaphore_signal(readEntered);
        dispatch_semaphore_wait(resumeRead, DISPATCH_TIME_FOREVER);
    }
    return ((NSData *(*)(id, SEL, NSString *))originalRead)(object, selector, path);
}
/* Answers every feed request in the probe without a network: a status, or a connection failure. */
static NSInteger stubStatus;
@interface FeedStub : NSURLProtocol
@end
@implementation FeedStub
+ (BOOL)canInitWithRequest:(NSURLRequest *)request { (void)request; return YES; }
+ (NSURLRequest *)canonicalRequestForRequest:(NSURLRequest *)request { return request; }
- (void)startLoading {
    if (!stubStatus) {
        [self.client URLProtocol:self didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorNotConnectedToInternet userInfo:nil]];
        return;
    }
    NSHTTPURLResponse *response = [[NSHTTPURLResponse alloc] initWithURL:self.request.URL statusCode:stubStatus HTTPVersion:@"HTTP/1.1" headerFields:@{}];
    [self.client URLProtocol:self didReceiveResponse:response cacheStoragePolicy:NSURLCacheStorageNotAllowed];
    [self.client URLProtocolDidFinishLoading:self];
}
- (void)stopLoading {}
@end
static IMP originalEphemeral;
static NSURLSessionConfiguration *stubbedEphemeral(id object, SEL selector)
{
    NSURLSessionConfiguration *configuration = ((NSURLSessionConfiguration *(*)(id, SEL))originalEphemeral)(object, selector);
    configuration.protocolClasses = @[FeedStub.class];
    return configuration;
}
static void waitForMenuChecks(void)
{
    for (int i = 0; i < 300; i++) {
        __block BOOL busy;
        dispatch_sync(updatesQueue(), ^{ busy = profileChecking || recordingChecking; });
        if (!busy) return;
        usleep(10000);
    }
    assert(!"menu checks finished");
}
int main(int argc, char **argv)
{
    @autoreleasepool {
        assert(publicKey(@"").length == 0);
        assert(publicKey([@"a" stringByPaddingToLength:64 withString:@"a" startingAtIndex:0]).length == 32);
        assert(publicKey([@"z" stringByPaddingToLength:64 withString:@"z" startingAtIndex:0]).length == 0);
        NSString *base = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        NSFileManager *fm = NSFileManager.defaultManager;
        [fm createDirectoryAtPath:base withIntermediateDirectories:YES attributes:nil error:NULL];
        struct host_device d; host_device_identify(&d);
        assert([profileFeedURL(@"https://example.com/packs/profiles", &d).absoluteString isEqual:@"https://example.com/packs/profiles/ios/iPhone18,1/_.json"]);
        assert(!profileFeedURL(@"https://user@example.com/packs", &d));
        assert(!profileFeedURL(@"https://example.com/packs?x=1", &d));
        strcpy(d.model,"../bad"); assert(!profileFeedURL(@"https://example.com/packs", &d));
        NSString *shaderBase = [base stringByAppendingPathComponent:@"shader-base"], *shaderAdd = [base stringByAppendingPathComponent:@"shader-add"];
        [fm createDirectoryAtPath:shaderBase withIntermediateDirectories:YES attributes:nil error:NULL];
        [fm createDirectoryAtPath:shaderAdd withIntermediateDirectories:YES attributes:nil error:NULL];
        NSString *ps = [@"00" stringByPaddingToLength:504 withString:@"00" startingAtIndex:0];
        NSString *descriptors = [NSString stringWithFormat:@"vs 0000000000000001 0 0\nps 2fba1332b14beb55 %@\n",ps];
        NSString *pipeline = @"pipeline 0000000000000001.0.0 2fba1332b14beb55 0 0 0 0 15 15 1 00000000000000000000000000000000 0 0\n";
        [[descriptors stringByAppendingString:pipeline] writeToFile:[shaderBase stringByAppendingPathComponent:@"a10.txt"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        NSString *changed = [pipeline stringByReplacingOccurrencesOfString:@"15 15 1" withString:@"15 0 1"];
        [[descriptors stringByAppendingString:changed] writeToFile:[shaderAdd stringByAppendingPathComponent:@"a10.txt"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        NSDictionary *frozen = snapshotShaderLists(shaderAdd,shaderBase);
        NSString *merged = [[NSString alloc] initWithData:frozen[@"a10"] encoding:NSUTF8StringEncoding];
        assert([merged containsString:pipeline] && [merged containsString:changed]);
        [@"invalid\n" writeToFile:[shaderAdd stringByAppendingPathComponent:@"a10.txt"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        assert([[[NSString alloc] initWithData:frozen[@"a10"] encoding:NSUTF8StringEncoding] isEqual:merged]);
        assert(!snapshotShaderLists(shaderAdd,shaderBase));
        [fm removeItemAtPath:shaderBase error:NULL]; [fm removeItemAtPath:shaderAdd error:NULL];
        NSString *current = nil;
        for (NSString *character in @[@"a", @"b", @"c", @"d"]) {
            NSString *name = [character stringByPaddingToLength:64 withString:character startingAtIndex:0];
            NSString *path = [base stringByAppendingPathComponent:name];
            [fm createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:NULL];
            current = path;
        }
        NSString *unknown = [base stringByAppendingPathComponent:@"keep-unrelated"];
        [fm createDirectoryAtPath:unknown withIntermediateDirectories:YES attributes:nil error:NULL];
        pruneCompatibilityRoots(current);
        assert([fm fileExistsAtPath:current]);
        assert([fm contentsOfDirectoryAtPath:base error:NULL].count == 3);
        NSString *file = [current stringByAppendingPathComponent:@"one.toml"];
        [@"one" writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        HostDataPackStore *pack = [[HostDataPackStore alloc] initWithRoot:[base stringByAppendingPathComponent:@"cache"] bundled:current kind:@"profiles" channel:@"stable" publicKey:[NSData data] compatibility:@"profiles-1" validator:^BOOL(NSString *folder, NSError **error) { (void)folder; (void)error; return YES; }];
        [pack beginLaunch];
        NSDictionary *before = identity(pack, current);
        assert([before[@"bundled"] boolValue] && [before[@"sha256"] length] == 64);
        [@"two" writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        assert(![before[@"sha256"] isEqual:identity(pack, current)[@"sha256"]]);
        // Benchmark readiness requires both its profile and recording catalog.
        menuInitialized = YES; profileReady = NO; recordingReady = YES; profileLoading = YES;
        publishMenuState();
        assert(![host_data_updates_menu_state()[@"ready"] boolValue]);
        assert([host_data_updates_menu_state()[@"loading"] boolValue]);
        profileReady = YES; publishMenuState();
        assert([host_data_updates_menu_state()[@"ready"] boolValue]);
        profileLoading = NO; publishMenuState();
        assert([host_data_updates_menu_state()[@"ready"] boolValue]);
        recordingReady = NO; publishMenuState();
        assert(![host_data_updates_menu_state()[@"ready"] boolValue]);
        assert(![host_data_updates_menu_state()[@"loading"] boolValue]);
        profileReady = NO; profileLoading = NO; recordingLoading = YES; publishMenuState();
        assert(![host_data_updates_menu_state()[@"loading"] boolValue]);
        assert([host_data_updates_menu_state()[@"reason"] isEqual:@"Can't reach the profile server. Benchmarking disabled."]);
        // Play can freeze the bundled selection between the worker's two publication locks.
        profiles = pack; menuRequested = YES;
        blockedPath = file; readEntered = dispatch_semaphore_create(0); resumeRead = dispatch_semaphore_create(0);
        dispatch_semaphore_t published = dispatch_semaphore_create(0);
        Method readMethod = class_getClassMethod(NSData.class, @selector(dataWithContentsOfFile:));
        originalRead = method_setImplementation(readMethod, (IMP)blockedRead);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
            publishMenuSnapshot(YES, @{@"folder": current, @"metadata": before});
            dispatch_semaphore_signal(published);
        });
        assert(dispatch_semaphore_wait(readEntered, dispatch_time(DISPATCH_TIME_NOW, 2*NSEC_PER_SEC)) == 0);
        host_data_updates_menu_finish();
        dispatch_semaphore_signal(resumeRead);
        assert(dispatch_semaphore_wait(published, dispatch_time(DISPATCH_TIME_NOW, 2*NSEC_PER_SEC)) == 0);
        method_setImplementation(readMethod, originalRead);
        blockedPath = nil;
        assert([host_data_updates_metadata()[@"profiles"][@"sha256"] length] == 64);
        // A selected fallback must remain available even while a validator owns the store lock.
        profiles = pack;
        dispatch_semaphore_t held = dispatch_semaphore_create(0), release = dispatch_semaphore_create(0);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
            @synchronized(pack) { dispatch_semaphore_signal(held); dispatch_semaphore_wait(release, DISPATCH_TIME_FOREVER); }
        });
        dispatch_semaphore_wait(held, DISPATCH_TIME_FOREVER);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{ dispatch_semaphore_signal(release); });
        NSDate *started = NSDate.date;
        host_data_updates_menu_finish();
        assert([host_profile_update_folder(@"/fallback") isEqual:@"/fallback"]);
        assert(host_data_updates_menu_state() != nil);
        publishMenuSnapshot(YES, @{@"folder": @"/newer", @"metadata": @{@"bundled": @NO, @"sequence": @2, @"sha256": @"new"}});
        assert([host_profile_update_folder(@"/fallback") isEqual:@"/fallback"]);
        // Initialization finishing after Play still identifies the frozen bundled bytes.
        assert([host_data_updates_metadata()[@"profiles"][@"sha256"] length] == 64);
        assert(-started.timeIntervalSinceNow < 0.2);
        dispatch_semaphore_signal(release);
        // The launch menu still offers verified profiles for gameplay breaks once Play froze the selection.
        HostDataPackStore *watched = [[HostDataPackStore alloc] initWithRoot:[base stringByAppendingPathComponent:@"watch-cache"] bundled:current kind:@"profiles" channel:@"stable" publicKey:[NSMutableData dataWithLength:32] compatibility:@"profiles-1" validator:^BOOL(NSString *folder, NSError **error) { (void)folder; (void)error; return YES; }];
        [watched beginLaunch];
        profiles = watched;
        dispatch_semaphore_t offered = dispatch_semaphore_create(0);
        __block NSString *offeredFolder;
        host_profile_updates_watch([base stringByAppendingPathComponent:@"device-profile.toml"], ^(NSString *folder, NSDictionary *offeredIdentity) {
            (void)offeredIdentity; offeredFolder = folder; dispatch_semaphore_signal(offered);
        });
        assert(dispatch_semaphore_wait(offered, dispatch_time(DISPATCH_TIME_NOW, 3*NSEC_PER_SEC)) == 0);
        assert(profiles == watched && [offeredFolder isEqual:current]);
        // While the menu is up, a failed check retries quietly with growing delays, and a model the feed has no
        // profile for (404) benchmarks with its bundled profiles.
        Method ephemeral = class_getClassMethod(NSURLSessionConfiguration.class, @selector(ephemeralSessionConfiguration));
        originalEphemeral = method_setImplementation(ephemeral, (IMP)stubbedEphemeral);
        HostDataPackStore *menuStore = [[HostDataPackStore alloc] initWithRoot:[base stringByAppendingPathComponent:@"menu-cache"] bundled:current kind:@"profiles" channel:@"tester" publicKey:[NSMutableData dataWithLength:32] compatibility:@"profiles-1" validator:^BOOL(NSString *folder, NSError **error) { (void)folder; (void)error; return YES; }];
        dispatch_sync(updatesQueue(), ^{
            profiles = menuStore; recordings = menuStore;
            menuProfileURL = [NSURL URLWithString:@"https://example.com/packs/profiles/ios/iPhone18,1/_.json"]; menuRecordingURL = nil;
            @synchronized(selectionLock()) {
                menuFrozen = NO; menuVisible = YES; menuInitialized = YES; profileFolder = nil; profileIdentity = nil;
                profileReady = NO; profileLoading = YES; recordingReady = YES; recordingLoading = NO;
            }
            resetMenuRetry(); profileAttempted = NO; stubStatus = 0;
            refreshMenu();
        });
        waitForMenuChecks();
        dispatch_sync(updatesQueue(), ^{
            assert(menuStore.lastFeedStatus == 0);
            assert(!profileReady && menuRetryScheduled && menuRetryAttempt == 1);
        });
        assert([host_data_updates_menu_state()[@"reason"] isEqual:@"Can't reach the profile server. Benchmarking disabled."]);
        // The retry itself does not bring back "Downloading": the failure text stays until the data loads.
        dispatch_sync(updatesQueue(), ^{ menuRetryScheduled = NO; refreshMenuKinds(YES, NO); });
        assert(![host_data_updates_menu_state()[@"loading"] boolValue]);
        waitForMenuChecks();
        dispatch_sync(updatesQueue(), ^{ assert(menuRetryScheduled && menuRetryAttempt == 2); });
        // A network path change retries at once and starts the delays over.
        dispatch_sync(updatesQueue(), ^{ stubStatus = 404; requestMenuRefresh(); assert(menuRetryAttempt == 0 && !menuRetryScheduled); });
        waitForMenuChecks();
        dispatch_sync(updatesQueue(), ^{ assert(menuStore.lastFeedStatus == 404 && profileReady && !menuRetryScheduled); });
        assert([host_data_updates_menu_state()[@"ready"] boolValue]);
        assert([host_data_updates_menu_state()[@"reason"] isEqual:@""]);
        // A server error is not "nothing published": it retries.
        dispatch_sync(updatesQueue(), ^{
            @synchronized(selectionLock()) { profileReady = NO; }
            stubStatus = 503; requestMenuRefresh();
        });
        waitForMenuChecks();
        dispatch_sync(updatesQueue(), ^{ assert(!profileReady && menuRetryScheduled && menuRetryAttempt == 1); });
        if (argc >= 3) {
            // A signed record from the publisher: staged, read back, and compared with the running version.
            NSMutableData *signer = [NSMutableData data];
            for (int i = 0; i < 64; i += 2) { unsigned v = 0; sscanf(argv[1] + i, "%2x", &v); uint8_t b = v; [signer appendBytes:&b length:1]; }
            HostDataPackStore *record = [[HostDataPackStore alloc] initWithRoot:[base stringByAppendingPathComponent:@"latest-cache"] bundled:[base stringByAppendingPathComponent:@"none"] kind:@"latest" channel:@"tester" publicKey:signer compatibility:@"latest-release-1" validator:^BOOL(NSString *folder, NSError **error) {
                (void)error; return latestRecordPlatforms([NSData dataWithContentsOfFile:[folder stringByAppendingPathComponent:@"latest-release.json"]]) != nil;
            }];
            assert([[record menuSnapshot][@"metadata"][@"bundled"] boolValue]);
            NSData *envelope = [NSData dataWithContentsOfFile:@(argv[2])];
            NSError *stageError = nil;
            assert(envelope.length > 0 && [record stageEnvelope:envelope error:&stageError]);
            NSDictionary *snapshot = [record menuSnapshot];
            assert(![snapshot[@"metadata"][@"bundled"] boolValue] && [snapshot[@"metadata"][@"kind"] isEqual:@"latest"]);
            NSData *file = [NSData dataWithContentsOfFile:[snapshot[@"folder"] stringByAppendingPathComponent:@"latest-release.json"]];
            assert([latestNewerFromRecord(file, @"ios", @"0.5.2") isEqual:@"0.6.0"] && !latestNewerFromRecord(file, @"tvos", @"0.5.2"));
            assert(![record stageEnvelope:envelope error:NULL]);   // the same sequence again
            // Another kind's store refuses it.
            HostDataPackStore *other = [[HostDataPackStore alloc] initWithRoot:[base stringByAppendingPathComponent:@"other-cache"] bundled:current kind:@"shaders" channel:@"tester" publicKey:signer compatibility:@"latest-release-1" validator:^BOOL(NSString *f, NSError **e) { (void)f; (void)e; return YES; }];
            assert(![other stageEnvelope:envelope error:NULL]);
            puts("PASS: a signed latest-release record is staged and read back");
        }
        // A build whose recording feed has nothing published says so and names the newer version, once the latest-release
        // record has been read; a transport failure keeps the earlier answer, and a cached catalog still wins.
        assert([latestNewerFromRecord([@"{\"schema\":1,\"platforms\":{\"ios\":\"0.6.0\",\"tvos\":\"0.5.2\"}}" dataUsingEncoding:NSUTF8StringEncoding], @"ios", @"0.5.2") isEqual:@"0.6.0"]);
        assert(!latestNewerFromRecord([@"{\"schema\":1,\"platforms\":{\"ios\":\"0.6.0\"}}" dataUsingEncoding:NSUTF8StringEncoding], @"ios", @"0.6.0"));
        assert(!latestNewerFromRecord([@"{\"schema\":1,\"platforms\":{\"ios\":\"0.6.0\"}}" dataUsingEncoding:NSUTF8StringEncoding], @"ios", @"0.10.0"));
        assert([latestNewerFromRecord([@"{\"schema\":1,\"platforms\":{\"ios\":\"0.10.0\"}}" dataUsingEncoding:NSUTF8StringEncoding], @"ios", @"0.9.9") isEqual:@"0.10.0"]);
        assert(!latestNewerFromRecord([@"{\"schema\":1,\"platforms\":{\"ios\":\"0.6.0\"}}" dataUsingEncoding:NSUTF8StringEncoding], @"tvos", @"0.5.2"));
        assert(!latestNewerFromRecord([@"{\"schema\":1,\"platforms\":{\"ios\":\"0.6.0\"}}" dataUsingEncoding:NSUTF8StringEncoding], @"ios", @"0.5.2-beta"));
        assert(!latestNewerFromRecord([@"{\"schema\":1,\"platforms\":{\"ios\":\"0.6\"}}" dataUsingEncoding:NSUTF8StringEncoding], @"ios", @"0.5.2"));
        assert(!latestNewerFromRecord([@"{\"schema\":2,\"platforms\":{\"ios\":\"0.6.0\"}}" dataUsingEncoding:NSUTF8StringEncoding], @"ios", @"0.5.2"));
        assert(!latestNewerFromRecord([@"{\"schema\":1,\"platforms\":{\"ios\":{\"version\":\"0.6.0\"}}}" dataUsingEncoding:NSUTF8StringEncoding], @"ios", @"0.5.2"));
        assert(!latestNewerFromRecord([@"nope" dataUsingEncoding:NSUTF8StringEncoding], @"ios", @"0.5.2"));
        NSString *newerText = @"Benchmarking is no longer supported on this version. Update to VISR 0.6.0.";
        dispatch_sync(updatesQueue(), ^{
            @synchronized(selectionLock()) {
                profileReady = YES; profileLoading = NO; recordingReady = NO; recordingLoading = NO; recordingUnpublished = NO; latestNewerVersion = nil;
            }
            publishMenuState();
        });
        assert([host_data_updates_menu_state()[@"reason"] isEqual:@"Benchmark recordings are unavailable"]);
        dispatch_sync(updatesQueue(), ^{ @synchronized(selectionLock()) { latestNewerVersion = @"0.6.0"; } publishMenuState(); });
        assert([host_data_updates_menu_state()[@"reason"] isEqual:@"Benchmark recordings are unavailable"]);   // the feed was not answered 404
        dispatch_sync(updatesQueue(), ^{ @synchronized(selectionLock()) { recordingUnpublished = YES; } publishMenuState(); });
        assert([host_data_updates_menu_state()[@"reason"] isEqual:newerText]);
        assert([host_data_updates_latest_newer() isEqual:@"0.6.0"]);
        dispatch_sync(updatesQueue(), ^{ @synchronized(selectionLock()) { profileReady = NO; } publishMenuState(); });
        assert([host_data_updates_menu_state()[@"reason"] isEqual:newerText]);   // it takes priority over the profile-server text
        dispatch_sync(updatesQueue(), ^{ @synchronized(selectionLock()) { profileReady = YES; recordingReady = YES; } publishMenuState(); });
        assert([host_data_updates_menu_state()[@"ready"] boolValue] && [host_data_updates_menu_state()[@"reason"] isEqual:@""]);   // a cached catalog wins
        // The recording completion latches the 404 and clears it only on an answer; a failed request leaves it alone.
        HostDataPackStore *recordingStore = [[HostDataPackStore alloc] initWithRoot:[base stringByAppendingPathComponent:@"recording-cache"] bundled:current kind:@"recordings" channel:@"tester" publicKey:[NSMutableData dataWithLength:32] compatibility:@"benchmark-catalog-1:test-guest" validator:^BOOL(NSString *folder, NSError **error) { (void)folder; (void)error; return NO; }];
        dispatch_sync(updatesQueue(), ^{
            recordings = recordingStore; menuRecordingURL = [NSURL URLWithString:@"https://example.com/packs/tester/recordings.json"];
            @synchronized(selectionLock()) { menuFrozen = NO; menuVisible = YES; recordingReady = NO; recordingUnpublished = NO; recordingAttempted = NO; }
            resetMenuRetry(); stubStatus = 404; refreshMenuKinds(NO, YES);
        });
        waitForMenuChecks();
        dispatch_sync(updatesQueue(), ^{ assert(recordingUnpublished); stubStatus = 0; resetMenuRetry(); refreshMenuKinds(NO, YES); });
        dispatch_sync(updatesQueue(), ^{ publishMenuState(); });
        waitForMenuChecks();
        dispatch_sync(updatesQueue(), ^{ assert(recordingUnpublished); stubStatus = 503; resetMenuRetry(); refreshMenuKinds(NO, YES); });
        waitForMenuChecks();
        dispatch_sync(updatesQueue(), ^{ assert(recordingUnpublished); });   // a server error says nothing about the feed
        dispatch_sync(updatesQueue(), ^{ stubStatus = 200; resetMenuRetry(); refreshMenuKinds(NO, YES); });
        waitForMenuChecks();
        dispatch_sync(updatesQueue(), ^{ assert(!recordingUnpublished); });   // an answer, even one that stages nothing, ends it
        // Once Play froze the selection, nothing retries.
        host_data_updates_menu_finish();
        dispatch_sync(updatesQueue(), ^{ resetMenuRetry(); scheduleMenuRetry(); assert(!menuRetryScheduled); });
        method_setImplementation(ephemeral, originalEphemeral);
        [fm removeItemAtPath:base error:NULL];
        puts("PASS: data update identity and bounded compatibility roots");
        puts("PASS: the launch menu retries failed data checks quietly and accepts an unpublished profile");
    }
    return 0;
}
