/* Apache-2.0. Frame entry points only publish fixed records. */
#pragma once
#include "../../runtime/game_perf.h"
void host_perf_reports_start(const char *guest_sha256);
/* Loading boundary, permitted to schedule worker operations. UI means menu. */
void host_perf_reports_activate(const char *map_name, int suppressed);
/* Loading boundary: refine a pending logical map without starting another report. */
void host_perf_reports_identity(const char *resolved_path);
void host_perf_reports_frame(const game_perf_frame *frame);
void host_perf_reports_event(const game_perf_event *event);
void host_perf_reports_suspend(void);
/* Orderly exit boundary; does not wait for network delivery. */
void host_perf_reports_finish(int interrupted);
int host_perf_reports_enabled(void);
int host_perf_reports_collecting(void);
/* Loading/configuration boundary only: accepts a bounded graphics snapshot. */
void host_perf_reports_graphics(const char *json);
struct game_perf_graphics_snapshot;
/* Producer publishes bounded cached scalars; worker formats the snapshot. */
void host_perf_reports_graphics_snapshot(const struct game_perf_graphics_snapshot *snapshot);
