#import <Foundation/Foundation.h>
#import "host_benchmark_outbox.h"
#include <assert.h>
#include <unistd.h>
int main(void) { @autoreleasepool {
    NSString *folder = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    NSData *first = [@"{\"code\":\"FIRST\",\"runs\":[]}" dataUsingEncoding:NSUTF8StringEncoding];
    NSData *second = [@"{\"code\":\"SECOND\",\"runs\":[{\"status\":\"partial\"}]}" dataUsingEncoding:NSUTF8StringEncoding];
    NSError *error=nil;
    NSString *a = host_benchmark_outbox_store(folder,first,&error);
    assert(a && !error && [host_benchmark_outbox_pending(folder) count]==1);
    assert([[NSData dataWithContentsOfFile:a] isEqual:first]);
    assert([host_benchmark_outbox_store(folder,first,&error) isEqual:a]);
    assert([host_benchmark_outbox_pending(folder) count]==1);
    NSString *b=host_benchmark_outbox_store(folder,second,&error);
    assert(b && ![a isEqual:b] && [host_benchmark_outbox_pending(folder) count]==2);
    /* A failed/ambiguous transport does not acknowledge or remove either report. */
    assert([host_benchmark_outbox_pending(folder) count]==2);
    assert(host_benchmark_outbox_acknowledge(a,&error));
    assert([host_benchmark_outbox_pending(folder) count]==1);
    assert([[NSData dataWithContentsOfFile:a] isEqual:first]);
    assert([host_benchmark_outbox_store(folder,first,&error) isEqual:a]);
    assert([host_benchmark_outbox_pending(folder) count]==1);
    assert(host_benchmark_outbox_acknowledge(b,&error));
    assert([host_benchmark_outbox_pending(folder) count]==0);
    assert(!host_benchmark_outbox_store(folder,[@"invalid" dataUsingEncoding:NSUTF8StringEncoding],&error));
    NSString *suite=[folder stringByAppendingPathComponent:@"suite"];
    assert([NSFileManager.defaultManager createDirectoryAtPath:[suite stringByAppendingPathComponent:@"recordings"] withIntermediateDirectories:YES attributes:nil error:nil]);
    assert([first writeToFile:[suite stringByAppendingPathComponent:@"run-00.json"] atomically:YES]);
    assert([second writeToFile:[suite stringByAppendingPathComponent:@"recordings/.snapshot.json"] atomically:YES]);
    assert([first writeToFile:[suite stringByAppendingPathComponent:@"journal.txt"] atomically:YES]);
    assert(host_benchmark_archive_interrupted(suite,&error));
    NSString *history=[suite stringByAppendingPathComponent:@"history"];
    NSArray *archives=[NSFileManager.defaultManager contentsOfDirectoryAtPath:history error:nil];
    assert(archives.count==1);
    NSString *copy=[history stringByAppendingPathComponent:archives[0]];
    assert([[NSData dataWithContentsOfFile:[copy stringByAppendingPathComponent:@"run-00.json"]] isEqual:first]);
    assert([[NSData dataWithContentsOfFile:[copy stringByAppendingPathComponent:@"recordings/.snapshot.json"]] isEqual:second]);
    assert([NSFileManager.defaultManager fileExistsAtPath:[copy stringByAppendingPathComponent:@"preservation.json"]]);
    /* Copying to a forbidden shape fails without removing original durable data. */
    assert(!host_benchmark_archive_interrupted([suite stringByAppendingPathComponent:@"journal.txt"],&error));
    assert([[NSData dataWithContentsOfFile:[suite stringByAppendingPathComponent:@"run-00.json"]] isEqual:first]);
    assert([NSFileManager.defaultManager removeItemAtPath:folder error:nil]);
    puts("benchmark outbox: PASS");
} }
