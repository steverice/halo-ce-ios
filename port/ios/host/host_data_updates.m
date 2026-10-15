/* Signed data snapshots are selected once, before any profile or replay is read. */
#import "host_data_updates.h"
#import "host_data_pack.h"
#import <CommonCrypto/CommonDigest.h>
#include "host_profile.h"
#include "host_recording_pack.h"
#ifndef HALO_GUEST_SHA256
#include "guest_image.h"
#endif
#include <sys/stat.h>

static HostDataPackStore *profiles, *recordings;
static NSString *profileFolder, *recordingFolder;
static NSDictionary *profileIdentity, *recordingIdentity;

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

static void initialize(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *resources = NSBundle.mainBundle.resourcePath;
        NSString *bundledProfiles = [resources stringByAppendingPathComponent:@"profiles"];
        NSString *bundledRecordings = [resources stringByAppendingPathComponent:@"benchmark-recordings"];
        profiles = store(@"profiles", bundledProfiles, @"profiles-1", ^BOOL(NSString *folder, NSError **error) {
            BOOL valid = host_profile_validate_folder(folder.fileSystemRepresentation, NULL);
            if (!valid && error) *error = [NSError errorWithDomain:@"VISRDataUpdates" code:1 userInfo:@{NSLocalizedDescriptionKey: @"Invalid profile snapshot"}];
            return valid;
        });
        recordings = store(@"recordings", bundledRecordings, @"benchmark-replay-1:" HALO_GUEST_SHA256, ^BOOL(NSString *folder, NSError **error) {
            BOOL valid = host_recording_pack_validate(folder.fileSystemRepresentation, NULL);
            if (!valid && error) *error = [NSError errorWithDomain:@"VISRDataUpdates" code:2 userInfo:@{NSLocalizedDescriptionKey: @"Invalid recording snapshot"}];
            return valid;
        });
        profileFolder = [profiles beginLaunch];
        recordingFolder = [recordings beginLaunch];
        profileIdentity = identity(profiles, profileFolder);
        recordingIdentity = identity(recordings, recordingFolder);
        if (publicKey([NSBundle.mainBundle objectForInfoDictionaryKey:@"HaloDataPackPublicKey"]).length == 32) {
            NSString *profileURL = [NSBundle.mainBundle objectForInfoDictionaryKey:@"HaloProfileFeedURL"];
            NSString *recordingURL = [NSBundle.mainBundle objectForInfoDictionaryKey:@"HaloRecordingFeedURL"];
            if (profileURL.length) [profiles checkForUpdateAtURL:[NSURL URLWithString:profileURL]];
            if (recordingURL.length) [recordings checkForUpdateAtURL:[NSURL URLWithString:recordingURL]];
        }
    });
}
NSString *host_profile_update_folder(NSString *bundled) { (void)bundled; initialize(); return profileFolder; }
NSString *host_recording_update_folder(NSString *bundled) { (void)bundled; initialize(); return recordingFolder; }
NSDictionary *host_data_updates_metadata(void) { initialize(); return @{@"profiles": profileIdentity, @"recordings": recordingIdentity}; }
void host_data_updates_healthy(void)
{
    /* Validation of a newly downloaded snapshot can hold the store lock. Keep it off presentation. */
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool { initialize(); [profiles markHealthy]; [recordings markHealthy]; }
    });
}
