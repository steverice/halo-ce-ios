/* AppKit adapters for the native host. SDL retains its Cocoa delegate and window. */
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include "host_macos.h"
#include "ios_host.h"
#include "host_external.h"
#include "host_benchmark.h"
#include "host_frame_counter_stats.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static atomic_int modal_depth;
static _Atomic double cached_refresh_rate = 60.0;
static void require_main(void) {
    if (!NSThread.isMainThread) host_fatal("Native AppKit window access requires the main thread");
}
static NSWindow *game_window(void) {
    require_main();
    SDL_Window *window = host_sdl_window();
    return window ? (__bridge NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL) : nil;
}
const char *host_macos_data_root(void) {
    static char root[4096];
    NSString *path = NSProcessInfo.processInfo.environment[@"HALO_DATA_ROOT"];
    if (!path.length) path = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,
        NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:NSBundle.mainBundle.bundleIdentifier];
    NSError *error = nil;
    if (![path isAbsolutePath] || ![NSFileManager.defaultManager createDirectoryAtPath:path
        withIntermediateDirectories:YES attributes:nil error:&error])
        host_fatal("Could not create the native app's data folder: %s", error.localizedDescription.UTF8String ?: "expected an absolute path");
    if (!path.fileSystemRepresentation || strlen(path.fileSystemRepresentation) >= sizeof(root))
        host_fatal("The native app's data folder path is too long");
    snprintf(root, sizeof(root), "%s", path.fileSystemRepresentation);
    return root;
}
@interface VISRMacActions : NSObject
- (void)quit:(id)sender;
@end
@implementation VISRMacActions
- (void)quit:(id)sender {
    (void)sender;
    SDL_Event event = { .type = SDL_EVENT_QUIT };
    SDL_PushEvent(&event);
}
@end
void host_macos_install(void) {
    require_main();
    /* SDL must create its Cocoa application/delegate before an open panel creates NSApp. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"0");
    if (!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_GAMEPAD))
        host_fatal("SDL initialization: %s",SDL_GetError());
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
}
void host_macos_window_ready(void *sdl_window) {
    require_main();
    (void)sdl_window;
    host_macos_refresh_rate();
    static id screen_observer, rate_observer;
    if (!screen_observer) {
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        screen_observer = [center addObserverForName:NSWindowDidChangeScreenNotification
            object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
                (void)note; host_macos_refresh_rate();
            }];
        rate_observer = [center addObserverForName:NSApplicationDidChangeScreenParametersNotification
            object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
                (void)note; host_macos_refresh_rate();
            }];
    }
    static VISRMacActions *actions;
    if (actions) return;
    actions = [VISRMacActions new];
    NSMenu *bar = [NSMenu new];
    NSMenuItem *application = [NSMenuItem new];
    [bar addItem:application];
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"VISR"];
    NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:@"Quit VISR" action:@selector(quit:) keyEquivalent:@"q"];
    quit.target = actions;
    [menu addItem:quit];
    application.submenu = menu;
    NSApp.mainMenu = bar;
    [NSApp activateIgnoringOtherApps:YES];
}
double host_macos_refresh_rate(void) {
    if (!NSThread.isMainThread) return atomic_load(&cached_refresh_rate);
    SDL_Window *window = host_sdl_window();
    SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) : SDL_GetPrimaryDisplay();
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(display);
    NSScreen *screen = game_window().screen ?: NSScreen.mainScreen;
    double rate = mode && mode->refresh_rate > 0 ? mode->refresh_rate : screen.maximumFramesPerSecond;
    if (rate < 60.0) rate = 60.0;
    atomic_store(&cached_refresh_rate,rate);
    return rate;
}
void host_macos_screen_size(int *width, int *height) {
    require_main();
    NSScreen *screen = game_window().screen ?: NSScreen.mainScreen;
    CGDirectDisplayID display = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
    *width = display ? (int)CGDisplayPixelsWide(display) : 0;
    *height = display ? (int)CGDisplayPixelsHigh(display) : 0;
}
CADisplayLink *host_macos_display_link(id target, SEL selector) {
    require_main();
    NSWindow *window = game_window();
    if (!window) host_fatal("The native display clock has no SDL window");
    /* NSWindow's display link follows the window across screens. It does not take over its delegate. */
    return [window displayLinkWithTarget:target selector:selector];
}
int host_alert(const char *title, const char *message, const char *first, const char *second) {
    if (!NSThread.isMainThread) return -1;
    NSAlert *alert = [NSAlert new];
    alert.messageText = @(title ?: "VISR"); alert.informativeText = @(message ?: "");
    [alert addButtonWithTitle:@(first ?: "OK")];
    if (second) [alert addButtonWithTitle:@(second)];
    modal_depth++;
    NSModalResponse result = [alert runModal];
    modal_depth--;
    return result == NSAlertFirstButtonReturn ? 0 : 1;
}
int host_window_up(void) { return modal_depth != 0; }
int posix_share_link(const char *link) {
    if (!NSThread.isMainThread || !link) return 0;
    [NSPasteboard.generalPasteboard clearContents];
    if (![NSPasteboard.generalPasteboard setString:@(link) forType:NSPasteboardTypeString]) return 0;
    host_alert("Invite link copied", "Paste this link to invite another player.", "OK", NULL);
    return 1;
}
void host_ios_touch_initialize(void) {}
void host_ios_touch_attach(SDL_Window *window) { host_macos_window_ready(window); }
void host_ios_touch_reset(void) {}
void host_touch_replay_active(int active) { (void)active; }
void host_ios_touch_pause(void) {}
int host_ios_gamepads(uint32_t *out, int capacity) {
    if (capacity <= 0 || !out) return 0;
    int count = 0; SDL_JoystickID *ids = SDL_GetGamepads(&count);
    int copied = count < capacity ? count : capacity;
    if (ids && copied > 0) memcpy(out, ids, (size_t)copied * sizeof(*out));
    SDL_free(ids); return copied;
}
int host_ios_gamepad_type(SDL_Gamepad *pad) { return SDL_GetGamepadType(pad); }
int host_ios_gamepad_axis(SDL_Gamepad *pad, int axis) { return SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)axis); }
int host_ios_gamepad_button(SDL_Gamepad *pad, int button) { return SDL_GetGamepadButton(pad, (SDL_GamepadButton)button); }
void host_lifecycle_install(void) {}
int host_lifecycle_in_background(void) { return 0; }
void host_lifecycle_hold(void) {}
/* Native Mac play keeps running outside the foreground; its guest clock never subtracts suspended time. */
unsigned long long host_lifecycle_background_milliseconds(void) { return 0; }
void host_external_install(void) {}
void host_external_attach(void *window, int metal) { (void)metal; host_macos_window_ready(window); }
void host_external_poll(void) {
    /* SDL pumps AppKit on this thread; refresh the worker cache even without a mode notification. */
    host_macos_refresh_rate();
}
void host_external_commit(int allow_pause) { (void)allow_pause; }
int host_external_active(void) { return 0; }
int host_external_size(int *width, int *height) { (void)width; (void)height; return 0; }
void *host_external_phone_layer(void) { return NULL; }
void *host_external_screen(void) { return (__bridge void *)game_window().screen; }
double host_external_companion_dim(void) { return 0; }
void host_external_snapshot(struct host_external_snapshot *out) {
    require_main();
    memset(out, 0, sizeof(*out));
    SDL_Window *window = host_sdl_window();
    if (window) SDL_GetWindowSizeInPixels(window, &out->width, &out->height);
    out->refresh_hertz = (int)host_macos_refresh_rate();
    snprintf(out->name, sizeof(out->name), "Mac game window");
    snprintf(out->reason, sizeof(out->reason), "native");
}
int host_external_cycle(int direction) { (void)direction; return 0; }
int host_external_force(const char *mode) { return !mode || !*mode || !strcmp(mode, "preferred"); }
int host_external_ready(void) { return 1; }
void host_external_mode_label(char *text, size_t size) { snprintf(text, size, "Mac game window"); }
void host_extra_scene_poll(void) {}
void host_extra_scene_test(void) {}
int host_scene_is_games(void *scene) { (void)scene; return 1; }
/* Native reports remain in the data folder; do not open a local HTTP server or another app. */
/* Native Mac uses the same progress snapshot as the UIKit tester screen. */
static NSView *benchmark_progress_view;
static NSTextField *benchmark_progress_label;
static NSTimer *benchmark_progress_timer;
static void benchmark_progress_refresh(void) {
    struct host_benchmark_progress progress;
    host_benchmark_progress(&progress);
    benchmark_progress_view.hidden = !host_benchmark_progress_visible(&progress);
    if (benchmark_progress_view.hidden) return;
    char detail[512];
    host_benchmark_progress_detail(&progress, detail, sizeof(detail));
    benchmark_progress_label.stringValue = [NSString stringWithFormat:@"%s\n\n%s\n\nBenchmark running — please don’t close the app",
        progress.title[0] ? progress.title : "Getting ready", detail];
}
void host_benchmark_overlay_presented(int presented) {
    void (^update)(void) = ^{ benchmark_progress_view.hidden = presented != 0; benchmark_progress_refresh(); };
    if (NSThread.isMainThread) update();
    else dispatch_sync(dispatch_get_main_queue(), update);
}
void host_benchmark_overlay_show(int visible) {
    if (!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(), ^{ host_benchmark_overlay_show(visible); });
        return;
    }
    if (!visible) {
        [benchmark_progress_timer invalidate]; benchmark_progress_timer = nil;
        [benchmark_progress_view removeFromSuperview]; benchmark_progress_view = nil;
        benchmark_progress_label = nil;
        return;
    }
    if (benchmark_progress_view) return;
    NSView *parent = game_window().contentView;
    if (!parent) return;
    benchmark_progress_view = [[NSView alloc] initWithFrame:parent.bounds];
    benchmark_progress_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    benchmark_progress_view.wantsLayer = YES;
    benchmark_progress_view.layer.backgroundColor = NSColor.blackColor.CGColor;
    benchmark_progress_label = [NSTextField wrappingLabelWithString:@""];
    benchmark_progress_label.textColor = NSColor.whiteColor;
    benchmark_progress_label.font = [NSFont systemFontOfSize:20];
    benchmark_progress_label.alignment = NSTextAlignmentCenter;
    benchmark_progress_label.translatesAutoresizingMaskIntoConstraints = NO;
    [benchmark_progress_view addSubview:benchmark_progress_label];
    [NSLayoutConstraint activateConstraints:@[
        [benchmark_progress_label.centerYAnchor constraintEqualToAnchor:benchmark_progress_view.centerYAnchor],
        [benchmark_progress_label.leadingAnchor constraintEqualToAnchor:benchmark_progress_view.leadingAnchor constant:24],
        [benchmark_progress_label.trailingAnchor constraintEqualToAnchor:benchmark_progress_view.trailingAnchor constant:-24]]];
    [parent addSubview:benchmark_progress_view];
    benchmark_progress_refresh();
    benchmark_progress_timer = [NSTimer scheduledTimerWithTimeInterval:0.25 repeats:YES block:^(NSTimer *timer) {
        (void)timer; benchmark_progress_refresh();
    }];
}
void host_benchmark_page(const char *path, const char *summary, const char *code, int minutes) {
    (void)code; (void)minutes;
    host_logf(HOST_LOG_INFO, "benchmark report: %s", path);
    host_alert("Benchmark complete", summary, "OK", NULL);
}

/* The shared bounded statistics keep asynchronous GPU samples on their producer thread. */
static struct host_frame_counter_stats counter;
static NSLock *counter_lock;
static NSTextField *counter_label;
static atomic_int counter_enabled;
static int counter_fixed;
static double counter_first, counter_next;
static NSString *counter_renderer;
void host_frame_counter_stop(void) {
    require_main();
    counter_enabled = 0;
    [counter_label removeFromSuperview]; counter_label = nil;
}
void host_frame_counter_start(int fixed, int metal) {
    require_main();
    host_frame_counter_stop();
    if (!counter_lock) counter_lock = [NSLock new];
    [counter_lock lock]; host_frame_counter_stats_reset(&counter); [counter_lock unlock];
    counter_fixed = fixed; counter_renderer = metal ? @"Metal" : @"GL";
    counter_first = CACurrentMediaTime(); counter_next = counter_first; counter_enabled = 1;
}
void host_frame_counter_sample(double interval, double cpu, double gpu) {
    if (!atomic_load(&counter_enabled)) return;
    [counter_lock lock];
    host_frame_counter_stats_sample(&counter, interval, cpu, gpu);
    [counter_lock unlock];
}
void host_frame_counter_show(unsigned long frame) {
    if (!counter_enabled || !NSThread.isMainThread) return;
    double now = CACurrentMediaTime();
    if (now < counter_next) return;
    counter_next = now + 0.25;
    [counter_lock lock];
    struct host_frame_counter_summary value = host_frame_counter_stats_read(&counter);
    host_frame_counter_stats_reset(&counter); [counter_lock unlock];
    NSView *view = game_window().contentView;
    if (!view) return;
    if (!counter_label) {
        counter_label = [NSTextField labelWithString:@""];
        counter_label.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightSemibold];
        counter_label.textColor = NSColor.whiteColor; counter_label.backgroundColor = NSColor.blackColor;
        counter_label.drawsBackground = YES; counter_label.frame = NSMakeRect(8, 8, 440, 64);
        [view addSubview:counter_label];
    }
    char clock[160]; host_frame_counter_clock(clock, sizeof(clock), counter_fixed, frame, now - counter_first);
    NSString *cpu=value.cpu_ms>=0 ? [NSString stringWithFormat:@"%.2f ms",value.cpu_ms] : @"n/a";
    NSString *gpu=value.gpu_ms>=0 ? [NSString stringWithFormat:@"%.2f ms",value.gpu_ms] : @"n/a";
    counter_label.stringValue = [NSString stringWithFormat:@"%@ | %s\n%.1f FPS | frame avg %.2f / max %.2f ms\nCPU %@ / GPU %@ (async)",
        counter_renderer, clock, value.fps, value.average_ms, value.worst_ms, cpu, gpu];
}

/* Peek without consuming quit, window, or controller events needed by the guest. */
static int benchmark_notice_stopped(void) {
    int stopped = 0;
    /* AppKit delivers input, but the guest must still receive every SDL event. */
    int count = SDL_PeepEvents(NULL, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST);
    SDL_Event *events = count > 0 ? SDL_malloc((size_t)count * sizeof(*events)) : NULL;
    if (events) {
        count = SDL_PeepEvents(events, count, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST);
        for (int i = 0; i < count; i++) {
            SDL_Event *event = &events[i];
            if (event->type == SDL_EVENT_QUIT || event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
                (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_ESCAPE) ||
                (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event->gbutton.button == SDL_GAMEPAD_BUTTON_BACK))
                stopped = 1;
        }
        SDL_free(events);
    }
    return stopped || host_benchmark_finish_requested();
}

int host_benchmark_notice(const char *heading, const char *detail, double seconds, int (*until)(void)) {
    if (!NSThread.isMainThread) return 0;
    NSPanel *panel=[[NSPanel alloc]initWithContentRect:NSMakeRect(0,0,520,160)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    panel.title=@(heading ?: "VISR");
    NSTextField *text=[NSTextField wrappingLabelWithString:@(detail ?: "")];
    text.frame=NSMakeRect(20,20,480,120);[panel.contentView addSubview:text];
    [panel center];[panel makeKeyAndOrderFront:nil];
    double end=CACurrentMediaTime()+seconds;int stopped=0;
    while(CACurrentMediaTime()<end && !(until && until())) {
        @autoreleasepool {
            SDL_PumpEvents();
            stopped = benchmark_notice_stopped();
            if(stopped)break;
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        }
    }
    [panel orderOut:nil];return stopped || host_benchmark_finish_requested();
}
