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
        NSString *captures = directory(root, @"captures");
        NSDictionary *missing = host_benchmark_shader_snapshot(captures, @"a10");
        CHECK([missing[@"status"] isEqual:@"missing"]);
        CHECK([host_benchmark_shader_snapshot(@"", @"a10")[@"status"] isEqual:@"disabled"]);
        CHECK([host_benchmark_shader_snapshot(captures, @"../secret")[@"status"] isEqual:@"invalid_map"]);
        put(captures, @"a10.txt", @"vs 1 2 3\n");
        NSDictionary *shader_before = host_benchmark_shader_snapshot(captures, @"a10");
        put(captures, @"a10.txt", @"vs 1 2 3\nvs 4 5 6\n");
        NSDictionary *shader_after = host_benchmark_shader_snapshot(captures, @"a10");
        NSDictionary *delta = host_benchmark_shader_delta(shader_before, shader_after);
        CHECK([delta[@"status"] isEqual:@"captured"]);
        CHECK([delta[@"text"] isEqual:@"vs 4 5 6\n"]);
        CHECK([delta[@"sha256"] isEqual:@"d61d2037218baf5ef10b7a4fb0ed6b345c446c1c7e6bf19876d10233785c6236"]);
        CHECK([delta[@"bytes"] isEqual:@9] && [delta[@"map"] isEqual:@"a10"]);
        CHECK([delta[@"baseline_sha256"] isEqual:shader_before[@"sha256"]]);
        NSDictionary *shader_run = @{ @"index": @0, @"map": @"a10", @"shader_capture": delta };
        NSDictionary *previous_output = @{ @"code": @"previous", @"app": @{ @"image": @"guest-a" },
            @"output": @{ @"width": @1920 }, @"runs": @[ shader_run ] };
        NSDictionary *evidence = host_benchmark_shader_evidence(@[ shader_run ], @[ previous_output ], captures);
        CHECK([evidence[@"shader_dependency_candidates"][@"a10"][@"text"] isEqual:shader_after[@"text"]]);
        CHECK([evidence[@"shader_dependency_candidates"][@"a10"][@"scope"] isEqual:@"cumulative_unattributed_dependency_candidates"]);
        NSDictionary *prior = [evidence[@"shader_output_intervals"] firstObject];
        CHECK([prior[@"code"] isEqual:@"previous"] && [prior[@"app"] isEqual:previous_output[@"app"]]);
        CHECK([[prior[@"runs"] firstObject][@"shader_capture"] isEqual:delta]);
        NSMutableDictionary *payload = [evidence mutableCopy]; payload[@"runs"] = @[ shader_run ];
        NSData *full = [NSJSONSerialization dataWithJSONObject:payload options:0 error:nil];
        NSMutableDictionary *preview = [NSJSONSerialization JSONObjectWithData:full options:NSJSONReadingMutableContainers error:nil];
        host_benchmark_shader_summarize(preview);
        CHECK(![preview[@"runs"][0][@"shader_capture"][@"text"] isEqual:delta[@"text"]]);
        CHECK(![preview[@"shader_output_intervals"][0][@"runs"][0][@"shader_capture"][@"text"] isEqual:delta[@"text"]]);
        CHECK(![preview[@"shader_dependency_candidates"][@"a10"][@"text"] isEqual:shader_after[@"text"]]);
        CHECK([payload[@"runs"][0][@"shader_capture"][@"text"] isEqual:delta[@"text"]]);
        CHECK([host_benchmark_shader_delta(shader_after, shader_after)[@"status"] isEqual:@"no_new_records"]);
        CHECK([host_benchmark_shader_delta(missing, shader_after)[@"text"] isEqual:shader_after[@"text"]]);
        CHECK([host_benchmark_shader_delta(missing, missing)[@"status"] isEqual:@"missing"]);
        put(captures, @"a10.txt", @"replaced\n");
        CHECK([host_benchmark_shader_delta(shader_after, host_benchmark_shader_snapshot(captures, @"a10"))[@"status"] isEqual:@"changed_baseline"]);
        put(captures, @"a10.txt", @"partial");
        CHECK([host_benchmark_shader_delta(missing, host_benchmark_shader_snapshot(captures, @"a10"))[@"status"] isEqual:@"incomplete_record"]);
        CHECK([files createSymbolicLinkAtPath:[captures stringByAppendingPathComponent:@"b30.txt"] withDestinationPath:[captures stringByAppendingPathComponent:@"a10.txt"] error:nil]);
        CHECK([host_benchmark_shader_snapshot(captures, @"b30")[@"status"] isEqual:@"unavailable"]);
        CHECK([host_benchmark_shader_delta(shader_after, host_benchmark_shader_snapshot(captures, @"b30"))[@"status"] isEqual:@"map_mismatch"]);
        NSData *serialized = [NSJSONSerialization dataWithJSONObject:delta options:0 error:nil];
        CHECK(serialized && [[[NSJSONSerialization JSONObjectWithData:serialized options:0 error:nil] objectForKey:@"text"] isEqual:delta[@"text"]]);
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
        NSData *large_input = [NSMutableData dataWithLength:3 * 1024 * 1024 + 64 * 1024];
        for (int index = 0; index < 4; index++) {
            NSString *name = [NSString stringWithFormat:@"large-%d.input", index];
            CHECK([large_input writeToFile:[source stringByAppendingPathComponent:name] atomically:YES]);
        }
        NSDictionary *large = host_benchmark_snapshot_create(target, @[ source ], identity, updates, &error);
        CHECK(large != nil && [host_benchmark_snapshot_load(target, identity) isEqual:large]);
        CHECK([large[@"files"] count] == 7);
        CHECK([files removeItemAtPath:root error:nil]);
        printf("benchmark_snapshot_probe: %d checks passed\n", checks);
    }
    return 0;
}
