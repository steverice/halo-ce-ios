/* Recordings and shaders stay fixed for a launch; verified profiles may be offered at gameplay breaks. */
#import "host_data_updates.h"
#import "host_data_pack.h"
#import <CommonCrypto/CommonDigest.h>
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

static NSString *digest(NSData *data)
{
    unsigned char bytes[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, bytes);
    NSMutableString *result = [NSMutableString string];
    for (NSUInteger i = 0; i < sizeof(bytes); i++) [result appendFormat:@"%02x", bytes[i]];
    return result;
}

/* Bundled data also needs a content identity: app versions may contain changed files. */
static NSDictionary *identity(HostDataPackStore *store, NSString *folder)
{
    NSMutableDictionary *result = [[store metadata] mutableCopy];
    if ([result[@"bundled"] boolValue]) {
        NSMutableData *bytes = [NSMutableData data];
        NSArray *paths = [[NSFileManager.defaultManager subpathsAtPath:folder] sortedArrayUsingSelector:@selector(compare:)];
        for (NSString *path in paths) {
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

static HostDataPackStore *store(NSString *kind, NSString *bundled, NSString *compatibility, HostPackValidator validator)
{
    NSBundle *bundle = NSBundle.mainBundle;
    NSString *channel = [bundle objectForInfoDictionaryKey:@"HaloDataPackChannel"];
    if (![channel isEqual:@"tester"] && ![channel isEqual:@"stable"]) channel = @"stable";
    NSData *key = publicKey([bundle objectForInfoDictionaryKey:@"HaloDataPackPublicKey"]);
    NSString *cache = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
    NSString *root = [[[[cache stringByAppendingPathComponent:@"visr-data-packs"] stringByAppendingPathComponent:kind] stringByAppendingPathComponent:channel] stringByAppendingPathComponent:digest([compatibility dataUsingEncoding:NSUTF8StringEncoding])];
    pruneCompatibilityRoots(root);
    return [[HostDataPackStore alloc] initWithRoot:root bundled:bundled kind:kind channel:channel publicKey:key compatibility:compatibility validator:validator];
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

static void initialize(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *resources = NSBundle.mainBundle.resourcePath;
        NSString *bundledProfiles = [resources stringByAppendingPathComponent:@"profiles"];
        NSString *bundledRecordings = [resources stringByAppendingPathComponent:@"benchmark-recordings"];
        struct host_device device; char deviceKey[192];
        host_device_identify(&device);
        BOOL identified = host_profile_device_key(&device,deviceKey,sizeof(deviceKey));
        NSString *profileCompatibility = identified ? [@"device-profiles-1:" stringByAppendingString:@(deviceKey)] : @"device-profiles-1:unidentified";
        profiles = store(@"profiles", bundledProfiles, profileCompatibility, ^BOOL(NSString *folder, NSError **error) {
            BOOL valid = identified && host_profile_validate_device_folder(folder.fileSystemRepresentation, &device, NULL);
            if (!valid && error) *error = [NSError errorWithDomain:@"VISRDataUpdates" code:1 userInfo:@{NSLocalizedDescriptionKey: @"Invalid profile snapshot"}];
            return valid;
        });
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
        profileRefreshQueue = dispatch_queue_create("visr.profile-refresh",
            dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
        [NSNotificationCenter.defaultCenter addObserverForName:HostDataPackSnapshotStagedNotification object:profiles queue:nil usingBlock:^(NSNotification *note) {
            (void)note;
            dispatch_async(profileRefreshQueue, ^{ @autoreleasepool { offerProfileSnapshot(); } });
        }];
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
                /* Match launch's conditional request and signature checks, with a bounded play cadence. */
                profileRefreshTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, profileRefreshQueue);
                dispatch_source_set_timer(profileRefreshTimer, dispatch_time(DISPATCH_TIME_NOW, 15 * 60 * NSEC_PER_SEC),
                    15 * 60 * NSEC_PER_SEC, 30 * NSEC_PER_SEC);
                dispatch_source_set_event_handler(profileRefreshTimer, ^{ @autoreleasepool { [profiles checkForUpdateAtURL:targetURL]; } });
                dispatch_resume(profileRefreshTimer);
            }
            NSString *shaderURL = [NSBundle.mainBundle objectForInfoDictionaryKey:@"HaloShaderFeedURL"];
            if (shaderURL.length) [shaders checkForUpdateAtURL:[NSURL URLWithString:shaderURL]];
            if (recordingURL.length) [recordings checkForUpdateAtURL:[NSURL URLWithString:recordingURL]];
        }
    });
}
void host_profile_updates_watch(NSString *dropIn, void (^visitor)(NSString *, NSDictionary *))
{
    initialize();
    dispatch_async(profileRefreshQueue, ^{ @autoreleasepool {
        profileVisitor = [visitor copy];
        profileDropIn = [dropIn copy];
        if (profileDirectoryWatch) dispatch_source_cancel(profileDirectoryWatch);
        profileDirectoryWatch = profileVnode(profileDropIn.stringByDeletingLastPathComponent);
        watchProfileFile();
        offerProfileSnapshot();
    } });
}
NSString *host_profile_update_folder(NSString *bundled) { (void)bundled; initialize(); return profileFolder; }
NSString *host_recording_update_folder(NSString *bundled) { (void)bundled; initialize(); return recordingFolder; }
NSData *host_shader_update_list(const char *name) { initialize(); return name && host_shader_map_name_valid(name) ? shaderLists[@(name)] : nil; }
NSDictionary *host_data_updates_metadata(void) { initialize(); return @{@"profiles": profileIdentity, @"recordings": recordingIdentity, @"shaders": shaderIdentity}; }
void host_data_updates_healthy(void)
{
    /* Validation of a newly downloaded snapshot can hold the store lock. Keep it off presentation. */
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool { initialize(); [profiles markHealthy]; [recordings markHealthy]; if (shaderLists) [shaders markHealthy]; }
    });
}
