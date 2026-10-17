#import <Foundation/Foundation.h>
#include "host_benchmark_presentation.h"
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <zlib.h>
static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while(0)
static struct host_benchmark_presentation_token token(double t, int cutscene) {
    return host_benchmark_presentation_reserve(t, cutscene);
}
static NSArray *trace(NSDictionary *report) {
    NSString *encoded=report[@"trace"][@"data"]; if (!encoded) return nil;
    NSData *data=[[NSData alloc] initWithBase64EncodedString:encoded options:0];
    unsigned char out[32768]; uLongf size=sizeof(out);
    if (!data || uncompress(out,&size,data.bytes,data.length)!=Z_OK) return nil;
    return [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:out length:size] options:0 error:NULL];
}
static struct host_benchmark_presentation_token delayed;
static void *callback(void *unused) { (void)unused; usleep(20000); host_benchmark_presentation_complete(delayed,1.1); return NULL; }
static void *tests(void *unused) { (void)unused; @autoreleasepool {
    host_benchmark_presentation_configure(HOST_PRESENTATION_METAL,1,0);
    host_benchmark_presentation_begin(BENCHMARK_SCENE_GAMEPLAY,8);
    CHECK(token(1,0).generation==0); /* lead-in cannot reserve */
    host_benchmark_presentation_window(); /* first legacy window may be closed */
    CHECK(token(1,1).generation==0); /* renderer scene, not lagging legacy flag */
    struct host_benchmark_presentation_token a=token(1,0), b=token(2,0);
    CHECK(a.generation!=0 && b.index==1);
    CHECK(token(3,1).generation==0);
    host_benchmark_presentation_window(); /* cutscene resume must not reset */
    struct host_benchmark_presentation_token c=token(4,0);
    CHECK(c.generation==a.generation && c.index==2);
    host_benchmark_presentation_complete(c,4);
    host_benchmark_presentation_complete(b,1.02);
    host_benchmark_presentation_complete(a,1); /* old segment accepted */
    NSDictionary *r=host_benchmark_presentation_finish(.1);
    CHECK([r[@"expected"] intValue]==3 && [r[@"received"] intValue]==3);
    CHECK([r[@"interval_count"] intValue]==1);
    CHECK([trace(r) count]==3);
    CHECK(token(5,0).generation==0); /* end closes admission */
    host_benchmark_presentation_complete(a,50); /* immutable after finalization */
    CHECK([r[@"received"] intValue]==3);
    host_benchmark_presentation_begin(BENCHMARK_SCENE_CUTSCENE,8);
    host_benchmark_presentation_window();
    delayed=token(1,1); CHECK(delayed.generation>a.generation);
    host_benchmark_presentation_complete(a,100); /* cannot touch next storage */
    pthread_t thread; pthread_create(&thread,NULL,callback,NULL);
    r=host_benchmark_presentation_finish(.5); pthread_join(thread,NULL);
    CHECK([r[@"received"] intValue]==1 && [r[@"missing_at_finalize"] intValue]==0);
    CHECK([r[@"drain_status"] isEqual:@"complete"]);
    host_benchmark_presentation_begin(BENCHMARK_SCENE_MENU,8);
    r=host_benchmark_presentation_finish(.01); /* early load failure */
    CHECK([r[@"expected"] intValue]==0 && [r[@"status"] isEqual:@"no_samples"]);
    CHECK([r[@"interval_ms"] isEqual:NSNull.null]);
    host_benchmark_presentation_begin(BENCHMARK_SCENE_MENU,2);
    host_benchmark_presentation_window(); a=token(1,1); b=token(2,0);
    CHECK(a.generation!=0 && token(3,0).generation==0);
    host_benchmark_presentation_complete(a,1);
    r=host_benchmark_presentation_finish(.01);
    CHECK([r[@"missing_at_finalize"] intValue]==1 && [r[@"overflow"] intValue]==1);
    CHECK([r[@"status"] isEqual:@"incomplete"] && [r[@"drain_status"] isEqual:@"timeout"]);
    CHECK(fabs([r[@"callback_coverage"] doubleValue]-1.0/3.0)<1e-12);
    host_benchmark_presentation_begin(BENCHMARK_SCENE_MENU,8);
    host_benchmark_presentation_window();
    a=token(NAN,0); b=token(2,0); c=token(3,0);
    host_benchmark_presentation_complete(a,NAN); host_benchmark_presentation_complete(b,1); host_benchmark_presentation_complete(c,DBL_MAX);
    r=host_benchmark_presentation_finish(.01);
    CHECK([r[@"invalid_timestamps"] intValue]==1 && [r[@"invalid_spans"] intValue]==1);
    NSArray *rows=trace(r);
    CHECK(rows.count==3 && [rows[0][2] isEqual:NSNull.null] && [rows[0][3] isEqual:NSNull.null]);
    CHECK([rows[2][6] isEqual:@"invalid_gap"] && [rows[2][8] isEqual:NSNull.null]);
    CHECK([NSJSONSerialization isValidJSONObject:r]);
    NSDictionary *old=@{@"index":@0,@"frames_a_second":@60};
    NSDictionary *fresh=host_benchmark_measurement_record(@{@"index":@1},r);
    NSArray *mixed=@[old,fresh ?: @{}];
    NSData *json=[NSJSONSerialization dataWithJSONObject:mixed options:0 error:NULL];
    NSArray *read=[NSJSONSerialization JSONObjectWithData:json options:0 error:NULL];
    CHECK(read[0][@"measurement_semantics_version"]==nil);
    CHECK([read[1][@"measurement_semantics_version"] intValue]==2 && read[1][@"presentation"]!=nil);
    CHECK([read[1][@"measurement_semantics"][@"decision_basis"] isEqual:@"submission_timing"]);
    NSDictionary *recovered=host_benchmark_measurement_record(old,nil);
    CHECK([recovered[@"presentation"][@"status"] isEqual:@"unavailable_recovered_run"]);
    CHECK([recovered[@"measurement_semantics_version"] intValue]==1);
    host_benchmark_presentation_configure(HOST_PRESENTATION_SIMULATOR,-1,-1);
    host_benchmark_presentation_begin(BENCHMARK_SCENE_MENU,8); host_benchmark_presentation_window();
    CHECK(token(1,0).generation==0);
    r=host_benchmark_presentation_finish(.1);
    CHECK(![r[@"supported"] boolValue] && [r[@"capability"] isEqual:@"simulator"]);
 } return NULL; }
int main(void) { @autoreleasepool {
    pthread_t t; pthread_create(&t,NULL,tests,NULL); pthread_join(t,NULL);
    host_benchmark_presentation_configure(HOST_PRESENTATION_METAL,0,1);
    host_benchmark_presentation_begin(BENCHMARK_SCENE_MENU,8); host_benchmark_presentation_window(); token(1,0);
    NSDictionary *r=host_benchmark_presentation_finish(.5);
    CHECK([r[@"drain_status"] isEqual:@"skipped_main_thread"]);
    CHECK([r[@"display_synchronized"] isEqual:@NO] && [r[@"allows_tearing"] isEqual:@YES]);
    host_benchmark_presentation_begin(BENCHMARK_SCENE_GAMEPLAY,8);
    host_benchmark_presentation_window();
    struct host_benchmark_presentation_token first=token(100,0), second=token(101,0);
    host_benchmark_presentation_end_async();
    CHECK(token(102,0).generation==0);
    host_benchmark_presentation_window(); /* terminal close cannot reopen */
    CHECK(token(103,0).generation==0);
    host_benchmark_presentation_complete(second,10.025);
    usleep(20000); /* callback delivery delay is not presentation time */
    host_benchmark_presentation_complete(first,10);
    NSDictionary *async=nil;
    for (int i=0; i<1000 && !async; i++) { async=host_benchmark_presentation_take_report(); if (!async) usleep(1000); }
    CHECK([async[@"received"] intValue]==2 && [async[@"interval_count"] intValue]==1);
    CHECK(fabs([async[@"interval_ms"][@"mean"] doubleValue]-25)<1e-9);
    CHECK([async[@"drain_status"] isEqual:@"complete"]);
    CHECK(host_benchmark_presentation_take_report()==nil);
    NSDictionary *diagnostics=host_benchmark_presentation_diagnostics();
    CHECK([diagnostics[@"rejected_generation"] intValue]>=1 && [diagnostics[@"rejected_after_finalize"] intValue]>=1);
    fprintf(stderr,"presentation host: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
} }
