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
int main(void)
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
        [fm removeItemAtPath:base error:NULL];
        puts("PASS: data update identity and bounded compatibility roots");
    }
    return 0;
}
