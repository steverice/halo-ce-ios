/* Exercise manager identity and retention without writing to the app cache. */
#import "../host/host_data_updates.m"
#include <assert.h>
#include <unistd.h>
int main(void)
{
    @autoreleasepool {
        assert(publicKey(@"").length == 0);
        assert(publicKey([@"a" stringByPaddingToLength:64 withString:@"a" startingAtIndex:0]).length == 32);
        assert(publicKey([@"z" stringByPaddingToLength:64 withString:@"z" startingAtIndex:0]).length == 0);
        NSString *base = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        NSFileManager *fm = NSFileManager.defaultManager;
        [fm createDirectoryAtPath:base withIntermediateDirectories:YES attributes:nil error:NULL];
        NSString *current = nil;
        for (NSString *character in @[@"a", @"b", @"c", @"d"]) {
            NSString *name = [character stringByPaddingToLength:64 withString:character startingAtIndex:0];
            NSString *path = [base stringByAppendingPathComponent:name];
            [fm createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:NULL];
            current = path;
        }
        NSString *unknown = [base stringByAppendingPathComponent:@"keep-unrelated"];
        [fm createDirectoryAtPath:unknown withIntermediateDirectories:YES attributes:nil error:NULL];
        pruneCompatibilityRoots(current);
        assert([fm fileExistsAtPath:current]);
        assert([fm contentsOfDirectoryAtPath:base error:NULL].count == 3);
        NSString *file = [current stringByAppendingPathComponent:@"one.toml"];
        [@"one" writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        HostDataPackStore *pack = [[HostDataPackStore alloc] initWithRoot:[base stringByAppendingPathComponent:@"cache"] bundled:current kind:@"profiles" channel:@"stable" publicKey:[NSData data] compatibility:@"profiles-1" validator:^BOOL(NSString *folder, NSError **error) { (void)folder; (void)error; return YES; }];
        [pack beginLaunch];
        NSDictionary *before = identity(pack, current);
        assert([before[@"bundled"] boolValue] && [before[@"sha256"] length] == 64);
        [@"two" writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        assert(![before[@"sha256"] isEqual:identity(pack, current)[@"sha256"]]);
        [fm removeItemAtPath:base error:NULL];
        puts("PASS: data update identity and bounded compatibility roots");
    }
    return 0;
}
