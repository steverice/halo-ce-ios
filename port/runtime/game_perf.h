#ifndef GAME_PERF_H
#define GAME_PERF_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define GAME_PERF_SCHEMA 1
#define GAME_PERF_ROW_BYTES 24
#define GAME_PERF_UNAVAILABLE UINT64_MAX
#define GAME_PERF_FRAME_CAPACITY 8192
#define GAME_PERF_EVENT_CAPACITY 4096
#define GAME_PERF_WINDOW_COUNT 8
#define GAME_PERF_WINDOW_FRAMES 512
#define GAME_PERF_LOADING 1u
#define GAME_PERF_PRESENT_FAILED 2u
#define GAME_PERF_GPU_UNAVAILABLE 4u
#define GAME_PERF_OVERFLOW 8u
#define GAME_PERF_GAP 16u
#define GAME_PERF_LATE 32u
/* All clocks are monotonic microseconds; elapsed_us is relative to game start.
 * One producer appends; one worker drains. Lifecycle calls run on the producer.
 * Reset/start requires an empty recorder and all other access quiescent.
 * Execute start on the serial worker while producer publication is disabled.
 * End must be serialized with the producer; status reads remain thread-safe. */
typedef struct game_perf_detail {
    uint32_t draws, packets_sent, packets_received, bytes_sent, bytes_received;
    uint32_t networked_objects;
    uint32_t pacing_us;
    uint32_t gpu_pass_us[8];
    uint32_t available;
} game_perf_detail;
/* Shadow decisions describe a fixed-resolution observation, never actuation.
 * available: 1 complete S/U/H, 2 actual presentation interval, 4 decision,
 * 8 stage-boundary counter capability. Missing samples stay explicit. */
typedef struct game_perf_dynamic_resolution {
    uint64_t scene_us, upscale_us, hud_us, presented_us, presentation_us;
    uint32_t actual_lines, chosen_lines, ceiling_lines, floor_lines;
    uint32_t ignore, action, thermal, available, late;
} game_perf_dynamic_resolution;
typedef struct game_perf_frame {
    uint64_t frame_id, elapsed_us, interval_us;
    uint64_t cpu_us, simulation_us, network_us, render_us, gpu_us, budget_us;
    uint32_t tick_delta;
    uint16_t flags;
    game_perf_detail detail;
    game_perf_dynamic_resolution drs;
} game_perf_frame;
typedef enum game_perf_event_type {
    GAME_PERF_EVENT_START=1, GAME_PERF_EVENT_END, GAME_PERF_EVENT_GAP,
    GAME_PERF_EVENT_MAP_LOAD_BEGIN, GAME_PERF_EVENT_MAP_LOAD_END,
    GAME_PERF_EVENT_CHECKPOINT_SAVE, GAME_PERF_EVENT_CHECKPOINT_LOAD,
    GAME_PERF_EVENT_PLAYERS, GAME_PERF_EVENT_THERMAL,
    GAME_PERF_EVENT_NETWORK_STALL, GAME_PERF_EVENT_PACKET_LOSS,
    GAME_PERF_EVENT_CONTEXT, GAME_PERF_EVENT_PROFILE_CHANGE = 13
} game_perf_event_type;
typedef enum game_perf_end_reason {
    GAME_PERF_END_NORMAL=0, GAME_PERF_END_SUSPEND, GAME_PERF_END_INTERRUPTED,
    GAME_PERF_END_DISCONNECT
} game_perf_end_reason;
typedef struct game_perf_event {
    uint64_t elapsed_us, value, denominator;
    uint32_t type, flags;
} game_perf_event;
typedef struct game_perf_status {
    uint64_t frames_seen, frames_dropped, events_dropped, detail_dropped;
    uint64_t first_gap_us, last_gap_us;
    uint64_t start_us, end_us;
    uint32_t active, end_reason;
} game_perf_status;
#define GAME_PERF_METRIC_COUNT 7
#define GAME_PERF_HISTOGRAM_BINS 64
/* Metrics: interval, CPU, simulation, network, render, GPU, budget. Group 0
 * excludes loading; group 1 contains loading. Histogram bucket 0 is 0..1 us,
 * bucket N is [2^N, 2^(N+1)-1] us. Quantiles derived from it are approximate.
 * Sums saturate at UINT64_MAX. Counts cover every submitted active frame,
 * including dropped trace rows. Concurrent snapshots are individually atomic,
 * not a coherent transaction; quiescent snapshots are final and coherent. */
typedef struct game_perf_metric_summary {
    uint64_t available, sum_us, max_us;
    uint64_t histogram[GAME_PERF_HISTOGRAM_BINS];
} game_perf_metric_summary;
typedef struct game_perf_summary {
    uint64_t frames[2], over_budget[2], budget_available[2];
    game_perf_metric_summary metrics[2][GAME_PERF_METRIC_COUNT];
} game_perf_summary;
typedef struct game_perf_window {
    uint64_t begin_us, end_us, peak_us;
    uint32_t frame_count, truncated;
    game_perf_frame frames[GAME_PERF_WINDOW_FRAMES];
} game_perf_window;
typedef struct game_perf_recorder game_perf_recorder;
typedef void (*game_perf_frame_enricher)(void *, game_perf_frame *);
typedef void (*game_perf_frame_callback)(void *, const game_perf_frame *);
typedef void (*game_perf_event_callback)(void *, const game_perf_event *);
game_perf_recorder *game_perf_create(void);
void game_perf_destroy(game_perf_recorder *);
/* Set while all recorder access is quiescent. Runs on the drain worker before
 * retaining detail windows and delivering a frame; preserved across starts.
 * The callback must preserve frame_id and elapsed_us.
 * Producer summaries describe submitted fields, before asynchronous enrichment. */
void game_perf_set_enricher(game_perf_recorder *, game_perf_frame_enricher, void *);
bool game_perf_start(game_perf_recorder *, uint64_t start_us);
void game_perf_end(game_perf_recorder *, uint64_t end_us, game_perf_end_reason);
bool game_perf_append_frame(game_perf_recorder *, const game_perf_frame *);
bool game_perf_append_event(game_perf_recorder *, const game_perf_event *);
size_t game_perf_drain(game_perf_recorder *, game_perf_frame_callback, game_perf_event_callback, void *);
void game_perf_summary_append(game_perf_summary *, const game_perf_frame *);
void game_perf_get_summary(const game_perf_recorder *, game_perf_summary *);
/* Three bounded attempts. A successful snapshot includes all submitted frames
 * through last_frame_id, including dropped rows. Callers ignore outputs on false.
 * Serialize lifecycle/event mutation when requiring a matching nonframe status. */
bool game_perf_checkpoint_snapshot(const game_perf_recorder *, game_perf_summary *, game_perf_status *, uint64_t *last_frame_id);
void game_perf_get_status(const game_perf_recorder *, game_perf_status *);
/* Worker-only view, valid until next drain; at most 128 KiB per window. */
size_t game_perf_get_windows(const game_perf_recorder *, const game_perf_window **);
/* Column-major little-endian v1: u32 interval, elapsed-ms; u16 CPU, simulation,
 * network, render, GPU, budget (0.1 ms), ticks, flags. Missing = UINT16_MAX;
 * saturation sets OVERFLOW; originals remain in the callback frame. */
bool game_perf_encode_columns(const game_perf_frame *, size_t, uint8_t *, size_t);
uint32_t game_perf_crc32(const void *, size_t);
/* 16-byte header: GPJ1, payload length, sequence, CRC32, all little-endian.
 * Recovery returns complete valid prefix; never trusts a truncated chunk. */
size_t game_perf_journal_chunk(void *, size_t, uint32_t, const void *, size_t);
size_t game_perf_journal_recover(const void *, size_t, uint32_t *);
#endif
