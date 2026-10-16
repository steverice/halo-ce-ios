#import "host_data_pack.h"
#import <CommonCrypto/CommonDigest.h>
#include "../../third_party/monocypher/monocypher-ed25519.h"
#include "host_recording_pack.h"
#include "host.h"
#include <sys/stat.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

static const uint64_t PackMaxSequence = 9007199254740991ULL;
static BOOL PackError(NSError **error, NSString *message) {
    if (error) *error = [NSError errorWithDomain:@"VISRDataPack" code:1 userInfo:@{NSLocalizedDescriptionKey: message}];
    return NO;
}
static BOOL PackKeys(id value, NSArray *keys) {
    return [value isKindOfClass:NSDictionary.class] &&
        [[NSSet setWithArray:[value allKeys]] isEqualToSet:[NSSet setWithArray:keys]];
}
static BOOL PackInteger(id value, uint64_t minimum, uint64_t maximum) {
    if (![value isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID()) return NO;
    if (CFNumberIsFloatType((__bridge CFNumberRef)value)) return NO;
    double number = [value doubleValue];
    return isfinite(number) && number >= minimum && number <= maximum;
}
static BOOL PackMatches(NSString *value, NSString *pattern) {
    return [value isKindOfClass:NSString.class] &&
        [value rangeOfString:pattern options:NSRegularExpressionSearch].location != NSNotFound;
}
static NSString *PackSHA256(NSData *data) {
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
    NSMutableString *hex = [NSMutableString stringWithCapacity:64];
    for (NSUInteger i = 0; i < sizeof(digest); i++) [hex appendFormat:@"%02x", digest[i]];
    return hex;
}
static NSData *PackBase64(id value, NSUInteger maxBytes) {
    if (![value isKindOfClass:NSString.class] || [value length] > ((maxBytes + 2) / 3) * 4) return nil;
    NSData *data = [[NSData alloc] initWithBase64EncodedString:value options:0];
    // Canonical encoding also rejects ignored characters and malformed padding.
    return data.length <= maxBytes && [[data base64EncodedStringWithOptions:0] isEqual:value] ? data : nil;
}
static BOOL PackNode(NSString *path, BOOL directory) {
    struct stat info;
    return lstat(path.fileSystemRepresentation, &info) == 0 && (directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode));
}
static NSData *PackRead(NSString *path, NSUInteger limit) {
    int descriptor = open(path.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) return nil;
    struct stat info;
    if (fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 || (uint64_t)info.st_size > limit) { close(descriptor); return nil; }
    NSMutableData *data = [NSMutableData dataWithCapacity:(NSUInteger)info.st_size];
    uint8_t buffer[65536];
    for (;;) {
        // Read one byte beyond a full buffer to detect a file growing after fstat.
        NSUInteger remaining = limit - data.length;
        ssize_t count = read(descriptor, buffer, MIN(sizeof(buffer), remaining ? remaining : 1));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || (NSUInteger)count > remaining) { close(descriptor); return nil; }
        if (!count) break;
        [data appendBytes:buffer length:(NSUInteger)count];
    }
    close(descriptor);
    return data;
}
// Foundation accepts duplicate object keys. Reject them before interpreting signed
// JSON, including escaped spellings of the same key, so every reader agrees.
static BOOL PackUniqueKeys(NSData *data) {
    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    if (!text) return NO;
    NSMutableArray *stack = [NSMutableArray array];
    for (NSUInteger i = 0; i < text.length; i++) {
        unichar c = [text characterAtIndex:i];
        if (c == '{') {
            if (stack.count >= 2) return NO;
            [stack addObject:[@{@"keys": [NSMutableSet set], @"key": @YES} mutableCopy]];
        } else if (c == '[') return NO; // None of the pack or state schemas permit arrays.
        else if (c == '}' || c == ']') { if (!stack.count) return NO; [stack removeLastObject]; }
        else if (c == ',' || c == ':') {
            if ([stack.lastObject isKindOfClass:NSMutableDictionary.class]) stack.lastObject[@"key"] = @(c == ',');
        } else if (c == '"') {
            NSUInteger start = i++;
            for (; i < text.length; i++) {
                unichar quoted = [text characterAtIndex:i];
                if (quoted == '\\') i++;
                else if (quoted == '"') break;
            }
            if (i >= text.length) return NO;
            if ([stack.lastObject isKindOfClass:NSMutableDictionary.class] && [stack.lastObject[@"key"] boolValue]) {
                if (i - start > 1024) return NO;
                NSData *token = [[text substringWithRange:NSMakeRange(start, i - start + 1)] dataUsingEncoding:NSUTF8StringEncoding];
                NSString *key = [NSJSONSerialization JSONObjectWithData:token options:NSJSONReadingFragmentsAllowed error:NULL];
                NSMutableSet *keys = stack.lastObject[@"keys"];
                if (![key isKindOfClass:NSString.class] || [keys containsObject:key] || keys.count >= 160) return NO;
                [keys addObject:key];
            }
        }
    }
    return stack.count == 0;
}
static NSDictionary *PackJSON(NSData *data) {
    if (!data || !PackUniqueKeys(data)) return nil;
    id value = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
    return [value isKindOfClass:NSDictionary.class] ? value : nil;
}

@interface HostDataPackStore () <NSURLSessionDataDelegate>
@property(nonatomic, copy) NSString *root, *bundled, *kind, *channel, *compatibility;
@property(nonatomic, copy) NSData *publicKey;
@property(nonatomic, copy) HostPackValidator validator;
@property(nonatomic) NSMutableDictionary *state;
@property(nonatomic) BOOL stateValid, launchStarted;
@property(nonatomic, copy) NSString *launchFolder;
@property(nonatomic, copy) NSDictionary *identity;
@property(nonatomic) NSURLSession *session;
@property(nonatomic) NSMutableData *download;
@property(nonatomic) BOOL responseAccepted;
@property(nonatomic, copy) NSString *responseValidator;
@end

@implementation HostDataPackStore
// The envelope is parsed before its signature is checked, so these bounds are also what an unauthenticated feed
// can make the app hold. The payload's files are base64 inside the payload, which is base64 again in the envelope:
// 12 MiB of recordings come to about 21.4 MiB. tools/data_pack.py refuses an envelope above these.
- (NSUInteger)envelopeLimit { return [_kind isEqual:@"profiles"] ? 4 * 1024 * 1024 : 24 * 1024 * 1024; }
- (NSDictionary *)bundledIdentity { return @{@"kind": _kind, @"channel": _channel, @"compatibility": _compatibility, @"sequence": @0, @"sha256": @"", @"bundled": @YES}; }
- (NSString *)statePath { return [_root stringByAppendingPathComponent:@"state.json"]; }
- (NSString *)generationPath:(NSString *)name { return [_root stringByAppendingPathComponent:name]; }
- (BOOL)generationName:(id)value {
    return [value isKindOfClass:NSString.class] && (![value length] || PackMatches(value, @"^[1-9][0-9]{0,15}-[0-9a-f]{64}\\z"));
}
- (instancetype)initWithRoot:(NSString *)root bundled:(NSString *)bundled kind:(NSString *)kind channel:(NSString *)channel publicKey:(NSData *)key compatibility:(NSString *)compatibility validator:(HostPackValidator)validator {
    if (!(self = [super init])) return nil;
    _root = [root copy]; _bundled = [bundled copy]; _kind = [kind copy]; _channel = [channel copy];
    _publicKey = [key copy]; _compatibility = [compatibility copy]; _validator = [validator copy];
    _identity = [self bundledIdentity]; _launchFolder = _bundled;
    if (!([kind isEqual:@"profiles"] || [kind isEqual:@"recordings"]) ||
        !([channel isEqual:@"stable"] || [channel isEqual:@"tester"]) || key.length != 32 || !validator || ![root isAbsolutePath]) return self;
    NSFileManager *manager = NSFileManager.defaultManager;
    if (![manager fileExistsAtPath:_root] && ![manager createDirectoryAtPath:_root withIntermediateDirectories:YES attributes:nil error:NULL]) return self;
    if (!PackNode(_root, YES)) return self;
    struct stat info;
    if (lstat(self.statePath.fileSystemRepresentation, &info) != 0 && errno != ENOENT) return self;
    if (![self loadState]) {
        // Missing or unusable state (the system may purge Caches file by file) starts the store over, as on a fresh
        // install: every cached generation goes, so none is activated without the state that admitted it.
        for (NSString *name in [manager contentsOfDirectoryAtPath:_root error:NULL]) {
            if (([self generationName:name] && name.length) || [name hasPrefix:@".stage-"] || [name isEqual:@"state.json"] || [name isEqual:@"feed-validator.txt"]) {
                if (![manager removeItemAtPath:[self generationPath:name] error:NULL]) return self;
            }
        }
        _state = [@{@"schema": @1, @"kind": kind, @"channel": channel, @"compatibility": compatibility, @"highest": @0, @"quarantined": @0, @"active": @"", @"healthy": @"", @"pending": @"", @"trial": @NO, @"last_check": @0} mutableCopy];
        _stateValid = YES;
    }
    return self;
}
- (BOOL)loadState {
    NSString *kind = _kind, *channel = _channel, *compatibility = _compatibility;
    {
        NSDictionary *state = PackJSON(PackRead(self.statePath, 8192));
        NSArray *keys = @[@"schema", @"kind", @"channel", @"compatibility", @"highest", @"quarantined", @"active", @"healthy", @"pending", @"trial", @"last_check"];
        if (!PackKeys(state, keys) || !PackInteger(state[@"schema"], 1, 1) || ![state[@"kind"] isEqual:kind] || ![state[@"channel"] isEqual:channel] || ![state[@"compatibility"] isEqual:compatibility] || !PackInteger(state[@"highest"], 0, PackMaxSequence) || !PackInteger(state[@"quarantined"], 0, [state[@"highest"] unsignedLongLongValue]) || ![self generationName:state[@"active"]] || ![self generationName:state[@"healthy"]] || ![self generationName:state[@"pending"]] || ![state[@"trial"] isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)state[@"trial"]) != CFBooleanGetTypeID() || ![state[@"last_check"] isKindOfClass:NSNumber.class] || !isfinite([state[@"last_check"] doubleValue]) || [state[@"last_check"] doubleValue] < 0) return NO;
        for (NSString *field in @[@"active", @"healthy", @"pending"]) {
            NSString *name = state[field];
            if (name.length && ([[name componentsSeparatedByString:@"-"][0] longLongValue] > [state[@"highest"] longLongValue] || [[name componentsSeparatedByString:@"-"][0] longLongValue] <= 0)) return NO;
        }
        if ([state[@"trial"] boolValue]) {
            if (![state[@"active"] length] || [state[@"active"] isEqual:state[@"healthy"]]) return NO;
        } else if (![state[@"active"] isEqual:state[@"healthy"]]) return NO;
        if ([state[@"pending"] length] && ([state[@"pending"] isEqual:state[@"active"]] || [state[@"pending"] isEqual:state[@"healthy"]])) return NO;
        _state = [state mutableCopy]; _stateValid = YES;
    }
    return YES;
}
- (BOOL)writeState:(NSDictionary *)state error:(NSError **)error {
    if (!PackNode(_root, YES)) return PackError(error, @"Pack root is unavailable");
    NSData *bytes = [NSJSONSerialization dataWithJSONObject:state options:0 error:error];
    // A failed atomic write leaves the previous file, which still matches _state.
    if (!bytes || ![bytes writeToFile:self.statePath options:NSDataWritingAtomic error:error]) return NO;
    _state = [state mutableCopy];
    return YES;
}
- (NSDictionary *)verifyEnvelope:(NSData *)data authenticated:(NSDictionary **)authenticated error:(NSError **)error {
    if (![data isKindOfClass:NSData.class] || data.length > self.envelopeLimit) { PackError(error, @"Pack envelope exceeds its size limit"); return nil; }
    NSDictionary *envelope = PackJSON(data);
    if (!PackKeys(envelope, @[@"manifest", @"signature", @"payload"])) { PackError(error, @"Pack envelope has invalid fields"); return nil; }
    NSData *manifestBytes = PackBase64(envelope[@"manifest"], 65536);
    NSData *signature = PackBase64(envelope[@"signature"], 64);
    if (!manifestBytes || signature.length != 64 || _publicKey.length != 32 || crypto_ed25519_check(signature.bytes, _publicKey.bytes, manifestBytes.bytes, manifestBytes.length) != 0) { PackError(error, @"Pack signature is invalid"); return nil; }
    NSDictionary *manifest = PackJSON(manifestBytes);
    NSArray *manifestKeys = @[@"schema", @"kind", @"channel", @"sequence", @"minimum_engine", @"maximum_engine", @"compatibility", @"sha256", @"notes"];
    if (!PackKeys(manifest, manifestKeys) || !PackInteger(manifest[@"schema"], 1, 1) || !PackInteger(manifest[@"sequence"], 1, PackMaxSequence) || !PackInteger(manifest[@"minimum_engine"], 1, 1) || !PackInteger(manifest[@"maximum_engine"], 1, 1) || ![manifest[@"kind"] isEqual:_kind] || ![manifest[@"channel"] isEqual:_channel] || ![manifest[@"compatibility"] isEqual:_compatibility] || !PackMatches(manifest[@"sha256"], @"^[0-9a-f]{64}\\z") || ![manifest[@"notes"] isKindOfClass:NSString.class] || [manifest[@"notes"] length] > 4096) { PackError(error, @"Pack manifest is incompatible or malformed"); return nil; }
    NSData *payloadBytes = PackBase64(envelope[@"payload"], self.envelopeLimit);
    if (!payloadBytes || ![PackSHA256(payloadBytes) isEqual:manifest[@"sha256"]]) { PackError(error, @"Pack payload hash is invalid"); return nil; }
    if (authenticated) *authenticated = manifest;
    NSDictionary *payload = PackJSON(payloadBytes);
    if (!PackKeys(payload, @[@"files"]) || ![payload[@"files"] isKindOfClass:NSDictionary.class]) { PackError(error, @"Pack payload has invalid fields"); return nil; }
    NSDictionary *encoded = payload[@"files"];
    BOOL profiles = [_kind isEqual:@"profiles"];
    NSUInteger fileLimit = profiles ? 64 * 1024 : HOST_RECORDING_PACK_MAX_FILE_BYTES;
    NSUInteger totalLimit = profiles ? 2 * 1024 * 1024 : HOST_RECORDING_PACK_MAX_TOTAL_BYTES;
    if (!encoded.count || encoded.count > (profiles ? 129 : 160)) { PackError(error, @"Pack file count exceeds its limit"); return nil; }
    NSMutableDictionary *files = [NSMutableDictionary dictionary];
    NSUInteger total = 0;
    for (NSString *name in encoded) {
        BOOL allowed = profiles ? ([name isEqual:@"hardware/catalog.toml"] || (name.length < 64 && PackMatches(name, @"^[a-z0-9]+(-[a-z0-9]+)*\\.toml\\z") && ![name isEqual:@"device-profile.toml"])) : PackMatches(name, @"^[a-z0-9][a-z0-9-]{0,47}\\.(input|actions|seeds|route|checks)\\z");
        NSData *file = allowed ? PackBase64(encoded[name], fileLimit) : nil;
        if (!file || file.length > totalLimit - total) { PackError(error, @"Pack file name, content, or size is invalid"); return nil; }
        total += file.length; files[name] = file;
    }
    return @{@"manifest": manifest, @"files": [files copy]};
}
- (NSDictionary *)generation:(NSString *)name {
    if (![self generationName:name] || !name.length || !PackNode(_root, YES)) return nil;
    NSString *folder = [self generationPath:name];
    NSString *filesPath = [folder stringByAppendingPathComponent:@"files"];
    if (!PackNode(folder, YES) || !PackNode(filesPath, YES)) return nil;
    NSDictionary *pack = [self verifyEnvelope:PackRead([folder stringByAppendingPathComponent:@"envelope.json"], self.envelopeLimit) authenticated:NULL error:NULL];
    NSDictionary *manifest = pack[@"manifest"], *files = pack[@"files"];
    if (!pack || ![[NSString stringWithFormat:@"%@-%@", manifest[@"sequence"], manifest[@"sha256"]] isEqual:name]) return nil;
    NSMutableSet *expected = [NSMutableSet setWithArray:files.allKeys];
    if (files[@"hardware/catalog.toml"]) [expected addObject:@"hardware"];
    NSDirectoryEnumerator *enumerator = [NSFileManager.defaultManager enumeratorAtPath:filesPath];
    NSMutableSet *actual = [NSMutableSet set];
    for (NSString *relative in enumerator) {
        NSString *path = [filesPath stringByAppendingPathComponent:relative];
        if (![expected containsObject:relative] || !PackNode(path, [relative isEqual:@"hardware"])) return nil;
        [actual addObject:relative];
        if (files[relative] && ![PackRead(path, [files[relative] length]) isEqual:files[relative]]) return nil;
    }
    NSError *validationError = nil;
    if (![actual isEqualToSet:expected] || !_validator(filesPath, &validationError)) return nil;
    return pack;
}
- (void)selectBundled { _launchFolder = _bundled; _identity = [self bundledIdentity]; }
- (void)selectGeneration:(NSString *)name pack:(NSDictionary *)pack {
    _launchFolder = [[self generationPath:name] stringByAppendingPathComponent:@"files"];
    NSDictionary *manifest = pack[@"manifest"];
    _identity = @{@"kind": _kind, @"channel": _channel, @"compatibility": _compatibility, @"sequence": manifest[@"sequence"], @"sha256": manifest[@"sha256"], @"bundled": @NO};
}
- (void)quarantine:(NSString *)name state:(NSMutableDictionary *)state {
    uint64_t sequence = [[[name componentsSeparatedByString:@"-"] firstObject] longLongValue];
    state[@"quarantined"] = @(MAX(sequence, [state[@"quarantined"] unsignedLongLongValue]));
}
- (void)prune {
    NSSet *keep = [NSSet setWithObjects:_state[@"active"], _state[@"healthy"], _state[@"pending"], @"state.json", @"feed-validator.txt", nil];
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:_root error:NULL]) {
        if (![keep containsObject:name] && ([self generationName:name] || [name hasPrefix:@".stage-"])) [NSFileManager.defaultManager removeItemAtPath:[self generationPath:name] error:NULL];
    }
}
- (NSString *)beginLaunch { @synchronized(self) {
    if (_launchStarted) {
        if (![_identity[@"bundled"] boolValue] && ![self generation:_state[@"active"]]) [self selectBundled];
        return [_launchFolder copy];
    }
    _launchStarted = YES;
    if (!_stateValid) return [_launchFolder copy];
    NSMutableDictionary *next = [_state mutableCopy];
    if ([next[@"trial"] boolValue]) {
        [self quarantine:next[@"active"] state:next];
        next[@"active"] = next[@"healthy"]; next[@"trial"] = @NO;
    }
    NSString *active = next[@"active"];
    NSDictionary *activePack = active.length ? [self generation:active] : nil;
    if (active.length && !activePack) {
        [self quarantine:active state:next];
        active = next[@"healthy"];
        activePack = active.length ? [self generation:active] : nil;
        if (!activePack) { active = @""; next[@"healthy"] = @""; }
        next[@"active"] = active;
    }
    NSString *pending = next[@"pending"];
    if (pending.length) {
        NSDictionary *candidate = [self generation:pending];
        if (candidate && [candidate[@"manifest"][@"sequence"] unsignedLongLongValue] > [next[@"quarantined"] unsignedLongLongValue]) {
            active = pending; activePack = candidate;
            next[@"active"] = pending; next[@"trial"] = @YES;
        } else [self quarantine:pending state:next];
        next[@"pending"] = @"";
    }
    if (![self writeState:next error:NULL]) { [self selectBundled]; return [_launchFolder copy]; }
    if (activePack) [self selectGeneration:active pack:activePack]; else [self selectBundled];
    [self prune];
    return [_launchFolder copy];
} }
- (void)markHealthy { @synchronized(self) {
    if (!_stateValid || !_launchStarted || [_identity[@"bundled"] boolValue] || ![_state[@"trial"] boolValue] || ![self generation:_state[@"active"]]) return;
    NSMutableDictionary *next = [_state mutableCopy];
    next[@"healthy"] = next[@"active"]; next[@"trial"] = @NO;
    if ([self writeState:next error:NULL]) {
        host_logf(HOST_LOG_INFO, "data-packs: %s snapshot %llu is healthy", _kind.UTF8String,
            [_identity[@"sequence"] unsignedLongLongValue]);
        [self prune];
    }
} }
- (void)rejectManifest:(NSDictionary *)manifest {
    uint64_t sequence = [manifest[@"sequence"] unsignedLongLongValue];
    if (sequence <= [_state[@"highest"] unsignedLongLongValue]) return;
    NSMutableDictionary *next = [_state mutableCopy];
    // Only the sequence is used up: raising the quarantine mark too would also refuse a valid pack already staged.
    next[@"highest"] = @(sequence);
    if ([self writeState:next error:NULL]) [self prune];
}
- (BOOL)stageEnvelope:(NSData *)data error:(NSError **)error { @synchronized(self) {
    if (!_stateValid) return PackError(error, @"Pack state is unavailable or malformed");
    NSDictionary *authenticated = nil;
    NSDictionary *pack = [self verifyEnvelope:data authenticated:&authenticated error:error];
    if (!pack) {
        if (authenticated) [self rejectManifest:authenticated];
        return NO;
    }
    NSDictionary *manifest = pack[@"manifest"], *files = pack[@"files"];
    if ([manifest[@"sequence"] unsignedLongLongValue] <= [_state[@"highest"] unsignedLongLongValue]) return PackError(error, @"Pack sequence was already seen");
    NSString *name = [NSString stringWithFormat:@"%@-%@", manifest[@"sequence"], manifest[@"sha256"]];
    NSString *temporary = [self generationPath:[@".stage-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    NSString *filesPath = [temporary stringByAppendingPathComponent:@"files"];
    NSFileManager *manager = NSFileManager.defaultManager;
    if (!PackNode(_root, YES) || ![manager createDirectoryAtPath:filesPath withIntermediateDirectories:YES attributes:nil error:error]) return NO;
    BOOL success = NO;
    @try {
        for (NSString *file in files) {
            NSString *path = [filesPath stringByAppendingPathComponent:file];
            if (![manager createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:error] || ![files[file] writeToFile:path options:NSDataWritingWithoutOverwriting error:error]) return NO;
        }
        NSError *validationError = nil;
        if (!_validator(filesPath, &validationError)) {
            [self rejectManifest:manifest];
            if (error) *error = validationError ?: [NSError errorWithDomain:@"VISRDataPack" code:2 userInfo:@{NSLocalizedDescriptionKey: @"Pack snapshot validation failed"}];
            return NO;
        }
        if (![data writeToFile:[temporary stringByAppendingPathComponent:@"envelope.json"] options:NSDataWritingWithoutOverwriting error:error]) return NO;
        NSData *metadata = [NSJSONSerialization dataWithJSONObject:manifest options:0 error:error];
        if (!metadata || ![metadata writeToFile:[temporary stringByAppendingPathComponent:@"metadata.json"] options:NSDataWritingWithoutOverwriting error:error]) return NO;
        // A rename publishes the complete generation before the atomic state update.
        NSString *destination = [self generationPath:name];
        if (![manager moveItemAtPath:temporary toPath:destination error:error]) return NO;
        NSMutableDictionary *next = [_state mutableCopy];
        next[@"highest"] = manifest[@"sequence"]; next[@"pending"] = name;
        if (![self writeState:next error:error]) { [manager removeItemAtPath:destination error:NULL]; return NO; }
        success = YES; [self prune];
        return YES;
    } @finally {
        if (!success) [manager removeItemAtPath:temporary error:NULL];
    }
} }
- (NSDictionary *)metadata { @synchronized(self) { return [_identity copy]; } }
// The file holds the sequence of the last accepted envelope, a newline, then its ETag. It applies only while that
// sequence is still the highest seen and a snapshot is installed, so a store that started over asks for the full feed.
- (NSString *)validatorPath { return [_root stringByAppendingPathComponent:@"feed-validator.txt"]; }
- (NSString *)savedValidator {
    NSString *text = [NSString stringWithContentsOfFile:self.validatorPath encoding:NSUTF8StringEncoding error:NULL];
    NSArray *parts = [text componentsSeparatedByString:@"\n"];
    BOOL installed = [_state[@"active"] length] || [_state[@"pending"] length];
    return parts.count == 2 && installed && [parts[0] isEqual:[_state[@"highest"] stringValue]] ? parts[1] : nil;
}
- (void)checkForUpdateAtURL:(NSURL *)url { @synchronized(self) {
    if (!_stateValid || _session || ![url.scheme.lowercaseString isEqual:@"https"] || !url.host.length || url.user || url.password || url.query || url.fragment) return;
    // Every launch checks. The request is cheap when the feed is unchanged: it carries the validator saved with the
    // last accepted envelope, and a 304 answer stops there.
    NSMutableDictionary *next = [_state mutableCopy]; next[@"last_check"] = @(NSDate.date.timeIntervalSince1970);
    if (![self writeState:next error:NULL]) return;
    NSURLSessionConfiguration *configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    configuration.URLCache = nil; configuration.HTTPCookieStorage = nil; configuration.URLCredentialStorage = nil;
    configuration.HTTPShouldSetCookies = NO; configuration.timeoutIntervalForRequest = 10; configuration.timeoutIntervalForResource = 30;
    configuration.waitsForConnectivity = NO;
    _download = [NSMutableData data]; _responseAccepted = NO; _responseValidator = nil;
    _session = [NSURLSession sessionWithConfiguration:configuration delegate:self delegateQueue:nil];
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    [request setValue:@"application/json" forHTTPHeaderField:@"Accept"];
    NSString *validator = [self savedValidator];
    if (validator.length) [request setValue:validator forHTTPHeaderField:@"If-None-Match"];
    [[_session dataTaskWithRequest:request] resume];
} }
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didReceiveChallenge:(NSURLAuthenticationChallenge *)challenge completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential * _Nullable))completionHandler {
    completionHandler([challenge.protectionSpace.authenticationMethod isEqual:NSURLAuthenticationMethodServerTrust] ? NSURLSessionAuthChallengePerformDefaultHandling : NSURLSessionAuthChallengeCancelAuthenticationChallenge, nil);
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request completionHandler:(void (^)(NSURLRequest * _Nullable))completionHandler {
    // Refuse every redirect, including same-host redirects, to keep the endpoint fixed.
    completionHandler(nil);
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveResponse:(NSURLResponse *)response completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler { @synchronized(self) {
    NSInteger status = [response isKindOfClass:NSHTTPURLResponse.class] ? [(NSHTTPURLResponse *)response statusCode] : 0;
    NSString *etag = status == 200 ? [(NSHTTPURLResponse *)response valueForHTTPHeaderField:@"ETag"] : nil;
    _responseValidator = etag.length && etag.length <= 256 && PackMatches(etag, @"^(W/)?\"[\\x21\\x23-\\x7e]*\"\\z") ? etag : nil;
    if (status != 200 && status != 304) host_logf(HOST_LOG_WARN, "data-packs: %s feed answered HTTP %ld; keeping the current snapshot", _kind.UTF8String, (long)status);
    _responseAccepted = status == 200 && (response.expectedContentLength < 0 || (uint64_t)response.expectedContentLength <= self.envelopeLimit);
    completionHandler(_responseAccepted ? NSURLSessionResponseAllow : NSURLSessionResponseCancel);
} }
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveData:(NSData *)data { @synchronized(self) {
    if (!_responseAccepted || data.length > self.envelopeLimit - _download.length) { _responseAccepted = NO; [_download setLength:0]; [task cancel]; return; }
    [_download appendData:data];
} }
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error { @synchronized(self) {
    if (error) host_logf(HOST_LOG_WARN, "data-packs: %s feed check ended (%s); keeping the current snapshot", _kind.UTF8String, error.localizedDescription.UTF8String);
    else if (_responseAccepted) @autoreleasepool {
        NSError *stageError = nil;
        if ([self stageEnvelope:_download error:&stageError]) {
            if (_responseValidator) [[NSString stringWithFormat:@"%@\n%@", _state[@"highest"], _responseValidator] writeToFile:self.validatorPath atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        } else host_logf(HOST_LOG_WARN, "data-packs: %s feed not staged (%s); keeping the current snapshot", _kind.UTF8String, stageError.localizedDescription.UTF8String);
    }
    _download = nil; _session = nil; [session finishTasksAndInvalidate];
} }
@end
