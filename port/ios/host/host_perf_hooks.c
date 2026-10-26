/* Apache-2.0. The game thread owns these scalars; collection adds no locks or allocation. */
#include "host_perf_hooks.h"
#include "host_perf_reports.h"
#include <time.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include "host_config.h"
#include "host_perf_metal.h"
#include "dynamic_resolution_shadow.h"
static uint64_t began, previous, interval, phases[3], sums[3], network_nested;
static uint64_t budget = GAME_PERF_UNAVAILABLE, serial;
static unsigned marked, ticks, loading;
static int frame_sealed = 1;
static game_perf_frame pending;
static struct dynamic_resolution_shadow shadow;
static _Atomic unsigned dynamic_events;
static struct { game_perf_frame frame; int width; } delayed[16];
static unsigned delayed_read, delayed_write;
static pthread_mutex_t dynamic_lock = PTHREAD_MUTEX_INITIALIZER;
static int dynamic_paused;
static int dynamic_width, dynamic_eligible, dynamic_configured;
static _Atomic int dynamic_setting = -1;
/* Actuation: display.dynamic_resolution, or a debug pin or sweep. Each is
 * read once, as the shadow switch is: a profile sets them for the session. */
static int dynamic_actuate, dynamic_pin, dynamic_sweep, dynamic_floor = 540;
static int setting_true(const char *name, const char *environment_name) {
    char text[16];
    const char *environment = getenv(environment_name);
    host_config_string(name, "false", text, sizeof(text));
    return !strcmp(environment ? environment : text, "true") || !strcmp(environment ? environment : text, "1");
}
static int setting_integer(const char *name, const char *environment_name, const char *fallback) {
    char text[16];
    const char *environment = getenv(environment_name);
    host_config_string(name, fallback, text, sizeof(text));
    return atoi(environment ? environment : text);
}
int host_perf_hooks_dynamic_enabled(void) {
    /* Not settled before the device profile is installed: a profile alone may turn it on. */
    if (dynamic_setting < 0 && !host_config_profile_installed()) {
        return setting_true("debug.dynamic_resolution_shadow", "HALO_DYNAMIC_RESOLUTION_SHADOW") ||
            setting_true("display.dynamic_resolution", "HALO_DYNAMIC_RESOLUTION") ||
            setting_true("debug.dynamic_resolution_validate", "HALO_DYNAMIC_RESOLUTION_VALIDATE") ||
            setting_integer("debug.dynamic_resolution_lines", "HALO_DYNAMIC_RESOLUTION_LINES", "0") > 0 ||
            setting_true("debug.dynamic_resolution_sweep", "HALO_DYNAMIC_RESOLUTION_SWEEP");
    }
    if (dynamic_setting < 0) {
        int shadow_on = setting_true("debug.dynamic_resolution_shadow", "HALO_DYNAMIC_RESOLUTION_SHADOW");
        int floor = setting_integer("display.dynamic_resolution_floor", "HALO_DYNAMIC_RESOLUTION_FLOOR", "540");
        dynamic_pin = setting_integer("debug.dynamic_resolution_lines", "HALO_DYNAMIC_RESOLUTION_LINES", "0");
        dynamic_sweep = setting_true("debug.dynamic_resolution_sweep", "HALO_DYNAMIC_RESOLUTION_SWEEP");
        dynamic_floor = floor > 0 ? floor : 540;
        dynamic_actuate = setting_true("display.dynamic_resolution", "HALO_DYNAMIC_RESOLUTION") ||
            setting_true("debug.dynamic_resolution_validate", "HALO_DYNAMIC_RESOLUTION_VALIDATE") ||
            dynamic_pin > 0 || dynamic_sweep;
        dynamic_setting = shadow_on || dynamic_actuate;
    }
    return dynamic_setting;
}
int host_perf_hooks_dynamic_actuates(void) { return host_perf_hooks_dynamic_enabled() && dynamic_setting >= 0 && dynamic_actuate; }
/* the lines the frame being presented rendered at, while dynamic resolution renders them; 0 otherwise */
int host_perf_hooks_dynamic_lines(void) {
    return host_perf_hooks_dynamic_actuates() && dynamic_eligible && host_perf_metal_stage_supported() ?
        (int)pending.drs.actual_lines : 0;
}
/* Without stage-boundary GPU timing the planner never has a decision to make,
 * so dynamic resolution is off rather than silently holding the ceiling. */
static int dynamic_timing_available(void) {
    static int said;
    if (host_perf_metal_stage_supported()) return 1;
    if (!said && dynamic_configured) {
        fprintf(stderr, "dynamic resolution: off: this GPU has no stage-boundary timing for the planner\n");
        said = 1;
    }
    return 0;
}
void host_perf_hooks_dynamic_ignore(unsigned reason) { atomic_fetch_or(&dynamic_events, reason); }
/* Between frames, on the game thread. The planner's level was decided from
 * frames presented at least eight frames ago (dynamic_drain_locked); the
 * guest renders the next frames at the returned lines. A refused frame resets
 * the planner to the ceiling, so a refusal never strands a lower picture. */
int host_dynamic_resolution_lines(int ceiling_lines) {
    int lines;
    if (!host_perf_hooks_dynamic_actuates()) return 0;
    /* (asked whether it is on at all, with nothing changed) */
    if (ceiling_lines < 0) return dynamic_timing_available();
    if (!dynamic_timing_available()) return 0;
    pthread_mutex_lock(&dynamic_lock);
    shadow.actuate = 1;
    /* A frame the planner could not measure (split screen's several views,
     * a stereo frame) returns to the ceiling too: no decision would follow. */
    if (ceiling_lines <= 0 || (dynamic_configured && !dynamic_eligible)) {
        if (shadow.plan.levels && shadow.plan.level) {
            shadow.plan.level = 0;
            shadow.plan.settle = DYNAMIC_RESOLUTION_SETTLE;
        }
        pthread_mutex_unlock(&dynamic_lock);
        return ceiling_lines > 0 ? ceiling_lines : 0;
    }
    lines = ceiling_lines;
    /* Until the first frame at this ceiling is measured, render the ceiling. */
    if (shadow.height == ceiling_lines && shadow.plan.levels) {
        int floor = shadow.plan.lines[shadow.plan.levels - 1];
        if (dynamic_pin > 0) lines = dynamic_pin < floor ? floor : dynamic_pin > ceiling_lines ? ceiling_lines : dynamic_pin;
        else if (dynamic_sweep && shadow.plan.levels > 1) {
            /* floor to ceiling and back, one level each half second at 60 a second */
            unsigned span = 2u * (unsigned)(shadow.plan.levels - 1), step = (unsigned)((serial / 30) % span);
            lines = shadow.plan.lines[step < (unsigned)shadow.plan.levels ? step : span - step];
        } else lines = shadow.plan.lines[shadow.plan.level];
    }
    pthread_mutex_unlock(&dynamic_lock);
    return lines;
}
void host_perf_hooks_dynamic_config(int enabled, int width, int height, int actual, int floor, int thermal, int eligible) {
    dynamic_width = width; dynamic_eligible = enabled && eligible; dynamic_configured = 1;
    pending.drs.ceiling_lines = height > 0 ? (uint32_t)height : 0;
    pending.drs.actual_lines = actual > 0 && actual <= height ? (uint32_t)actual : pending.drs.ceiling_lines;
    if (floor <= 0) floor = dynamic_floor;
    pending.drs.floor_lines = floor > 0 ? (uint32_t)floor : 540;
    if ((height + 1) / 2 > (int)pending.drs.floor_lines) pending.drs.floor_lines = (height + 1) / 2;
    if (height > 0 && pending.drs.floor_lines > (uint32_t)height) pending.drs.floor_lines = height;
    pending.drs.thermal = thermal >= 0 && thermal <= 3 ? (uint32_t)thermal : 0;
}
static void dynamic_drain_locked(uint64_t frame_id) {
    while (delayed_read != delayed_write) {
        unsigned slot = delayed_read % 16;
        game_perf_frame *frame = &delayed[slot].frame;
        /* Bounded latency gives asynchronous GPU and display callbacks time to
         * finish. A missing callback remains unavailable; never block the GPU. */
        if (frame_id < frame->frame_id + 8) break;
        host_perf_hooks_gpu_join(frame);
        host_perf_metal_join(frame);
        dynamic_resolution_shadow_frame(&shadow, frame, delayed[slot].width);
        if (frame->drs.action || frame->frame_id % 600 == 0) {
            fprintf(stderr, "%s frame=%llu actual=%u chosen=%u decision_available=%u action=%u S/U/H=%llu/%llu/%llu cpu=%llu budget=%llu late=%u ignore=%u available=%u thermal=%u clock=%s\n",
                shadow.actuate ? "DRS:" : "DRS shadow:",
                (unsigned long long)frame->frame_id, frame->drs.actual_lines, frame->drs.chosen_lines,
                (frame->drs.available & 4) != 0, frame->drs.action, (unsigned long long)frame->drs.scene_us, (unsigned long long)frame->drs.upscale_us,
                (unsigned long long)frame->drs.hud_us, (unsigned long long)frame->cpu_us,
                (unsigned long long)frame->budget_us, frame->drs.late, frame->drs.ignore, frame->drs.available, frame->drs.thermal,
                shadow.actuate ? "actuated" : "unvalidated_fixed_pixels");
        }
        host_perf_reports_frame(frame);
        ++delayed_read;
    }
}
static void dynamic_drain(uint64_t frame_id) {
    pthread_mutex_lock(&dynamic_lock);
    dynamic_drain_locked(frame_id);
    pthread_mutex_unlock(&dynamic_lock);
}
void host_perf_hooks_dynamic_flush(void) { dynamic_drain(UINT64_MAX); }
void host_perf_hooks_dynamic_pause(void) {
    pthread_mutex_lock(&dynamic_lock);
    dynamic_paused = 1;
    dynamic_drain_locked(UINT64_MAX);
    pthread_mutex_unlock(&dynamic_lock);
}
void host_perf_hooks_dynamic_resume(void) {
    pthread_mutex_lock(&dynamic_lock);
    dynamic_paused = 0;
    host_perf_hooks_dynamic_ignore(DYNAMIC_RESOLUTION_RESUME);
    pthread_mutex_unlock(&dynamic_lock);
}
static uint64_t context_values[10];
static unsigned context_seen;
static uint32_t frame_draws, networked_objects;
static uint64_t last_network_sample, last_stall_us;
static _Atomic uint64_t last_received_us;
static int objects_known;
static _Atomic uint32_t packets_sent, packets_received, bytes_sent, bytes_received;
/* Each atomic value carries its generation, so a late callback cannot change a reused slot. */
static struct { _Atomic uint64_t sum, pending, sealed; } gpu_slots[128];
uint64_t host_perf_hooks_frame_id(void) { return serial; }
uint64_t host_perf_hooks_gpu_reserve(void) {
    uint64_t token = serial, generation = (uint64_t)(uint32_t)token << 32;
    unsigned slot = token % 128;
    uint64_t seen = atomic_load(&gpu_slots[slot].pending);
    if ((seen & UINT64_C(0xffffffff00000000)) != generation) return 0;
    atomic_fetch_add(&gpu_slots[slot].pending, 1);
    return token;
}
void host_perf_hooks_gpu_complete(uint64_t token, uint64_t duration_us, int valid) {
    if (!token) return;
    unsigned slot = token % 128;
    uint64_t generation = (uint64_t)(uint32_t)token << 32;
    uint64_t seen = atomic_load(&gpu_slots[slot].sum);
    while ((seen & UINT64_C(0xffffffff00000000)) == generation) {
        uint64_t sum = (seen & 0x7fffffff) + duration_us;
        if (sum > 0x7fffffff) sum = 0x7fffffff;
        uint64_t next = generation | sum | ((seen & 0x80000000) || !valid ? UINT64_C(0x80000000) : 0);
        if (atomic_compare_exchange_weak(&gpu_slots[slot].sum, &seen, next)) break;
    }
    seen = atomic_load(&gpu_slots[slot].pending);
    while ((seen & UINT64_C(0xffffffff00000000)) == generation && (uint32_t)seen) {
        if (atomic_compare_exchange_weak(&gpu_slots[slot].pending, &seen, seen - 1)) break;
    }
}
void host_perf_hooks_gpu_join(game_perf_frame *frame) {
    unsigned slot = frame->frame_id % 128;
    uint64_t generation = (uint64_t)(uint32_t)frame->frame_id << 32;
    uint64_t pending_count = atomic_load(&gpu_slots[slot].pending);
    uint64_t sum = atomic_load(&gpu_slots[slot].sum);
    if (atomic_load(&gpu_slots[slot].sealed) == frame->frame_id && pending_count == generation &&
        (sum & UINT64_C(0xffffffff00000000)) == generation && !(sum & 0x80000000) && (uint32_t)sum) {
        frame->gpu_us = (uint32_t)sum;
        frame->flags &= ~GAME_PERF_GPU_UNAVAILABLE;
    }
}
static uint64_t now_us(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000000 + (uint64_t)value.tv_nsec / 1000;
}
void host_perf_hooks_context(unsigned field, uint64_t value) {
    if (!field || field >= 10) return;
    if (!(context_seen & (1u << field)) || context_values[field] != value) {
        game_perf_event event = {.elapsed_us = now_us(), .type = GAME_PERF_EVENT_CONTEXT, .flags = field, .value = value, .denominator = serial};
        host_perf_reports_event(&event);
        context_seen |= 1u << field;
        context_values[field] = value;
    }
}
void host_perf_hooks_budget(uint64_t value) { budget = value; pending.budget_us = value; }
void host_perf_hooks_loading(int value) {
    loading = value != 0;
    /* Start a full second of presented frames after either loading edge. */
    host_perf_hooks_dynamic_ignore(DYNAMIC_RESOLUTION_LOAD);
    if (loading) {
        frame_sealed = 1;
        context_seen = 0;last_received_us = last_stall_us = 0;
        game_perf_event event = {.elapsed_us=now_us(),.type=GAME_PERF_EVENT_MAP_LOAD_BEGIN};
        host_perf_reports_event(&event);
    }
}
void host_perf_hooks_phase(int phase, int value) { host_perf_hooks_phase_at(phase, value, now_us()); }
void host_perf_hooks_phase_at(int phase, int value, uint64_t now) {
    if (phase == 7) {
        if (!frame_sealed) return;
        frame_sealed = 0;
        interval = previous && now >= previous ? now - previous : GAME_PERF_UNAVAILABLE;
        began = previous = now;
        ++serial;
        if (host_perf_hooks_dynamic_enabled()) dynamic_drain(serial);
        unsigned slot = serial % 128;
        uint64_t generation = (uint64_t)(uint32_t)serial << 32;
        atomic_store(&gpu_slots[slot].sum, generation);
        atomic_store(&gpu_slots[slot].pending, generation);
        atomic_store(&gpu_slots[slot].sealed, 0);
        memset(phases, 0, sizeof(phases));
        memset(sums, 0, sizeof(sums));
        network_nested = marked = ticks = 0;
        return;
    }
    if (phase == 22 && value >= 0 && now - last_network_sample >= 1000000) {
        game_perf_event event = {.elapsed_us=now, .type=15, .value=(uint64_t)value*1000, .denominator=serial};
        host_perf_reports_event(&event);last_network_sample=now;return;
    }
    if (phase == 23) { objects_known = value >= 0; networked_objects = value >= 0 ? (uint32_t)value : 0; return; }
    if (phase == 19) { frame_draws += (uint32_t)value; return; }
    if (phase == 20 && value >= 0) {
        atomic_fetch_add(&packets_sent, 1); atomic_fetch_add(&bytes_sent, (uint32_t)value); return;
    }
    if (phase == 21 && value >= 0) {
        last_received_us = now;
        atomic_fetch_add(&packets_received, 1); atomic_fetch_add(&bytes_received, (uint32_t)value); return;
    }
    if (phase >= 10 && phase <= 18) {
        host_perf_hooks_context((unsigned)phase - 9, (uint32_t)value);
        return;
    }
    if (phase == 8) {
        game_perf_event event = {.elapsed_us = now, .type = (uint32_t)value};
        host_perf_reports_event(&event);
        return;
    }
    if (phase == 4 || phase < 0 || phase > 6) return;
    unsigned which = phase >= 5 ? 2 : phase >= 2 ? 1 : 0;
    if (phase == 0 || phase == 2 || phase == 5) phases[which] = now;
    else if (phases[which] && now >= phases[which]) {
        uint64_t elapsed = now - phases[which];
        sums[which] += elapsed;
        if (which == 2 && phases[0]) network_nested += elapsed;
        if (which == 2 && elapsed > 50000) {
            game_perf_event event = {.elapsed_us=now, .type=GAME_PERF_EVENT_NETWORK_STALL, .value=elapsed, .flags=1};
            host_perf_reports_event(&event); /* measured network CPU span, not a claim about packet progress */
        }
        phases[which] = 0;
        marked |= 1u << which;
        if (phase == 1 && value > 0) ticks += (unsigned)value;
    }
}
void host_perf_hooks_present_begin(void) { host_perf_hooks_present_begin_at(now_us()); }
void host_perf_hooks_present_end(void) { host_perf_hooks_present_end_at(now_us()); }
void host_perf_hooks_present_begin_at(uint64_t now) {
    memset(&pending, 0, sizeof(pending));
    pending.frame_id = serial;
    pending.elapsed_us = now;
    pending.interval_us = interval;
    pending.cpu_us = began && now >= began ? now - began : GAME_PERF_UNAVAILABLE;
    pending.simulation_us = marked & 1 ? sums[0] - (network_nested < sums[0] ? network_nested : sums[0]) : GAME_PERF_UNAVAILABLE;
    pending.network_us = sums[2];
    pending.render_us = marked & 2 ? sums[1] : GAME_PERF_UNAVAILABLE;
    pending.gpu_us = GAME_PERF_UNAVAILABLE;
    pending.budget_us = budget;
    pending.tick_delta = ticks;
    int expects_progress = context_values[3] >= 2 && context_values[4] > context_values[5];
    if (!loading && expects_progress && last_received_us && now >= last_received_us &&
        now - last_received_us > 50000 && now - last_stall_us > 500000) {
        game_perf_event event = {.elapsed_us=now,.type=GAME_PERF_EVENT_NETWORK_STALL,.value=now-last_received_us,.flags=2};
        host_perf_reports_event(&event);last_stall_us=now;
    }
    pending.detail.draws = frame_draws; frame_draws = 0;
    pending.detail.packets_sent = atomic_exchange(&packets_sent, 0);
    pending.detail.packets_received = atomic_exchange(&packets_received, 0);
    pending.detail.bytes_sent = atomic_exchange(&bytes_sent, 0);
    pending.detail.bytes_received = atomic_exchange(&bytes_received, 0);
    pending.detail.networked_objects = networked_objects;
    pending.detail.available = 3 | (objects_known ? 8 : 0); /* draws and successful application datagrams */
    pending.flags = GAME_PERF_GPU_UNAVAILABLE | (loading ? GAME_PERF_LOADING : 0);
}
void host_perf_hooks_cpu(uint64_t microseconds) { pending.cpu_us = microseconds; }
void host_perf_hooks_pacing(uint64_t microseconds) {
    pending.detail.pacing_us = microseconds > UINT32_MAX ? UINT32_MAX : (uint32_t)microseconds;
    pending.detail.available |= 4;
}
void host_perf_hooks_present_failed(void) { pending.flags |= GAME_PERF_PRESENT_FAILED; }
void host_perf_hooks_present_end_at(uint64_t now) {
    (void)now;
    atomic_store(&gpu_slots[pending.frame_id % 128].sealed, pending.frame_id);
    /* Setting off: the report path as before, with no lock on the frame path. */
    if (!host_perf_hooks_dynamic_enabled()) {
        host_perf_reports_frame(&pending);
        frame_sealed = 1;
        return;
    }
    pthread_mutex_lock(&dynamic_lock);
    if (dynamic_eligible && !dynamic_paused) {
        pending.drs.ignore = atomic_exchange(&dynamic_events, 0);
        /* A full ring publishes its oldest row early rather than overwrite it. */
        if (delayed_write - delayed_read >= 16)
            dynamic_drain_locked(delayed[delayed_read % 16].frame.frame_id + 8);
        unsigned slot = delayed_write++ % 16;
        delayed[slot].frame = pending; delayed[slot].width = dynamic_width;
    } else host_perf_reports_frame(&pending);
    pthread_mutex_unlock(&dynamic_lock);
    frame_sealed = 1;
}
