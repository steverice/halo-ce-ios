/* Foundation-only regression coverage for the benchmark's durable replay pin. */
#import <Foundation/Foundation.h>
#import "../host/host_benchmark_snapshot.h"
#include <stdio.h>
#include <stdlib.h>

static int checks;
#define CHECK(expression) do { checks++; if (!(expression)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); exit(1); } } while (0)
static void put(NSString *folder, NSString *name, NSString *text) {
    CHECK([text writeToFile:[folder stringByAppendingPathComponent:name] atomically:YES encoding:NSUTF8StringEncoding error:nil]);
}
static NSString *directory(NSString *parent, NSString *name) {
    NSString *path = [parent stringByAppendingPathComponent:name];
    CHECK([NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES attributes:@{ NSFilePosixPermissions: @0700 } error:nil]);
    return path;
}
int main(void) {
    @autoreleasepool {
        NSFileManager *files = NSFileManager.defaultManager;
        NSString *root = directory(NSTemporaryDirectory(), [@"visr-snapshot-" stringByAppendingString:NSUUID.UUID.UUIDString]);
        NSString *source = directory(root, @"source"), *target = directory(root, @"benchmark");
        NSDictionary *identity = @{ @"guest": @"guest-a", @"profile_sha256": @"profile-a", @"mode": @"quick" };
        NSDictionary *updates = @{ @"recordings": @{ @"sequence": @1, @"sha256": @"first" } };
        put(source, @"route.input", @"halo input recording 1\n");
        put(source, @"route.actions", @"halo action recording 1\n");
        put(source, @"route.route", @"map = a10\ndifficulty = normal\n");
        NSError *error = nil;
        NSDictionary *first = host_benchmark_snapshot_create(target, @[ source ], identity, updates, &error);
        CHECK(first != nil && error == nil);
        CHECK([first[@"files"] count] == 3); /* Bundled legacy recordings do not require a seed sidecar. */
        CHECK([first[@"sha256"] length] == 64);
        CHECK([host_benchmark_snapshot_load(target, identity) isEqual:first]);
        put(source, @"route.input", @"new update bytes\n");
        CHECK([host_benchmark_snapshot_load(target, identity) isEqual:first]);
        CHECK([host_benchmark_snapshot_load(target, identity)[@"updates"] isEqual:updates]);
        CHECK(host_benchmark_snapshot_load(target, @{ @"guest": @"guest-b" }) == nil);
        CHECK(host_benchmark_snapshot_load(target, @{ @"guest": @"guest-a", @"profile_sha256": @"profile-b", @"mode": @"quick" }) == nil);
        CHECK(host_benchmark_snapshot_load(target, @{ @"guest": @"guest-a", @"profile_sha256": @"profile-a", @"mode": @"full" }) == nil);
        NSString *pinned = [target stringByAppendingPathComponent:@"recordings"];
        put(pinned, @"route.input", @"tampered bytes\n");
        CHECK(host_benchmark_snapshot_load(target, identity) == nil);
        NSDictionary *second = host_benchmark_snapshot_create(target, @[ source ], identity,
            @{ @"recordings": @{ @"sequence": @2, @"sha256": @"second" } }, &error);
        CHECK(second != nil && ![second[@"sha256"] isEqual:first[@"sha256"]]);
        CHECK([host_benchmark_snapshot_load(target, identity) isEqual:second]);
        put(pinned, @"unexpected.cfg", @"quit\n");
        CHECK(host_benchmark_snapshot_load(target, identity) == nil);
        CHECK([files removeItemAtPath:[pinned stringByAppendingPathComponent:@"unexpected.cfg"] error:nil]);
        CHECK([host_benchmark_snapshot_load(target, identity) isEqual:second]);
        NSString *overlay = directory(root, @"local");
        put(overlay, @"route.input", @"local override\n");
        NSDictionary *third = host_benchmark_snapshot_create(target, @[ source, overlay ], identity, updates, &error);
        CHECK(third != nil);
        CHECK([[NSString stringWithContentsOfFile:[pinned stringByAppendingPathComponent:@"route.input"] encoding:NSUTF8StringEncoding error:nil] isEqual:@"local override\n"]);
        CHECK([host_benchmark_snapshot_load(target, identity) isEqual:third]);
        CHECK([files createSymbolicLinkAtPath:[overlay stringByAppendingPathComponent:@"bad.route"] withDestinationPath:[source stringByAppendingPathComponent:@"route.route"] error:nil]);
        /* A source's link, folder, stray file or empty file is skipped, never followed or copied. */
        directory(overlay, @"nested");
        put(overlay, @".DS_Store", @"finder\n");
        put(overlay, @"README.md", @"notes\n");
        put(overlay, @"empty.checks", @"");
        NSDictionary *skipped = host_benchmark_snapshot_create(target, @[ source, overlay ], identity, updates, &error);
        CHECK(skipped != nil && [skipped[@"files"] count] == 3 && !skipped[@"files"][@"bad.route"]);
        CHECK([host_benchmark_snapshot_load(target, identity) isEqual:skipped]);
        for (NSString *name in @[ @"bad.route", @"nested", @".DS_Store", @"README.md", @"empty.checks" ])
            CHECK([files removeItemAtPath:[overlay stringByAppendingPathComponent:name] error:nil]);
        /* A failed replacement preserves the old pin: a source that is itself a link is refused. */
        NSString *linked = [root stringByAppendingPathComponent:@"linked"];
        CHECK([files createSymbolicLinkAtPath:linked withDestinationPath:overlay error:nil]);
        CHECK(host_benchmark_snapshot_create(target, @[ linked ], identity, updates, &error) == nil);
        CHECK(error != nil);
        CHECK([host_benchmark_snapshot_load(target, identity) isEqual:skipped]);
        third = host_benchmark_snapshot_create(target, @[ source, overlay ], identity, updates, &error);
        CHECK(third != nil);
        CHECK([files removeItemAtPath:[pinned stringByAppendingPathComponent:@"route.route"] error:nil]);
        CHECK(host_benchmark_snapshot_load(target, identity) == nil);
        NSDictionary *empty = host_benchmark_snapshot_create(target, @[], identity, @{}, &error);
        CHECK(empty != nil && [empty[@"files"] count] == 0);
        CHECK([host_benchmark_snapshot_load(target, identity) isEqual:empty]);
        CHECK([files removeItemAtPath:root error:nil]);
        printf("benchmark_snapshot_probe: %d checks passed\n", checks);
    }
    return 0;
}
