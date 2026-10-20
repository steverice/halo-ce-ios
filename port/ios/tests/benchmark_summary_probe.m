/* Exercise the production searchable summary and full gzip asset without CloudKit requests. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <assert.h>
#include <stdarg.h>
#define HALO_BENCHMARK_CLOUD "iCloud.org.steverice.visr"
#include "../host/host_benchmark_send.m"
void host_logf(int priority, const char *format, ...) { (void)priority; (void)format; }
static NSData *encode(id value) { return [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingSortedKeys error:nil]; }
static NSData *failed_encode(id self, SEL selector, id value, NSJSONWritingOptions options, NSError **error) {
    (void)self; (void)selector; (void)value; (void)options; (void)error; return nil;
}
static void verify(NSData *raw) {
    NSData *original=[raw copy];
    NSString *summary=summary_json(raw);
    assert([summary lengthOfBytesUsingEncoding:NSUTF8StringEncoding]<=512*1024);
    id parsed=[NSJSONSerialization JSONObjectWithData:[summary dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
    assert([parsed isKindOfClass:NSDictionary.class]);
    assert([raw isEqual:original]);
    NSData *asset=gzipped(raw);
    assert(asset);
    NSMutableData *decoded=[NSMutableData dataWithLength:raw.length+1];
    z_stream stream={0}; assert(inflateInit2(&stream,15+32)==Z_OK);
    stream.next_in=(Bytef *)asset.bytes; stream.avail_in=(uInt)asset.length;
    stream.next_out=decoded.mutableBytes; stream.avail_out=(uInt)decoded.length;
    assert(inflate(&stream,Z_FINISH)==Z_STREAM_END); decoded.length=stream.total_out; inflateEnd(&stream);
    assert([decoded isEqual:original]);
}
int main(int argc, char **argv) { @autoreleasepool {
    NSString *trace=[@"A" stringByPaddingToLength:9*1024*1024 withString:@"A" startingAtIndex:0];
    NSDictionary *report=@{ @"code":@"LARGE", @"format":@1, @"suite":@3, @"mode":@"full-unified",
        @"flags":@[@"finished_early"], @"runs":@[@{ @"index":@0, @"scene":@"a10", @"status":@"partial",
        @"presentation":@{ @"positive":@100, @"trace":@{ @"status":@"ok", @"encoding":@"zlib+base64", @"data":trace } } }] };
    NSData *raw=encode(report); verify(raw);
    NSDictionary *summary=[NSJSONSerialization JSONObjectWithData:[summary_json(raw) dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
    NSDictionary *stripped=summary[@"runs"][0][@"presentation"][@"trace"];
    assert(!stripped[@"data"] && [stripped[@"encoded_bytes"] unsignedIntegerValue]==trace.length);
    assert([stripped[@"encoding"] isEqual:@"zlib+base64"] && [stripped[@"encoded_sha256"] length]==64);
    assert([stripped[@"payload_location"] isEqual:@"report_asset"]);
    assert([summary[@"runs"][0][@"presentation"][@"positive"] isEqual:@100]);
    NSMutableDictionary *nested=[report mutableCopy];
    nested[@"shader_output_intervals"]=@[@{ @"runs":report[@"runs"] }];
    NSData *nested_raw=encode(nested); verify(nested_raw);
    NSDictionary *nested_summary=[NSJSONSerialization JSONObjectWithData:[summary_json(nested_raw) dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
    assert(!nested_summary[@"shader_output_intervals"][0][@"runs"][0][@"presentation"][@"trace"][@"data"]);
    NSMutableDictionary *graphics=[report mutableCopy]; graphics[@"graphics"]=trace;
    NSData *huge=encode(graphics); verify(huge);
    NSDictionary *compact=[NSJSONSerialization JSONObjectWithData:[summary_json(huge) dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
    assert([compact[@"summary_status"] isEqual:@"compact"] && [compact[@"code"] isEqual:@"LARGE"]);
    assert([compact[@"run_count"] isEqual:@1] && [compact[@"runs"][0][@"status"] isEqual:@"partial"]);
    assert([compact[@"detail_asset_field"] isEqual:@"report"]);
    assert([compact[@"flags"] containsObject:@"finished_early"] && [compact[@"statuses_omitted"] isEqual:@0]);
    NSMutableDictionary *many=[graphics mutableCopy];
    NSMutableArray *rows=[NSMutableArray array];
    for(int i=0;i<100;i++)[rows addObject:@{ @"index":@(i), @"scene":@"a10", @"status":@"partial" }];
    many[@"runs"]=rows;
    NSData *many_raw=encode(many); verify(many_raw);
    NSDictionary *limited=[NSJSONSerialization JSONObjectWithData:[summary_json(many_raw) dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
    assert([limited[@"run_count"] isEqual:@100] && [limited[@"runs"] count]==64 && [limited[@"statuses_omitted"] isEqual:@36]);
    assert([limited[@"completion"] isEqual:@"finished_early"]);

    assert([summary_json([@"invalid" dataUsingEncoding:NSUTF8StringEncoding]) length]==0);
    assert([summary_json([@"[]" dataUsingEncoding:NSUTF8StringEncoding]) length]==0);
    Method method=class_getClassMethod(NSJSONSerialization.class,@selector(dataWithJSONObject:options:error:));
    IMP previous=method_setImplementation(method,(IMP)failed_encode);
    NSString *failed=summary_json(raw);
    method_setImplementation(method,previous);
    assert([failed containsString:@"serialization_failed"] && [raw isEqual:encode(report)]);
    if(argc>1) {
        NSData *fleet=[NSData dataWithContentsOfFile:@(argv[1])]; assert(fleet); verify(fleet);
        printf("real fleet summary: %lu -> %lu UTF-8 bytes; full asset round-trip identical\n",(unsigned long)fleet.length,
            (unsigned long)[summary_json(fleet) lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
    }
    puts("benchmark summary: PASS (large trace, compact fallback, raw asset and serialization failures)");
} }
