/* Exercise manager identity and retention without writing to the app cache. */
#import "../host/host_data_updates.m"
#include <assert.h>
#include <unistd.h>
void host_logf(int priority, const char *format, ...) { (void)priority; (void)format; }
void host_device_identify(struct host_device *d) { memset(d,0,sizeof(*d)); strcpy(d->platform,"ios"); strcpy(d->model,"iPhone18,1"); }
int main(void)
{
    @autoreleasepool {
        assert(publicKey(@"").length == 0);
        assert(publicKey([@"a" stringByPaddingToLength:64 withString:@"a" startingAtIndex:0]).length == 32);
        assert(publicKey([@"z" stringByPaddingToLength:64 withString:@"z" startingAtIndex:0]).length == 0);
        NSString *base = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        NSFileManager *fm = NSFileManager.defaultManager;
        [fm createDirectoryAtPath:base withIntermediateDirectories:YES attributes:nil error:NULL];
        struct host_device d; host_device_identify(&d);
        assert([profileFeedURL(@"https://example.com/packs/profiles", &d).absoluteString isEqual:@"https://example.com/packs/profiles/ios/iPhone18,1/_.json"]);
        assert(!profileFeedURL(@"https://user@example.com/packs", &d));
        assert(!profileFeedURL(@"https://example.com/packs?x=1", &d));
        strcpy(d.model,"../bad"); assert(!profileFeedURL(@"https://example.com/packs", &d));
        NSString *shaderBase = [base stringByAppendingPathComponent:@"shader-base"], *shaderAdd = [base stringByAppendingPathComponent:@"shader-add"];
        [fm createDirectoryAtPath:shaderBase withIntermediateDirectories:YES attributes:nil error:NULL];
        [fm createDirectoryAtPath:shaderAdd withIntermediateDirectories:YES attributes:nil error:NULL];
        NSString *ps = [@"00" stringByPaddingToLength:504 withString:@"00" startingAtIndex:0];
        NSString *descriptors = [NSString stringWithFormat:@"vs 0000000000000001 0 0\nps 2fba1332b14beb55 %@\n",ps];
        NSString *pipeline = @"pipeline 0000000000000001.0.0 2fba1332b14beb55 0 0 0 0 15 15 1 00000000000000000000000000000000 0 0\n";
        [[descriptors stringByAppendingString:pipeline] writeToFile:[shaderBase stringByAppendingPathComponent:@"a10.txt"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        NSString *changed = [pipeline stringByReplacingOccurrencesOfString:@"15 15 1" withString:@"15 0 1"];
        [[descriptors stringByAppendingString:changed] writeToFile:[shaderAdd stringByAppendingPathComponent:@"a10.txt"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        NSDictionary *frozen = snapshotShaderLists(shaderAdd,shaderBase);
        NSString *merged = [[NSString alloc] initWithData:frozen[@"a10"] encoding:NSUTF8StringEncoding];
        assert([merged containsString:pipeline] && [merged containsString:changed]);
        [@"invalid\n" writeToFile:[shaderAdd stringByAppendingPathComponent:@"a10.txt"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        assert([[[NSString alloc] initWithData:frozen[@"a10"] encoding:NSUTF8StringEncoding] isEqual:merged]);
        assert(!snapshotShaderLists(shaderAdd,shaderBase));
        [fm removeItemAtPath:shaderBase error:NULL]; [fm removeItemAtPath:shaderAdd error:NULL];
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
