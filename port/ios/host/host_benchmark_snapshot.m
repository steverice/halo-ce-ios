/* The session's replay files and their identity travel together, even across an interrupted launch. */
#import "host_benchmark_snapshot.h"
#import <CommonCrypto/CommonDigest.h>
#include "host_recording_pack.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static NSString *const metadata_name = @".snapshot.json";
static NSString *const error_domain = @"VISRBenchmarkSnapshot";
static NSData *json(id value) {
    return [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingSortedKeys error:nil];
}
static NSString *digest(NSData *data) {
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    if (!data || data.length > UINT32_MAX) return nil;
    CC_SHA256(data.bytes, (CC_LONG)data.length, hash);
    NSMutableString *result = [NSMutableString stringWithCapacity:64];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [result appendFormat:@"%02x", hash[i]];
    return result;
}
NSString *host_benchmark_snapshot_digest(id value) { return digest(json(value)); }
static BOOL hash_valid(id value) {
    return [value isKindOfClass:NSString.class] && [value length] == 64 &&
        [value rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"].invertedSet].location == NSNotFound;
}
static BOOL file_name(NSString *name) {
    NSString *base = name.stringByDeletingPathExtension;
    static NSSet *extensions;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ extensions = [NSSet setWithArray:@[ @"input", @"actions", @"seeds", @"route", @"checks" ]]; });
    return base.length && base.length <= 48 && ![base hasPrefix:@"-"] && ![base hasSuffix:@"-"] &&
        [base rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyz0123456789-"].invertedSet].location == NSNotFound &&
        [extensions containsObject:name.pathExtension] && [name.lastPathComponent isEqual:name];
}
static NSData *read_regular(int directory, const char *name, size_t maximum) {
    int fd = openat(directory, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat st;
    NSMutableData *data = nil;
    if (fd < 0) return nil;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size >= 0 && (uint64_t)st.st_size <= maximum) {
        data = [NSMutableData dataWithLength:(NSUInteger)st.st_size];
        size_t at = 0;
        while (at < data.length) {
            ssize_t count = read(fd, (char *)data.mutableBytes + at, data.length - at);
            if (count <= 0) { data = nil; break; }
            at += (size_t)count;
        }
        char extra;
        if (data && read(fd, &extra, 1) != 0) data = nil;
    }
    close(fd); return data;
}
/* Return names from the held directory descriptor; neither a link nor a non-regular entry is ever followed. */
static NSArray<NSString *> *inventory(int fd, BOOL metadata) {
    int copy = dup(fd);
    DIR *directory = copy >= 0 ? fdopendir(copy) : NULL;
    if (!directory) { if (copy >= 0) close(copy); return nil; }
    NSMutableArray *names = [NSMutableArray array];
    struct dirent *entry;
    BOOL valid = YES;
    errno = 0;
    while ((entry = readdir(directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        NSString *name = [[NSString alloc] initWithBytes:entry->d_name length:strlen(entry->d_name) encoding:NSUTF8StringEncoding];
        struct stat st;
        if (metadata && [name isEqual:metadata_name]) continue;
        if (!name || !file_name(name) || fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) ||
            !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > HOST_RECORDING_PACK_MAX_FILE_BYTES ||
            names.count >= HOST_RECORDING_PACK_MAX_FILES) { valid = NO; break; }
        [names addObject:name];
        errno = 0;
    }
    if (errno) valid = NO;
    closedir(directory);
    return valid ? [names sortedArrayUsingSelector:@selector(compare:)] : nil;
}
static NSDictionary *hashes(int fd, NSArray<NSString *> *names) {
    if (!names) return nil;
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    NSUInteger total = 0;
    for (NSString *name in names) {
        NSData *data = read_regular(fd, name.fileSystemRepresentation, HOST_RECORDING_PACK_MAX_FILE_BYTES);
        if (!data.length || total + data.length > HOST_RECORDING_PACK_MAX_TOTAL_BYTES) return nil;
        total += data.length;
        result[name] = digest(data);
    }
    return result;
}
static NSDictionary *metadata(NSDictionary *identity, NSDictionary *updates, NSDictionary *files) {
    NSMutableDictionary *value = [@{ @"schema": @1, @"identity": identity, @"updates": updates, @"files": files } mutableCopy];
    NSString *sha = host_benchmark_snapshot_digest(value);
    if (!sha) return nil;
    value[@"sha256"] = sha;
    return value;
}
NSDictionary *host_benchmark_snapshot_load(NSString *folder, NSDictionary *identity) {
    NSString *recordings = [folder stringByAppendingPathComponent:@"recordings"];
    int fd = open(recordings.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return nil;
    NSData *data = read_regular(fd, metadata_name.fileSystemRepresentation, 512 * 1024);
    id value = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    NSDictionary *actual = hashes(fd, inventory(fd, YES));
    close(fd);
    if (![value isKindOfClass:NSDictionary.class] || [value count] != 5 || ![value[@"schema"] isEqual:@1] ||
        ![value[@"identity"] isEqual:identity] || ![value[@"updates"] isKindOfClass:NSDictionary.class] ||
        ![value[@"files"] isKindOfClass:NSDictionary.class] || !hash_valid(value[@"sha256"]) ||
        ![value[@"files"] isEqual:actual]) return nil;
    NSDictionary *expected = metadata(identity, value[@"updates"], actual);
    return [value isEqual:expected] ? value : nil;
}
NSDictionary *host_benchmark_snapshot_create(NSString *folder, NSArray<NSString *> *sources,
    NSDictionary *identity, NSDictionary *updates, NSError **error) {
    NSFileManager *files = NSFileManager.defaultManager;
    NSString *stage = [folder stringByAppendingPathComponent:[@".snapshot-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    NSString *target = [folder stringByAppendingPathComponent:@"recordings"];
    NSDictionary *result = nil;
    NSMutableDictionary *lengths = [NSMutableDictionary dictionary];
    NSUInteger total = 0;
    if (error) *error = nil;
    if (![files createDirectoryAtPath:stage withIntermediateDirectories:YES attributes:@{ NSFilePosixPermissions: @0700 } error:error]) return nil;
    for (NSString *source in sources) {
        int fd = open(source.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        NSArray *names = fd >= 0 ? inventory(fd, NO) : nil;
        if (!names) { if (fd >= 0) close(fd); goto failed; }
        BOOL good = YES;
        for (NSString *name in names) {
            NSData *data = read_regular(fd, name.fileSystemRepresentation, HOST_RECORDING_PACK_MAX_FILE_BYTES);
            if (!data.length || total - [lengths[name] unsignedIntegerValue] + data.length > HOST_RECORDING_PACK_MAX_TOTAL_BYTES ||
                (!lengths[name] && lengths.count >= HOST_RECORDING_PACK_MAX_FILES)) { good = NO; break; }
            total = total - [lengths[name] unsignedIntegerValue] + data.length;
            lengths[name] = @(data.length);
            if (![data writeToFile:[stage stringByAppendingPathComponent:name] options:NSDataWritingAtomic error:error]) { good = NO; break; }
        }
        close(fd);
        if (!good) goto failed;
    }
    {
        int fd = open(stage.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        NSDictionary *contents = fd >= 0 ? hashes(fd, inventory(fd, NO)) : nil;
        if (fd >= 0) close(fd);
        if (!contents || !(result = metadata(identity, updates, contents))) goto failed;
        NSData *data = json(result);
        if (!data || ![data writeToFile:[stage stringByAppendingPathComponent:metadata_name] options:NSDataWritingAtomic error:error]) goto failed;
        struct stat st;
        if (!lstat(target.fileSystemRepresentation, &st)) {
            if (!S_ISDIR(st.st_mode) || renameatx_np(AT_FDCWD, stage.fileSystemRepresentation, AT_FDCWD,
                target.fileSystemRepresentation, RENAME_SWAP)) goto failed;
            /* The old snapshot now has our unique staging name. */
            [files removeItemAtPath:stage error:nil];
        } else if (errno != ENOENT || rename(stage.fileSystemRepresentation, target.fileSystemRepresentation)) goto failed;
        return result;
    }
failed:
    [files removeItemAtPath:stage error:nil];
    if (error && !*error) *error = [NSError errorWithDomain:error_domain code:1
        userInfo:@{ NSLocalizedDescriptionKey: @"The benchmark recording snapshot could not be copied or verified." }];
    return nil;
}
