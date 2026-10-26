/* Actual native service entry points, with SDL event/window boundaries instrumented.
   This Foundation probe creates neither NSApplication nor an NSWindow. */
#import <Foundation/Foundation.h>
#include <assert.h>
#include "../port/macos/host_macos_event.h"
#include "../port/ios/host/host_benchmark.h"
#import <AppKit/AppKit.h>
static int mode_copies, mode_releases;
static CGDisplayModeRef probe_copy_mode(CGDirectDisplayID display) {
    mode_copies++;
    return display == 1 ? (CGDisplayModeRef)(uintptr_t)1 : NULL;
}
static size_t probe_pixel_width(CGDisplayModeRef mode) { assert(mode); return 3024; }
static size_t probe_pixel_height(CGDisplayModeRef mode) { assert(mode); return 1964; }
static size_t probe_logical_size(CGDirectDisplayID display) { (void)display; return 1512; }
static void probe_release_mode(CGDisplayModeRef mode) { assert(mode); mode_releases++; }
#define CGDisplayCopyDisplayMode probe_copy_mode
#define CGDisplayModeGetPixelWidth probe_pixel_width
#define CGDisplayModeGetPixelHeight probe_pixel_height
#define CGDisplayModeRelease probe_release_mode
#define CGDisplayPixelsWide probe_logical_size
#define CGDisplayPixelsHigh probe_logical_size
#include "../port/macos/host_macos_services.m"

static int finish_requested;
int host_benchmark_finish_requested(void) { return finish_requested; }
void host_benchmark_progress(struct host_benchmark_progress *progress) { memset(progress, 0, sizeof(*progress)); }

static int window_reads, pushes;
static Uint32 last_event;
static SDL_Event pending[4];
static int pending_count;
int SDL_PeepEvents(SDL_Event *events, int count, SDL_EventAction action, Uint32 first, Uint32 last) {
    assert(action == SDL_PEEKEVENT && first == SDL_EVENT_FIRST && last == SDL_EVENT_LAST);
    if (!events) return pending_count;
    int copied = count < pending_count ? count : pending_count;
    memcpy(events, pending, (size_t)copied * sizeof(*events));
    return copied;
}
void host_fatal(const char *format, ...) {
    (void)format;
    @throw [NSException exceptionWithName:@"MainThreadRequired" reason:@"expected thread guard" userInfo:nil];
}
/* the main-thread guard is a bug, so it crashes (host_crash.c); here it throws like host_fatal */
void host_crash(const char *format, ...) {
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
        int width = -1, height = -1;
        display_pixel_size(1, &width, &height);
        assert(width == 3024 && height == 1964);
        assert(mode_copies == 1 && mode_releases == 1);
        display_pixel_size(2, &width, &height);
        assert(width == 0 && height == 0);
        assert(mode_copies == 2 && mode_releases == 1);
        display_pixel_size(0, &width, &height);
        assert(width == 0 && height == 0 && mode_copies == 2);
        expect_main_guard(^{int w, h; host_macos_screen_size(&w, &h);});
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
        assert(host_lifecycle_background_milliseconds()==0);
        [[VISRMacActions new]quit:nil];
        assert(pushes==1 && last_event==SDL_EVENT_QUIT);
        SDL_Event event={.type=SDL_EVENT_WINDOW_CLOSE_REQUESTED};
        host_macos_normalize_event(&event);assert(event.type==SDL_EVENT_QUIT);
        event.type=SDL_EVENT_KEY_DOWN;event.key.scancode=SDL_SCANCODE_A;
        host_macos_normalize_event(&event);
        assert(event.type==SDL_EVENT_KEY_DOWN && event.key.scancode==SDL_SCANCODE_A);
        host_benchmark_overlay_show(0);
        run_worker(^{host_benchmark_overlay_show(1);});
        pending[0].type = SDL_EVENT_GAMEPAD_ADDED;
        pending_count = 1;
        assert(!benchmark_notice_stopped() && pending_count == 1);
        const Uint32 stops[] = {SDL_EVENT_QUIT, SDL_EVENT_WINDOW_CLOSE_REQUESTED, SDL_EVENT_KEY_DOWN,
            SDL_EVENT_GAMEPAD_BUTTON_DOWN};
        for (int i = 0; i < 4; i++) {
            pending[1] = (SDL_Event){.type = stops[i]};
            pending[1].key.key = SDLK_ESCAPE;
            if (stops[i] == SDL_EVENT_GAMEPAD_BUTTON_DOWN) pending[1].gbutton.button = SDL_GAMEPAD_BUTTON_BACK;
            pending_count = 2;
            assert(benchmark_notice_stopped() && pending_count == 2);
            assert(pending[0].type == SDL_EVENT_GAMEPAD_ADDED && pending[1].type == stops[i]);
        }
        pending_count = 0;
        finish_requested = 1;
        assert(benchmark_notice_stopped());
        assert(NSApp==nil);
        puts("Native service physical pixels, main-thread guards, cached worker rate, and normal quit contracts passed");
    }
    return 0;
}
