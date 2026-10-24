#import <Foundation/Foundation.h>
#import "host_benchmark_outbox.h"
#include <assert.h>
int main(void) { @autoreleasepool {
    NSString *root=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    NSData *payload=[@"{\"schema\":1,\"descriptors\":[\"pixel x\"],\"dependencies\":[]}" dataUsingEncoding:NSUTF8StringEncoding];
    NSString *path=host_typed_outbox_store(root,payload,@"shader",^BOOL(NSDictionary *value) { return [value[@"schema"] isEqual:@1]; },nil);
    assert(path && [path.lastPathComponent hasPrefix:@"shader-"]);
    assert(host_typed_outbox_pending(root,@"shader").count==1);
    assert(host_typed_outbox_pending(root,@"benchmark").count==0);
    assert(host_benchmark_outbox_store(root,payload,nil)==nil);
    assert(host_benchmark_outbox_acknowledge(path,nil));
    assert(host_typed_outbox_pending(root,@"shader").count==0);
    [NSFileManager.defaultManager removeItemAtPath:root error:nil];
} return 0; }
