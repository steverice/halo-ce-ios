#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>
#if defined(PRESENTATION_TEST_SIMULATOR)
#undef TARGET_OS_SIMULATOR
#define TARGET_OS_SIMULATOR 1
#elif defined(PRESENTATION_TEST_VISIONOS)
#undef TARGET_OS_VISION
#define TARGET_OS_VISION 1
#endif
#include "gpu_metal_presentation.h"
#include "host_benchmark_presentation.h"
#include <math.h>
#include <unistd.h>
static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while(0)
/* A controllable drawable transport; the actual adapter and owner run unchanged.
   No texture or command buffer is needed to deliver a Metal presented handler. */
@interface TestDrawable : NSObject
@property(nonatomic) double presentedTime;
@property(nonatomic) NSUInteger registrations;
@property(nonatomic,copy) void (^handler)(id<MTLDrawable>);
@end
@implementation TestDrawable
- (void)addPresentedHandler:(void (^)(id<MTLDrawable>))handler { self.registrations++; self.handler=handler; }
- (void)deliver:(double)time { self.presentedTime=time; if(self.handler) self.handler((id<MTLDrawable>)self); }
@end
static void observe(TestDrawable *d, double submitted, int cutscene, int transition) {
    gpu_metal_presentation_attempt(cutscene,transition);
    gpu_metal_presentation_observe((id<CAMetalDrawable>)d,submitted,cutscene,transition);
}
static NSDictionary *finish(void) {
    host_benchmark_presentation_end_async();
    NSDictionary *report=nil;
    for(int i=0; i<1000 && !report; i++) { report=host_benchmark_presentation_take_report(); if(!report) usleep(1000); }
    CHECK(report!=nil); return report;
}
int main(void) { @autoreleasepool {
    gpu_metal_presentation_configure(nil);
    host_benchmark_presentation_begin(BENCHMARK_SCENE_GAMEPLAY,16);
    TestDrawable *a=[TestDrawable new];
    observe(a,1,0,0); CHECK(a.registrations==0);
    host_benchmark_presentation_window();
#if TARGET_OS_SIMULATOR || TARGET_OS_VISION
    observe(a,1,0,0); CHECK(a.registrations==0);
    NSDictionary *r=finish(); CHECK(![r[@"supported"] boolValue]);
#if TARGET_OS_VISION
    CHECK([r[@"capability"] isEqual:@"visionos_compositor_unavailable"]);
#else
    CHECK([r[@"capability"] isEqual:@"simulator"]);
#endif
#else
    TestDrawable *b=[TestDrawable new], *excluded=[TestDrawable new];
    observe(a,1,0,0); observe(b,2,0,0);
    gpu_metal_presentation_configure(nil); /* per-frame policy observation cannot reset storage */
    CHECK(a.registrations==1 && b.registrations==1);
    /* A new cutscene closes the segment before the legacy guest window toggles. */
    observe(excluded,3,1,0); CHECK(excluded.registrations==0);
    TestDrawable *resumed=[TestDrawable new]; observe(resumed,4,0,0);
    CHECK(resumed.registrations==1); /* first resumed frame, legacy window still closed */
    [resumed deliver:20]; [b deliver:10.025]; [a deliver:10];
    NSDictionary *r=finish();
    CHECK([r[@"expected"] intValue]==3 && [r[@"received"] intValue]==3);
    CHECK([r[@"interval_count"] intValue]==1);
    CHECK([r[@"interval_ms"] isKindOfClass:NSDictionary.class] && fabs([r[@"interval_ms"][@"mean"] doubleValue]-25)<1e-8);
    /* Excluded output transitions must also break adjacency. */
    host_benchmark_presentation_begin(BENCHMARK_SCENE_MENU,16); host_benchmark_presentation_window();
    TestDrawable *before=[TestDrawable new], *after=[TestDrawable new];
    observe(before,30,0,0); observe(excluded,31,0,1); observe(after,32,0,0);
    CHECK(excluded.registrations==0); [before deliver:30]; [after deliver:32];
    r=finish(); CHECK([r[@"expected"] intValue]==2 && [r[@"interval_count"] intValue]==0);
    /* A transition may be the first cutscene frame or repeat while excluded.
       Every scene kind must exclude transitions without admitting a record. */
    for(int kind=BENCHMARK_SCENE_MENU; kind<=BENCHMARK_SCENE_GAMEPLAY; kind++) {
        host_benchmark_presentation_begin((enum benchmark_scene_kind)kind,16);
        host_benchmark_presentation_window();
        TestDrawable *old=[TestDrawable new], *next=[TestDrawable new];
        observe(old,35,0,0); observe(excluded,36,1,1); observe(excluded,37,1,1);
        observe(next,38,0,0); [old deliver:35]; [next deliver:38];
        r=finish(); CHECK([r[@"expected"] intValue]==2 && [r[@"interval_count"] intValue]==0);
        CHECK(excluded.registrations==0);
    }
    /* Missing eligible drawables remain real elapsed stalls. An entirely
       unshown cutscene or output transition deliberately breaks adjacency. */
    for(int excludedScene=0; excludedScene<3; excludedScene++) {
        host_benchmark_presentation_begin(BENCHMARK_SCENE_GAMEPLAY,16); host_benchmark_presentation_window();
        TestDrawable *old=[TestDrawable new], *next=[TestDrawable new];
        observe(old,100,0,0);
        gpu_metal_presentation_attempt(excludedScene==1,excludedScene==2); /* no drawable */
        observe(next,101,0,0); [old deliver:100]; [next deliver:101];
        r=finish(); CHECK([r[@"expected"] intValue]==2);
        CHECK([r[@"interval_count"] intValue]==(excludedScene?0:1));
        if(!excludedScene) CHECK([r[@"interval_ms"][@"mean"] doubleValue]==1000);
    }
    /* Cutscene runs retain cutscene frames; callback receipt delay does not alter timestamps. */
    host_benchmark_presentation_begin(BENCHMARK_SCENE_CUTSCENE,16); host_benchmark_presentation_window();
    TestDrawable *zero=[TestDrawable new], *invalid=[TestDrawable new], *missing=[TestDrawable new];
    observe(zero,40,1,0); observe(invalid,41,1,0); observe(missing,42,1,0);
    [zero deliver:0]; [invalid deliver:NAN]; r=finish();
    CHECK([r[@"expected"] intValue]==3 && [r[@"received"] intValue]==2);
    CHECK([r[@"unpresented"] intValue]==1 && [r[@"invalid_timestamps"] intValue]==1);
    CHECK([r[@"missing_at_finalize"] intValue]==1);
    /* The callback keeps only a scalar token: old callbacks cannot change a new run. */
    host_benchmark_presentation_begin(BENCHMARK_SCENE_MENU,16); host_benchmark_presentation_window();
    TestDrawable *last=[TestDrawable new]; observe(last,50,0,0);
    host_benchmark_presentation_end_async(); CHECK(host_benchmark_presentation_take_report()==nil);
    [missing deliver:99]; [last deliver:50];
    NSDictionary *ended=nil;
    for(int i=0; i<1000 && !ended; i++) { ended=host_benchmark_presentation_take_report(); if(!ended) usleep(1000); }
    CHECK([ended[@"received"] intValue]==1 && [ended[@"missing_at_finalize"] intValue]==0);
    CHECK(host_benchmark_presentation_take_report()==nil);
    [last deliver:51]; CHECK([ended[@"received"] intValue]==1);
    /* Keeping a pending handler cannot keep its drawable alive. */
    host_benchmark_presentation_begin(BENCHMARK_SCENE_MENU,16); host_benchmark_presentation_window();
    __weak TestDrawable *weakDrawable;
    void (^pending)(id<MTLDrawable>);
    @autoreleasepool {
        TestDrawable *temporary=[TestDrawable new]; weakDrawable=temporary;
        observe(temporary,60,0,0); pending=temporary.handler;
    }
    CHECK(weakDrawable==nil && pending!=nil);
    TestDrawable *shown=[TestDrawable new]; shown.presentedTime=60;
    if(pending) pending((id<MTLDrawable>)shown);
    r=finish(); CHECK([r[@"received"] intValue]==1);
#endif
    fprintf(stderr,"presentation drawable: %d checks, %d failures\n",checks,failures);
    return failures?1:0;
} }
