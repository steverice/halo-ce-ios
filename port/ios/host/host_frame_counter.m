/* debug.frame_counter is a host UI diagnostic. Wall time and rendered Present
   calls are independent of simulation ticks and replay time. Existing renderer
   timings feed a bounded window; UIKit text changes at most four times a second. */
#import <UIKit/UIKit.h>
#import <os/lock.h>
#include "ios_host.h"
#include "host_frame_counter_stats.h"
#include <stdatomic.h>
#include <time.h>

static UILabel *label;
static _Atomic int enabled, update_pending;
static _Atomic unsigned generation, text_updates;
static os_unfair_lock sample_lock = OS_UNFAIR_LOCK_INIT;
static struct host_frame_counter_stats stats;
static double first, next_update;
static NSString *renderer;
static int fixed;

static double wall_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + now.tv_nsec / 1e9;
}
static void on_main(dispatch_block_t block) {
    if (NSThread.isMainThread) block();
    else dispatch_async(dispatch_get_main_queue(), block);
}
static void remove_label(unsigned token) {
    on_main(^{
        if (token != atomic_load(&generation)) return;
        [label removeFromSuperview];
        label = nil;
    });
}
void host_frame_counter_stop(void) {
    atomic_store(&enabled, 0);
    unsigned token = atomic_fetch_add(&generation, 1) + 1;
    os_unfair_lock_lock(&sample_lock);
    host_frame_counter_stats_reset(&stats);
    os_unfair_lock_unlock(&sample_lock);
    remove_label(token);
}
void host_frame_counter_start(int fixed_timestep, int metal) {
    host_frame_counter_stop();
    unsigned token = atomic_fetch_add(&generation, 1) + 1;
    os_unfair_lock_lock(&sample_lock);
    first = wall_seconds(); next_update = first;
    fixed = fixed_timestep;
    renderer = metal ? @"Metal" : @"GL";
    atomic_store(&text_updates, 0);
    atomic_store(&enabled, 1);
    os_unfair_lock_unlock(&sample_lock);
    remove_label(token);
    host_logf(HOST_LOG_INFO, "frame counter on (%s; wall time, rendered frames; fixed timestep %s)",
              metal ? "Metal" : "GL", fixed_timestep ? "on" : "off");
}
void host_frame_counter_sample(double interval, double cpu, double gpu) {
    if (!atomic_load_explicit(&enabled, memory_order_relaxed)) return;
    os_unfair_lock_lock(&sample_lock);
    if (atomic_load(&enabled)) host_frame_counter_stats_sample(&stats, interval, cpu, gpu);
    os_unfair_lock_unlock(&sample_lock);
}
static UIView *overlay_root(void) {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        for (UIWindow *window in ((UIWindowScene *)scene).windows)
            if (window.isKeyWindow) return window.rootViewController.view;
    }
    return nil;
}
static UILabel *make_label(UIView *root) {
    UILabel *made = [UILabel new];
    made.font = [UIFont monospacedDigitSystemFontOfSize:12 weight:UIFontWeightSemibold];
    made.textColor = UIColor.whiteColor;
    made.backgroundColor = [UIColor colorWithWhite:0 alpha:0.65];
    made.textAlignment = NSTextAlignmentCenter;
    made.numberOfLines = 0;
    made.layer.cornerRadius = 6; made.clipsToBounds = YES;
    made.userInteractionEnabled = NO;
    made.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:made];
    [made.leadingAnchor constraintEqualToAnchor:root.safeAreaLayoutGuide.leadingAnchor constant:8].active = YES;
    [made.topAnchor constraintEqualToAnchor:root.safeAreaLayoutGuide.topAnchor constant:8].active = YES;
    [made.trailingAnchor constraintLessThanOrEqualToAnchor:root.safeAreaLayoutGuide.trailingAnchor constant:-8].active = YES;
    NSLayoutConstraint *width = [made.widthAnchor constraintEqualToConstant:440];
    width.priority = UILayoutPriorityDefaultHigh; width.active = YES;
    [made.heightAnchor constraintGreaterThanOrEqualToConstant:62].active = YES;
    return made;
}
static NSString *milliseconds(double value) {
    return value >= 0.0 ? [NSString stringWithFormat:@"%.2f ms", value] : @"n/a";
}
void host_frame_counter_show(unsigned long frame) {
    if (!atomic_load_explicit(&enabled, memory_order_relaxed)) return;
    double now = wall_seconds(), elapsed;
    struct host_frame_counter_summary value;
    unsigned token;
    NSString *name;
    int clock_fixed;
    os_unfair_lock_lock(&sample_lock);
    if (!frame) first = now;
    elapsed = now - first;
    if (frame % 600 == 0) host_logf(HOST_LOG_INFO, "frame counter: %lu, %.2f wall s; %u text updates",
                                   frame, elapsed, atomic_load(&text_updates));
    if (now < next_update || !atomic_load(&enabled)) { os_unfair_lock_unlock(&sample_lock); return; }
    if (atomic_exchange(&update_pending, 1)) { os_unfair_lock_unlock(&sample_lock); return; }
    next_update = now + 0.25;
    value = host_frame_counter_stats_read(&stats);
    host_frame_counter_stats_reset(&stats);
    token = atomic_load(&generation); name = renderer;
    clock_fixed = fixed;
    os_unfair_lock_unlock(&sample_lock);
    on_main(^{ @autoreleasepool {
        atomic_store(&update_pending, 0);
        if (!atomic_load(&enabled) || token != atomic_load(&generation)) return;
        UIView *root = overlay_root();
        if (label.superview != root) { [label removeFromSuperview]; label = nil; }
        if (!root) return;
        if (!label) label = make_label(root);
        char clock_text[160];
        host_frame_counter_clock(clock_text, sizeof(clock_text), clock_fixed, frame, elapsed);
        NSString *clock = [NSString stringWithUTF8String:clock_text];
        NSString *fps = value.fps > 0.0 ? [NSString stringWithFormat:@"%.1f FPS", value.fps] : @"FPS n/a";
        NSString *frames = value.frames ? [NSString stringWithFormat:@"frame avg %.2f / max %.2f ms", value.average_ms, value.worst_ms] : @"frame avg/max n/a";
        /* Metal reports work completed asynchronously, not utilization or a
           guaranteed measurement of the frame whose number is displayed. */
        label.text = [NSString stringWithFormat:@"%@ | %@\n%@ | %@\nCPU avg %@ | GPU avg %@ (async)",
                      name, clock, fps, frames, milliseconds(value.cpu_ms), milliseconds(value.gpu_ms)];
        atomic_fetch_add(&text_updates, 1);
    } });
}
