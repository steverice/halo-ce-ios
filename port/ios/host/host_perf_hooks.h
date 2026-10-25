/* Apache-2.0. Game-thread scalar adapter. All timestamps are monotonic microseconds. */
#pragma once
#include <stdint.h>
void host_perf_hooks_phase(int phase, int value);
void host_perf_hooks_phase_at(int phase, int value, uint64_t now);
void host_perf_hooks_budget(uint64_t budget_us);
void host_perf_hooks_loading(int loading);
void host_perf_hooks_present_begin(void);
void host_perf_hooks_present_end(void);
void host_perf_hooks_present_begin_at(uint64_t now);
void host_perf_hooks_present_end_at(uint64_t now);
void host_perf_hooks_present_failed(void);
void host_perf_hooks_context(unsigned field, uint64_t value);
struct game_perf_frame;
uint64_t host_perf_hooks_frame_id(void);
uint64_t host_perf_hooks_gpu_reserve(void);
void host_perf_hooks_gpu_complete(uint64_t token, uint64_t duration_us, int valid);
void host_perf_hooks_gpu_join(struct game_perf_frame *frame);
void host_perf_hooks_pacing(uint64_t microseconds);
/* Renderer work excludes its measured pacing waits; supplied before present returns. */
void host_perf_hooks_cpu(uint64_t microseconds);

/* The planner's inputs are collected (shadow mode or actuation). */
int host_perf_hooks_dynamic_enabled(void);
/* display.dynamic_resolution (or a debug pin or sweep): the guest renders the
 * planner's lines (host_dynamic_resolution_lines, a guest import). */
int host_perf_hooks_dynamic_actuates(void);
int host_dynamic_resolution_lines(int ceiling_lines);
/* width x height: the scene target's allocation, the ceiling; actual: the
 * lines this frame's scene rendered (its content), 0 for the ceiling; floor 0
 * for display.dynamic_resolution_floor */
void host_perf_hooks_dynamic_config(int enabled, int width, int height, int actual, int floor, int thermal, int eligible);
/* Thread-safe events are consumed by the next presented frame. */
void host_perf_hooks_dynamic_ignore(unsigned reason);

/* Game-thread map boundary: publish remaining rows before changing report. */
void host_perf_hooks_dynamic_flush(void);

/* Lifecycle threads serialize only the delayed ring, never producer scalars. */
void host_perf_hooks_dynamic_pause(void);
void host_perf_hooks_dynamic_resume(void);
