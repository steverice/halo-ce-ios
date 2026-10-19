/* Foundation-only import publication tests; no application or game is launched. */
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include "../port/macos/host_macos_import.m"

static int remove_incoming;
int xiso_maps_ready(const char *maps, char *error, size_t size) {
    (void)error; (void)size;
    NSString *path=@(maps);
    BOOL ready=[NSFileManager.defaultManager fileExistsAtPath:[path stringByAppendingPathComponent:@"valid"]];
    if(ready && remove_incoming) {
        remove_incoming=0;
        [NSFileManager.defaultManager removeItemAtPath:path error:nil];
    }
    return ready;
}
int xiso_extract_maps(const char *image, const char *destination, xiso_progress_proc callback,
    void *context, char *error, size_t size) {
    (void)image;(void)destination;(void)callback;(void)context;(void)error;(void)size;return 0;
}
void host_logf(int priority, const char *format, ...) { (void)priority;(void)format; }
void host_exit(int code) { exit(code); }
int host_alert(const char *title,const char *message,const char *first,const char *second) {
    (void)title;(void)message;(void)first;(void)second;return 1;
}
static void put(NSString *path,NSString *value) {
    assert([NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil]);
    assert([value writeToFile:[path stringByAppendingPathComponent:@"valid"] atomically:YES encoding:NSUTF8StringEncoding error:nil]);
}
static NSString *read_marker(NSString *path) {
    return [NSString stringWithContentsOfFile:[path stringByAppendingPathComponent:@"valid"] encoding:NSUTF8StringEncoding error:nil];
}
int main(void) {
    @autoreleasepool {
        NSFileManager *files=NSFileManager.defaultManager;
        NSString *root=[NSTemporaryDirectory() stringByAppendingPathComponent:[@"visr-import-test-" stringByAppendingString:NSUUID.UUID.UUIDString]];
        NSString *maps=[root stringByAppendingPathComponent:@"maps"];
        NSString *staging=[root stringByAppendingPathComponent:@"staging"];
        NSString *incoming=[staging stringByAppendingPathComponent:@"maps"];
        NSError *error=nil;
        put(maps,@"original");
        assert(!publish(staging,root,&error));
        assert([read_marker(maps)isEqualToString:@"original"]);
        put(incoming,@"replacement");
        remove_incoming=1; /* The input disappears after validation: exercise the actual rename rollback. */
        assert(!publish(staging,root,&error));
        assert([read_marker(maps)isEqualToString:@"original"]);
        assert(![files fileExistsAtPath:[staging stringByAppendingPathComponent:@"previous-maps"]]);
        put(incoming,@"replacement");
        assert(publish(staging,root,&error));
        assert([read_marker(maps)isEqualToString:@"replacement"]);
        assert([read_marker([staging stringByAppendingPathComponent:@"previous-maps"])isEqualToString:@"original"]);
        /* Recovery restores owned backups and leaves unrelated similarly named directories intact. */
        NSString *owned=[root stringByAppendingPathComponent:[@".visr-import-" stringByAppendingString:NSUUID.UUID.UUIDString]];
        NSString *backup=[owned stringByAppendingPathComponent:@"previous-maps"];
        put(backup,@"recovered");
        assert([@"VISR native import v1" writeToFile:[owned stringByAppendingPathComponent:@"owner.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil]);
        assert([files removeItemAtPath:maps error:nil]);
        NSString *unowned=[root stringByAppendingPathComponent:@".visr-import-unowned"];
        put(unowned,@"keep");
        recover(root);
        assert([read_marker(maps)isEqualToString:@"recovered"]);
        assert([read_marker(unowned)isEqualToString:@"keep"]);
        assert(![files fileExistsAtPath:owned]);
        VISRMacImport *operation=[VISRMacImport new];
        assert(progress((__bridge void *)operation,"",0,1));
        [operation cancel:nil];
        assert(!progress((__bridge void *)operation,"",0,1));
        assert([files removeItemAtPath:root error:nil]);
        puts("Native import publication, rollback, recovery, and cancellation checks passed");
    }
    return 0;
}
