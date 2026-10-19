/* Actual native service entry points, with SDL event/window boundaries instrumented.
   This Foundation probe creates neither NSApplication nor an NSWindow. */
#import <Foundation/Foundation.h>
#include <assert.h>
#include "../port/macos/host_macos_event.h"
#include "../port/macos/host_macos_services.m"

static int window_reads, pushes;
static Uint32 last_event;
void host_fatal(const char *format, ...) {
    (void)format;
    @throw [NSException exceptionWithName:@"MainThreadRequired" reason:@"expected thread guard" userInfo:nil];
}
void host_logf(int priority,const char *format,...) { (void)priority;(void)format; }
SDL_Window *host_sdl_window(void) { window_reads++; return NULL; }
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window *window) { (void)window;window_reads++;return 0; }
void *SDL_GetPointerProperty(SDL_PropertiesID props,const char *name,void *fallback) {
    (void)props;(void)name;window_reads++;return fallback;
}
bool SDL_PushEvent(SDL_Event *event) { pushes++;last_event=event->type;return true; }
bool SDL_Init(SDL_InitFlags flags) { (void)flags;assert(!"SDL initialization must not be reached from worker");return false; }
bool SDL_SetHint(const char *name,const char *value) { (void)name;(void)value;return true; }
const char *SDL_GetError(void) { return "test"; }
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *window) { (void)window;window_reads++;return 0; }
SDL_DisplayID SDL_GetPrimaryDisplay(void) { window_reads++;return 0; }
const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID display) { (void)display;window_reads++;return NULL; }
bool SDL_GetWindowSizeInPixels(SDL_Window *window,int *width,int *height) {
    (void)window;window_reads++;*width=*height=0;return false;
}
static void run_worker(void (^action)(void)) {
    dispatch_semaphore_t finished=dispatch_semaphore_create(0);
    NSThread *thread=[[NSThread alloc]initWithBlock:^{
        @autoreleasepool { action(); }
        dispatch_semaphore_signal(finished);
    }];
    [thread start];
    assert(dispatch_semaphore_wait(finished,dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC))==0);
}
static void expect_main_guard(void (^action)(void)) {
    __block BOOL guarded=NO;
    run_worker(^{
        @try { action(); }
        @catch(NSException *error) { guarded=[error.name isEqualToString:@"MainThreadRequired"]; }
    });
    assert(guarded);
}
int main(void) {
    @autoreleasepool {
        expect_main_guard(^{host_macos_install();});
        expect_main_guard(^{host_macos_window_ready(NULL);});
        expect_main_guard(^{(void)host_macos_display_link(nil,NULL);});
        expect_main_guard(^{(void)host_external_screen();});
        expect_main_guard(^{struct host_external_snapshot out;host_external_snapshot(&out);});
        expect_main_guard(^{host_frame_counter_start(0,1);});
        expect_main_guard(^{host_frame_counter_stop();});
        run_worker(^{
            assert(host_macos_refresh_rate()==60.0);
            assert(host_alert("test","test","OK",NULL)==-1);
        });
        assert(window_reads==0);
        [[VISRMacActions new]quit:nil];
        assert(pushes==1 && last_event==SDL_EVENT_QUIT);
        SDL_Event event={.type=SDL_EVENT_WINDOW_CLOSE_REQUESTED};
        host_macos_normalize_event(&event);assert(event.type==SDL_EVENT_QUIT);
        event.type=SDL_EVENT_KEY_DOWN;event.key.scancode=SDL_SCANCODE_A;
        host_macos_normalize_event(&event);
        assert(event.type==SDL_EVENT_KEY_DOWN && event.key.scancode==SDL_SCANCODE_A);
        assert(NSApp==nil);
        puts("Native service main-thread guards, cached worker rate, and normal quit contracts passed");
    }
    return 0;
}
