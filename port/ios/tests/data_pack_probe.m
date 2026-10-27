#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#import "../host/host_data_pack.h"
#include "../../third_party/monocypher/monocypher-ed25519.h"
#import <objc/runtime.h>
#include <sys/stat.h>
#include <stdarg.h>

static BOOL loggedHTTPFailure, loggedNetworkFailure, loggedHealthy, loggedDropped, loggedStateMissing;
void host_logf(int priority, const char *format, ...) {
    (void)priority;
    char line[2048];
    va_list arguments;
    va_start(arguments, format); vsnprintf(line, sizeof(line), format, arguments); va_end(arguments);
    if (strstr(line, "HTTP 500")) loggedHTTPFailure = YES;
    if (strstr(line, "feed check ended")) loggedNetworkFailure = YES;
    if (strstr(line, "snapshot 1 is healthy")) loggedHealthy = YES;
    if (strstr(line, "dropped (its files are missing)")) loggedDropped = YES;
    if (strstr(line, "state is missing")) loggedStateMissing = YES;
    if (getenv("PACK_PROBE_LOG") && strstr(line, "data-packs:")) fprintf(stderr, "log: %s\n", line);
}

static uint8_t secretKey[64], publicKey[32];
static int failures;
#define CHECK(condition, label) do { if (!(condition)) { fprintf(stderr, "FAIL: %s\n", label); failures++; } } while (0)

static NSString *Digest(NSData *data) {
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
    NSMutableString *hex = [NSMutableString string];
    for (NSUInteger i = 0; i < sizeof(digest); i++) [hex appendFormat:@"%02x", digest[i]];
    return hex;
}
static NSData *JSON(id object) { return [NSJSONSerialization dataWithJSONObject:object options:0 error:NULL]; }
static NSData *Wrap(NSData *raw, NSData *payload) {
    uint8_t signature[64];
    crypto_ed25519_sign(signature, secretKey, raw.bytes, raw.length);
    return JSON(@{@"manifest": [raw base64EncodedStringWithOptions:0], @"signature": [[NSData dataWithBytes:signature length:64] base64EncodedStringWithOptions:0], @"payload": [payload base64EncodedStringWithOptions:0]});
}
static NSData *Envelope(NSInteger sequence, NSDictionary *changes, NSDictionary *files) {
    NSData *payload = JSON(@{@"files": files ?: @{@"test.toml": [[@"profile" dataUsingEncoding:NSUTF8StringEncoding] base64EncodedStringWithOptions:0]}});
    NSMutableDictionary *manifest = [@{@"schema": @1, @"kind": @"profiles", @"channel": @"stable", @"sequence": @(sequence), @"minimum_engine": @1, @"maximum_engine": @1, @"compatibility": @"profiles-1", @"sha256": Digest(payload), @"notes": @"test"} mutableCopy];
    [manifest addEntriesFromDictionary:changes ?: @{}];
    NSData *raw = JSON(manifest);
    return Wrap(raw, payload);
}
static NSInteger requests;
static NSData *networkEnvelope;
static NSInteger networkStatus = 200;
static NSInteger notModified;
static BOOL networkFails;
@interface PackProtocol : NSURLProtocol
@end
@implementation PackProtocol
+ (BOOL)canInitWithRequest:(NSURLRequest *)request { return YES; }
+ (NSURLRequest *)canonicalRequestForRequest:(NSURLRequest *)request { return request; }
- (void)startLoading {
    requests++;
    if (networkFails) { [self.client URLProtocol:self didFailWithError:[NSError errorWithDomain:NSURLErrorDomain code:NSURLErrorTimedOut userInfo:nil]]; return; }
    BOOL matches = [[self.request valueForHTTPHeaderField:@"If-None-Match"] isEqual:@"\"feed-1\""];
    NSInteger status = matches ? 304 : networkStatus;
    if (matches) notModified++;
    NSHTTPURLResponse *response = [[NSHTTPURLResponse alloc] initWithURL:self.request.URL statusCode:status HTTPVersion:@"HTTP/1.1" headerFields:@{@"Content-Type": @"application/json", @"ETag": @"\"feed-1\""}];
    [self.client URLProtocol:self didReceiveResponse:response cacheStoragePolicy:NSURLCacheStorageNotAllowed];
    if (status == 200) [self.client URLProtocol:self didLoadData:networkEnvelope];
    [self.client URLProtocolDidFinishLoading:self];
}
- (void)stopLoading {}
@end
static IMP originalConfiguration;
static NSURLSessionConfiguration *ProbeConfiguration(id object, SEL selector) {
    NSURLSessionConfiguration *configuration = ((id (*)(id, SEL))originalConfiguration)(object, selector);
    configuration.protocolClasses = @[PackProtocol.class];
    return configuration;
}
static void WaitForPending(NSString *root) {
    for (int attempt = 0; attempt < 200; attempt++) {
        NSDictionary *state = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:@"state.json"]] options:0 error:NULL];
        if ([state[@"pending"] length]) return;
        [NSThread sleepForTimeInterval:0.01];
    }
}
static NSString *NewRoot(void) {
    return [NSTemporaryDirectory() stringByAppendingPathComponent:[@"visr-pack-probe-" stringByAppendingString:NSUUID.UUID.UUIDString]];
}
static HostDataPackStore *Store(NSString *root) {
    return [[HostDataPackStore alloc] initWithRoot:root bundled:@"/bundled" kind:@"profiles" channel:@"stable" publicKey:[NSData dataWithBytes:publicKey length:32] compatibility:@"profiles-1" validator:^BOOL(NSString *folder, NSError **error) {
        (void)error;
        return [[NSFileManager defaultManager] fileExistsAtPath:[folder stringByAppendingPathComponent:@"test.toml"]];
    }];
}
static BOOL Rejected(NSData *data) {
    NSString *root = NewRoot();
    HostDataPackStore *fresh = Store(root);
    BOOL rejected = ![fresh stageEnvelope:data error:NULL];
    [[NSFileManager defaultManager] removeItemAtPath:root error:NULL];
    return rejected;
}
static BOOL FailsClosedPromptly(NSString *root) {
    NSTask *child = [NSTask new];
    child.executableURL = [NSURL fileURLWithPath:NSProcessInfo.processInfo.arguments[0]];
    child.arguments = @[@"--read-fifo", root];
    if (![child launchAndReturnError:NULL]) return NO;
    for (int attempt = 0; attempt < 200 && child.isRunning; attempt++) [NSThread sleepForTimeInterval:0.01];
    BOOL finished = !child.isRunning;
    if (!finished) [child terminate];
    [child waitUntilExit];
    return finished && child.terminationStatus == 0;
}
int main(int argc, const char **argv) { @autoreleasepool {
    uint8_t seed[32] = {1}; // Public test fixture, never a release signing key.
    crypto_ed25519_key_pair(secretKey, publicKey, seed);
    if (argc == 3 && strcmp(argv[1], "--read-fifo") == 0) {
        return [[Store([NSString stringWithUTF8String:argv[2]]) beginLaunch] isEqual:@"/bundled"] ? 0 : 2;
    }
    NSString *shaderRoot = NewRoot();
    HostDataPackStore *shaderStore = [[HostDataPackStore alloc] initWithRoot:shaderRoot bundled:@"/bundled" kind:@"shaders" channel:@"stable" publicKey:[NSData dataWithBytes:publicKey length:32] compatibility:@"shader-lists-1" validator:^BOOL(NSString *folder, NSError **error) {
        (void)error;
        return [NSFileManager.defaultManager fileExistsAtPath:[folder stringByAppendingPathComponent:@"a10.txt"]];
    }];
    CHECK([shaderStore stageEnvelope:Envelope(1, @{@"kind": @"shaders", @"compatibility": @"shader-lists-1"}, @{@"a10.txt": @"eA=="}) error:NULL], "signed shader additions stage through the shared store");
    CHECK([[shaderStore beginLaunch] hasPrefix:shaderRoot], "shader additions activate on a new launch");
    [NSFileManager.defaultManager removeItemAtPath:shaderRoot error:NULL];
    NSString *root = NewRoot();
    HostDataPackStore *store = Store(root);
    CHECK([[store beginLaunch] isEqual:@"/bundled"], "fresh launch uses bundled data");
    CHECK([store stageEnvelope:Envelope(1, nil, nil) error:NULL], "valid signed pack stages");
    CHECK([[store metadata][@"bundled"] boolValue], "staging does not replace active identity");
    CHECK([[store beginLaunch] isEqual:@"/bundled"], "staging does not change current launch");
    store = Store(root);
    NSString *first = [store beginLaunch];
    CHECK(![first isEqual:@"/bundled"], "next launch activates pending pack");
    CHECK([[store metadata][@"sequence"] integerValue] == 1, "active metadata identifies signed sequence");
    CHECK([[store metadata][@"compatibility"] isEqual:@"profiles-1"], "active metadata includes compatibility identity");
    [store markHealthy];
    CHECK(loggedHealthy, "accepted snapshots reach the runtime logger");
    CHECK([store stageEnvelope:Envelope(2, nil, nil) error:NULL], "newer pack stages");
    CHECK([[store beginLaunch] isEqual:first], "active folder stays fixed after staging");
    store = Store(root);
    CHECK(![[store beginLaunch] isEqual:first], "second generation activates");
    store = Store(root);
    CHECK([[store beginLaunch] isEqual:first], "unhealthy generation restores last healthy");
    CHECK(![store stageEnvelope:Envelope(2, nil, nil) error:NULL], "quarantined sequence cannot retry");
    CHECK(![store stageEnvelope:Envelope(1, nil, nil) error:NULL], "older sequence cannot replay");
    CHECK([store stageEnvelope:Envelope(3, nil, nil) error:NULL], "operator rollback uses new sequence with old content");
    store = Store(root); [store beginLaunch]; [store markHealthy];
    CHECK([[store metadata][@"sequence"] integerValue] == 3, "rollback identity uses new sequence");
    CHECK(Rejected(Envelope(4, @{@"sha256": [@"0" stringByPaddingToLength:64 withString:@"0" startingAtIndex:0]}, nil)), "payload hash mismatch rejected");
    CHECK(Rejected(Envelope(4, @{@"compatibility": @"other"}, nil)), "incompatible data rejected");
    CHECK(Rejected(Envelope(4, @{@"schema": @YES}, nil)), "boolean schema rejected");
    CHECK(Rejected(Envelope(4, @{@"sequence": @1.5}, nil)), "fractional sequence rejected");
    CHECK(Rejected(Envelope(4, @{@"extra": @1}, nil)), "unknown manifest keys rejected");
    CHECK(Rejected(Envelope(4, @{@"minimum_engine": @2}, nil)), "incompatible engine rejected");
    CHECK(Rejected(Envelope(4, nil, @{@"test.toml": @"eA==", @"../test.toml": @"eA=="})), "path traversal rejected");
    CHECK(Rejected(Envelope(4, nil, @{@"test.toml": @"eA==", @"device-profile.toml": @"eA=="})), "template profile rejected");
    CHECK(Rejected(Envelope(4, nil, @{@"test.toml": [[NSMutableData dataWithLength:65537] base64EncodedStringWithOptions:0]})), "oversized file rejected");
    CHECK(Rejected(JSON(@{@"payload": @"e30="})), "unsigned envelope rejected");
    NSMutableDictionary *tampered = [[NSJSONSerialization JSONObjectWithData:Envelope(4, nil, nil) options:0 error:NULL] mutableCopy];
    tampered[@"signature"] = [[NSMutableData dataWithLength:64] base64EncodedStringWithOptions:0];
    CHECK(Rejected(JSON(tampered)), "tampered signature rejected");
    CHECK(Rejected([NSMutableData dataWithLength:4 * 1024 * 1024 + 1]), "oversized envelope rejected before parsing");
    CHECK(Rejected(Envelope(4, @{@"notes": [@"x" stringByPaddingToLength:4097 withString:@"x" startingAtIndex:0]}, nil)), "oversized release notes rejected");
    CHECK(Rejected(Envelope(4, @{@"channel": @"tester"}, nil)), "cross-channel pack rejected");
    CHECK(Rejected(Envelope(4, @{@"kind": @"recordings"}, nil)), "cross-kind pack rejected");
    CHECK(Rejected(Envelope(4, nil, @{@"test.toml": @"eA==", @"test.toml\n": @"eA=="})), "trailing newline in file name rejected");
    NSString *longName = [[@"a" stringByPaddingToLength:59 withString:@"a" startingAtIndex:0] stringByAppendingString:@".toml"];
    CHECK(Rejected(Envelope(4, nil, @{@"test.toml": @"eA==", longName: @"eA=="})), "profile filenames are shorter than 64 bytes");
    NSMutableDictionary *manyFiles = [NSMutableDictionary dictionaryWithObject:@"eA==" forKey:@"test.toml"];
    for (int i = 0; i < 129; i++) manyFiles[[NSString stringWithFormat:@"file%d.toml", i]] = @"eA==";
    CHECK(Rejected(Envelope(4, nil, manyFiles)), "profile file count limit enforced");
    NSMutableDictionary *largeFiles = [NSMutableDictionary dictionaryWithObject:@"eA==" forKey:@"test.toml"];
    NSString *largeContent = [[NSMutableData dataWithLength:65536] base64EncodedStringWithOptions:0];
    for (int i = 0; i < 33; i++) largeFiles[[NSString stringWithFormat:@"file%d.toml", i]] = largeContent;
    CHECK(Rejected(Envelope(4, nil, largeFiles)), "profile aggregate decoded size limit enforced");
    NSData *duplicatePayload = [@"{\"files\":{\"test.toml\":\"eA==\",\"test.toml\":\"eA==\"}}" dataUsingEncoding:NSUTF8StringEncoding];
    NSDictionary *duplicateManifest = @{@"schema": @1, @"kind": @"profiles", @"channel": @"stable", @"sequence": @4, @"minimum_engine": @1, @"maximum_engine": @1, @"compatibility": @"profiles-1", @"sha256": Digest(duplicatePayload), @"notes": @""};
    CHECK(Rejected(Wrap(JSON(duplicateManifest), duplicatePayload)), "duplicate payload keys rejected");
    NSData *escapedPayload = JSON(@{@"files": @{@"test.toml": @"eA=="}});
    NSMutableDictionary *escapedManifest = [duplicateManifest mutableCopy];
    NSString *plainNotes = [@"x" stringByPaddingToLength:4096 withString:@"x" startingAtIndex:0];
    escapedManifest[@"notes"] = plainNotes; escapedManifest[@"sha256"] = Digest(escapedPayload);
    NSString *escapedNotes = [@"\\u0078" stringByPaddingToLength:4096 * 6 withString:@"\\u0078" startingAtIndex:0];
    NSString *escapedRaw = [[[NSString alloc] initWithData:JSON(escapedManifest) encoding:NSUTF8StringEncoding] stringByReplacingOccurrencesOfString:plainNotes withString:escapedNotes];
    CHECK(!Rejected(Wrap([escapedRaw dataUsingEncoding:NSUTF8StringEncoding], escapedPayload)), "escaped valid notes fit the manifest byte bound");
    NSString *active = [store beginLaunch];
    [@"corrupt" writeToFile:[active stringByAppendingPathComponent:@"test.toml"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    store = Store(root);
    CHECK([[store beginLaunch] isEqual:@"/bundled"], "corrupted cached files fall back to bundled");
    [[NSFileManager defaultManager] removeItemAtPath:root error:NULL];
    root = NewRoot();
    [[NSFileManager defaultManager] createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:NULL];
    [@"broken" writeToFile:[root stringByAppendingPathComponent:@"state.json"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    store = Store(root);
    CHECK([[store beginLaunch] isEqual:@"/bundled"], "malformed state falls back to bundled data");
    CHECK([store stageEnvelope:Envelope(1, nil, nil) error:NULL], "malformed state starts the store over instead of disabling updates");
    [[NSFileManager defaultManager] removeItemAtPath:root error:NULL];
    root = NewRoot(); store = Store(root);
    CHECK([store stageEnvelope:Envelope(10, nil, nil) error:NULL], "orphan test stages generation");
    [[NSFileManager defaultManager] removeItemAtPath:[root stringByAppendingPathComponent:@"state.json"] error:NULL];
    store = Store(root);
    CHECK([[store beginLaunch] isEqual:@"/bundled"], "missing cache state does not trust orphan generation");
    CHECK([[[NSFileManager defaultManager] contentsOfDirectoryAtPath:root error:NULL] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"SELF BEGINSWITH '10-'"]].count == 0,
          "missing cache state removes orphan generations");
    CHECK([store stageEnvelope:Envelope(1, nil, nil) error:NULL], "missing cache state starts the store over, as a fresh install");
    [[NSFileManager defaultManager] removeItemAtPath:root error:NULL];
    root = NewRoot();
    HostDataPackStore *rejecting = [[HostDataPackStore alloc] initWithRoot:root bundled:@"/bundled" kind:@"profiles" channel:@"stable" publicKey:[NSData dataWithBytes:publicKey length:32] compatibility:@"profiles-1" validator:^BOOL(NSString *folder, NSError **error) { (void)folder; (void)error; return NO; }];
    CHECK(![rejecting stageEnvelope:Envelope(1, nil, nil) error:NULL], "snapshot validator rejects otherwise signed data");
    CHECK([[rejecting beginLaunch] isEqual:@"/bundled"], "failed snapshot does not activate");
    store = Store(root);
    CHECK(![store stageEnvelope:Envelope(1, nil, nil) error:NULL], "semantic failure quarantines authenticated sequence");
    CHECK([store stageEnvelope:Envelope(2, nil, nil) error:NULL], "higher sequence recovers from semantic failure");
    {
        HostDataPackStore *same = [[HostDataPackStore alloc] initWithRoot:root bundled:@"/bundled" kind:@"profiles" channel:@"stable" publicKey:[NSData dataWithBytes:publicKey length:32] compatibility:@"profiles-1" validator:^BOOL(NSString *folder, NSError **error) { (void)folder; (void)error; return NO; }];
        CHECK(![same stageEnvelope:Envelope(3, nil, nil) error:NULL], "a broken later publish is refused");
        CHECK(![Store(root) stageEnvelope:Envelope(3, nil, nil) error:NULL], "a broken later publish uses up its sequence");
        CHECK(![[Store(root) beginLaunch] isEqual:@"/bundled"], "a broken later publish keeps the valid pack already staged");
    }
    [[NSFileManager defaultManager] removeItemAtPath:root error:NULL];
    root = NewRoot();
    store = Store(root);
    CHECK([store stageEnvelope:Envelope(1, nil, nil) error:NULL], "symlink test stages data");
    NSString *symlinkFolder = [store beginLaunch]; [store markHealthy];
    NSString *symlinkFile = [symlinkFolder stringByAppendingPathComponent:@"test.toml"];
    [[NSFileManager defaultManager] removeItemAtPath:symlinkFile error:NULL];
    [[NSFileManager defaultManager] createSymbolicLinkAtPath:symlinkFile withDestinationPath:@"/dev/null" error:NULL];
    CHECK([[Store(root) beginLaunch] isEqual:@"/bundled"], "cached symlink is rejected");
    [[NSFileManager defaultManager] removeItemAtPath:root error:NULL];
    root = NewRoot();
    NSString *replayCompatibility = [@"benchmark-replay-1:" stringByAppendingString:[@"a" stringByPaddingToLength:64 withString:@"a" startingAtIndex:0]];
    HostDataPackStore *recordings = [[HostDataPackStore alloc] initWithRoot:root bundled:@"/bundled-recordings" kind:@"recordings" channel:@"tester" publicKey:[NSData dataWithBytes:publicKey length:32] compatibility:replayCompatibility validator:^BOOL(NSString *folder, NSError **error) { (void)error; return [[NSFileManager defaultManager] fileExistsAtPath:[folder stringByAppendingPathComponent:@"test.input"]]; }];
    NSDictionary *recordingManifest = @{@"kind": @"recordings", @"channel": @"tester", @"compatibility": replayCompatibility};
    CHECK(![recordings stageEnvelope:Envelope(1, recordingManifest, @{@"test.input": @"eA==", @"test.toml": @"eA=="}) error:NULL], "recording extension allowlist enforced");
    CHECK(![recordings stageEnvelope:Envelope(2, recordingManifest, @{@"test.input": @"eA==", @"path/test.input": @"eA=="}) error:NULL], "recording subdirectories rejected");
    NSDictionary *wrongGuest = @{@"kind": @"recordings", @"channel": @"tester", @"compatibility": [@"benchmark-replay-1:" stringByAppendingString:[@"b" stringByPaddingToLength:64 withString:@"b" startingAtIndex:0]]};
    CHECK(![recordings stageEnvelope:Envelope(100, wrongGuest, @{@"test.input": @"eA=="}) error:NULL], "recording guest hash mismatch rejected");
    CHECK([recordings stageEnvelope:Envelope(3, recordingManifest, @{@"test.input": @"eA==", @"test.actions": @"eA==", @"test.seeds": @"eA==", @"test.route": @"eA==", @"test.checks": @"eA=="}) error:NULL], "signed recording pack stages");
    CHECK(![[recordings beginLaunch] isEqual:@"/bundled-recordings"], "recording pack activates independently");
    [recordings markHealthy];
    [[NSFileManager defaultManager] removeItemAtPath:root error:NULL];
    root = NewRoot();
    [[NSFileManager defaultManager] createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:NULL];
    CHECK(mkfifo([root stringByAppendingPathComponent:@"state.json"].fileSystemRepresentation, 0600) == 0, "state FIFO fixture created");
    CHECK(FailsClosedPromptly(root), "state FIFO fails closed without blocking");
    [[NSFileManager defaultManager] removeItemAtPath:root error:NULL];
    root = NewRoot(); store = Store(root);
    CHECK([store stageEnvelope:Envelope(1, nil, nil) error:NULL], "envelope FIFO fixture stages");
    NSString *fifoFolder = [store beginLaunch]; [store markHealthy];
    NSString *fifoEnvelope = [fifoFolder.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"envelope.json"];
    [[NSFileManager defaultManager] removeItemAtPath:fifoEnvelope error:NULL];
    CHECK(mkfifo(fifoEnvelope.fileSystemRepresentation, 0600) == 0, "envelope FIFO fixture created");
    CHECK(FailsClosedPromptly(root), "envelope FIFO fails closed without blocking");
    [[NSFileManager defaultManager] removeItemAtPath:root error:NULL];
    // tvOS keeps the store in Caches, which the system purges file by file between launches. Every partial purge must
    // leave a store that downloads the feed's current sequence again instead of refusing it as "already seen".
    @autoreleasepool {
        NSFileManager *manager = NSFileManager.defaultManager;
        // 1. The staged (pending) generation is purged before the next launch.
        root = NewRoot(); store = Store(root);
        CHECK([store stageEnvelope:Envelope(1, nil, nil) error:NULL], "purge 1: the pack stages");
        NSString *pendingName = [[manager contentsOfDirectoryAtPath:root error:NULL] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"SELF BEGINSWITH '1-'"]].firstObject;
        [manager removeItemAtPath:[root stringByAppendingPathComponent:pendingName] error:NULL];
        loggedDropped = NO;
        store = Store(root);
        CHECK([[store beginLaunch] isEqual:@"/bundled"], "purge 1: a purged pending pack falls back to bundled");
        CHECK(loggedDropped, "purge 1: the dropped snapshot is logged with its reason");
        NSError *stageError = nil;
        CHECK([store stageEnvelope:Envelope(1, nil, nil) error:&stageError], "purge 1: the same sequence downloads again after its files were purged");
        store = Store(root);
        CHECK(![[store beginLaunch] isEqual:@"/bundled"], "purge 1: the re-downloaded pack activates");
        // 2. The healthy, active generation's folder is purged after an update.
        [store markHealthy];
        NSString *activeFolder = [store beginLaunch].stringByDeletingLastPathComponent;
        [manager removeItemAtPath:activeFolder error:NULL];
        store = Store(root);
        CHECK([[store beginLaunch] isEqual:@"/bundled"], "purge 2: a purged active pack falls back to bundled");
        CHECK([store stageEnvelope:Envelope(1, nil, nil) error:&stageError], "purge 2: the purged sequence stages again");
        // 3. Only the envelope is purged, then only the files folder.
        for (NSString *piece in @[@"envelope.json", @"files"]) {
            store = Store(root); [store beginLaunch]; [store markHealthy];
            NSString *folder = [store beginLaunch].stringByDeletingLastPathComponent;
            CHECK(![folder isEqual:@"/bundled"], "purge 3: the pack is active before the purge");
            [manager removeItemAtPath:[folder stringByAppendingPathComponent:piece] error:NULL];
            store = Store(root);
            CHECK([[store beginLaunch] isEqual:@"/bundled"], "purge 3: a pack missing its envelope or files falls back to bundled");
            CHECK([store stageEnvelope:Envelope(1, nil, nil) error:&stageError], "purge 3: a partly purged pack's sequence stages again");
        }
        // 4. A newer pack was staged, then purged, while an older healthy pack survives: only the newer one is forgotten.
        store = Store(root); [store beginLaunch]; [store markHealthy];
        CHECK([store stageEnvelope:Envelope(2, nil, nil) error:NULL], "purge 4: a newer pack stages");
        for (NSString *name in [manager contentsOfDirectoryAtPath:root error:NULL])
            if ([name hasPrefix:@"2-"]) [manager removeItemAtPath:[root stringByAppendingPathComponent:name] error:NULL];
        store = Store(root);
        CHECK(![[store beginLaunch] isEqual:@"/bundled"], "purge 4: the surviving healthy pack stays in use");
        CHECK([store stageEnvelope:Envelope(2, nil, nil) error:&stageError], "purge 4: the purged newer sequence stages again");
        CHECK(![store stageEnvelope:Envelope(1, nil, nil) error:NULL], "purge 4: the surviving sequence is still not replayed");
        // 5. The whole folder disappears while the app is running: writes recreate it.
        [manager removeItemAtPath:root error:NULL];
        CHECK([store stageEnvelope:Envelope(3, nil, nil) error:&stageError], "purge 5: staging recreates a purged store folder");
        CHECK([manager fileExistsAtPath:[root stringByAppendingPathComponent:@"state.json"]], "purge 5: the state file is written again");
        [manager removeItemAtPath:root error:NULL];
        // 6. A file sits where the store's folder belongs.
        [manager createDirectoryAtPath:root.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:NULL];
        [@"x" writeToFile:root atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        store = Store(root);
        CHECK([store stageEnvelope:Envelope(1, nil, nil) error:&stageError], "purge 6: a file in the folder's place is replaced");
        [manager removeItemAtPath:root error:NULL];
        // 7. State written by 0.6.1 (schema 1) that names nothing but quarantined the purged sequence is started over.
        root = NewRoot(); store = Store(root);
        CHECK([store stageEnvelope:Envelope(5, nil, nil) error:NULL], "purge 7: a pack stages");
        NSMutableDictionary *old = [[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:@"state.json"]] options:0 error:NULL] mutableCopy];
        CHECK([old[@"schema"] intValue] == 2, "purge 7: state carries schema 2");
        old[@"schema"] = @1; old[@"pending"] = @""; old[@"quarantined"] = @5;
        [JSON(old) writeToFile:[root stringByAppendingPathComponent:@"state.json"] atomically:YES];
        loggedStateMissing = NO;
        store = Store(root);
        CHECK([store stageEnvelope:Envelope(5, nil, nil) error:&stageError], "purge 7: the old stuck state no longer refuses the feed's sequence");
        [manager removeItemAtPath:root error:NULL];
        // 8. The state file is purged but the generations stay: the store starts over and says why.
        root = NewRoot(); store = Store(root);
        [store stageEnvelope:Envelope(4, nil, nil) error:NULL];
        [manager removeItemAtPath:[root stringByAppendingPathComponent:@"state.json"] error:NULL];
        loggedStateMissing = NO;
        store = Store(root);
        CHECK(loggedStateMissing, "purge 8: a missing state file is logged as the reason");
        CHECK([store stageEnvelope:Envelope(4, nil, nil) error:&stageError], "purge 8: the same sequence stages after the state was purged");
        [manager removeItemAtPath:root error:NULL];
    }
    Method method = class_getClassMethod(NSURLSessionConfiguration.class, @selector(ephemeralSessionConfiguration));
    originalConfiguration = method_setImplementation(method, (IMP)ProbeConfiguration);
    root = NewRoot(); store = Store(root); [store beginLaunch];
    networkEnvelope = Envelope(1, nil, nil);
    [store checkForUpdateAtURL:[NSURL URLWithString:@"http://packs.invalid/profiles.json"]];
    [store checkForUpdateAtURL:[NSURL URLWithString:@"https://packs.invalid/profiles.json?id=device"]];
    [store checkForUpdateAtURL:[NSURL URLWithString:@"https://name:secret@packs.invalid/profiles.json"]];
    CHECK(requests == 0, "unsafe update URLs do not start requests");
    dispatch_semaphore_t checked = dispatch_semaphore_create(0);
    __block BOOL checkSucceeded = NO;
    [store checkForUpdateAtURL:[NSURL URLWithString:@"https://packs.invalid/profiles.json"] completion:^(BOOL success) {
        checkSucceeded = success; dispatch_semaphore_signal(checked);
    }];
    WaitForPending(root);
    for (int attempt = 0; attempt < 200 && ![[NSFileManager defaultManager] fileExistsAtPath:[root stringByAppendingPathComponent:@"feed-validator.txt"]]; attempt++) [NSThread sleepForTimeInterval:0.01];
    CHECK(requests == 1, "HTTPS update is fetched once");
    CHECK(dispatch_semaphore_wait(checked, dispatch_time(DISPATCH_TIME_NOW, 2*NSEC_PER_SEC)) == 0 && checkSucceeded,
        "successful staging completes the readiness check");
    CHECK([[store beginLaunch] isEqual:@"/bundled"], "network update does not hot swap");
    store = Store(root);
    CHECK(![[store beginLaunch] isEqual:@"/bundled"], "download activates on next launch");
    [store markHealthy];
    checkSucceeded = NO;
    [store checkForUpdateAtURL:[NSURL URLWithString:@"https://packs.invalid/profiles.json"] completion:^(BOOL success) {
        checkSucceeded = success; dispatch_semaphore_signal(checked);
    }];
    for (int attempt = 0; attempt < 200 && requests < 2; attempt++) [NSThread sleepForTimeInterval:0.01];
    CHECK(requests == 2, "every launch checks the feed again");
    for (int attempt = 0; attempt < 200 && !notModified; attempt++) [NSThread sleepForTimeInterval:0.01];
    CHECK(notModified == 1, "an unchanged feed answers the saved validator with 304");
    CHECK(dispatch_semaphore_wait(checked, dispatch_time(DISPATCH_TIME_NOW, 2*NSEC_PER_SEC)) == 0 && checkSucceeded,
        "a valid cached 304 completes successfully despite response cancellation");
    CHECK([[store beginLaunch] isEqual:[Store(root) beginLaunch]], "a 304 leaves the snapshot in use");
    [NSThread sleepForTimeInterval:0.3];
    [[NSFileManager defaultManager] removeItemAtPath:[root stringByAppendingPathComponent:@"feed-validator.txt"] error:NULL];
    networkStatus = 500;
    [store checkForUpdateAtURL:[NSURL URLWithString:@"https://packs.invalid/profiles.json"]];
    for (int attempt = 0; attempt < 200 && requests < 3; attempt++) [NSThread sleepForTimeInterval:0.01];
    [NSThread sleepForTimeInterval:0.3];
    networkFails = YES;
    [store checkForUpdateAtURL:[NSURL URLWithString:@"https://packs.invalid/profiles.json"]];
    for (int attempt = 0; attempt < 200 && requests < 4; attempt++) [NSThread sleepForTimeInterval:0.01];
    [NSThread sleepForTimeInterval:0.3];
    if (requests != 4) fprintf(stderr, "requests = %ld\n", (long)requests);
    CHECK(requests == 4, "an HTTP error and a network failure each end one check without retrying");
    CHECK(loggedHTTPFailure && loggedNetworkFailure, "feed failures reach the runtime logger");
    store = Store(root);
    CHECK(![[store beginLaunch] isEqual:@"/bundled"], "failed checks leave the current snapshot in use");
    // A purged snapshot must not be answered with a 304: the saved validator is useless without the files it described.
    {
        networkFails = NO; networkStatus = 200;
        NSString *purgedRoot = NewRoot();
        HostDataPackStore *purged = Store(purgedRoot);
        [purged beginLaunch];
        networkEnvelope = Envelope(1, nil, nil);
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [purged checkForUpdateAtURL:[NSURL URLWithString:@"https://packs.invalid/profiles.json"] completion:^(BOOL success) { (void)success; dispatch_semaphore_signal(done); }];
        CHECK(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 2*NSEC_PER_SEC)) == 0, "purged 304: the first check completes");
        purged = Store(purgedRoot); [purged beginLaunch]; [purged markHealthy];
        // The system purges every generation folder but leaves state.json and feed-validator.txt.
        for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:purgedRoot error:NULL])
            if ([name hasPrefix:@"1-"]) [NSFileManager.defaultManager removeItemAtPath:[purgedRoot stringByAppendingPathComponent:name] error:NULL];
        purged = Store(purgedRoot);
        NSInteger notModifiedBefore = notModified;
        __block BOOL refetched = NO;
        [purged checkForUpdateAtURL:[NSURL URLWithString:@"https://packs.invalid/profiles.json"] completion:^(BOOL success) { refetched = success; dispatch_semaphore_signal(done); }];
        CHECK(dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 2*NSEC_PER_SEC)) == 0 && refetched, "purged 304: the check downloads the pack again");
        CHECK(notModified == notModifiedBefore, "purged 304: no conditional request is sent for a snapshot that is gone");
        [NSThread sleepForTimeInterval:0.3];
        purged = Store(purgedRoot);
        CHECK(![[purged beginLaunch] isEqual:@"/bundled"], "purged 304: the re-downloaded pack activates");
        [NSFileManager.defaultManager removeItemAtPath:purgedRoot error:NULL];
    }
    // A valid recording library may exceed both former aggregate/download caps.
    @autoreleasepool {
        NSString *recordingRoot = NewRoot();
        NSString *compatibility = @"benchmark-replay-1:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        NSString *encoded = [[NSMutableData dataWithLength:3600000] base64EncodedStringWithOptions:0];
        NSMutableDictionary *recordings = [NSMutableDictionary dictionary];
        for (int index = 0; index < 4; index++)
            recordings[[NSString stringWithFormat:@"large-%d.input", index]] = encoded;
        networkEnvelope = Envelope(1, @{@"kind": @"recordings", @"compatibility": compatibility}, recordings);
        CHECK(networkEnvelope.length > 24 * 1024 * 1024, "large fixture exceeds former download cap");
        networkFails = NO; networkStatus = 200;
        HostDataPackStore *largeStore = [[HostDataPackStore alloc] initWithRoot:recordingRoot bundled:@"/bundled"
            kind:@"recordings" channel:@"stable" publicKey:[NSData dataWithBytes:publicKey length:32]
            compatibility:compatibility validator:^BOOL(NSString *folder, NSError **error) {
                (void)error;
                return [[NSFileManager defaultManager] fileExistsAtPath:[folder stringByAppendingPathComponent:@"large-3.input"]];
            }];
        [largeStore checkForUpdateAtURL:[NSURL URLWithString:@"https://packs.invalid/recordings.json"]];
        for (int attempt = 0; attempt < 1500; attempt++) {
            NSDictionary *state = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:
                [recordingRoot stringByAppendingPathComponent:@"state.json"]] options:0 error:NULL];
            if ([state[@"pending"] length]) break;
            [NSThread sleepForTimeInterval:0.01];
        }
        CHECK(largeStore.resourceTimeout >= 300, "a recording download gets minutes, not the 30 s of the small feeds");
        CHECK([HostDataPackStore resourceTimeoutForKind:@"profiles"] == 30 && [HostDataPackStore resourceTimeoutForKind:@"latest"] == 30,
            "small feeds keep the short resource limit");
        for (int attempt = 0; attempt < 500 && (largeStore.downloadReceived || largeStore.downloadExpected); attempt++) [NSThread sleepForTimeInterval:0.01];
        CHECK(!largeStore.downloadReceived && !largeStore.downloadExpected, "download progress resets when the check ends");
        NSString *generation = [largeStore beginLaunch];
        CHECK(![generation isEqual:@"/bundled"], "large recording pack downloads, verifies and activates");
        CHECK([NSData dataWithContentsOfFile:[generation stringByAppendingPathComponent:@"large-3.input"]].length == 3600000,
            "large recording file survives activation");
        [largeStore markHealthy];
        // Keep the last good catalog: a later download that times out leaves it selected and staged.
        networkFails = YES;
        NSInteger before = requests;
        [largeStore checkForUpdateAtURL:[NSURL URLWithString:@"https://packs.invalid/recordings.json"]];
        for (int attempt = 0; attempt < 300 && requests == before; attempt++) [NSThread sleepForTimeInterval:0.01];
        [NSThread sleepForTimeInterval:0.3];
        HostDataPackStore *again = [[HostDataPackStore alloc] initWithRoot:recordingRoot bundled:@"/bundled"
            kind:@"recordings" channel:@"stable" publicKey:[NSData dataWithBytes:publicKey length:32]
            compatibility:compatibility validator:^BOOL(NSString *folder, NSError **error) {
                (void)error;
                return [[NSFileManager defaultManager] fileExistsAtPath:[folder stringByAppendingPathComponent:@"large-3.input"]];
            }];
        CHECK([[again beginLaunch] isEqual:generation], "a timed-out recording download keeps the last good catalog in use");
        networkFails = NO;
        [[NSFileManager defaultManager] removeItemAtPath:recordingRoot error:NULL];
        networkEnvelope = nil;
    }
    method_setImplementation(method, originalConfiguration);
    [[NSFileManager defaultManager] removeItemAtPath:root error:NULL];
    fprintf(stdout, "data_pack_probe: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
} }
