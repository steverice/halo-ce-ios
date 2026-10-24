#import "host_map_identity.h"
#import "host_shader_upload.h"
#include <assert.h>
#include <unistd.h>
int main(void) { @autoreleasepool {
    NSString *root=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    assert([NSFileManager.defaultManager createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil]);
    assert(chdir(root.fileSystemRepresentation)==0);
    assert(!host_shader_upload_map_identity("maps/new.map"));
    assert(![NSFileManager.defaultManager fileExistsAtPath:@"shader-upload"]);
    host_map_identity_init();
#if !HALO_PERF_REPORTS && !HALO_SHADER_UPLOAD
    assert(!host_map_identity_resolve("maps/new.map"));
    assert(![NSFileManager.defaultManager fileExistsAtPath:@"shader-upload"]);
#else
    host_map_identity_wait();
    assert([NSFileManager.defaultManager createDirectoryAtPath:@"maps" withIntermediateDirectories:YES attributes:nil error:nil]);
    NSMutableData *data=[NSMutableData dataWithLength:131072]; uint8_t *bytes=data.mutableBytes;
    memcpy(bytes,"head",4); bytes[4]=7; bytes[data.length-1]=1;
    assert([data writeToFile:@"maps/new.map" atomically:NO]);
    assert(!host_shader_upload_map_identity("maps/new.map")); host_map_identity_wait();
    NSDictionary *first=host_shader_upload_map_identity("maps/new.map");
    assert([first[@"fingerprint"] length]==64 && [first[@"format"] isEqual:@"cache-7"]);
    assert(![first isKindOfClass:NSMutableDictionary.class]);
    NSString *hash=first[@"fingerprint"];
    bytes[data.length-1]=2; assert([data writeToFile:@"maps/new.map" atomically:NO]);
    assert(!host_shader_upload_map_identity("maps/new.map")); host_map_identity_wait();
    NSDictionary *second=host_shader_upload_map_identity("maps/new.map");
    assert(second && ![second[@"fingerprint"] isEqual:hash]);
    assert([first[@"fingerprint"] isEqual:hash]);
    assert([host_map_identity_resolve("maps/new.map") isEqual:second]);
    assert([NSFileManager.defaultManager fileExistsAtPath:@"shader-upload/map-identities.json"]);
    assert([NSFileManager.defaultManager removeItemAtPath:@"maps/new.map" error:nil]);
    assert(!host_shader_upload_map_identity("maps/new.map"));
#endif
    host_map_identity_wait(); assert(chdir("/")==0);
    assert([NSFileManager.defaultManager removeItemAtPath:root error:nil]); return 0;
} }
