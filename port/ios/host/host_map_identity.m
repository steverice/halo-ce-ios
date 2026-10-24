/* Apache-2.0. Hashing stays on utility workers and outside frame hooks. */
#import "host_map_identity.h"
#import <CommonCrypto/CommonDigest.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#define CACHE_ROOT @"shader-upload"
#define CACHE_PATH @"shader-upload/map-identities.json"
static dispatch_queue_t identity_worker, priority_worker;
static NSLock *cache_lock;
static NSMutableDictionary *map_cache;
static NSMutableSet *pending;
static atomic_bool ready;
static const char identity_key, priority_key;
static NSString *map_path(NSString *path) {
    return [(path.isAbsolutePath ? path : [NSFileManager.defaultManager.currentDirectoryPath stringByAppendingPathComponent:path]) stringByStandardizingPath];
}
static NSString *signature(NSString *path) {
    struct stat info;
    if(stat(path.fileSystemRepresentation,&info) || !S_ISREG(info.st_mode))return nil;
    return [NSString stringWithFormat:@"%llu:%llu:%lld:%lld:%ld:%lld:%ld",(unsigned long long)info.st_dev,(unsigned long long)info.st_ino,(long long)info.st_size,(long long)info.st_mtimespec.tv_sec,info.st_mtimespec.tv_nsec,(long long)info.st_ctimespec.tv_sec,info.st_ctimespec.tv_nsec];
}
#if HALO_SHADER_UPLOAD || HALO_PERF_REPORTS
static BOOL valid_identity(id identity) {
    if(![identity isKindOfClass:NSDictionary.class])return NO;
    NSString *fingerprint=identity[@"fingerprint"],*format=identity[@"format"],*stamp=identity[@"signature"];
    return [fingerprint isKindOfClass:NSString.class] && fingerprint.length==64 &&
        [fingerprint rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location==NSNotFound &&
        [format isKindOfClass:NSString.class] && format.length<32 && [format hasPrefix:@"cache-"] &&
        [stamp isKindOfClass:NSString.class] && stamp.length<256;
}
#endif
static void persist_locked(void) {
    /* Keep publication and persistence ordered across priority/background hashes. */
    NSData *bytes=[NSJSONSerialization dataWithJSONObject:map_cache options:NSJSONWritingSortedKeys error:nil];
    if(bytes.length>65536 || ![bytes writeToFile:CACHE_PATH options:NSDataWritingAtomic error:nil])return;
    int fd=open(CACHE_PATH.fileSystemRepresentation,O_RDONLY);if(fd>=0){fsync(fd);close(fd);}
}
static void hash_map(NSString *path) {
    NSString *before=signature(path);if(!before)return;
    [cache_lock lock];BOOL unchanged=[map_cache[path][@"signature"] isEqual:before];[cache_lock unlock];
    if(unchanged)return;
    NSInputStream *input=[NSInputStream inputStreamWithFileAtPath:path];[input open];
    CC_SHA256_CTX context;CC_SHA256_Init(&context);uint8_t buffer[65536],header[8];NSInteger length;
    size_t header_size=0;
    while((length=[input read:buffer maxLength:sizeof(buffer)])>0) {
        size_t take=MIN((size_t)length,sizeof(header)-header_size);
        if(take){memcpy(header+header_size,buffer,take);header_size+=take;}
        CC_SHA256_Update(&context,buffer,(CC_LONG)length);
    }
    [input close];if(length<0 || header_size<sizeof(header) || ![before isEqual:signature(path)])return;
    uint32_t version=(uint32_t)header[4]|(uint32_t)header[5]<<8|(uint32_t)header[6]<<16|(uint32_t)header[7]<<24;
    uint8_t bytes[32];CC_SHA256_Final(bytes,&context);NSMutableString *hex=[NSMutableString string];
    for(size_t i=0;i<32;i++)[hex appendFormat:@"%02x",bytes[i]];
    NSString *fingerprint=[hex copy];
    [cache_lock lock];
    for(NSDictionary *existing in map_cache.allValues) {
        NSString *prior=existing[@"fingerprint"];
        if([[prior substringToIndex:60] isEqual:[fingerprint substringToIndex:60]] && ![prior isEqual:fingerprint]){[cache_lock unlock];return;}
    }
    if(map_cache[path] || map_cache.count<128) {
        map_cache[path]=@{@"fingerprint":fingerprint,@"format":[NSString stringWithFormat:@"cache-%u",version],@"signature":before};
        persist_locked();
    }
    [cache_lock unlock];
}
void host_map_identity_init(void) {
#if HALO_SHADER_UPLOAD || HALO_PERF_REPORTS
    static dispatch_once_t once;dispatch_once(&once,^{
        cache_lock=[NSLock new];map_cache=[NSMutableDictionary dictionary];pending=[NSMutableSet set];
        identity_worker=dispatch_queue_create("org.visr.map-identity",dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,QOS_CLASS_UTILITY,0));
        priority_worker=dispatch_queue_create("org.visr.ui-map-identity",dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,QOS_CLASS_UTILITY,0));
        dispatch_queue_set_specific(identity_worker,&identity_key,(void *)&identity_key,NULL);
        dispatch_queue_set_specific(priority_worker,&priority_key,(void *)&priority_key,NULL);
        [NSFileManager.defaultManager createDirectoryAtPath:CACHE_ROOT withIntermediateDirectories:YES attributes:nil error:nil];
        NSNumber *size=[NSFileManager.defaultManager attributesOfItemAtPath:CACHE_PATH error:nil][NSFileSize];
        NSData *bytes=size && size.unsignedLongLongValue<=65536 ? [NSData dataWithContentsOfFile:CACHE_PATH] : nil;
        id saved=bytes ? [NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil] : nil;
        if([saved isKindOfClass:NSDictionary.class] && [saved count]<=128)for(id path in saved) {
            id identity=saved[path];
            if([path isKindOfClass:NSString.class] && [path length]<=4096 && valid_identity(identity))
                map_cache[path]=@{@"fingerprint":[identity[@"fingerprint"] copy],@"format":[identity[@"format"] copy],@"signature":[identity[@"signature"] copy]};
        }
        atomic_store_explicit(&ready,true,memory_order_release);
        dispatch_async(priority_worker,^{@autoreleasepool{hash_map(map_path(@"maps/ui.map"));}});
        dispatch_async(identity_worker,^{
            for(NSString *folder in @[@"maps",@"custom_maps"]) {
                NSDirectoryEnumerator *entries=[NSFileManager.defaultManager enumeratorAtPath:folder];
                for(NSString *name in entries)if([name.pathExtension isEqual:@"map"] && ![name isEqual:@"ui.map"])
                    @autoreleasepool{hash_map(map_path([folder stringByAppendingPathComponent:name]));}
            }
        });
    });
#endif
}
NSDictionary *host_map_identity_lookup(const char *name) {
    if(!name || !*name || !atomic_load_explicit(&ready,memory_order_acquire))return nil;
    NSString *path=map_path(@(name));if(![path.pathExtension isEqual:@"map"])return nil;
    NSString *stamp=signature(path);
    [cache_lock lock];NSDictionary *identity=map_cache[path];
    if(!stamp || ![identity[@"signature"] isEqual:stamp])identity=nil;
    BOOL schedule=stamp && !identity && ![pending containsObject:path] && pending.count<128;
    if(schedule)[pending addObject:path];[cache_lock unlock];
    if(schedule)dispatch_async(identity_worker,^{@autoreleasepool{hash_map(path);[cache_lock lock];[pending removeObject:path];[cache_lock unlock];}});
    return identity;
}
NSDictionary *host_map_identity_resolve(const char *name) {
    if(!name || !*name || !atomic_load_explicit(&ready,memory_order_acquire))return nil;
    NSString *path=map_path(@(name));if(![path.pathExtension isEqual:@"map"])return nil;
    if(dispatch_get_specific(&priority_key))hash_map(path);
    else dispatch_sync(priority_worker,^{@autoreleasepool{hash_map(path);}});
    return host_map_identity_lookup(name);
}
void host_map_identity_prepare_ui(void) { host_map_identity_resolve("maps/ui.map"); }
void host_map_identity_wait(void) {
    if(!atomic_load_explicit(&ready,memory_order_acquire))return;
    if(!dispatch_get_specific(&identity_key))dispatch_sync(identity_worker,^{});
    if(!dispatch_get_specific(&priority_key))dispatch_sync(priority_worker,^{});
}
