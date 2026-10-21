/* Exercise signed per-map acquisition through the real store and NSURLSession delegate. */
#import <Foundation/Foundation.h>
#import <CommonCrypto/CommonDigest.h>
#import <objc/runtime.h>
#include <zlib.h>
#include "../host/host_data_pack.h"
#include "../host/host_shader_pack.h"
#include "../../third_party/monocypher/monocypher-ed25519.h"
#include <stdarg.h>
void host_logf(int priority, const char *format, ...) { (void)priority; (void)format; }
static int failures;
#define CHECK(c, label) do { if (!(c)) { fprintf(stderr,"FAIL: %s\n",label); failures++; } } while (0)
static uint8_t secret[64], public[32];
static NSMutableDictionary *responses, *counts;
static NSString *redirectPath;
static NSData *JSON(id object) { return [NSJSONSerialization dataWithJSONObject:object options:0 error:NULL]; }
static NSString *SHA(NSData *data) {
    unsigned char bytes[32]; CC_SHA256(data.bytes,(CC_LONG)data.length,bytes);
    NSMutableString *s = [NSMutableString string];
    for (int i=0;i<32;i++) [s appendFormat:@"%02x",bytes[i]];
    return s;
}
static NSData *Compressed(NSData *data) {
    uLongf size=compressBound(data.length); NSMutableData *out=[NSMutableData dataWithLength:size];
    if (compress2(out.mutableBytes,&size,data.bytes,data.length,9)!=Z_OK) abort();
    out.length=size; return out;
}
static NSData *SignedIndex(NSInteger sequence, NSDictionary *descriptors) {
    NSData *payload=JSON(@{@"files":descriptors});
    NSData *manifest=JSON(@{@"schema":@1,@"kind":@"shaders",@"channel":@"tester",@"sequence":@(sequence),@"minimum_engine":@1,@"maximum_engine":@1,@"compatibility":@"shader-lists-2",@"sha256":SHA(payload),@"notes":@"probe"});
    uint8_t signature[64]; crypto_ed25519_sign(signature,secret,manifest.bytes,manifest.length);
    return JSON(@{@"manifest":[manifest base64EncodedStringWithOptions:0],@"signature":[[NSData dataWithBytes:signature length:64] base64EncodedStringWithOptions:0],@"payload":[payload base64EncodedStringWithOptions:0]});
}
static NSData *Index(NSInteger sequence, NSDictionary *files) {
    NSMutableDictionary *descriptors=[NSMutableDictionary dictionary];
    for (NSString *name in files) {
        NSData *raw=files[name], *compressed=Compressed(raw);
        NSString *hash=SHA(compressed);
        descriptors[name]=[NSString stringWithFormat:@"%@:%lu:%@:%lu",SHA(raw),(unsigned long)raw.length,hash,(unsigned long)compressed.length];
        responses[[[@"/packs/shader-maps/" stringByAppendingString:hash] stringByAppendingString:@".zlib"]]=compressed;
    }
    return SignedIndex(sequence,descriptors);
}
@interface ShaderProtocol : NSURLProtocol
@end
@implementation ShaderProtocol
+ (BOOL)canInitWithRequest:(NSURLRequest *)request { (void)request; return YES; }
+ (NSURLRequest *)canonicalRequestForRequest:(NSURLRequest *)request { return request; }
- (void)startLoading {
    NSString *path=self.request.URL.path;
    @synchronized(counts) { counts[path]=@([counts[path] integerValue]+1); }
    CHECK(![self.request valueForHTTPHeaderField:@"Authorization"] && ![self.request valueForHTTPHeaderField:@"Cookie"],"asset requests omit inherited credentials");
    if ([path isEqual:redirectPath]) {
        NSHTTPURLResponse *response=[[NSHTTPURLResponse alloc] initWithURL:self.request.URL statusCode:302 HTTPVersion:@"HTTP/1.1" headerFields:@{@"Location":@"https://other.invalid/asset"}];
        [self.client URLProtocol:self wasRedirectedToRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@"https://other.invalid/asset"]] redirectResponse:response];
        return;
    }
    NSData *data=responses[path];
    NSHTTPURLResponse *response=[[NSHTTPURLResponse alloc] initWithURL:self.request.URL statusCode:data?200:404 HTTPVersion:@"HTTP/1.1" headerFields:@{}];
    [self.client URLProtocol:self didReceiveResponse:response cacheStoragePolicy:NSURLCacheStorageNotAllowed];
    if (data) {
        // Chunking exercises the delegate's streamed limit rather than trusting Content-Length.
        for (NSUInteger offset=0;offset<data.length;offset+=1024)
            [self.client URLProtocol:self didLoadData:[data subdataWithRange:NSMakeRange(offset,MIN(1024,data.length-offset))]];
    }
    [self.client URLProtocolDidFinishLoading:self];
}
- (void)stopLoading {}
@end
static IMP original;
static NSURLSessionConfiguration *Configuration(id object,SEL selector) {
    NSURLSessionConfiguration *configuration=((id(*)(id,SEL))original)(object,selector);
    configuration.protocolClasses=@[ShaderProtocol.class]; return configuration;
}
static HostDataPackStore *Store(NSString *root) {
    return [[HostDataPackStore alloc] initWithRoot:root bundled:@"/bundled" kind:@"shaders" channel:@"tester" publicKey:[NSData dataWithBytes:public length:32] compatibility:@"shader-lists-2" validator:^BOOL(NSString *folder,NSError **error) {
        (void)error; return host_shader_pack_validate(folder.fileSystemRepresentation,NULL,NULL);
    }];
}
static void Update(HostDataPackStore *store,NSData *index) {
    responses[@"/packs/shaders.json"]=index;
    [store checkForUpdateAtURL:[NSURL URLWithString:@"https://feed.invalid/packs/shaders.json"]];
    for (int i=0;i<600 && [store valueForKey:@"session"];i++) [NSThread sleepForTimeInterval:0.01];
    CHECK(![store valueForKey:@"session"],"bounded acquisition finishes");
}
static NSInteger AssetRequests(void) {
    NSInteger n=0; @synchronized(counts) { for (NSString *path in counts) if (![path hasSuffix:@"shaders.json"]) n+=[counts[path] integerValue]; } return n;
}
int main(void) { @autoreleasepool {
    uint8_t seed[32]={7}; crypto_ed25519_key_pair(secret,public,seed);
    responses=[NSMutableDictionary dictionary]; counts=[NSMutableDictionary dictionary];
    Method method=class_getClassMethod(NSURLSessionConfiguration.class,@selector(ephemeralSessionConfiguration));
    original=method_setImplementation(method,(IMP)Configuration);
    NSString *root=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    NSData *a=[@"# a10\nvs 0000000000000001 1 0\n" dataUsingEncoding:NSUTF8StringEncoding];
    NSData *b=[@"# b30\nvs 0000000000000002 1 0\n" dataUsingEncoding:NSUTF8StringEncoding];
    HostDataPackStore *store=Store(root); [store beginLaunch];
    NSData *one=Index(1,@{@"a10.txt":a,@"b30.txt":b}); Update(store,one);
    NSInteger firstRequests=AssetRequests();
    CHECK(firstRequests==2,"initial index downloads each missing map");
    CHECK([[store metadata][@"bundled"] boolValue],"downloads do not alter this launch");
    store=Store(root); NSString *active=[store beginLaunch]; [store markHealthy];
    CHECK([[store metadata][@"sequence"] integerValue]==1,"complete candidate activates next launch");
    [counts removeAllObjects];
    NSData *changed=[@"# changed a10\nvs 0000000000000003 1 0\n" dataUsingEncoding:NSUTF8StringEncoding];
    NSData *two=Index(2,@{@"a10.txt":changed,@"b30.txt":b}); Update(store,two);
    NSInteger changedRequests=AssetRequests();
    CHECK(changedRequests==1,"changing one map downloads only that map");
    CHECK([[NSData dataWithContentsOfFile:[active stringByAppendingPathComponent:@"a10.txt"]] isEqual:a],"previous active bytes remain usable during update");
    store=Store(root); active=[store beginLaunch]; [store markHealthy];
    CHECK([[store metadata][@"sequence"] integerValue]==2,"changed map candidate activates");
    [counts removeAllObjects]; Update(store,Index(3,@{@"a10.txt":changed,@"b30.txt":b}));
    NSInteger unchangedRequests=AssetRequests();
    CHECK(unchangedRequests==0,"new signed sequence reuses verified unchanged map bytes");
    printf("shader map requests: initial=%ld changed=%ld unchanged=%ld\n",(long)firstRequests,(long)changedRequests,(long)unchangedRequests);
    store=Store(root); active=[store beginLaunch]; [store markHealthy];
    [counts removeAllObjects];
    [@"corrupt" writeToFile:[active stringByAppendingPathComponent:@"b30.txt"] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    Update(store,Index(4,@{@"a10.txt":changed,@"b30.txt":b}));
    CHECK(AssetRequests()==1,"corrupt cached map is downloaded again");
    store=Store(root); active=[store beginLaunch]; [store markHealthy];
    [counts removeAllObjects];
    NSData *bad=Index(5,@{@"a10.txt":a,@"b30.txt":b});
    NSString *asset=[@"/packs/shader-maps/" stringByAppendingString:[SHA(Compressed(a)) stringByAppendingString:@".zlib"]];
    responses[asset]=[@"bad" dataUsingEncoding:NSUTF8StringEncoding]; Update(store,bad);
    store=Store(root); [store beginLaunch];
    CHECK([[store metadata][@"sequence"] integerValue]==4,"failed asset leaves healthy generation active");
    [counts removeAllObjects]; Update(store,one);
    CHECK(AssetRequests()==0,"stale signed index does not trigger downloads");
    NSData *redirect=Index(6,@{@"a10.txt":a,@"b30.txt":b}); redirectPath=asset;
    Update(store,redirect); redirectPath=nil;
    CHECK(!counts[@"/asset"],"asset redirects do not follow a second origin");
    store=Store(root); [store beginLaunch];
    CHECK([[store metadata][@"sequence"] integerValue]==4,"redirect failure preserves healthy generation");
    // Authenticated malformed data must still leave the last healthy generation usable.
    NSData *trailingRaw=a;
    NSMutableData *trailing=[Compressed(a) mutableCopy]; [trailing appendData:[@"trailer" dataUsingEncoding:NSUTF8StringEncoding]];
    NSMutableData *truncated=[Compressed(a) mutableCopy]; truncated.length--;
    NSData *bombRaw=[NSMutableData dataWithLength:1024*1024];
    NSData *oversized=[NSMutableData dataWithLength:4096];
    NSArray *compressedCases=@[trailing,truncated,Compressed(bombRaw),oversized];
    for (NSUInteger i=0;i<compressedCases.count;i++) {
        NSData *compressed=compressedCases[i];
        NSUInteger declaredRaw=i==2?1:trailingRaw.length;
        NSString *hash=SHA(compressed);
        NSString *descriptor=[NSString stringWithFormat:@"%@:%lu:%@:%lu",SHA(trailingRaw),(unsigned long)declaredRaw,hash,(unsigned long)(i==3?16:compressed.length)];
        responses[[[@"/packs/shader-maps/" stringByAppendingString:hash] stringByAppendingString:@".zlib"]]=compressed;
        Update(store,SignedIndex(7+i,@{@"a10.txt":descriptor}));
        store=Store(root); [store beginLaunch];
        CHECK([[store metadata][@"sequence"] integerValue]==4,"trailing, incomplete, bomb and excess streamed bytes cannot activate");
    }
    [counts removeAllObjects];
    NSString *tooLarge=[NSString stringWithFormat:@"%@:4194304:%@:1",SHA(a),SHA(Compressed(a))];
    Update(store,SignedIndex(11,@{@"a10.txt":tooLarge,@"b30.txt":tooLarge,@"c10.txt":tooLarge,@"d40.txt":tooLarge,@"a50.txt":tooLarge}));
    CHECK(AssetRequests()==0,"aggregate raw limit rejects signed index before downloading");
    [counts removeAllObjects];
    NSMutableDictionary *badEnvelope=[[NSJSONSerialization JSONObjectWithData:Index(12,@{@"a10.txt":a}) options:0 error:NULL] mutableCopy];
    NSMutableData *badSignature=[[[NSData alloc] initWithBase64EncodedString:badEnvelope[@"signature"] options:0] mutableCopy];
    ((unsigned char *)badSignature.mutableBytes)[0]^=1; badEnvelope[@"signature"]=[badSignature base64EncodedStringWithOptions:0];
    Update(store,JSON(badEnvelope));
    CHECK(AssetRequests()==0,"invalid signature does not trigger map downloads");
    NSData *dependency=[@"pipeline 0000000000000001.1.0 0000000000000001 0 1 1 1 1 1 0 00000000000000000000000000000000 1 1\n" dataUsingEncoding:NSUTF8StringEncoding];
    Update(store,Index(13,@{@"a10.txt":dependency}));
    store=Store(root); [store beginLaunch];
    CHECK([[store metadata][@"sequence"] integerValue]==4,"missing pipeline dependencies preserve healthy generation");
    NSData *partial=Index(14,@{@"a10.txt":a,@"b30.txt":[@"vs 0000000000000009 1 0\n" dataUsingEncoding:NSUTF8StringEncoding]});
    NSString *missing=[@"/packs/shader-maps/" stringByAppendingString:[SHA(Compressed([@"vs 0000000000000009 1 0\n" dataUsingEncoding:NSUTF8StringEncoding])) stringByAppendingString:@".zlib"]];
    [responses removeObjectForKey:missing]; Update(store,partial);
    store=Store(root); [store beginLaunch];
    CHECK([[store metadata][@"sequence"] integerValue]==4,"partial multi-map fetch does not publish a generation");
    Update(store,Index(20,@{@"a10.txt":changed,@"b30.txt":b}));
    store=Store(root); [store beginLaunch];
    CHECK([[store metadata][@"sequence"] integerValue]==20,"complete trial activates");
    store=Store(root); [store beginLaunch];
    CHECK([[store metadata][@"sequence"] integerValue]==4,"unhealthy trial rolls back");
    [NSFileManager.defaultManager removeItemAtPath:root error:NULL];
    method_setImplementation(method,original);
    if (!failures) puts("shader downloads: cache identity, atomicity, stale index and rollback pass");
    return failures?1:0;
} }
