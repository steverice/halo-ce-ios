/* The 0.6.0 profile feed (docs/data-packs.md, "Profile feed generations"): an iPhone 17 Pro keeps every bundled
   key, network.player_capacity included, with a stale device-profiles-1 shard on disk, with a current
   device-profiles-2 shard, and after an update makes a cached shard older than its bundled profiles; an Apple TV 4K
   (3rd generation) likewise keeps display.upscaled_textures on, which its live device-profiles-1 shard turns off.
   Usage: profile-feed-probe REPOSITORY V1_PUBLIC_KEY V2_PUBLIC_KEY V2_IPHONE_SHARD V2_APPLE_TV_SHARD
   (the device-profiles-1 shards are the live ones checked in under docs/site/packs/tester/profiles) */
#import "../host/host_data_updates.m"
#include <stdarg.h>
#import <objc/runtime.h>
void host_logf(int priority, const char *format, ...)
{
    (void)priority; va_list arguments; va_start(arguments, format); vfprintf(stderr, format, arguments); va_end(arguments); fputc('\n', stderr);
}
static struct host_device phone(void)
{
    struct host_device d; memset(&d, 0, sizeof(d));
    strcpy(d.platform, "ios"); strcpy(d.model, "iPhone18,1");
    d.gpu_family = 10; d.memory_megabytes = 12000; d.pixel_width = 2622; d.pixel_height = 1206; d.refresh_rate = 120;
    return d;
}
void host_device_identify(struct host_device *d) { *d = phone(); }
static int failures;
#define CHECK(condition, label) do { if (!(condition)) { fprintf(stderr, "FAIL: %s\n", label); failures++; } else printf("ok: %s\n", label); } while (0)
static struct host_device appleTV(void)
{
    struct host_device d; memset(&d, 0, sizeof(d));
    strcpy(d.platform, "tvos"); strcpy(d.model, "AppleTV14,1");
    d.gpu_family = 8; d.memory_megabytes = 4096; d.pixel_width = 3840; d.pixel_height = 2160; d.refresh_rate = 60;
    return d;
}
static NSString *resolved(NSString *folder, struct host_device d, const char *name)
{
    static struct host_profile profile; char text[80];
    host_profile_resolve(&d, folder.fileSystemRepresentation, NULL, "", NULL, &profile);
    return host_profile_value(&profile, name, text, sizeof(text)) ? @(text) : nil;
}
static NSString *capacity(NSString *folder) { return resolved(folder, phone(), "network.player_capacity"); }
static NSString *upscaling(NSString *folder) { return resolved(folder, appleTV(), "display.upscaled_textures"); }
static HostDataPackStore *generationOne(NSString *bundled, struct host_device d)
{
    char key[192]; assert(host_profile_device_key(&d, key, sizeof(key)));
    return store(@"profiles", bundled, [@"device-profiles-1:" stringByAppendingString:@(key)], ^BOOL(NSString *folder, NSError **error) {
        (void)error; return host_profile_validate_device_folder(folder.fileSystemRepresentation, &d, NULL);
    });
}
/* Serves one envelope for every feed request. */
static NSData *served;
@interface ShardStub : NSURLProtocol
@end
@implementation ShardStub
+ (BOOL)canInitWithRequest:(NSURLRequest *)request { (void)request; return YES; }
+ (NSURLRequest *)canonicalRequestForRequest:(NSURLRequest *)request { return request; }
- (void)startLoading {
    NSHTTPURLResponse *response = [[NSHTTPURLResponse alloc] initWithURL:self.request.URL statusCode:200 HTTPVersion:@"HTTP/1.1" headerFields:@{}];
    [self.client URLProtocol:self didReceiveResponse:response cacheStoragePolicy:NSURLCacheStorageNotAllowed];
    [self.client URLProtocol:self didLoadData:served];
    [self.client URLProtocolDidFinishLoading:self];
}
- (void)stopLoading {}
@end
static IMP originalEphemeral;
static NSURLSessionConfiguration *stubbedEphemeral(id object, SEL selector)
{
    NSURLSessionConfiguration *configuration = ((NSURLSessionConfiguration *(*)(id, SEL))originalEphemeral)(object, selector);
    configuration.protocolClasses = @[ShardStub.class];
    return configuration;
}
static void writeRevision(NSString *profiles, long long revision)
{
    NSString *path = [profiles stringByAppendingPathComponent:@"hardware/revision.toml"];
    NSString *text = [NSString stringWithFormat:@"schema = 1\nrevision = %lld\nsha256 = \"%@\"\n", revision, [@"" stringByPaddingToLength:64 withString:@"0" startingAtIndex:0]];
    [NSFileManager.defaultManager removeItemAtPath:path error:NULL];
    assert([text writeToFile:path atomically:NO encoding:NSUTF8StringEncoding error:NULL]);
}
int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 6) { fprintf(stderr, "usage: %s REPOSITORY V1_KEY V2_KEY V2_IPHONE_SHARD V2_APPLE_TV_SHARD\n", argv[0]); return 2; }
        NSString *live = [@(argv[1]) stringByAppendingPathComponent:@"docs/site/packs/tester/profiles"];
        NSFileManager *fm = NSFileManager.defaultManager;
        NSString *base = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        NSString *bundled = [base stringByAppendingPathComponent:@"Resources/profiles"];
        assert([fm createDirectoryAtPath:bundled.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:NULL]);
        assert([fm copyItemAtPath:[@(argv[1]) stringByAppendingPathComponent:@"profiles"] toPath:bundled error:NULL]);
        long long bundledRevision = host_profile_revision_read(bundled.fileSystemRepresentation, NULL);
        CHECK(bundledRevision >= 1, "the bundled profiles carry a revision");
        CHECK([capacity(bundled) isEqual:@"32"], "the bundled profiles give the iPhone 17 Pro network.player_capacity 32");
        dataPackCacheOverride = [base stringByAppendingPathComponent:@"Caches"]; dataPackChannelOverride = @"tester";
        NSError *error = nil;

        // Apple TV 4K (3rd generation): its live device-profiles-1 shard predates upscaled textures there.
        struct host_device tv = appleTV();
        CHECK([upscaling(bundled) isEqual:@"true"], "the bundled profiles turn upscaled textures on for the Apple TV 4K (3rd generation)");
        dataPackKeyOverride = publicKey(@(argv[2]));
        HostDataPackStore *tvOld = generationOne(bundled, tv);
        CHECK([tvOld stageEnvelope:[NSData dataWithContentsOfFile:[live stringByAppendingPathComponent:@"tvos/AppleTV14,1/_.json"]] error:&error],
            "the Apple TV's live device-profiles-1 shard stages as it did on 0.5.1");
        NSString *tvOldFolder = [tvOld beginLaunch]; [tvOld markHealthy];
        CHECK(![tvOldFolder isEqual:bundled] && ![upscaling(tvOldFolder) isEqual:@"true"], "resolving that shard leaves upscaled textures off");
        dataPackKeyOverride = publicKey(@(argv[3]));
        HostDataPackStore *tvCurrent = profileStore(bundled, tv);
        NSString *tvLaunch = [tvCurrent beginLaunch];
        CHECK([fm fileExistsAtPath:tvOldFolder] && [tvLaunch isEqual:bundled] && [upscaling(tvLaunch) isEqual:@"true"],
            "0.6.0 on the Apple TV ignores the stored v1 shard and keeps upscaled textures on");
        CHECK([tvCurrent stageEnvelope:[NSData dataWithContentsOfFile:@(argv[5])] error:&error], "the Apple TV's device-profiles-2 shard stages");
        NSString *tvNext = [profileStore(bundled, tv) beginLaunch];
        CHECK(![tvNext isEqual:bundled] && [upscaling(tvNext) isEqual:@"true"], "the Apple TV's v2 shard keeps upscaled textures on");

        struct host_device d = phone();

        // 0.5.1 left its live device-profiles-1 shard active in the cache: the audit's masking.
        dataPackKeyOverride = publicKey(@(argv[2]));
        HostDataPackStore *old = generationOne(bundled, d);
        CHECK([old stageEnvelope:[NSData dataWithContentsOfFile:[live stringByAppendingPathComponent:@"ios/iPhone18,1/_.json"]] error:&error],
            "the live device-profiles-1 shard stages as it did on 0.5.1");
        NSString *oldFolder = [old beginLaunch]; [old markHealthy];
        CHECK(![oldFolder isEqual:bundled] && !capacity(oldFolder), "resolving the live v1 shard loses network.player_capacity (the 0.5.1 bug)");

        // 0.6.0 on the same device: a new store for its own generation, the v1 shard still on disk.
        dataPackKeyOverride = publicKey(@(argv[3]));
        HostDataPackStore *current = profileStore(bundled, d);
        NSString *launch = [current beginLaunch];
        CHECK([fm fileExistsAtPath:oldFolder], "the stale v1 shard is still on disk");
        CHECK([launch isEqual:bundled] && [capacity(launch) isEqual:@"32"], "0.6.0 ignores the v1 shard and keeps network.player_capacity 32");

        // A device-profiles-2 shard signed from these sources stages, activates and keeps every key.
        NSData *v2 = [NSData dataWithContentsOfFile:@(argv[4])];
        CHECK([current stageEnvelope:v2 error:&error] && !current.lastFeedSuperseded, "the device-profiles-2 shard at the bundled revision stages");
        HostDataPackStore *next = profileStore(bundled, d);
        NSString *v2Folder = [next beginLaunch]; [next markHealthy];
        CHECK(![v2Folder isEqual:bundled] && [v2Folder hasPrefix:dataPackCacheOverride], "the next launch uses the downloaded v2 shard");
        CHECK([capacity(v2Folder) isEqual:@"32"], "the v2 shard keeps network.player_capacity 32");
        CHECK(host_profile_revision_read(v2Folder.fileSystemRepresentation, NULL) == bundledRevision, "the v2 shard carries the bundled revision");

        // An app update raises the bundled revision: the cached shard is now older and gives way.
        writeRevision(bundled, bundledRevision + 1);
        HostDataPackStore *updated = profileStore(bundled, d);
        NSString *afterUpdate = [updated beginLaunch];
        CHECK([afterUpdate isEqual:bundled] && [capacity(afterUpdate) isEqual:@"32"], "a cached shard older than the bundled profiles is not used");
        // The feed serves the same, now older, shard again: refused as superseded although its sequence was seen.
        CHECK(![updated stageEnvelope:v2 error:&error] && updated.lastFeedSuperseded, "an older served shard is refused as superseded");
        // The same revision stamped from other sources is older too: the bundle wins.
        writeRevision(bundled, bundledRevision);
        HostDataPackStore *sameNumber = profileStore(bundled, d);
        CHECK([[sameNumber beginLaunch] isEqual:bundled] && ![sameNumber stageEnvelope:v2 error:&error] && sameNumber.lastFeedSuperseded,
            "a shard with the bundled revision number but other sources is refused");
        // A bundle without a revision accepts no download.
        [fm removeItemAtPath:[bundled stringByAppendingPathComponent:@"hardware/revision.toml"] error:NULL];
        HostDataPackStore *unstamped = profileStore(bundled, d);
        CHECK([[unstamped beginLaunch] isEqual:bundled] && ![unstamped stageEnvelope:v2 error:&error] && unstamped.lastFeedSuperseded,
            "without a bundled revision no shard is used");
        writeRevision(bundled, bundledRevision + 1);

        // A shard refused as older does not use up its sequence: in a fresh cache, a build bundling newer profiles
        // refuses it, then a build sharing that cache with the shard's own revision (a downgrade) still takes it.
        dataPackCacheOverride = [base stringByAppendingPathComponent:@"Caches-downgrade"];
        HostDataPackStore *newer = profileStore(bundled, d);
        CHECK(![newer stageEnvelope:v2 error:&error] && newer.lastFeedSuperseded, "a build with newer bundled profiles refuses the shard");
        writeRevision(bundled, bundledRevision);
        [fm removeItemAtPath:[bundled stringByAppendingPathComponent:@"hardware/revision.toml"] error:NULL];
        assert([fm copyItemAtPath:[@(argv[1]) stringByAppendingPathComponent:@"profiles/hardware/revision.toml"]
            toPath:[bundled stringByAppendingPathComponent:@"hardware/revision.toml"] error:NULL]);
        HostDataPackStore *downgraded = profileStore(bundled, d);
        [downgraded beginLaunch];
        CHECK([downgraded stageEnvelope:v2 error:&error] && !downgraded.lastFeedSuperseded, "after a downgrade the same shard still stages");
        NSString *downgradedFolder = [profileStore(bundled, d) beginLaunch];
        CHECK(![downgradedFolder isEqual:bundled] && [capacity(downgradedFolder) isEqual:@"32"], "and the downgraded build uses it");
        dataPackCacheOverride = [base stringByAppendingPathComponent:@"Caches"];
        writeRevision(bundled, bundledRevision + 1);

        // The launch menu counts that as ready: the bundled profiles are the best there is, as for a 404.
        Method ephemeral = class_getClassMethod(NSURLSessionConfiguration.class, @selector(ephemeralSessionConfiguration));
        originalEphemeral = method_setImplementation(ephemeral, (IMP)stubbedEphemeral);
        served = v2;
        HostDataPackStore *menu = profileStore(bundled, d);
        [menu menuSnapshot];
        dispatch_sync(updatesQueue(), ^{
            profiles = menu; recordings = menu;
            menuProfileURL = [NSURL URLWithString:@"https://example.com/packs/tester/device-profiles-2/ios/iPhone18,1/_.json"]; menuRecordingURL = nil;
            @synchronized(selectionLock()) {
                menuFrozen = NO; menuVisible = YES; menuInitialized = YES; profileFolder = nil; profileIdentity = nil;
                profileReady = NO; profileLoading = YES; recordingReady = YES; recordingLoading = NO;
            }
            refreshMenuKinds(YES, NO);
        });
        for (int i = 0; i < 300; i++) {
            __block BOOL busy; dispatch_sync(updatesQueue(), ^{ busy = profileChecking; });
            if (!busy) break;
            usleep(10000);
        }
        __block BOOL ready; __block NSString *selected;
        dispatch_sync(updatesQueue(), ^{ @synchronized(selectionLock()) { ready = profileReady; selected = profileFolder; } });
        CHECK(ready && menu.lastFeedSuperseded && [selected isEqual:bundled], "the menu is ready with the bundled profiles when the feed is older");
        method_setImplementation(ephemeral, originalEphemeral);
        [fm removeItemAtPath:base error:NULL];
    }
    if (failures) { fprintf(stderr, "%d profile feed checks failed\n", failures); return 1; }
    puts("PASS: 0.6.0 profiles keep their bundled keys with a stale v1 shard, a current v2 shard and an older cached shard");
    return 0;
}
