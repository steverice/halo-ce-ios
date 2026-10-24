#import "host_benchmark_outbox.h"
#import <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <unistd.h>

static BOOL sync_file(NSString *path) {
    int fd=open(path.fileSystemRepresentation,O_RDONLY);
    if(fd<0)return NO;
    int result=fsync(fd); close(fd); return result==0;
}
NSString *host_typed_outbox_store(NSString *folder, NSData *report, NSString *prefix, HostOutboxValidator validate, NSError **error) {
    id parsed=report ? [NSJSONSerialization JSONObjectWithData:report options:0 error:error] : nil;
    if(![parsed isKindOfClass:NSDictionary.class] || !validate || !validate(parsed) ||
        ![@[@"benchmark", @"shader", @"perf"] containsObject:prefix])return nil;
    if(![NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:error])return nil;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(report.bytes,(CC_LONG)report.length,digest);
    NSMutableString *name=[NSMutableString stringWithFormat:@"%@-",prefix];
    for(size_t i=0;i<sizeof(digest);i++)[name appendFormat:@"%02x",digest[i]];
    NSString *path=[folder stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"json"]];
    if([NSFileManager.defaultManager fileExistsAtPath:path])
        return [[NSData dataWithContentsOfFile:path] isEqual:report] ? path : nil;
    if(![report writeToFile:path options:NSDataWritingAtomic error:error] || !sync_file(path) || !sync_file(folder))return nil;
    return path;
}
NSArray<NSString *> *host_typed_outbox_pending(NSString *folder, NSString *prefix) {
    NSMutableArray *pending=[NSMutableArray array];
    NSArray *names=[NSFileManager.defaultManager contentsOfDirectoryAtPath:folder error:nil];
    for(NSString *name in [names sortedArrayUsingSelector:@selector(compare:)]) {
        NSString *path=[folder stringByAppendingPathComponent:name];
        if([name hasPrefix:[prefix stringByAppendingString:@"-"]] && [name.pathExtension isEqual:@"json"] &&
            ![NSFileManager.defaultManager fileExistsAtPath:[path stringByAppendingPathExtension:@"sent"]])
            [pending addObject:path];
    }
    return pending;
}
NSString *host_benchmark_outbox_store(NSString *folder, NSData *report, NSError **error) {
    return host_typed_outbox_store(folder,report,@"benchmark",^BOOL(NSDictionary *value) {
        return [value[@"code"] isKindOfClass:NSString.class] && [value[@"runs"] isKindOfClass:NSArray.class];
    },error);
}
NSArray<NSString *> *host_benchmark_outbox_pending(NSString *folder) { return host_typed_outbox_pending(folder,@"benchmark"); }
BOOL host_benchmark_outbox_acknowledge(NSString *path, NSError **error) {
    NSString *receipt=[path stringByAppendingPathExtension:@"sent"];
    NSData *data=[@"CloudKit confirmed this immutable report.\n" dataUsingEncoding:NSUTF8StringEncoding];
    return [data writeToFile:receipt options:NSDataWritingAtomic error:error] && sync_file(receipt) && sync_file(path.stringByDeletingLastPathComponent);
}

static BOOL verify_copy(NSString *source, NSString *destination, NSError **error) {
    NSNumber *directory=nil, *regular=nil;
    NSURL *url=[NSURL fileURLWithPath:source];
    if(![url getResourceValue:&directory forKey:NSURLIsDirectoryKey error:error] ||
        ![url getResourceValue:&regular forKey:NSURLIsRegularFileKey error:error])return NO;
    if(regular.boolValue)return [[NSData dataWithContentsOfFile:source] isEqual:[NSData dataWithContentsOfFile:destination]] && sync_file(destination);
    if(!directory.boolValue)return NO;
    NSArray *names=[NSFileManager.defaultManager contentsOfDirectoryAtPath:source error:error];
    if(!names)return NO;
    for(NSString *name in names)
        if(!verify_copy([source stringByAppendingPathComponent:name],[destination stringByAppendingPathComponent:name],error))return NO;
    return sync_file(destination);
}
BOOL host_benchmark_archive_interrupted(NSString *folder, NSError **error) {
    NSFileManager *files=NSFileManager.defaultManager;
    NSArray *names=[files contentsOfDirectoryAtPath:folder error:error];
    if(!names)return NO;
    NSString *archive=[[folder stringByAppendingPathComponent:@"history"] stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    if(![files createDirectoryAtPath:archive withIntermediateDirectories:YES attributes:nil error:error])return NO;
    NSMutableArray *preserved=[NSMutableArray array];
    for(NSString *name in names) {
        if(!([name isEqual:@"journal.txt"] || [name isEqual:@"journal-snapshot.sha256"] || [name isEqual:@"recordings"] ||
            [name isEqual:@"report.json"] || ([name hasPrefix:@"run-"] && [name.pathExtension isEqual:@"json"]) ||
            ([name hasPrefix:@"output-"] && [name.pathExtension isEqual:@"json"])))continue;
        NSString *source=[folder stringByAppendingPathComponent:name], *target=[archive stringByAppendingPathComponent:name];
        if(![files copyItemAtPath:source toPath:target error:error] || !verify_copy(source,target,error))return NO;
        [preserved addObject:name];
    }
    NSData *receipt=[NSJSONSerialization dataWithJSONObject:@{ @"status":@"verified_raw_interrupted_suite", @"files":preserved }
        options:NSJSONWritingSortedKeys error:error];
    NSString *receipt_path=[archive stringByAppendingPathComponent:@"preservation.json"];
    return receipt && [receipt writeToFile:receipt_path options:NSDataWritingAtomic error:error] &&
        sync_file(receipt_path) && sync_file(archive) && sync_file(archive.stringByDeletingLastPathComponent);
}
NSString *host_typed_outbox_store_asset(NSString *folder,NSString *asset,NSDictionary *metadata,NSString *prefix,NSString *hash,HostOutboxAssetValidator validate,NSError **error) {
    if(![prefix isEqual:@"perf"] || ![metadata isKindOfClass:NSDictionary.class] || hash.length!=64 ||
       [hash rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location!=NSNotFound || !validate || !validate(asset,metadata,hash))return nil;
    if(![NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:error])return nil;
    NSString *path=[folder stringByAppendingPathComponent:[NSString stringWithFormat:@"%@-%@.json",prefix,hash]],*destination=[path stringByAppendingPathExtension:@"gz"];
    NSData *manifest=[NSJSONSerialization dataWithJSONObject:metadata options:NSJSONWritingSortedKeys error:error];
    if([NSFileManager.defaultManager fileExistsAtPath:path])return [[NSData dataWithContentsOfFile:path] isEqual:manifest] && validate(destination,metadata,hash) ? path:nil;
    NSString *temporary=[destination stringByAppendingPathExtension:NSUUID.UUID.UUIDString];
    if(![NSFileManager.defaultManager copyItemAtPath:asset toPath:temporary error:error] || !sync_file(temporary))return nil;
    [NSFileManager.defaultManager removeItemAtPath:destination error:nil];
    BOOL ok=[NSFileManager.defaultManager moveItemAtPath:temporary toPath:destination error:error] &&
        [manifest writeToFile:path options:NSDataWritingAtomic error:error] && sync_file(path) && sync_file(folder);
    [NSFileManager.defaultManager removeItemAtPath:temporary error:nil];return ok?path:nil;
}
