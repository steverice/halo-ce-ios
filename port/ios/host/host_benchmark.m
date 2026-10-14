/* The device benchmark's host side (host_benchmark.h).

   Everything it writes is in the data folder's "benchmark" folder (the process's
   working directory is the data folder): journal.txt, one run-NN.json per finished
   run, and report.json once the suite is over. The report holds the device's model
   identifier, system version, GPU and display, the app's version, and each run's
   settings and measurements. It never holds the device's name, an identifier of the
   device or its owner, an address, a path or anything from the game's data. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#include "ios_host.h"
#include "guest_image.h"
#include "host_benchmark.h"
#include "host_profile.h"
#include "benchmark_metrics.h"
#include "benchmark_plan.h"
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

static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
/* under the lock: the open window's measurements, and what happened during it */
static struct benchmark_metrics metrics;
static int window_open, window_interrupted, window_width, window_height;
static double refresh_hertz;
/* the screen's own rate, for a presenter that reports no refresh period (the Mac's) */
static double screen_hertz;

static _Atomic int running;
static struct benchmark_plan plan;
static struct benchmark_outcome outcomes[BENCHMARK_RUNS_MAXIMUM];
static int outcome_count;
static struct benchmark_run run;
static int run_open;
static char report_code[16];
static NSMutableArray<NSString *> *flags;
static NSDate *started;
static dispatch_source_t sampler;
static int last_run_throttled;

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

void host_benchmark_frame(double interval, double cpu, double gpu, double refresh_period) {
    if (!atomic_load(&running)) return;
    os_unfair_lock_lock(&lock);
    if (refresh_period > 0.0) refresh_hertz = 1.0 / refresh_period;
    if (window_open && interval > 0.0) {
        if (metrics.refresh_hertz <= 0.0 && refresh_hertz > 0.0) metrics.refresh_hertz = refresh_hertz;
        benchmark_metrics_frame(&metrics, interval * 1000.0, cpu * 1000.0, gpu * 1000.0);
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
#if !TARGET_OS_MACCATALYST && !TARGET_OS_SIMULATOR
    *left = (long)(os_proc_available_memory() >> 20);
#endif
}

/* once a second while a suite runs: memory and heat into the open window, and a memory line in the journal every
   five seconds of a run, which is what tells a memory kill from another end */
static void sample(void) {
    static int tick;
    long footprint, left;
    int thermal = (int)NSProcessInfo.processInfo.thermalState, open;

    memory_now(&footprint, &left);
    os_unfair_lock_lock(&lock);
    open = window_open;
    if (open) benchmark_metrics_sample(&metrics, footprint, left, thermal);
    os_unfair_lock_unlock(&lock);
    if (run_open && ++tick % 5 == 0) benchmark_journal_memory(JOURNAL, footprint, left);
}

static void sampler_start(void) {
    if (sampler) return;
    sampler = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_queue_create("halo.benchmark", DISPATCH_QUEUE_SERIAL));
    dispatch_source_set_timer(sampler, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
    dispatch_source_set_event_handler(sampler, ^{ sample(); });
    dispatch_resume(sampler);
    [NSNotificationCenter.defaultCenter addObserverForName:UISceneDidEnterBackgroundNotification object:nil queue:nil
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

static NSString *model_identifier(void) {
    char text[64] = "";
    size_t size = sizeof(text);
#if TARGET_OS_SIMULATOR
    const char *simulated = getenv("SIMULATOR_MODEL_IDENTIFIER");

    return [NSString stringWithFormat:@"%s (simulator)", simulated ? simulated : "unknown"];
#elif TARGET_OS_MACCATALYST
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
#elif TARGET_OS_MACCATALYST
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
#if TARGET_OS_MACCATALYST
    sysctlbyname("machdep.cpu.brand_string", chip, &chip_size, NULL, 0);
#else
    (void)chip_size;
#endif
    return [NSString stringWithFormat:@"\"format\":%d,\"code\":%@,\"suite\":%d,\"mode\":\"%s\",\"started\":\"%@\","
        "\"device\":{\"model\":%@,\"chip\":%@,\"platform\":\"%@\",\"os\":\"%ld.%ld.%ld\",\"os_build\":%@,"
        "\"gpu\":%@,\"gpu_family\":%d,\"memory_mb\":%llu,"
        "\"screen\":{\"width\":%.0f,\"height\":%.0f,\"maximum_frames_a_second\":%ld}},"
        "\"app\":{\"version\":%@,\"build\":%@,\"image\":\"%s\"},"
        "\"profile\":{\"class\":%@,\"model\":%@,\"drop_in\":%s,\"drawable_lines\":%ld,\"render_lines\":%ld,\"anisotropic_filtering\":%ld,\"shadow_map_size\":%ld,"
        "\"model_lod\":\"%s\"},\"flags\":%@",
        REPORT_FORMAT, json_string(@(report_code)), BENCHMARK_SUITE_VERSION, benchmark_mode_name(plan.mode),
        [minutes stringFromDate:started ?: NSDate.date],
        json_string(model_identifier()), json_string(@(chip)), platform_name(), (long)version.majorVersion,
        (long)version.minorVersion, (long)version.patchVersion,
        /* "Version 26.6 (Build 23M5311a)": the build's name alone */
        json_string([[process.operatingSystemVersionString componentsSeparatedByString:@"Build "].lastObject
            stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@")"]]),
        json_string(gpu.name), gpu ? gpu_family(gpu) : 0, process.physicalMemory >> 20,
        screen_width, screen_height, (long)screen_rate(),
        json_string([NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"]),
        json_string([NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"]), HALO_GUEST_SHA256,
        json_string(@(host_device_profile()->class_file)), json_string(@(host_device_profile()->model_file)),
        host_device_profile()->drop_in ? "true" : "false",
        plan.device.drawable_lines, plan.device.base_lines, plan.device.anisotropy, plan.device.shadow_size,
        plan.device.model_lod_max ? "max" : "auto",
        [[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:flags options:0 error:nil] encoding:NSUTF8StringEncoding]];
}

static NSString *run_path(int index) {
    return [NSString stringWithFormat:@FOLDER "/run-%02d.json", index];
}

/* one finished run as a JSON object in its own file; the report is these put together */
static void write_run(const struct benchmark_run *finished, enum benchmark_status status, NSString *note,
    const char *measurements) {
    const struct benchmark_scene *scene = &plan.scenes[finished->scene];
    NSString *text = [NSString stringWithFormat:@"{\"index\":%d,\"stage\":\"%c\",\"scene\":\"%s\",\"map\":\"%s\","
        "\"difficulty\":\"%s\",\"recording\":%@,\"kind\":\"%s\",\"settings\":{\"render_lines\":%ld,"
        "\"anisotropic_filtering\":%ld,\"shadow_map_size\":%ld,\"model_lod\":\"%s\"},\"status\":\"%s\",\"note\":%@,"
        "\"render_size\":[%d,%d]%s%s}",
        finished->index, finished->stage, scene->id, scene->map, scene->difficulty,
        scene->recording ? json_string(@(scene->recording)) : @"null",
        scene->kind == BENCHMARK_SCENE_MENU ? "menu" : scene->kind == BENCHMARK_SCENE_CUTSCENE ? "cutscene" : "gameplay",
        /* (a baseline run leaves the size to the profile: what it drew at is the setting it ran with) */
        finished->lines ? finished->lines : (long)window_height, finished->anisotropy, finished->shadow_size,
        finished->model_lod_max ? "max" : "auto",
        benchmark_status_name(status), json_string(note), window_width, window_height, measurements ? "," : "",
        measurements ?: ""];

    [text writeToFile:run_path(finished->index) atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

static void write_report(void) {
    NSMutableString *report = [NSMutableString stringWithFormat:@"{%@,\"runs\":[", report_header()];
    int index, written = 0;

    for (index = 0; index < outcome_count; index++) {
        NSString *text = [NSString stringWithContentsOfFile:run_path(index) encoding:NSUTF8StringEncoding error:nil];

        if (!text) continue;
        [report appendFormat:@"%s\n%@", written++ ? "," : "", text];
    }
    [report appendString:@"\n]}\n"];
    [report writeToFile:@REPORT atomically:YES encoding:NSUTF8StringEncoding error:nil];
    host_logf(HOST_LOG_INFO, "benchmark: report %s, %d runs, %lu bytes: %s/" REPORT, report_code, written,
        (unsigned long)report.length, NSFileManager.defaultManager.currentDirectoryPath.fileSystemRepresentation);
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
            /* "on time" against the scene's own target: 30 for a cutscene, 60 for the rest (a frame every two
               refreshes or better, or every refresh, on a 60 Hz display) */
            BOOL cutscene = [one[@"kind"] isEqual:@"cutscene"];

            [summary appendFormat:@"%@: %.0f frames a second, %.0f%% of frames on time for %d\n", one[@"scene"],
                [one[@"frames_a_second"] doubleValue],
                100.0 * [one[cutscene ? @"on_time_half" : @"on_time_full"] doubleValue], cutscene ? 30 : 60];
        } else {
            NSDictionary *said = @{ @"skipped": @"not run on this device", @"diverged": @"the replay left its route",
                @"thermal": @"the device got too hot", @"interrupted": @"interrupted", @"crashed": @"the app stopped",
                @"killed": @"the app was stopped by the system", @"failed": @"did not run" };

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
#if TARGET_OS_VISION
    return 90.0;
#else
    return (double)UIScreen.mainScreen.maximumFramesPerSecond;
#endif
}

static void screen_size(double *width, double *height) {
#if TARGET_OS_VISION
    *width = *height = 0.0;
#else
    *width = UIScreen.mainScreen.nativeBounds.size.width;
    *height = UIScreen.mainScreen.nativeBounds.size.height;
#endif
}

static int cooled(void) {
    return NSProcessInfo.processInfo.thermalState <= NSProcessInfoThermalStateNominal;
}

int host_benchmark_start(const char *mode, int drawable_lines, int base_lines, int anisotropy, int shadow_size,
    int model_lod_max) {
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   /* nothing that reads as another */
    struct benchmark_journal journal;
    NSProcessInfo *process = NSProcessInfo.processInfo;
    NSFileManager *files = NSFileManager.defaultManager;
    int index;

    if (atomic_load(&running)) return 1;
    if (process.lowPowerModeEnabled) {
        host_logf(HOST_LOG_INFO, "benchmark: Low Power Mode is on; the benchmark does not run with it");
        return 0;
    }
    mkdir(FOLDER, 0755);
    /* The recordings, where the game reads them (benchmark_suite.c): the folder is made over at each start from
       the app's own, then from benchmark/local-routes in the data folder, where a developer drops a route to try
       (tools/replay_check.py); a tester's device has none */
    {
        NSString *bundled = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"benchmark-recordings"];

        [files removeItemAtPath:@FOLDER "/recordings" error:nil];
        mkdir(FOLDER "/recordings", 0755);
#ifdef HALO_BENCHMARK_NO_ROUTES
        /* a build without recorded routes: the folder stays empty, so the suite has only its own scenes */
        (void)bundled;
        for (NSString *source in @[])
#else
        for (NSString *source in @[ bundled, @FOLDER "/local-routes" ])
#endif
            for (NSString *name in [files contentsOfDirectoryAtPath:source error:nil]) {
                NSString *target = [@FOLDER "/recordings" stringByAppendingPathComponent:name];

                [files removeItemAtPath:target error:nil];
                [files copyItemAtPath:[source stringByAppendingPathComponent:name] toPath:target error:nil];
            }
    }
    flags = [NSMutableArray array];
    started = NSDate.date;
    plan.mode = !strncmp(mode, "full", 4) ? BENCHMARK_FULL : BENCHMARK_QUICK;
    /* "quick:a10-crew,c40-bridge": only the scenes named after the colon */
    NSArray<NSString *> *only = strchr(mode, ':') ? [@(strchr(mode, ':') + 1) componentsSeparatedByString:@","] : nil;
    {
        /* the scenes: the ones that play by themselves, then one for each recorded route the app carries, by name */
        static struct benchmark_scene scenes[BENCHMARK_SCENES_MAXIMUM];
        static struct benchmark_route routes[BENCHMARK_SCENES_MAXIMUM];
        NSString *bundled = @FOLDER "/recordings";
        int count = 0, route_count = 0;

        for (index = 0; index < benchmark_suite_scene_count; index++)
            if (!only || [only containsObject:@(benchmark_suite_scenes[index].id)]) scenes[count++] = benchmark_suite_scenes[index];
        for (NSString *name in [[files contentsOfDirectoryAtPath:bundled error:nil] sortedArrayUsingSelector:@selector(compare:)]) {
            NSString *text;

            if (![name.pathExtension isEqualToString:@"route"] || count >= BENCHMARK_SCENES_MAXIMUM) continue;
            text = [NSString stringWithContentsOfFile:[bundled stringByAppendingPathComponent:name] encoding:NSUTF8StringEncoding error:nil];
            if (text && benchmark_scene_from_route(text.UTF8String, name.stringByDeletingPathExtension.UTF8String, &routes[route_count])) {
                if (!only || [only containsObject:@(routes[route_count].id)]) scenes[count++] = routes[route_count++].scene;
            }
            else host_logf(HOST_LOG_INFO, "benchmark: %s is not a route this suite can use", name.UTF8String);
        }
        plan.scenes = scenes;
        plan.scene_count = count;
    }
    plan.device = (struct benchmark_device){ drawable_lines, base_lines, anisotropy, shadow_size, model_lod_max };

    benchmark_journal_load(JOURNAL, &journal);
    if (journal.valid && !journal.finished && journal.mode == plan.mode && journal.signature == benchmark_plan_signature(&plan) &&
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
                        "%ld MB in use, %ld MB left before the system's limit", journal.last_footprint, journal.last_left], NULL);
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
        host_logf(HOST_LOG_INFO, "benchmark: suite %d %s, report %s; display %d lines, in force: %d lines, %dx filtering, "
            "%d shadows, model detail %s", BENCHMARK_SUITE_VERSION, mode, report_code, drawable_lines, base_lines, anisotropy,
            shadow_size, model_lod_max ? "max" : "auto");
    }
    atomic_store(&marker, benchmark_journal_marker_open(JOURNAL));
    atomic_store(&running, 1);
    sampler_start();

    /* conditions: a warm device gets time to cool, then runs flagged; the display's rate is recorded as it is */
    if (process.thermalState > NSProcessInfoThermalStateNominal) {
        host_logf(HOST_LOG_INFO, "benchmark: the device is warm (thermal state %ld); waiting up to %.0f s for it to cool",
            (long)process.thermalState, COOL_WAIT_SECONDS);
        if (host_benchmark_notice("The device is warm", "Waiting for it to cool before measuring, for up to five minutes.",
            COOL_WAIT_SECONDS, cooled)) {
            host_logf(HOST_LOG_INFO, "benchmark: stopped from the screen before the first run");
            atomic_store(&running, 0);
            return 0;
        }
        if (process.thermalState > NSProcessInfoThermalStateNominal) flag(@"warm_at_start");
    }
    screen_hertz = screen_rate();
    if (screen_hertz != 60.0) flag(@"refresh_not_60");
    if (only) flag(@"scenes_filtered");
    /* a drop-in profile is somebody trying values: the report must never be read as the shipped profile's */
    if (host_device_profile()->drop_in) flag(@"drop_in_profile");
    if ([files contentsOfDirectoryAtPath:@FOLDER "/local-routes" error:nil].count) flag(@"local_routes");
#if TARGET_OS_SIMULATOR
    flag(@"simulator");
#endif
    return 1;
}

int host_benchmark_next(char *text, unsigned int size) {
    const struct benchmark_scene *scene;

    if (!atomic_load(&running)) return 0;
    if (!benchmark_plan_next(&plan, outcomes, outcome_count, &run)) {
        const char *page = getenv("HALO_BENCHMARK_PAGE");
#ifdef HALO_TESTER_BUILD
        /* a tester has no other way to get the report off the device */
        if (!page) page = "30";
#endif
        int descriptor = atomic_exchange(&marker, -1);

        benchmark_journal_done(JOURNAL);
        if (descriptor >= 0) close(descriptor);
        write_report();
        NSString *summary = report_summary();
#ifdef HALO_BENCHMARK_CLOUD
        /* a build that can send: the report is shown and the question asked first (a tester build always asks;
           elsewhere HALO_BENCHMARK_SEND=1 in the launch's environment does) */
#ifndef HALO_TESTER_BUILD
        if (getenv("HALO_BENCHMARK_SEND"))
#endif
        {
            char message[256];

            host_benchmark_send(REPORT, summary.UTF8String, message, sizeof(message));
            host_logf(HOST_LOG_INFO, "benchmark: %s", message);
            summary = [NSString stringWithFormat:@"%s\n%@", message, summary];
        }
#endif
        if (page && atoi(page) > 0) host_benchmark_page(REPORT, summary.UTF8String, report_code, atoi(page));
        atomic_store(&running, 0);
        return 0;
    }
    scene = &plan.scenes[run.scene];
    if (outcome_count) {
        /* the pause between runs, on a still screen that says what comes next; Back there stops the suite, which
           carries on from its journal the next time it is started */
        char heading[96], detail[160];
        double minutes = -[started timeIntervalSinceNow] / 60.0;

        snprintf(heading, sizeof(heading), "Benchmark: run %d is next", run.index + 1);
        snprintf(detail, sizeof(detail), "%s%s, %ld lines. Started %.0f minute%s ago.",
            last_run_throttled ? "The device got hot: cooling for a minute. Then " : "", scene->id, run.lines, minutes,
            minutes >= 1.5 || minutes < 0.5 ? "s" : "");
        if (host_benchmark_notice(heading, detail, last_run_throttled ? PAUSE_AFTER_THROTTLE_SECONDS : PAUSE_SECONDS, NULL)) {
            host_logf(HOST_LOG_INFO, "benchmark: stopped from the screen before run %d; the journal keeps what is done", run.index);
            flag(@"stopped");
            atomic_store(&running, 0);
            return 0;
        }
    }
    snprintf(text, size, "scene=%s map=%s difficulty=%s recording=%s kind=%s lead=%.1f seconds=%.1f lines=%ld anisotropy=%ld "
        "shadow=%ld lod=%d", scene->id, scene->map, scene->difficulty, scene->recording ? scene->recording : "",
        scene->kind == BENCHMARK_SCENE_MENU ? "menu" : scene->kind == BENCHMARK_SCENE_CUTSCENE ? "cutscene" : "gameplay",
        scene->lead, scene->seconds, run.lines, run.anisotropy, run.shadow_size, run.model_lod_max);
    os_unfair_lock_lock(&lock);
    window_open = window_interrupted = window_width = window_height = 0;
    benchmark_metrics_reset(&metrics, refresh_hertz);
    os_unfair_lock_unlock(&lock);
    benchmark_journal_begin(JOURNAL, run.index, run.key);
    run_open = 1;
    host_logf(HOST_LOG_INFO, "benchmark: run %d: %s", run.index, run.key);
    return 1;
}

void host_benchmark_window(int open, int width, int height) {
    os_unfair_lock_lock(&lock);
    window_open = open;
    if (open) {
        window_width = width;
        window_height = height;
        if (metrics.refresh_hertz <= 0.0) metrics.refresh_hertz = refresh_hertz > 0.0 ? refresh_hertz : screen_hertz;
    }
    os_unfair_lock_unlock(&lock);
}

void host_benchmark_end(const char *status_name, const char *note) {
    struct benchmark_outcome outcome = { BENCHMARK_FAILED, 0, 0, 0, 0, -1 };
    struct benchmark_summary summary;
    char *measurements = NULL;
    int status, interrupted;

    if (!run_open) return;
    for (status = 0; status < BENCHMARK_STATUS_COUNT; status++)
        if (!strcmp(status_name, benchmark_status_name((enum benchmark_status)status))) outcome.status = (enum benchmark_status)status;
    os_unfair_lock_lock(&lock);
    window_open = 0;
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
    if (outcome.status == BENCHMARK_OK) {
        if (interrupted) outcome.status = BENCHMARK_INTERRUPTED;
        else if (last_run_throttled) outcome.status = BENCHMARK_THERMAL;
        else if (!summary.frames) outcome.status = BENCHMARK_FAILED;
    }
    if (outcome.status == BENCHMARK_INTERRUPTED) flag(@"interrupted");
    if (outcome.status == BENCHMARK_THERMAL) flag(@"throttled");
    if (outcome.status == BENCHMARK_DIVERGED) flag(@"diverged");
    write_run(&run, outcome.status, @(note ?: ""), measurements);
    free(measurements);
    benchmark_journal_end(JOURNAL, run.index, &outcome);
    run_open = 0;
    if (outcome_count < BENCHMARK_RUNS_MAXIMUM) outcomes[outcome_count++] = outcome;
    host_logf(HOST_LOG_INFO, "benchmark: run %d %s: %zu frames, %.1f a second, frame 99th %.1f ms, CPU 95th %.1f, GPU 95th %.1f, "
        "at %.1f Hz, %dx%d, least memory left %ld MB", run.index, benchmark_status_name(outcome.status), summary.frames,
        summary.frames_a_second, outcome.interval_p99, outcome.cpu_p95, outcome.gpu_p95, outcome.refresh_hertz, window_width,
        window_height, outcome.left_least);
}
