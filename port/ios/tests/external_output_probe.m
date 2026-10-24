#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdio.h>

@interface UIScreen : NSObject
@end
@implementation UIScreen
@end
static UIScreen *output_screen, *expected_screen;
static int output_failures, renderer_ready;
#define HOST_LOG_WARN 1
static void host_logf(int priority,const char *format,...) { (void)priority;(void)format; }
static int gpu_metal_output(void *layer,void *screen,int width,int height) {
    (void)layer;(void)width;(void)height;
    assert(screen==(__bridge void *)expected_screen);
    /* Pacing restarts inside gpu_metal_output and asks host_external_screen. */
    assert(output_screen==expected_screen);
    return renderer_ready;
}
#include "external_output_under_test.inc"
int main(void) {
    @autoreleasepool {
        UIScreen *local=[UIScreen new],*tv=[UIScreen new];
        output_screen=local;expected_screen=tv;renderer_ready=1;
        assert(output_switch((void *)1,tv,1920,1080));
        assert(output_screen==tv);
        expected_screen=local;renderer_ready=0;
        assert(!output_switch((void *)1,local,1280,720));
        assert(output_screen==tv && output_failures==1);
        renderer_ready=1;
        assert(output_switch((void *)1,local,1280,720));
        assert(output_screen==local && !output_failures);
        puts("PASS: output pacing sees the destination screen; renderer refusal restores the previous screen");
    }
}
