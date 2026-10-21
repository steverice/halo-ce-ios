/* The device benchmark's host side (host_benchmark.h).

   Everything it writes is in the data folder's "benchmark" folder (the process's
   working directory is the data folder): journal.txt, one run-NN.json per finished
   run, and report.json once the suite is over. The report holds the device's model
   identifier, system version, GPU and display, the app's version, and each run's
   settings and measurements. It never holds the device's name, an identifier of the
   device or its owner, an address, a path or game assets. Shader compilation descriptors accompany the measurements. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#if HALO_NATIVE_MAC
#import <AppKit/AppKit.h>
#include "host_macos.h"
#include <SDL3/SDL.h>
#else
#import <UIKit/UIKit.h>
#endif
#include "ios_host.h"
#include "guest_image.h"
#include "host_benchmark.h"
#include "host_benchmark_live.h"
#import <QuartzCore/CAMediaTiming.h>
#include "host_benchmark_page_policy.h"
#include "host_benchmark_offscreen.h"
#import "host_benchmark_presentation.h"
#import "host_benchmark_snapshot.h"
#import "host_data_updates.h"
#import "host_benchmark_catalog.h"
#import "host_benchmark_outbox.h"
#include "host_profile.h"
#include "host_config.h"
#include "host_external.h"
#include "benchmark_metrics.h"
#include "benchmark_plan.h"
#include "../../runtime/graphics_capture.h"
#include <math.h>
#include <mach/mach.h>
#include <os/lock.h>
#include <os/proc.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define FOLDER "benchmark"
#define JOURNAL FOLDER "/journal.txt"
#define REPORT FOLDER "/report.json"
#define REPORT_FORMAT 1
/* seconds on a still screen between runs, so one run's heat does not land on the next; and after a run that throttled */
#define PAUSE_SECONDS 15.0
#define PAUSE_AFTER_THROTTLE_SECONDS 60.0
/* how long a warm device is given to cool before the suite starts anyway, flagged */
#define COOL_WAIT_SECONDS 300.0
/* an offscreen case measures at most this much wall time of its window: a slow device measures what fits and
   moves on (the report's capped and measured_ticks); presented runs are never cut */
#define OFFSCREEN_WINDOW_WALL_SECONDS 60.0

static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
/* under the lock: the open window's measurements, and what happened during it */
static struct benchmark_metrics metrics;
static int window_open, window_interrupted, window_width, window_height;
static double refresh_hertz;
/* the screen's own rate, for a presenter that reports no refresh period (the Mac's) */
static double screen_hertz;

static _Atomic int running, finish_requested;
static struct benchmark_live live;
void host_benchmark_request_finish(void) { atomic_store(&finish_requested,1); }
int host_benchmark_finish_requested(void) { return atomic_load(&finish_requested); }
void host_benchmark_live_reset(void) {
    os_unfair_lock_lock(&lock);
    benchmark_live_reset(&live);
    os_unfair_lock_unlock(&lock);
}
void host_benchmark_live(struct benchmark_live_snapshot *snapshot) {
    os_unfair_lock_lock(&lock);
    if(atomic_load(&running))benchmark_live_read(&live,CACurrentMediaTime(),snapshot);
    else memset(snapshot,0,sizeof(*snapshot));
    os_unfair_lock_unlock(&lock);
}

static _Atomic int fault_seen;
/* every frame offered to host_benchmark_frame and every frame not shown, since the run began (the end's log line) */
static _Atomic unsigned long frames_offered, frames_no_drawable, frames_no_picture;
/* the run's game ticks so far, for the progress screen; when it began, in real time */
static _Atomic long run_ticks;
static CFAbsoluteTime run_started_at;
/* the run's measured ticks, when its window first opened, and whether its wall-time cap ended it */
static _Atomic long window_ticks;
static CFAbsoluteTime window_started_at;
static int run_capped;
static struct benchmark_plan plan;
static struct benchmark_outcome outcomes[BENCHMARK_RUNS_MAXIMUM];
static int outcome_count;
static struct benchmark_run run;
static int run_open, run_finishing;
static struct benchmark_outcome pending_outcome;
static struct benchmark_summary pending_summary;
static NSString *pending_note;
static char *pending_measurements;
static unsigned long pending_offered, pending_no_drawable, pending_no_picture;
static BOOL finish_run(NSDictionary *presentation);
static NSDictionary *pending_presentation;
static NSDictionary *run_shader_before, *run_shader_after;
static NSString *run_shader_directory;
static NSString *shader_directory(void) {
    char path[4096];
    host_config_string("debug.shader_list_record", "shader-lists-missed", path, sizeof(path));
    const char *override = getenv("HALO_SHADER_LIST_RECORD");
    return @(override ? override : path);
}
static char report_code[16];
static NSMutableArray<NSString *> *flags;
static NSDate *started;
static dispatch_source_t sampler;
static int last_run_throttled;
static NSDictionary *pinned_snapshot;
static NSString *catalog_status, *catalog_note;
static NSDictionary *run_environment_start, *run_environment_end, *run_output, *suite_output;
static unsigned run_output_generation;
static NSArray<NSString *> *external_modes;
static NSMutableArray<NSString *> *output_reports;
static NSUInteger external_mode_index;
static NSString *suite_request;
static int output_waiting, output_failed;
static CFAbsoluteTime output_wait_started;

static NSDictionary *output_now(void) {
    struct host_external_snapshot output;
    host_external_snapshot(&output);
    NSMutableArray *modes = [NSMutableArray array];
    for (int i = 0; i < output.mode_count && i < 64; i++)
        [modes addObject:@{ @"width": @(output.modes[i].width), @"height": @(output.modes[i].height) }];
    return @{ @"kind": output.external ? @"external" : @"builtin", @"width": @(output.width),
        @"height": @(output.height), @"refresh_hertz": @(output.refresh_hertz), @"name": @(output.name),
        @"reason": @(output.reason), @"offered_modes": modes };
}

static NSDictionary *environment_now(void) {
    float level = -1;
    NSString *charging = @"unavailable";
#if !TARGET_OS_TV && !TARGET_OS_VISION && !TARGET_OS_MACCATALYST && !HALO_NATIVE_MAC
    UIDevice.currentDevice.batteryMonitoringEnabled = YES;
    level = UIDevice.currentDevice.batteryLevel;
    switch (UIDevice.currentDevice.batteryState) {
        case UIDeviceBatteryStateUnplugged: charging = @"unplugged"; break;
        case UIDeviceBatteryStateCharging: charging = @"charging"; break;
        case UIDeviceBatteryStateFull: charging = @"full"; break;
        default: charging = @"unknown"; break;
    }
#endif
    return @{ @"battery_level": @(level), @"charging": charging,
        @"thermal_state": @((int)NSProcessInfo.processInfo.thermalState) };
}
#define JOURNAL_SNAPSHOT FOLDER "/journal-snapshot.sha256"

/* ---------- the crash marker */

static _Atomic int marker = -1;
static const int crash_signals[] = { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE, SIGTRAP };
#define CRASH_SIGNAL_COUNT ((int)(sizeof(crash_signals) / sizeof(crash_signals[0])))
static struct sigaction previous_actions[CRASH_SIGNAL_COUNT];

/* marks the journal, then hands the signal to whoever had it before (the default ends the app) */
static void crash_handler(int number, siginfo_t *info, void *context) {
    int index;

    benchmark_journal_mark_crash(atomic_load(&marker), number);
    for (index = 0; index < CRASH_SIGNAL_COUNT; index++) {
        struct sigaction *previous = &previous_actions[index];

        if (crash_signals[index] != number) continue;
        sigaction(number, previous, NULL);
        if ((previous->sa_flags & SA_SIGINFO) && previous->sa_sigaction) previous->sa_sigaction(number, info, context);
        else if (previous->sa_handler != SIG_DFL && previous->sa_handler != SIG_IGN) previous->sa_handler(number);
        else raise(number);
        return;
    }
}

void host_benchmark_install(void) {
    struct sigaction action = { 0 };
    int index;

    action.sa_sigaction = crash_handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (index = 0; index < CRASH_SIGNAL_COUNT; index++) sigaction(crash_signals[index], &action, &previous_actions[index]);
}

int host_benchmark_running(void) {
    return atomic_load(&running);
}

/* ---------- measuring */

static unsigned upscaler_requests, upscaler_paths;
static uint64_t upscaler_frames[5];
void host_benchmark_upscaler(const char *requested, unsigned effective_paths)
{
    if (!atomic_load(&running)) return;
    os_unfair_lock_lock(&lock);
    if (window_open) {
        upscaler_requests |= !strcmp(requested,"metalfx") ? HOST_UPSCALER_METALFX :
            !strcmp(requested,"bilinear") ? HOST_UPSCALER_BILINEAR : HOST_UPSCALER_UNKNOWN;
        unsigned path = effective_paths && !(effective_paths & ~15u) ? effective_paths : HOST_UPSCALER_UNKNOWN;
        upscaler_paths |= path;
        int index = path == HOST_UPSCALER_NONE ? 0 : path == HOST_UPSCALER_BILINEAR ? 1 :
            path == HOST_UPSCALER_METALFX ? 2 : path == HOST_UPSCALER_UNKNOWN ? 4 : 3;
        upscaler_frames[index]++;
    }
    os_unfair_lock_unlock(&lock);
}
static NSString *upscaler_name(unsigned paths)
{
    if (!paths || paths == HOST_UPSCALER_UNKNOWN) return @"unknown";
    if (paths == HOST_UPSCALER_NONE) return @"none";
    if (paths == HOST_UPSCALER_BILINEAR) return @"bilinear";
    if (paths == HOST_UPSCALER_METALFX) return @"metalfx";
    return @"mixed";
}
static NSDictionary *upscaler_metadata(void)
{
    os_unfair_lock_lock(&lock);
    NSDictionary *value = @{@"requested": upscaler_name(upscaler_requests), @"effective": upscaler_name(upscaler_paths),
        @"none_frames": @(upscaler_frames[0]), @"bilinear_frames": @(upscaler_frames[1]),
        @"metalfx_frames": @(upscaler_frames[2]), @"mixed_frames": @(upscaler_frames[3]), @"unknown_frames": @(upscaler_frames[4])};
    os_unfair_lock_unlock(&lock);
    return value;
}

static NSDictionary *graphics_first, *graphics_last;
static BOOL graphics_changed;
static NSUInteger graphics_snapshots, graphics_invalid;
static BOOL graphics_key_allowed(NSString *name)
{
    if (![name isKindOfClass:NSString.class] || name.length > 64) return NO;
    if ([name hasPrefix:@"display."]) return YES;
#define GRAPHICS_ALLOWED(key) if ([name isEqualToString:@key]) return YES;
    GRAPHICS_CAPTURE_DEBUG_KEYS(GRAPHICS_ALLOWED)
#undef GRAPHICS_ALLOWED
    return NO;
}
static BOOL graphics_snapshot_valid(NSDictionary *snapshot)
{
    if (![snapshot isKindOfClass:NSDictionary.class]) return NO;
    BOOL extended = [snapshot[@"schema"] isKindOfClass:NSNumber.class] && [snapshot[@"schema"] doubleValue] == 2;
    NSSet *keys = [NSSet setWithArray:extended ?
        @[@"schema",@"scene",@"configuration_revision",@"external_output",@"values",@"applied_overrides",@"renderer_state"] :
        @[@"schema",@"scene",@"configuration_revision",@"external_output",@"values"]];
    if (![[NSSet setWithArray:snapshot.allKeys] isEqual:keys] ||
        ![snapshot[@"schema"] isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)snapshot[@"schema"]) == CFBooleanGetTypeID() ||
        ![@[@1,@2] containsObject:snapshot[@"schema"]] || ![@[@"gameplay",@"menu",@"cutscene"] containsObject:snapshot[@"scene"]] ||
        ![snapshot[@"configuration_revision"] isKindOfClass:NSNumber.class] ||
        CFGetTypeID((__bridge CFTypeRef)snapshot[@"configuration_revision"]) == CFBooleanGetTypeID() ||
        CFNumberIsFloatType((__bridge CFNumberRef)snapshot[@"configuration_revision"]) ||
        [snapshot[@"configuration_revision"] doubleValue] < 0 || [snapshot[@"configuration_revision"] doubleValue] > 9007199254740991.0 ||
        ![snapshot[@"external_output"] isKindOfClass:NSNumber.class] ||
        CFGetTypeID((__bridge CFTypeRef)snapshot[@"external_output"]) != CFBooleanGetTypeID()) return NO;
    NSDictionary *values = snapshot[@"values"];
    if (![values isKindOfClass:NSDictionary.class] || !values.count || values.count > CONFIG_GRAPHICS_MAXIMUM_KEYS) return NO;
    for (NSString *name in values) {
        if (!graphics_key_allowed(name)) return NO;
        id value = values[name];
        if ([value isKindOfClass:NSString.class]) {
            if ([value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > CONFIG_GRAPHICS_MAXIMUM_STRING_BYTES) return NO;
        } else if (![value isKindOfClass:NSNumber.class] || !isfinite([value doubleValue])) return NO;
    }
    if (extended) {
        NSDictionary *overrides = snapshot[@"applied_overrides"], *renderer = snapshot[@"renderer_state"];
        if (![overrides isKindOfClass:NSDictionary.class] || ![renderer isKindOfClass:NSDictionary.class] ||
            ![[NSSet setWithArray:overrides.allKeys] isEqual:[NSSet setWithArray:
                @[@"render_lines",@"shadow_map_size",@"anisotropic_filtering",@"model_lod"]]] ||
            ![[NSSet setWithArray:renderer.allKeys] isEqual:[NSSet setWithArray:
                @[@"shadow_target_size",@"anisotropic_sampler_max",@"model_lod",@"texture_replacement_selections",
                    @"texture_replacement_source",@"upscaled_texture_assets"]]]) return NO;
        for (NSString *key in @[@"render_lines",@"shadow_map_size",@"anisotropic_filtering"]) {
            id value = overrides[key];
            if (![value isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() ||
                CFNumberIsFloatType((__bridge CFNumberRef)value) || [value longLongValue] < 0 || [value longLongValue] > 8192) return NO;
        }
        if (![@[@"auto",@"max"] containsObject:overrides[@"model_lod"]] ||
            ![@[@"auto",@"max",@"unavailable"] containsObject:renderer[@"model_lod"]]) return NO;
        for (NSString *key in @[@"shadow_target_size",@"anisotropic_sampler_max"]) {
            id value = renderer[key];
            if (value == NSNull.null) continue;
            if (![value isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() ||
                CFNumberIsFloatType((__bridge CFNumberRef)value) || [value longLongValue] < 1 || [value longLongValue] > 8192) return NO;
        }
        id selections = renderer[@"texture_replacement_selections"];
        if (![selections isKindOfClass:NSNumber.class] || CFGetTypeID((__bridge CFTypeRef)selections) == CFBooleanGetTypeID() ||
            CFNumberIsFloatType((__bridge CFNumberRef)selections) || [selections longLongValue] < 0 ||
            [selections doubleValue] > 9007199254740991.0 ||
            ![@[@"debug_directory",@"unavailable"] containsObject:renderer[@"texture_replacement_source"]] ||
            ![renderer[@"upscaled_texture_assets"] isEqual:@"unavailable"] ||
            ([selections longLongValue] > 0) != [renderer[@"texture_replacement_source"] isEqual:@"debug_directory"]) return NO;
    }
    return YES;
}
void host_benchmark_graphics(const char *json)
{
    if (!atomic_load(&running)) return;
    size_t length = json ? strnlen(json,CONFIG_GRAPHICS_MAXIMUM_BYTES) : 0;
    NSDictionary *snapshot = length && length < CONFIG_GRAPHICS_MAXIMUM_BYTES ?
        [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:json length:length] options:0 error:NULL] : nil;
    BOOL valid = graphics_snapshot_valid(snapshot);
    os_unfair_lock_lock(&lock);
    if (window_open) {
        if (valid) {
            if (graphics_last && (![graphics_last[@"values"] isEqual:snapshot[@"values"]] ||
                ![graphics_last[@"scene"] isEqual:snapshot[@"scene"]] ||
                ![graphics_last[@"external_output"] isEqual:snapshot[@"external_output"]] ||
                ((graphics_last[@"applied_overrides"] || snapshot[@"applied_overrides"]) &&
                    ![graphics_last[@"applied_overrides"] isEqual:snapshot[@"applied_overrides"]]))) graphics_changed = YES;
            if (!graphics_first) graphics_first = snapshot;
            graphics_last = snapshot; graphics_snapshots++;
        } else graphics_invalid++;
    }
    os_unfair_lock_unlock(&lock);
}
static NSDictionary *graphics_metadata(void)
{
    os_unfair_lock_lock(&lock);
    NSDictionary *result = @{@"available": @(graphics_first != nil), @"first": graphics_first ?: @{}, @"last": graphics_last ?: @{},
        @"changed": @(graphics_changed), @"snapshots": @(graphics_snapshots), @"invalid_snapshots": @(graphics_invalid)};
    os_unfair_lock_unlock(&lock);
    return result;
}

static _Atomic int offscreen_decided = HOST_BENCHMARK_OFFSCREEN_OFF;
/* host_benchmark_presented: the frames are shown, in real time, for now */
static _Atomic int presented_now;

int host_benchmark_offscreen_request(int setting, int suite_named) {
#ifdef HALO_TESTER_BUILD
    /* the tester's benchmark measures offscreen, time-compressed, wherever the device allows it */
    if (suite_named) {
        char suite[64];
        const char *named = getenv("HALO_BENCHMARK_SUITE");

        if (!named) { host_config_string("debug.benchmark_suite", "", suite, sizeof(suite)); named = suite; }
        if (!strncmp(named, "full-unified", 12)) setting = 1;
    }
#endif
    enum host_benchmark_offscreen decided = host_benchmark_offscreen_decide(setting, suite_named, TARGET_OS_VISION);

    atomic_store(&offscreen_decided, decided);
    if (decided == HOST_BENCHMARK_OFFSCREEN_ON)
        host_logf(HOST_LOG_INFO, "benchmark: debug.benchmark_offscreen: the suite's frames are drawn offscreen, uncapped, "
            "one tick each");
    else if (decided == HOST_BENCHMARK_OFFSCREEN_NO_SUITE)
        host_logf(HOST_LOG_INFO, "benchmark: debug.benchmark_offscreen ignored: no debug.benchmark_suite at launch");
    else if (decided == HOST_BENCHMARK_OFFSCREEN_STEREO)
        host_logf(HOST_LOG_INFO, "benchmark: debug.benchmark_offscreen ignored: Vision Pro runs in real time");
    return decided == HOST_BENCHMARK_OFFSCREEN_ON;
}

int host_benchmark_offscreen_requested(void) {
    return atomic_load(&offscreen_decided) == HOST_BENCHMARK_OFFSCREEN_ON;
}

int host_benchmark_offscreen_active(void) {
    return host_benchmark_offscreen_frame(atomic_load(&offscreen_decided), atomic_load(&running)) &&
        !atomic_load(&presented_now);
}

int host_benchmark_presented(int on) {
    on = on != 0;
    if (!host_benchmark_offscreen_requested()) return 0;
    if (atomic_exchange(&presented_now, on) != on) {
        gpu_metal_benchmark_realtime(on);
        host_logf(HOST_LOG_INFO, "benchmark: frames %s", on ? "shown in real time" : "drawn offscreen, time-compressed");
    }
    return 1;
}

void host_benchmark_frame_unshown(int why) {
    if (!atomic_load(&running)) return;
    atomic_fetch_add(why ? &frames_no_picture : &frames_no_drawable, 1);
}

/* the game's marks since the last present, on the pacing's clock: when the open phase began, and the seconds summed
   in ticks [0], in the render [1] and in the render's translation [2] */
static os_unfair_lock split_lock = OS_UNFAIR_LOCK_INIT;
static double split_began[2], split_seconds[3];
static int split_ticks, split_marked, split_translated;

void host_frame_cpu_phase(int phase, int ticks) {
    double now = CACurrentMediaTime();
    int which = phase >= HOST_FRAME_RENDER_BEGIN;
    if (phase == HOST_FRAME_TRANSLATION) {
        if (ticks < 0) return;
        os_unfair_lock_lock(&split_lock);
        split_seconds[2] += (double)ticks / 1e9;
        split_translated = 1;
        os_unfair_lock_unlock(&split_lock);
        return;
    }
    os_unfair_lock_lock(&split_lock);
    if (phase == HOST_FRAME_SIMULATION_BEGIN || phase == HOST_FRAME_RENDER_BEGIN) split_began[which] = now;
    else if (split_began[which] > 0.0) {
        split_seconds[which] += now - split_began[which];
        split_began[which] = 0.0;
        split_marked = 1;
        if (phase == HOST_FRAME_SIMULATION_END && ticks > 0) split_ticks += ticks;
    }
    os_unfair_lock_unlock(&split_lock);
}

void host_frame_cpu_split(double *simulation, double *render, double *translation, int *ticks) {
    os_unfair_lock_lock(&split_lock);
    *simulation = split_marked ? split_seconds[0] : -1.0;
    *render = split_marked ? split_seconds[1] : -1.0;
    *translation = split_marked && split_translated ? split_seconds[2] : -1.0;
    *ticks = split_ticks;
    split_seconds[0] = split_seconds[1] = split_seconds[2] = 0.0;
    split_ticks = split_marked = split_translated = 0;
    os_unfair_lock_unlock(&split_lock);
}

void host_benchmark_frame(double interval, double cpu, double gpu, double refresh_period, double simulation_cpu,
    double render_cpu, double translation_cpu, int ticks) {
    host_frame_counter_sample(interval, cpu, gpu);
    if (!atomic_load(&running)) return;
    atomic_fetch_add(&frames_offered, 1);
    if (ticks > 0) atomic_fetch_add(&run_ticks, ticks);
    os_unfair_lock_lock(&lock);
    benchmark_live_add(&live,CACurrentMediaTime(),interval,cpu,gpu);
    if (refresh_period > 0.0) refresh_hertz = 1.0 / refresh_period;
    if (ticks > 0 && window_open) atomic_fetch_add(&window_ticks, ticks);
    if (window_open && interval > 0.0) {
        if (metrics.refresh_hertz <= 0.0 && refresh_hertz > 0.0) metrics.refresh_hertz = refresh_hertz;
        benchmark_metrics_frame_split(&metrics, interval * 1000.0, cpu * 1000.0, gpu * 1000.0,
            simulation_cpu >= 0.0 ? simulation_cpu * 1000.0 : -1.0, render_cpu >= 0.0 ? render_cpu * 1000.0 : -1.0,
            translation_cpu >= 0.0 ? translation_cpu * 1000.0 : -1.0, ticks);
    }
    os_unfair_lock_unlock(&lock);
}

void host_benchmark_compile(double seconds) {
    if (!atomic_load(&running)) return;
    os_unfair_lock_lock(&lock);
    if (window_open) benchmark_metrics_compile(&metrics, seconds * 1000.0);
    os_unfair_lock_unlock(&lock);
}

static void memory_now(long *footprint, long *left) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;

    *footprint = *left = -1;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        *footprint = (long)(info.phys_footprint >> 20);
#if !TARGET_OS_MACCATALYST && !TARGET_OS_SIMULATOR && !HALO_NATIVE_MAC
    *left = (long)(os_proc_available_memory() >> 20);
#endif
}

/* once a second while a suite runs: memory and heat into the open window, and a memory line in the journal every
   five seconds of a run, which is what tells a memory kill from another end */
static void sample(void) {
    static int tick;
    long footprint, left;
    int thermal = (int)NSProcessInfo.processInfo.thermalState, open;

    if (!atomic_load(&running)) return;
    memory_now(&footprint, &left);
    os_unfair_lock_lock(&lock);
    open = window_open;
    if (open) benchmark_metrics_sample(&metrics, footprint, left, thermal);
    os_unfair_lock_unlock(&lock);
    if (atomic_load(&running) && run_open && ++tick % 5 == 0)
        benchmark_journal_memory(JOURNAL, footprint, left);
}

static void sampler_start(void) {
    if (sampler) return;
    sampler = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_queue_create("halo.benchmark", DISPATCH_QUEUE_SERIAL));
    dispatch_source_set_timer(sampler, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
    dispatch_source_set_event_handler(sampler, ^{ sample(); });
    dispatch_resume(sampler);
#if HALO_NATIVE_MAC
    NSNotificationName inactive=NSApplicationDidResignActiveNotification;
#else
    NSNotificationName inactive=UISceneDidEnterBackgroundNotification;
#endif
    [NSNotificationCenter.defaultCenter addObserverForName:inactive object:nil queue:nil
        usingBlock:^(NSNotification *notification) {
            (void)notification;
            os_unfair_lock_lock(&lock);
            if (window_open) window_interrupted = 1;
            os_unfair_lock_unlock(&lock);
        }];
}

static double screen_rate(void);
static void screen_size(double *width, double *height);

/* ---------- the report */

static NSString *json_string(NSString *text) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:text ?: @"" options:NSJSONWritingFragmentsAllowed error:nil];

    return data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"\"\"";
}

static NSString *json_object(id value) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:value ?: @{} options:NSJSONWritingSortedKeys error:nil];
    return data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"{}";
}

static NSString *model_identifier(void) {
    char text[64] = "";
    size_t size = sizeof(text);
#if TARGET_OS_SIMULATOR
    const char *simulated = getenv("SIMULATOR_MODEL_IDENTIFIER");

    return [NSString stringWithFormat:@"%s (simulator)", simulated ? simulated : "unknown"];
#elif TARGET_OS_MACCATALYST || HALO_NATIVE_MAC
    sysctlbyname("hw.model", text, &size, NULL, 0);
#else
    sysctlbyname("hw.machine", text, &size, NULL, 0);
#endif
    return @(text);
}

static NSString *platform_name(void) {
#if TARGET_OS_TV
    return @"tvos";
#elif TARGET_OS_VISION
    return @"visionos";
#elif TARGET_OS_MACCATALYST || HALO_NATIVE_MAC
    return @"mac";
#else
    return @"ios";
#endif
}

/* the highest Apple GPU family the device supports, probed by number so a family newer than the SDK counts */
static int gpu_family(id<MTLDevice> device) {
    int family, highest = 0;

    for (family = 1; family <= 20; family++)
        if ([device supportsFamily:(MTLGPUFamily)(1000 + family)]) highest = family;
    return highest;
}

/* the report's first members: the device, the app and the suite; never a name or an identifier of the device */
/* whether the pinned snapshot's catalog came from a signed recordings pack */
static BOOL catalog_signed(void) {
    return [pinned_snapshot[@"updates"][@"benchmark_catalog_status"] isEqual:@"signed"];
}

static NSString *report_header(void) {
    NSProcessInfo *process = NSProcessInfo.processInfo;
    NSOperatingSystemVersion version = process.operatingSystemVersion;
    id<MTLDevice> gpu = MTLCreateSystemDefaultDevice();
    double screen_width, screen_height;

    screen_size(&screen_width, &screen_height);
    NSDateFormatter *minutes = [NSDateFormatter new];
    char chip[128] = "";
    size_t chip_size = sizeof(chip);

    minutes.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    minutes.timeZone = [NSTimeZone timeZoneForSecondsFromGMT:0];
    minutes.dateFormat = @"yyyy-MM-dd'T'HH:mm'Z'";
#if TARGET_OS_MACCATALYST || HALO_NATIVE_MAC
    sysctlbyname("machdep.cpu.brand_string", chip, &chip_size, NULL, 0);
#else
    (void)chip_size;
#endif
    return [NSString stringWithFormat:@"\"format\":%d,\"code\":%@,\"suite\":%d,\"mode\":\"%s\",\"started\":\"%@\","
        "\"device\":{\"model\":%@,\"chip\":%@,\"platform\":\"%@\",\"os\":\"%ld.%ld.%ld\",\"os_build\":%@,"
        "\"gpu\":%@,\"gpu_family\":%d,\"memory_mb\":%llu,"
        "\"screen\":{\"width\":%.0f,\"height\":%.0f,\"maximum_frames_a_second\":%ld}},"
        "\"external_modes_complete\":%s,\"output\":%@,\"output_reports\":%@,\"requested_output\":%@,\"app\":{\"version\":%@,\"build\":%@,\"image\":\"%s\"},"
        "\"profile\":{\"selection\":%@,\"source_hardware\":%@,\"target_hardware\":%@,\"source_tier\":%d,\"target_tier\":%d,\"catalog_version\":%@,\"class\":%@,\"model\":%@,\"drop_in\":%s,\"drawable_lines\":%ld,\"render_lines\":%ld,\"anisotropic_filtering\":%ld,\"shadow_map_size\":%ld,"
        "\"model_lod\":\"%s\"},\"data_updates\":%@,\"recording_snapshot\":%@,\"profile_sha256\":%@,"
        "\"benchmark_catalog\":{\"status\":%@,\"sha256\":%@,\"note\":%@},\"catalog_signed\":%s,\"flags\":%@",
        REPORT_FORMAT, json_string(@(report_code)), BENCHMARK_SUITE_VERSION, benchmark_mode_name(plan.mode),
        [minutes stringFromDate:started ?: NSDate.date],
        json_string(model_identifier()), json_string(@(chip)), platform_name(), (long)version.majorVersion,
        (long)version.minorVersion, (long)version.patchVersion,
        /* "Version 26.6 (Build 23M5311a)": the build's name alone */
        json_string([[process.operatingSystemVersionString componentsSeparatedByString:@"Build "].lastObject
            stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@")"]]),
        json_string(gpu.name), gpu ? gpu_family(gpu) : 0, process.physicalMemory >> 20,
        screen_width, screen_height, (long)screen_rate(),
        external_modes.count && external_mode_index + 1 < external_modes.count ? "false" : "true",
        json_object(suite_output ?: output_now()), json_object(output_reports ?: @[]),
        json_string(external_modes.count ? external_modes[external_mode_index] : @""),
        json_string([NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"]),
        json_string([NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"]), HALO_GUEST_SHA256,
        json_string(host_device_profile()->inherited ? @"inherited" : host_device_profile()->model_file[0] ? @"exact" : @"class"),
        json_string(@(host_device_profile()->source_hardware)), json_string(@(host_device_profile()->target_hardware)),
        host_device_profile()->source_tier, host_device_profile()->target_tier, json_string(@(host_device_profile()->catalog_version)),
        json_string(@(host_device_profile()->class_file)), json_string(@(host_device_profile()->model_file)),
        host_device_profile()->drop_in ? "true" : "false",
        plan.device.drawable_lines, plan.device.base_lines, plan.device.anisotropy, plan.device.shadow_size,
        plan.device.model_lod_max ? "max" : "auto",
        json_object(pinned_snapshot[@"updates"]), json_string(pinned_snapshot[@"sha256"]),
        json_string(pinned_snapshot[@"identity"][@"profile_sha256"]),
        json_string(catalog_status ?: @"unavailable"),
        json_string(pinned_snapshot[@"identity"][@"catalog_sha256"] ?: @"unavailable"),
        json_string(catalog_note ?: @""), catalog_signed() ? "true" : "false",
        [[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:flags options:0 error:nil] encoding:NSUTF8StringEncoding]];
}

static NSString *run_path(int index) {
    return [NSString stringWithFormat:@FOLDER "/run-%02d.json", index];
}

/* The session snapshot is pinned before execution and retained across resume.
   Disclose its replay policy without annotating the route/error note channel. */
static NSDictionary *replay_environment_for_recording(const char *name) {
    return host_benchmark_snapshot_replay_environment(@FOLDER, pinned_snapshot, @(name));
}

/* a setting as the run had it: the text of the configuration's value for it, or the default when there is none */
static NSString *configured_text(const char *name, const char *fallback) {
    char value[64];

    host_config_string(name, fallback, value, sizeof(value));
    return @(value);
}

/* The values of the settings a cost run varies (tools/cost_suite.py), as the run had them: the case's own where it sets
   one, the configuration's otherwise, so every run of a suite names the same settings. The simulation changes read
   "on" or "off", the other switches "true" or "false"; render_lines, filtering, shadows and detail are the run's own. */
static void record_applied_settings(NSMutableDictionary *settings, const struct benchmark_scene *scene, char stage) {
    static const char *const switches[] = { "debug.collision_type_filter", "debug.sphere_query_type_filter",
        "debug.static_light_relink", "debug.immediate_merge", "debug.collision_candidates" };
    /* (each switch's default in port_config.c, for a configuration that does not name it) */
    static const char *const defaults[] = { "false", "false", "false", "false", "true" };
    const struct benchmark_knobs *knobs = &scene->knobs;
    unsigned mask = stage == 'A' ? knobs->mask : 0;

    /* (each switch's default in port_config.c, for a configuration that does not name it) */
    static const char *const defaults[] = { "true", "true", "true", "false", "true" };
    for (unsigned bit = 0; bit < sizeof(switches) / sizeof(switches[0]); bit++) {
        NSString *configured = configured_text(switches[bit], defaults[bit]);
        BOOL enabled = scene->sim_experiments_mask & (1u << bit) ?
            (scene->sim_experiments & (1u << bit)) != 0 :
            ![@[@"0", @"false", @"no", @"off"] containsObject:configured.lowercaseString];
        settings[@(switches[bit])] = enabled ? @"on" : @"off";
    }
    settings[@"display.anti_aliasing"] = mask & BENCHMARK_KNOB_ANTI_ALIASING ?
        @[@"off", @"fxaa", @"smaa"][knobs->anti_aliasing] : configured_text("display.anti_aliasing", "off");
    settings[@"display.effect_resolution"] = mask & BENCHMARK_KNOB_EFFECT ? (knobs->effect_full ? @"true" : @"false") :
        configured_text("display.effect_resolution", "true");
    settings[@"display.mirror_resolution"] = mask & BENCHMARK_KNOB_MIRROR ? (knobs->mirror_full ? @"full" : @"half") :
        configured_text("display.mirror_resolution", "half");
    settings[@"display.render_scale"] = mask & BENCHMARK_KNOB_RENDER_SCALE ?
        [NSString stringWithFormat:@"%g", knobs->render_scale] : [NSString stringWithFormat:@"%g", host_config_real("display.render_scale", 1.0)];
    settings[@"display.clutter_distance"] = [NSString stringWithFormat:@"%g", mask & BENCHMARK_KNOB_CLUTTER_DISTANCE ?
        knobs->clutter_distance : host_config_real("display.clutter_distance", 0.0)];
    settings[@"debug.clutter_lowest_lod"] = mask & BENCHMARK_KNOB_CLUTTER_LOD ? (knobs->clutter_lowest_lod ? @"on" : @"off") :
        [configured_text("debug.clutter_lowest_lod", "false") isEqual:@"true"] ? @"on" : @"off";
}

/* one finished run as a JSON object in its own file; the report is these put together */
static BOOL write_run(const struct benchmark_run *finished, enum benchmark_status status, NSString *note,
    const char *measurements, NSDictionary *presentation) {
    const struct benchmark_scene *scene = &plan.scenes[finished->scene];
    NSString *text = [NSString stringWithFormat:@"{\"index\":%d,\"stage\":\"%c\",\"scene\":\"%s\",\"map\":\"%s\","
        "\"difficulty\":\"%s\",\"recording\":%@,\"kind\":\"%s\",\"settings\":{\"render_lines\":%ld,"
        "\"anisotropic_filtering\":%ld,\"shadow_map_size\":%ld,\"model_lod\":\"%s\"},\"status\":\"%s\",\"note\":%@,"
        "\"render_size\":[%d,%d],\"output\":%@,\"environment\":%@%s%s}",
        finished->index, finished->stage, scene->id, scene->map, scene->difficulty,
        scene->recording ? json_string(@(scene->recording)) : @"null",
        scene->kind == BENCHMARK_SCENE_MENU ? "menu" : scene->kind == BENCHMARK_SCENE_CUTSCENE ? "cutscene" :
            scene->kind == BENCHMARK_SCENE_MULTIPLAYER ? "multiplayer_synthetic" : "gameplay",
        /* (a baseline run leaves the size to the profile: what it drew at is the setting it ran with) */
        finished->lines ? finished->lines : (long)window_height, finished->anisotropy, finished->shadow_size,
        finished->model_lod_max ? "max" : "auto",
        benchmark_status_name(status), json_string(note), window_width, window_height, json_object(run_output ?: suite_output ?: output_now()),
        json_object(@{ @"start": run_environment_start ?: @{}, @"end": run_environment_end ?: environment_now() }), measurements ? "," : "",
        measurements ?: ""];

    NSMutableDictionary *record = [[NSJSONSerialization JSONObjectWithData:[text dataUsingEncoding:NSUTF8StringEncoding] options:NSJSONReadingMutableContainers error:nil] mutableCopy];
    NSDictionary *upscaler = upscaler_metadata();
    record[@"upscaler"] = upscaler;
    record[@"graphics"] = graphics_metadata();
    record[@"catalog_signed"] = @(catalog_signed());
    NSMutableDictionary *settings = [record[@"settings"] mutableCopy];
    settings[@"upscaler_requested"] = upscaler[@"requested"];
    settings[@"sim_experiments"] = @(scene->sim_experiments);
    settings[@"sim_experiments_mask"] = @(scene->sim_experiments_mask);
    record_applied_settings(settings, scene, finished->stage);
    record[@"workload"] = @(scene->workload ?: scene->id);
    record[@"settings"] = settings;
    if (scene->recording && scene->recording[0])
        record[@"replay_environment"] = replay_environment_for_recording(scene->recording);
    if (scene->kind == BENCHMARK_SCENE_MULTIPLAYER)
        record[@"simulation"] = @{ @"mode": @(scene->simulation ?: ""), @"requested_players": @(scene->players),
            @"requested_views": @(scene->views), @"game_mode": @(scene->game_mode ?: ""),
            @"expected_local_players": @(!strcmp(scene->simulation, "network_players") ? 1 : scene->players),
            @"expected_synthetic_players": @(!strcmp(scene->simulation, "network_players") ? scene->players - 1 : 0),
            @"expected_external_clients": @0, @"bot_workload": @3,
            @"score_limit": @(scene->score_limit), @"time_limit_minutes": @(scene->time_limit_minutes),
            @"requested_arena": @{ @"columns": @(scene->arena_columns), @"spacing_milliunits": @(llround(scene->arena_spacing * 1000)),
                @"distance_milliunits": @(llround(scene->arena_distance * 1000)), @"movement_radius_milliunits": @(llround(scene->arena_movement_radius * 1000)) },
            @"warmup_ticks": @((long)llround(scene->lead * 30.0)), @"window_ticks": @((long)llround(scene->seconds * 30.0)) };
    NSString *map = scene->map[0] ? @(scene->map) : @"ui";
    record[@"shader_capture"] = host_benchmark_shader_delta(run_shader_before,
        run_shader_after ?: host_benchmark_shader_snapshot(run_shader_directory, map));
    /* (a presented reality check inside an offscreen suite measures what a player sees) */
    int offscreen = host_benchmark_offscreen_requested() && finished->stage != 'P';
    record[@"presented"] = @(!offscreen);
    record[@"reality_check"] = @(finished->stage == 'P');
    record[@"measured_ticks"] = @(atomic_load(&window_ticks));
    record[@"capped"] = @(run_capped != 0);
    if (run_started_at > 0) record[@"wall_seconds"] = @(round((CFAbsoluteTimeGetCurrent() - run_started_at) * 10.0) / 10.0);
    text = json_object(host_benchmark_measurement_record(record, presentation, offscreen));
    return [text writeToFile:run_path(finished->index) atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

static BOOL write_report(void) {
    NSMutableString *report = [NSMutableString stringWithFormat:@"{%@,\"runs\":[", report_header()];
    int index, written = 0;
    NSMutableArray *runs = [NSMutableArray array];

    for (index = 0; index < outcome_count; index++) {
        NSString *text = [NSString stringWithContentsOfFile:run_path(index) encoding:NSUTF8StringEncoding error:nil];

        if (!text) return NO;
        NSDictionary *record = [NSJSONSerialization JSONObjectWithData:[text dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
        if (!record) return NO;
        [runs addObject:record];
        [report appendFormat:@"%s\n%@", written++ ? "," : "", text];
    }
    NSMutableArray *previous = [NSMutableArray array];
    NSString *current = [NSString stringWithFormat:@"output-%02lu.json", (unsigned long)external_mode_index];
    for (NSString *name in output_reports) {
        if ([name isEqual:current]) continue;
        NSData *data = [NSData dataWithContentsOfFile:[@FOLDER stringByAppendingPathComponent:name]];
        NSDictionary *prior = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        if (prior) [previous addObject:prior];
    }
    NSDictionary *evidence = host_benchmark_shader_evidence(runs, previous, shader_directory());
    [report appendFormat:@"\n],\"shader_dependency_candidates\":%@,\"shader_output_intervals\":%@}\n",
        json_object(evidence[@"shader_dependency_candidates"]), json_object(evidence[@"shader_output_intervals"])];
    if(![report writeToFile:@REPORT atomically:YES encoding:NSUTF8StringEncoding error:nil])return NO;
    host_logf(HOST_LOG_INFO, "benchmark: report %s, %d runs, %lu bytes: %s/" REPORT, report_code, written,
        (unsigned long)report.length, NSFileManager.defaultManager.currentDirectoryPath.fileSystemRepresentation);
    return YES;
}

/* a few lines a person can read: each baseline scene's frame rate */
static NSString *report_summary(void) {
    NSMutableString *summary = [NSMutableString string];
    int index;

    for (index = 0; index < outcome_count; index++) {
        NSData *data = [NSData dataWithContentsOfFile:run_path(index)];
        NSDictionary *one = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;

        if (![one isKindOfClass:NSDictionary.class] || ![one[@"stage"] isEqual:@"A"]) continue;
        if ([one[@"status"] isEqual:@"ok"]) {
            [summary appendString:host_benchmark_submission_summary(one)];
        } else {
            NSDictionary *said = @{ @"skipped": @"not run on this device", @"diverged": @"the replay left its route",
                @"thermal": @"the device got too hot", @"interrupted": @"interrupted", @"crashed": @"the app stopped",
                @"killed": @"the app was stopped by the system", @"failed": @"did not run", @"partial": @"finished early; partial measurements retained" };

            [summary appendFormat:@"%@: %@\n", one[@"scene"], said[one[@"status"]] ?: one[@"status"]];
        }
    }
    return summary;
}

/* ---------- the guest's calls (guest_host.h) */

static void flag(NSString *name) {
    if (![flags containsObject:name]) [flags addObject:name];
}

/* the main screen's highest rate and its size in pixels; visionOS has no screen (UIScreen): its displays run at 90 */
static double screen_rate(void) {
#if HALO_NATIVE_MAC
    return host_macos_refresh_rate();
#elif TARGET_OS_VISION
    return 90.0;
#else
    return (double)UIScreen.mainScreen.maximumFramesPerSecond;
#endif
}

static void screen_size(double *width, double *height) {
#if HALO_NATIVE_MAC
    int w=0,h=0;
    SDL_Window *window=host_sdl_window();
    if(window)SDL_GetWindowSizeInPixels(window,&w,&h);
    *width=w;*height=h;
#elif TARGET_OS_VISION
    *width = *height = 0.0;
#else
    *width = UIScreen.mainScreen.nativeBounds.size.width;
    *height = UIScreen.mainScreen.nativeBounds.size.height;
#endif
}

static int cooled(void) {
    return NSProcessInfo.processInfo.thermalState <= NSProcessInfoThermalStateNominal;
}

/* A build that carries routes (the Mac runner, development builds; never a tester or release build, which
   tools/release_validation.py checks) runs an unsigned local catalog when no signed one was downloaded:
   benchmark/local-routes with its own benchmark-catalog.json (tools/replay_check.py writes one). Every report
   from it says catalog_signed false. nil when there is none or the build cannot have one. */
static NSString *local_catalog_folder(void) {
#if HALO_BENCHMARK_NO_ROUTES
    return nil;
#else
    NSString *local = @FOLDER "/local-routes";
    return [NSFileManager.defaultManager fileExistsAtPath:[local stringByAppendingPathComponent:@HOST_BENCHMARK_CATALOG_NAME]] ?
        local : nil;
#endif
}

static NSString *active_catalog_sha256(void) {
    NSString *bundled = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"benchmark-recordings"];
    NSDictionary *updates = host_data_updates_metadata();
    NSDictionary *recording_update = updates[@"recordings"];
    BOOL signed_feed = [recording_update isKindOfClass:NSDictionary.class] && ![recording_update[@"bundled"] boolValue];
    NSString *sha = signed_feed ? host_benchmark_catalog_file_sha256(host_recording_update_folder(bundled)) : nil;
    NSString *local = sha ? nil : local_catalog_folder();
    if (local) sha = host_benchmark_catalog_file_sha256(local);
    return sha ?: @"unavailable";
}

/* A resumed journal must describe the same guest, catalog, resolved profile and baseline settings. The pinned
   snapshot still owns the exact replay files, so a later feed update cannot replace an unfinished suite. */
static NSDictionary *snapshot_identity(const char *mode) {
    const struct host_profile *profile = host_device_profile();
    NSMutableArray *values = [NSMutableArray array];
    for (int i = 0; i < profile->count; i++) {
        const struct host_profile_value *value = &profile->values[i];
        [values addObject:@{ @"name": @(value->name), @"value": @(value->text), @"scene": @(value->scene), @"external": @(value->external), @"source": @(value->source) }];
    }
    NSDictionary *resolved = @{ @"values": values, @"class": @(profile->class_file), @"model": @(profile->model_file),
        @"class_name": @(profile->class_name), @"model_name": @(profile->model_name), @"drop_in": @(profile->drop_in),
        @"inherited": @(profile->inherited), @"source_hardware": @(profile->source_hardware),
        @"target_hardware": @(profile->target_hardware), @"source_tier": @(profile->source_tier),
        @"target_tier": @(profile->target_tier), @"catalog_version": @(profile->catalog_version) };
    return @{ @"guest": @HALO_GUEST_SHA256, @"profile_sha256": host_benchmark_snapshot_digest(resolved),
        @"catalog_sha256": active_catalog_sha256(),
        @"output_mode": external_modes.count ? external_modes[external_mode_index] : @"",
        @"suite": @(BENCHMARK_SUITE_VERSION), @"mode": @(mode),
        @"app_build": [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"",
        @"device": @[ @(plan.device.drawable_lines), @(plan.device.base_lines), @(plan.device.anisotropy),
            @(plan.device.shadow_size), @(plan.device.model_lod_max) ] };
}

static NSDictionary *fresh_snapshot(NSDictionary *identity) {
    NSString *bundled = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"benchmark-recordings"];
    NSString *selected = host_recording_update_folder(bundled);
    NSMutableArray *sources = [NSMutableArray array];
    NSMutableDictionary *updates = [host_data_updates_metadata() mutableCopy];
    if (!updates) updates = [NSMutableDictionary dictionary];
    NSDictionary *recording_update = updates[@"recordings"];
    BOOL signed_feed = [recording_update isKindOfClass:NSDictionary.class] && ![recording_update[@"bundled"] boolValue];
    BOOL has_catalog = signed_feed && selected && [NSFileManager.defaultManager
        fileExistsAtPath:[selected stringByAppendingPathComponent:@HOST_BENCHMARK_CATALOG_NAME]];
    if (has_catalog) [sources addObject:selected];
    updates[@"benchmark_catalog_status"] = has_catalog ? @"signed" : @"unavailable";
    NSString *local = has_catalog ? nil : local_catalog_folder();
    if (local) {
        [sources addObject:local];
        updates[@"benchmark_catalog_status"] = @"local_unsigned";
        host_logf(HOST_LOG_INFO, "benchmark: " HOST_BENCHMARK_LOCAL_CATALOG_NOTE);
    }
    NSError *error = nil;
    NSDictionary *snapshot = host_benchmark_snapshot_create(@FOLDER, sources, identity, updates, &error);
    if (!snapshot) host_logf(HOST_LOG_INFO, "benchmark: cannot prepare recording snapshot: %s", error.localizedDescription.UTF8String);
    return snapshot;
}

static void install_scenes(NSArray<NSString *> *only) {
    static struct host_benchmark_catalog catalog;
    static struct benchmark_scene selected[BENCHMARK_SCENES_MAXIMUM];
    const char *filters[BENCHMARK_SCENES_MAXIMUM];
    int filter_count = (int)only.count;
    NSError *error = nil;
    NSString *folder = @FOLDER "/recordings";

    catalog_status = @"available";
    catalog_note = @"";
    if (filter_count > BENCHMARK_SCENES_MAXIMUM) {
        catalog_status = @"unavailable";
        catalog_note = @"Too many benchmark cases were selected.";
        plan.scene_count = 0;
        plan.scenes = selected;
        return;
    }
    BOOL loaded = host_benchmark_catalog_load(folder, &catalog, &error);
    NSString *loaded_sha = loaded ? [NSString stringWithUTF8String:catalog.sha256] : nil;
    if (!loaded || ![loaded_sha isEqualToString:pinned_snapshot[@"identity"][@"catalog_sha256"]]) {
        catalog_status = @"unavailable";
        catalog_note = error.localizedDescription ?: @"The signed benchmark catalog identity does not match the snapshot.";
        host_logf(HOST_LOG_INFO, "benchmark: catalog unavailable: %s", catalog_note.UTF8String);
        plan.scene_count = 0;
        plan.scenes = selected;
        return;
    }
    for (int index = 0; index < filter_count; index++) filters[index] = only[index].UTF8String;
    int count = benchmark_scenes_select(plan.mode, catalog.scenes, catalog.count,
        only ? filters : NULL, filter_count, selected, BENCHMARK_SCENES_MAXIMUM);
    if (count <= 0) {
        catalog_status = @"unavailable";
        catalog_note = count < 0 ? @"The catalog case list exceeded runtime capacity." : @"No catalog cases matched this selection.";
        host_logf(HOST_LOG_INFO, "benchmark: %s", catalog_note.UTF8String);
        count = 0;
    }
    plan.scenes = selected;
    plan.scene_count = count;
}

int host_benchmark_start(const char *mode, int drawable_lines, int base_lines, int anisotropy, int shadow_size,
    int model_lod_max) {
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   /* nothing that reads as another */
    struct benchmark_journal journal;
    NSProcessInfo *process = NSProcessInfo.processInfo;
    NSFileManager *files = NSFileManager.defaultManager;
    int index;

    if (atomic_load(&running)) return 1;
    if (!suite_request) {
        suite_request = @(mode);
        char configured[1024];
        host_config_string("debug.external_mode", "", configured, sizeof(configured));
        const char *requested = getenv("HALO_EXTERNAL_MODE");
        if (!requested) requested = configured;
        external_modes = requested && *requested ? [@(requested) componentsSeparatedByString:@","] : @[];
        output_reports = [NSMutableArray array];
        external_mode_index = 0;
        if (external_modes.count) {
            host_external_force(external_modes[0].UTF8String);
            output_waiting = 1;
            output_wait_started = CFAbsoluteTimeGetCurrent();
        }
    }
    if (process.lowPowerModeEnabled) {
        host_logf(HOST_LOG_INFO, "benchmark: Low Power Mode is on; the benchmark does not run with it");
        return 0;
    }
    output_failed = 0;
    mkdir(FOLDER, 0755);
    /* Preserve the previous report before any new suite can replace its run files. */
    NSData *previous=[NSData dataWithContentsOfFile:@REPORT];
    if(previous && !host_benchmark_outbox_store(@FOLDER "/outbox",previous,NULL)) {
        host_logf(HOST_LOG_INFO,"benchmark: cannot preserve previous report; refusing to replace it");
        return 0;
    }
    /* ("full-unified:a10-mess-hall,mp16-chillout": the unified pass over the named cases only) */
    /* ("cost:a10-mess-hall-min,a10-mess-hall-max": the unified pass over the named cases, which set their own values,
       with no presented checks after; tools/cost_suite.py writes the catalog) */
    int cost_suite = !strncmp(mode, "cost:", 5);
    plan.mode = cost_suite || !strcmp(mode,"full-unified") || !strncmp(mode,"full-unified:",13) ? BENCHMARK_UNIFIED : !strncmp(mode, "full", 4) ? BENCHMARK_FULL : BENCHMARK_QUICK;
    plan.device = (struct benchmark_device){ drawable_lines, base_lines, anisotropy, shadow_size, model_lod_max };
    plan.reality_checks = plan.mode == BENCHMARK_UNIFIED && !cost_suite && host_benchmark_offscreen_requested();
    benchmark_journal_load(JOURNAL, &journal);
    NSDictionary *identity = snapshot_identity(mode);
    pinned_snapshot = journal.valid && !journal.finished ? host_benchmark_snapshot_load(@FOLDER, identity) : nil;
    NSString *journal_pin = [NSString stringWithContentsOfFile:@JOURNAL_SNAPSHOT encoding:NSUTF8StringEncoding error:nil];
    BOOL pinned_resume = pinned_snapshot && [journal_pin isEqual:pinned_snapshot[@"sha256"]];
    if (!pinned_resume) {
        if(journal.valid && !journal.finished && journal.count>0 && !host_benchmark_archive_interrupted(@FOLDER,NULL))return 0;
        pinned_snapshot = fresh_snapshot(identity);
    }
    if (!pinned_snapshot) return 0;
    flags = [NSMutableArray array];
    if (host_benchmark_offscreen_requested()) flag(@"benchmark_offscreen");
    started = NSDate.date;
    /* "quick:a10-crew,c40-bridge": only the scenes named after the colon */
    NSArray<NSString *> *only = strchr(mode, ':') ? [@(strchr(mode, ':') + 1) componentsSeparatedByString:@","] : nil;
    install_scenes(only);
    if ([catalog_status isEqual:@"unavailable"]) flag(@"benchmark_catalog_unavailable");
    else if (!catalog_signed()) flag(@"benchmark_catalog_unsigned");
    if (pinned_resume && !(journal.mode == plan.mode && journal.signature == benchmark_plan_signature(&plan) &&
        journal.device.drawable_lines == plan.device.drawable_lines && journal.device.base_lines == plan.device.base_lines &&
        journal.device.anisotropy == plan.device.anisotropy && journal.device.shadow_size == plan.device.shadow_size &&
        journal.device.model_lod_max == plan.device.model_lod_max)) {
        pinned_resume = NO;
        if(!host_benchmark_archive_interrupted(@FOLDER,NULL))return 0;
        pinned_snapshot = fresh_snapshot(identity);
        if (!pinned_snapshot) return 0;
        install_scenes(only);
    }
    if (pinned_resume && journal.valid && !journal.finished && journal.mode == plan.mode && journal.signature == benchmark_plan_signature(&plan) &&
        journal.device.drawable_lines == plan.device.drawable_lines && journal.device.base_lines == plan.device.base_lines &&
        journal.device.anisotropy == plan.device.anisotropy && journal.device.shadow_size == plan.device.shadow_size &&
        journal.device.model_lod_max == plan.device.model_lod_max) {
        /* a suite that was cut off carries on after the run it was cut in */
        snprintf(report_code, sizeof(report_code), "%s", journal.id);
        outcome_count = journal.count;
        memcpy(outcomes, journal.outcomes, sizeof(outcomes));
        flag(@"resumed");
        if (journal.cut_off) {
            struct benchmark_run cut;
            struct benchmark_outcome *outcome = &outcomes[outcome_count - 1];

            benchmark_journal_end(JOURNAL, outcome_count - 1, outcome);
            if (benchmark_plan_next(&plan, outcomes, outcome_count - 1, &cut))
                write_run(&cut, outcome->status, journal.crash_signal ?
                    [NSString stringWithFormat:@"the app ended during this run with signal %d", journal.crash_signal] :
                    [NSString stringWithFormat:@"the app ended during this run with no signal seen; its last memory sample was "
                        "%ld MB in use, %ld MB left before the system's limit", journal.last_footprint, journal.last_left], NULL, nil);
            flag(outcome->status == BENCHMARK_CRASHED ? @"crashed" : @"ended_by_system");
            host_logf(HOST_LOG_INFO, "benchmark: carrying on suite %s after run %d, which %s", report_code, outcome_count - 1,
                journal.crash_signal ? "crashed" : "the system ended");
        } else host_logf(HOST_LOG_INFO, "benchmark: carrying on suite %s after run %d", report_code, outcome_count - 1);
    } else {
        for (index = 0; index < 8; index++) report_code[index] = alphabet[arc4random_uniform(sizeof(alphabet) - 1)];
        report_code[8] = 0;
        outcome_count = 0;
        for (index = 0; index < BENCHMARK_RUNS_MAXIMUM; index++) [files removeItemAtPath:run_path(index) error:nil];
        [files removeItemAtPath:@REPORT error:nil];
        if (!benchmark_journal_start(JOURNAL, plan.mode, report_code, &plan.device, benchmark_plan_signature(&plan))) {
            host_logf(HOST_LOG_INFO, "benchmark: cannot write %s (is the device's storage full?)", JOURNAL);
            return 0;
        }
        if (![pinned_snapshot[@"sha256"] writeToFile:@JOURNAL_SNAPSHOT atomically:YES encoding:NSUTF8StringEncoding error:nil]) {
            host_logf(HOST_LOG_INFO, "benchmark: cannot bind the journal to its recording snapshot");
            return 0;
        }
        host_logf(HOST_LOG_INFO, "benchmark: suite %d %s, report %s; display %d lines, in force: %d lines, %dx filtering, "
            "%d shadows, model detail %s", BENCHMARK_SUITE_VERSION, mode, report_code, drawable_lines, base_lines, anisotropy,
            shadow_size, model_lod_max ? "max" : "auto");
    }
    atomic_store(&marker, benchmark_journal_marker_open(JOURNAL));
    atomic_store(&fault_seen, 0);
    atomic_store(&finish_requested,0);
    host_benchmark_live_reset();
    atomic_store(&running, 1);
    if(plan.mode==BENCHMARK_UNIFIED)host_benchmark_overlay_show(1);
    sampler_start();

    /* conditions: a warm device gets time to cool, then runs flagged; the display's rate is recorded as it is */
    if (process.thermalState > NSProcessInfoThermalStateNominal) {
        host_logf(HOST_LOG_INFO, "benchmark: the device is warm (thermal state %ld); waiting up to %.0f s for it to cool",
            (long)process.thermalState, COOL_WAIT_SECONDS);
        if (host_benchmark_notice("The device is warm", "Waiting for it to cool before measuring, for up to five minutes.",
            COOL_WAIT_SECONDS, cooled)) {
            host_logf(HOST_LOG_INFO, "benchmark: stopped from the screen before the first run");
            host_benchmark_request_finish();
        }
        if (process.thermalState > NSProcessInfoThermalStateNominal) flag(@"warm_at_start");
    }
    suite_output = output_now();
    screen_hertz = [suite_output[@"refresh_hertz"] doubleValue];
    if (!output_waiting && screen_hertz != 60.0) flag(@"refresh_not_60");
    if(external_modes.count && !output_waiting && host_external_ready() &&
        host_benchmark_notice("Display mode selected","Waiting for the display to settle before measuring.",PAUSE_SECONDS,NULL)) {
        host_benchmark_request_finish();
    }
    if (only) flag(@"scenes_filtered");
    /* a drop-in profile is somebody trying values: the report must never be read as the shipped profile's */
    if (host_device_profile()->drop_in) flag(@"drop_in_profile");
#if TARGET_OS_SIMULATOR
    flag(@"simulator");
#endif
    return 1;
}

/* a presented reality check's window: the scene's, up to BENCHMARK_REALITY_CHECK_SECONDS */
static double reality_check_seconds(const struct benchmark_scene *scene) {
    return scene->seconds > 0.0 && scene->seconds < BENCHMARK_REALITY_CHECK_SECONDS ? scene->seconds :
        BENCHMARK_REALITY_CHECK_SECONDS;
}

/* a scene by a name a player knows: the level, or the map and its players */
static NSString *scene_title(const struct benchmark_scene *scene) {
    static NSDictionary<NSString *, NSString *> *names;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        names = @{ @"a10": @"The Pillar of Autumn", @"a30": @"Halo", @"a50": @"The Truth and Reconciliation",
            @"b30": @"The Silent Cartographer", @"b40": @"Assault on the Control Room", @"c10": @"343 Guilty Spark",
            @"c20": @"The Library", @"c40": @"Two Betrayals", @"d20": @"Keyes", @"d40": @"The Maw",
            @"beavercreek": @"Battle Creek", @"bloodgulch": @"Blood Gulch", @"boardingaction": @"Boarding Action",
            @"carousel": @"Derelict", @"chillout": @"Chill Out", @"damnation": @"Damnation", @"dangercanyon": @"Danger Canyon",
            @"deathisland": @"Death Island", @"gephyrophobia": @"Gephyrophobia", @"hangemhigh": @"Hang 'Em High",
            @"icefields": @"Ice Fields", @"infinity": @"Infinity", @"longest": @"Longest", @"prisoner": @"Prisoner",
            @"putput": @"Chiron TL34", @"ratrace": @"Rat Race", @"sidewinder": @"Sidewinder", @"timberland": @"Timberland",
            @"wizard": @"Wizard" };
    });
    NSString *map = @(scene->map).lastPathComponent.lowercaseString;
    NSString *name = names[map];
    if (scene->kind == BENCHMARK_SCENE_MULTIPLAYER)
        return [NSString stringWithFormat:@"%@, %d players", name ?: @"Multiplayer", scene->players];
    if (scene->kind == BENCHMARK_SCENE_MENU) return @"Main menu";
    return name ?: @"Campaign";
}

void host_benchmark_progress(struct host_benchmark_progress *progress) {
    memset(progress, 0, sizeof(*progress));
    progress->offscreen = host_benchmark_offscreen_active();
    if (!atomic_load(&running) || !run_open || run.scene < 0 || run.scene >= plan.scene_count) return;
    const struct benchmark_scene *scene = &plan.scenes[run.scene];
    double lead = scene->lead * 30.0, window = (run.stage == 'P' ? reality_check_seconds(scene) :
        scene->seconds > 0.0 ? scene->seconds : 60.0) * 30.0;
    double within = lead + window > 0.0 ? fmin(1.0, (double)atomic_load(&run_ticks) / (lead + window)) : 0.0;

    snprintf(progress->title, sizeof(progress->title), "%s", scene_title(scene).UTF8String);
    progress->number = outcome_count + 1;
    progress->total = benchmark_plan_total(&plan, outcomes, outcome_count);
    if (progress->total < progress->number) progress->total = progress->number;
    progress->fraction = fmin(1.0, ((double)outcome_count + within) / (double)progress->total);
    progress->reality_check = run.stage == 'P';
}

int host_benchmark_next(char *text, unsigned int size) {
    const struct benchmark_scene *scene;

    if(!text && !size) {
        if (atomic_load(&finish_requested)) return 5;
        if (run_open && !run_finishing && window_started_at > 0 && host_benchmark_offscreen_active() &&
            CFAbsoluteTimeGetCurrent() - window_started_at >= OFFSCREEN_WINDOW_WALL_SECONDS) {
            if (!run_capped) host_logf(HOST_LOG_INFO, "benchmark: run %d: the window's %.0f s of wall time are up after %ld ticks",
                run.index, OFFSCREEN_WINDOW_WALL_SECONDS, atomic_load(&window_ticks));
            run_capped = 1;
            return 6;
        }
        return 0;
    }
    if (!atomic_load(&running)) return 0;
    if (run_finishing) {
        if(!pending_presentation)pending_presentation=host_benchmark_presentation_take_report();
        if (!pending_presentation || !finish_run(pending_presentation)) return 3;
        pending_presentation=nil;
    }
    if (output_waiting && !atomic_load(&finish_requested)) {
        if (!host_external_ready() && CFAbsoluteTimeGetCurrent() - output_wait_started < 5.0) return 3;
        output_waiting = 0;
        suite_output = output_now();
        screen_hertz = [suite_output[@"refresh_hertz"] doubleValue];
        refresh_hertz = screen_hertz;
        if (!host_external_ready() || ![suite_output[@"kind"] isEqual:@"external"]) {
            flag(@"external_output_unavailable");
            output_failed = 1;
        }
        NSString *requested = external_modes[external_mode_index];
        NSString *actual = [NSString stringWithFormat:@"%@x%@", suite_output[@"width"], suite_output[@"height"]];
        if (screen_hertz < 60.0 || (![requested isEqual:@"preferred"] && ![requested isEqual:actual])) {
            flag(@"external_output_mismatch");
            output_failed = 1;
        }
        if (screen_hertz != 60.0) flag(@"refresh_not_60");
        if(output_failed) {
            pinned_snapshot=fresh_snapshot(snapshot_identity(suite_request.UTF8String));
            outcome_count=0;
        }
        if(!output_failed) {
            int descriptor=atomic_exchange(&marker,-1);
            if(descriptor>=0)close(descriptor);
            atomic_store(&running,0);
            /* The guest recomputes drawable/profile sizing on its next frame. */
            return 4;
        }
        if (host_benchmark_notice("Display mode selected", "Waiting for the display to settle before measuring.", PAUSE_SECONDS, NULL)) {
            host_benchmark_request_finish();
            return host_benchmark_next(text,size);
        }
    }
    if (atomic_load(&finish_requested) || output_failed || !benchmark_plan_next(&plan, outcomes, outcome_count, &run)) {
        const char *page = getenv("HALO_BENCHMARK_PAGE");
        int page_minutes = host_benchmark_page_minutes(page);
        int completion_screen_requested = page && atoi(page) > 0;
#ifdef HALO_TESTER_BUILD
        /* Internal tester builds offer local download as a fallback. */
        if (!page) completion_screen_requested = 1;
#endif
        /* (a scripted launch with debug.benchmark_offscreen quits; a tester's goes back to real time and its menu) */
#ifndef HALO_TESTER_BUILD
        if (host_benchmark_offscreen_requested()) completion_screen_requested = 0;
#endif
        int descriptor = atomic_exchange(&marker, -1);

        if(atomic_load(&finish_requested))flag(@"finished_early");
        if (descriptor >= 0) close(descriptor);
        if (external_modes.count) {
            NSString *path = [NSString stringWithFormat:@FOLDER "/output-%02lu.json", (unsigned long)external_mode_index];
            [output_reports addObject:path.lastPathComponent];
            if(!write_report())return 3;
            [NSFileManager.defaultManager removeItemAtPath:path error:nil];
            [NSFileManager.defaultManager copyItemAtPath:@REPORT toPath:path error:nil];
            if (!atomic_load(&finish_requested) && external_mode_index + 1 < external_modes.count) {
                external_mode_index++;
                /* Each mode owns its outcomes, flags and report identity, even
                   when negotiation fails before a replan can start any run. */
                outcome_count=0;
                memset(outcomes,0,sizeof(outcomes));
                flags=[NSMutableArray array];
                started=NSDate.date;
                output_failed=0;
                static const char alphabet[]="ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
                for(int i=0;i<8;i++)report_code[i]=alphabet[arc4random_uniform(sizeof(alphabet)-1)];
                report_code[8]=0;
                host_external_force(external_modes[external_mode_index].UTF8String);
                output_waiting = 1;
                output_wait_started = CFAbsoluteTimeGetCurrent();
                return 3;
            }
        } else if(!write_report())return 3;
        benchmark_journal_done(JOURNAL);
        NSString *summary = report_summary();
        host_benchmark_overlay_show(0);
        NSError *preservation_error=nil;
        BOOL preserved=host_benchmark_outbox_store(@FOLDER "/outbox",[NSData dataWithContentsOfFile:@REPORT],&preservation_error)!=nil;
        if(!preserved)host_logf(HOST_LOG_INFO,"benchmark: report remains at report.json; outbox preservation failed: %s",
            preservation_error.localizedDescription.UTF8String ?: "unknown error");
#ifdef HALO_BENCHMARK_CLOUD
        /* Tester benchmarks deliver automatically; scripted builds opt in with
           their existing launch setting. The outbox precedes every attempt. */
#ifndef HALO_TESTER_BUILD
        if (getenv("HALO_BENCHMARK_SEND"))
#endif
        {
            char message[256];

            host_benchmark_send(REPORT, summary.UTF8String, message, sizeof(message));
            host_logf(HOST_LOG_INFO, "benchmark: %s", message);
            summary = [NSString stringWithFormat:@"%s\n%@", message, summary];
#ifdef HALO_TESTER_BUILD
            host_benchmark_menu_status(message);
#endif
        }
#endif
        if (plan.mode!=BENCHMARK_UNIFIED && page_minutes > 0) host_benchmark_page(REPORT, summary.UTF8String, report_code, page_minutes);
        atomic_store(&running, 0);
        suite_request = nil;
#ifdef HALO_TESTER_BUILD
        if(plan.mode==BENCHMARK_UNIFIED && completion_screen_requested) {
#ifndef HALO_BENCHMARK_CLOUD
            host_benchmark_menu_status(preserved ? "Benchmark data saved on this device. Cloud upload is unavailable in this build." :
                "Could not queue the report. The original is retained; another benchmark cannot replace it.");
#endif
            const char *again=host_benchmark_menu();
            return again ? 4 : 2;
        }
#endif
        /* A requested completion screen returns to the main menu (2); a runner or scripted launch quits (0). */
        return completion_screen_requested ? 2 : 0;
    }
    scene = &plan.scenes[run.scene];
    if (outcome_count) {
        /* the pause between runs, on a still screen that says what comes next; Back there stops the suite, which
           carries on from its journal the next time it is started */
        char heading[96], detail[160];
        double minutes = -[started timeIntervalSinceNow] / 60.0;

        int total = benchmark_plan_total(&plan, outcomes, outcome_count);

        if (total > 0) {
            snprintf(heading, sizeof(heading), "Next: %s", scene_title(scene).UTF8String);
            snprintf(detail, sizeof(detail), "%sPart %d of %d%s. Started %.0f minute%s ago.",
                last_run_throttled ? "The device got hot: cooling for a minute. " : "", run.index + 1, total,
                run.stage == 'P' ? ", played in real time" : "", minutes, minutes >= 1.5 || minutes < 0.5 ? "s" : "");
        } else {
            snprintf(heading, sizeof(heading), "Benchmark: run %d is next", run.index + 1);
            snprintf(detail, sizeof(detail), "%s%s, %ld lines. Started %.0f minute%s ago.",
                last_run_throttled ? "The device got hot: cooling for a minute. Then " : "", scene->id, run.lines, minutes,
                minutes >= 1.5 || minutes < 0.5 ? "s" : "");
        }
        if (host_benchmark_notice(heading, detail, last_run_throttled ? PAUSE_AFTER_THROTTLE_SECONDS : PAUSE_SECONDS, NULL)) {
            host_logf(HOST_LOG_INFO, "benchmark: stopped from the screen before run %d; the journal keeps what is done", run.index);
            flag(@"stopped");
            host_benchmark_request_finish();
            return host_benchmark_next(text,size);
        }
    }
    /* (only the baseline run of the unified pass sets what its case asks for: benchmark_plan.c) */
    struct benchmark_knobs knobs = run.stage == 'A' ? scene->knobs : (struct benchmark_knobs){0};
    snprintf(text, size, "scene=%s map=%s difficulty=%s recording=%s kind=%s lead=%.1f seconds=%.1f "
        "simulation=%s players=%d views=%d game_mode=%s score_limit=%d time_limit_minutes=%d "
        "arena_columns=%d arena_spacing=%.6f arena_distance=%.6f arena_radius=%.6f enemy_multiplier=%.3f "
        "sim_experiments=%u sim_experiments_mask=%u lines=%ld anisotropy=%ld shadow=%ld lod=%d presented=%d offscreen=%d knob_aa=%d "
        "knob_effect=%d knob_mirror=%d knob_scale=%.3f knob_clutter=%.3f knob_clutter_lod=%d", scene->id, scene->map, scene->difficulty, scene->recording ? scene->recording : "",
        scene->kind == BENCHMARK_SCENE_MENU ? "menu" : scene->kind == BENCHMARK_SCENE_CUTSCENE ? "cutscene" :
            scene->kind == BENCHMARK_SCENE_MULTIPLAYER ? "multiplayer_synthetic" : "gameplay",
        scene->lead, run.stage == 'P' ? reality_check_seconds(scene) : scene->seconds, scene->simulation ? scene->simulation : "",
        scene->players, scene->views,
        scene->game_mode ? scene->game_mode : "", scene->score_limit, scene->time_limit_minutes,
        scene->arena_columns, scene->arena_spacing, scene->arena_distance, scene->arena_movement_radius,
        scene->enemy_multiplier, scene->sim_experiments, scene->sim_experiments_mask,
        run.lines, run.anisotropy, run.shadow_size, run.model_lod_max, run.stage == 'P',
        host_benchmark_offscreen_requested(),
        /* (-1 for a setting the case leaves to the configuration) */
        knobs.mask & BENCHMARK_KNOB_ANTI_ALIASING ? knobs.anti_aliasing : -1,
        knobs.mask & BENCHMARK_KNOB_EFFECT ? knobs.effect_full : -1, knobs.mask & BENCHMARK_KNOB_MIRROR ? knobs.mirror_full : -1,
        knobs.mask & BENCHMARK_KNOB_RENDER_SCALE ? knobs.render_scale : 0.0,
        knobs.mask & BENCHMARK_KNOB_CLUTTER_DISTANCE ? knobs.clutter_distance : -1.0,
        knobs.mask & BENCHMARK_KNOB_CLUTTER_LOD ? knobs.clutter_lowest_lod : -1);
    atomic_store(&run_ticks, 0);
    atomic_store(&window_ticks, 0);
    run_started_at = CFAbsoluteTimeGetCurrent();
    window_started_at = 0;
    run_capped = 0;
    os_unfair_lock_lock(&lock);
    window_open = window_interrupted = window_width = window_height = 0;
    benchmark_live_reset(&live);
    /* <= 2 GiB devices retain a 4 MiB collector; others allow 16 MiB. */
    host_benchmark_presentation_begin(scene->kind,
        NSProcessInfo.processInfo.physicalMemory <= (2ULL << 30) ? 65536 : 262144);
    atomic_store(&frames_offered, 0); atomic_store(&frames_no_drawable, 0); atomic_store(&frames_no_picture, 0);
    benchmark_metrics_reset(&metrics, refresh_hertz);
    upscaler_requests = upscaler_paths = 0; memset(upscaler_frames,0,sizeof(upscaler_frames));
    graphics_first = graphics_last = nil; graphics_changed = NO; graphics_snapshots = graphics_invalid = 0;
    os_unfair_lock_unlock(&lock);
    run_shader_after = nil;
    run_environment_end = nil;
    run_shader_directory = shader_directory();
    run_shader_before = host_benchmark_shader_snapshot(run_shader_directory, scene->map[0] ? @(scene->map) : @"ui");
    benchmark_journal_begin(JOURNAL, run.index, run.key);
    run_environment_start = environment_now();
    run_output = output_now();
    struct host_external_snapshot output;
    host_external_snapshot(&output);
    run_output_generation = output.generation;
    run_open = 1;
    host_logf(HOST_LOG_INFO, "benchmark: run %d: %s", run.index, run.key);
    return 1;
}

void host_benchmark_window(int open, int width, int height, int opened_tick) {
    os_unfair_lock_lock(&lock);
    host_benchmark_presentation_window();
    if(window_open!=open)benchmark_live_reset(&live);
    window_open = open;
    if (open) {
        if (window_started_at <= 0) window_started_at = CFAbsoluteTimeGetCurrent();
        window_width = width;
        window_height = height;
        /* (the tick of the first frame counted: a window that opens again, as a cutscene gives way to the game, keeps it) */
        if (metrics.frames == 0) metrics.window_opened_tick = opened_tick;
        if (metrics.refresh_hertz <= 0.0) metrics.refresh_hertz = refresh_hertz > 0.0 ? refresh_hertz : screen_hertz;
    }
    os_unfair_lock_unlock(&lock);
}

void host_benchmark_end(const char *status_name, const char *note) {
    struct benchmark_outcome outcome = { BENCHMARK_FAILED, 0, 0, 0, 0, -1 };
    struct benchmark_summary summary;
    char *measurements = NULL;
    int status, interrupted;

    if (!run_open || run_finishing) return;
    for (status = 0; status < BENCHMARK_STATUS_COUNT; status++)
        if (!strcmp(status_name, benchmark_status_name((enum benchmark_status)status))) outcome.status = (enum benchmark_status)status;
    os_unfair_lock_lock(&lock);
    window_open = 0;
    benchmark_live_reset(&live);
    /* Admission closes on this thread before it can return to rendering.
       The worker never needs the legacy metrics lock. */
    host_benchmark_presentation_end_async();
    interrupted = window_interrupted;
    benchmark_metrics_summarize(&metrics, &summary);
    if (summary.frames) measurements = benchmark_metrics_json(&metrics);
    outcome.left_least = metrics.left_least;
    last_run_throttled = metrics.thermal_worst >= BENCHMARK_THERMAL_SERIOUS;
    os_unfair_lock_unlock(&lock);
    outcome.interval_p99 = summary.interval.higher;
    outcome.cpu_p95 = summary.cpu.high;
    outcome.gpu_p95 = summary.gpu.high;
    outcome.refresh_hertz = summary.refresh_hertz;
    /* what happened around a run outranks how it ended: the app left the screen, the device throttled, or a run
       that says it went well has no frames */
    struct host_external_snapshot output;
    host_external_snapshot(&output);
    if (output.generation != run_output_generation) {
        interrupted = 1;
        flag(@"output_changed");
        note = "the display connected, disconnected, or changed mode during this run";
    }
    if (external_modes.count) {
        NSString *wanted = external_modes[external_mode_index];
        NSString *actual = [NSString stringWithFormat:@"%dx%d", output.width, output.height];
        if (!output.external || (![wanted isEqual:@"preferred"] && ![wanted isEqual:actual]) || output.refresh_hertz < 60) {
            interrupted = 1;
            flag(@"external_output_mismatch");
            note = "the requested external output was not active at 60 Hz or better";
        }
    }
    if (outcome.status == BENCHMARK_OK) {
        if (interrupted) outcome.status = BENCHMARK_INTERRUPTED;
        else if (last_run_throttled) outcome.status = BENCHMARK_THERMAL;
        else if (!summary.frames) outcome.status = BENCHMARK_FAILED;
    }
    if (outcome.status == BENCHMARK_INTERRUPTED) flag(@"interrupted");
    if (outcome.status == BENCHMARK_THERMAL) flag(@"throttled");
    if (outcome.status == BENCHMARK_DIVERGED) flag(@"diverged");
    pending_offered = atomic_load(&frames_offered);
    pending_no_drawable = atomic_load(&frames_no_drawable);
    pending_no_picture = atomic_load(&frames_no_picture);
    run_environment_end = environment_now();
    const char *map = plan.scenes[run.scene].map;
    run_shader_after = host_benchmark_shader_snapshot(run_shader_directory, map[0] ? @(map) : @"ui");
    pending_outcome = outcome;
    pending_summary = summary;
    pending_note = @(note ?: "");
    pending_measurements = measurements;
    run_finishing = 1;
}

/* Called on the guest's assertion path. The next guest frame may be the blue
   halt screen forever, so finalize here rather than waiting for next_run. */
void host_benchmark_fault(const char *note) {
    struct benchmark_outcome outcome = { BENCHMARK_FAILED, 0, 0, 0, 0, -1 };
    int descriptor;

    if (atomic_exchange(&fault_seen, 1)) return;
    atomic_store(&running, 0);
    if (!run_open) return;
    os_unfair_lock_lock(&lock);
    window_open = 0;
    benchmark_live_reset(&live);
    os_unfair_lock_unlock(&lock);
    /* A normal end may already have dispatched its presentation worker. Do
       not finalize the same collector concurrently; use its report if ready. */
    free(pending_measurements);
    pending_measurements = NULL;
    pending_note = nil;
    NSDictionary *presentation = run_finishing ? host_benchmark_presentation_take_report() :
        host_benchmark_presentation_finish(0.0);
    if (!presentation) presentation = @{ @"supported": NSNull.null, @"capability": @"unknown",
        @"status": @"unavailable_at_guest_fault", @"interpretation": @"partial_failure_receipt" };
    else {
        NSMutableDictionary *partial = [presentation mutableCopy];
        partial[@"collector_status"] = presentation[@"status"] ?: @"unknown";
        partial[@"status"] = @"guest_fault_partial";
        partial[@"interpretation"] = @"partial_failure_receipt";
        presentation = partial;
    }
    run_environment_end = environment_now();
    const char *map = plan.scenes[run.scene].map;
    run_shader_after = host_benchmark_shader_snapshot(run_shader_directory, map[0] ? @(map) : @"ui");
    flag(@"guest_fault");
    write_run(&run, outcome.status, @(note ?: "guest fatal assertion"), NULL, presentation);
    benchmark_journal_end(JOURNAL, run.index, &outcome);
    run_open = run_finishing = 0;
    if (outcome_count < BENCHMARK_RUNS_MAXIMUM) outcomes[outcome_count++] = outcome;
    write_report();
    descriptor = atomic_exchange(&marker, -1);
    if (descriptor >= 0) close(descriptor);
    host_logf(HOST_LOG_INFO, "benchmark: run %d failed on guest assertion; partial report saved", run.index);
}

/* Only the guest thread writes reports/journals and advances the planner. The
   existing retry result keeps next-run rendering out until the worker seals. */
static BOOL finish_run(NSDictionary *presentation) {
    struct benchmark_outcome outcome = pending_outcome;
    struct benchmark_summary summary = pending_summary;
    if(outcome.status==BENCHMARK_PARTIAL) {
        NSMutableDictionary *partial=[presentation mutableCopy];
        partial[@"collector_status"]=presentation[@"status"] ?: @"unknown";
        partial[@"status"]=@"user_finished_partial";
        partial[@"interpretation"]=@"partial_measurement_not_complete_workload";
        presentation=partial;
    }
    if(!write_run(&run, outcome.status, pending_note, pending_measurements, presentation))return NO;
    free(pending_measurements);
    pending_measurements = NULL;
    pending_note = nil;
    run_finishing = 0;
    benchmark_journal_end(JOURNAL, run.index, &outcome);
    run_open = 0;
    if (outcome_count < BENCHMARK_RUNS_MAXIMUM) outcomes[outcome_count++] = outcome;
    host_logf(HOST_LOG_INFO, "benchmark: run %d %s: %zu submissions, %.1f a second, submission interval 99th %.1f ms, CPU 95th %.1f, GPU 95th %.1f, "
        "at %.1f Hz, %dx%d, least memory left %ld MB", run.index, benchmark_status_name(outcome.status), summary.frames,
        summary.frames_a_second, outcome.interval_p99, outcome.cpu_p95, outcome.gpu_p95, outcome.refresh_hertz, window_width,
        window_height, outcome.left_least);
    host_logf(HOST_LOG_INFO, "benchmark: run %d offered %lu submissions; %lu had no drawable and %lu no picture", run.index,
        pending_offered, pending_no_drawable, pending_no_picture);
    /* (the OpenGL ES renderer measures no GPU time: its GPU 95th of 0.0 is no measurement) */
    if (summary.gpu_frames < summary.frames)
        host_logf(HOST_LOG_INFO, "benchmark: run %d GPU time measured in %zu of %zu submissions%s", run.index, summary.gpu_frames,
            summary.frames, summary.gpu_frames ? "" : " (the GPU 95th is no measurement)");
    return YES;
}
