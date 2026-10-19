#import "host_benchmark_outbox.h"
#import <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <unistd.h>

static BOOL sync_file(NSString *path) {
    int fd=open(path.fileSystemRepresentation,O_RDONLY);
    if(fd<0)return NO;
    int result=fsync(fd); close(fd); return result==0;
}
NSString *host_benchmark_outbox_store(NSString *folder, NSData *report, NSError **error) {
    id parsed=report ? [NSJSONSerialization JSONObjectWithData:report options:0 error:error] : nil;
    if(![parsed isKindOfClass:NSDictionary.class] || ![parsed[@"code"] isKindOfClass:NSString.class] ||
        ![parsed[@"runs"] isKindOfClass:NSArray.class])return nil;
    if(![NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:error])return nil;
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(report.bytes,(CC_LONG)report.length,digest);
    NSMutableString *name=[NSMutableString stringWithString:@"benchmark-"];
    for(size_t i=0;i<sizeof(digest);i++)[name appendFormat:@"%02x",digest[i]];
    NSString *path=[folder stringByAppendingPathComponent:[name stringByAppendingPathExtension:@"json"]];
    if([NSFileManager.defaultManager fileExistsAtPath:path])
        return [[NSData dataWithContentsOfFile:path] isEqual:report] ? path : nil;
    if(![report writeToFile:path options:NSDataWritingAtomic error:error] || !sync_file(path) || !sync_file(folder))return nil;
    return path;
}
NSArray<NSString *> *host_benchmark_outbox_pending(NSString *folder) {
    NSMutableArray *pending=[NSMutableArray array];
    NSArray *names=[NSFileManager.defaultManager contentsOfDirectoryAtPath:folder error:nil];
    for(NSString *name in [names sortedArrayUsingSelector:@selector(compare:)]) {
        NSString *path=[folder stringByAppendingPathComponent:name];
        if([name.pathExtension isEqual:@"json"] &&
            ![NSFileManager.defaultManager fileExistsAtPath:[path stringByAppendingPathExtension:@"sent"]])
            [pending addObject:path];
    }
    return pending;
}
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
