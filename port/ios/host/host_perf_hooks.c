/* Apache-2.0. The game thread owns these scalars; collection adds no locks or allocation. */
#include "host_perf_hooks.h"
#include "host_perf_reports.h"
#include <time.h>
#include <string.h>
#include <stdatomic.h>
#if HALO_PERF_REPORTS
static uint64_t began, previous, interval, phases[3], sums[3], network_nested;
static uint64_t budget = GAME_PERF_UNAVAILABLE, serial;
static unsigned marked, ticks, loading;
static int frame_sealed = 1;
static game_perf_frame pending;
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
void host_perf_hooks_budget(uint64_t value) { budget = value; }
void host_perf_hooks_loading(int value) {
    loading = value != 0;
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
    host_perf_reports_frame(&pending);
    frame_sealed = 1;
}
#else
uint64_t host_perf_hooks_frame_id(void) { return 0; }
uint64_t host_perf_hooks_gpu_reserve(void) { return 0; }
void host_perf_hooks_gpu_complete(uint64_t token, uint64_t duration_us, int valid) { (void)token; (void)duration_us; (void)valid; }
void host_perf_hooks_gpu_join(game_perf_frame *frame) { (void)frame; }
void host_perf_hooks_context(unsigned field, uint64_t value) { (void)field; (void)value; }
void host_perf_hooks_phase(int phase, int value) { (void)phase; (void)value; }
void host_perf_hooks_phase_at(int phase, int value, uint64_t now) { (void)phase; (void)value; (void)now; }
void host_perf_hooks_budget(uint64_t value) { (void)value; }
void host_perf_hooks_loading(int value) { (void)value; }
void host_perf_hooks_present_begin(void) {}
void host_perf_hooks_present_end(void) {}
void host_perf_hooks_present_begin_at(uint64_t now) { (void)now; }
void host_perf_hooks_present_end_at(uint64_t now) { (void)now; }
void host_perf_hooks_present_failed(void) {}
void host_perf_hooks_cpu(uint64_t microseconds) { (void)microseconds; }
void host_perf_hooks_pacing(uint64_t microseconds) { (void)microseconds; }
#endif
