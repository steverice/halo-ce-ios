/* The device benchmark's host side (host_benchmark.m): which run comes next
   (port/runtime/benchmark_plan.c), every frame's time, memory and heat while a
   run's measured window is open (port/runtime/benchmark_metrics.c), the journal
   that lets a cut-off suite carry on, the checks that say whether a run can be
   trusted, and the report. The game image's side is
   port/linux/src/benchmark_suite.c, which calls host_benchmark_start, _next,
   _window and _end through guest_host.h. */
#ifndef HALO_HOST_BENCHMARK_H
#define HALO_HOST_BENCHMARK_H
#include <stdint.h>
#include <stddef.h>

/* Only the primary physical Metal drawable may supply presentation evidence.
   Configure from the renderer before a run; -1 means unknown policy. */
enum host_presentation_capability { HOST_PRESENTATION_UNCONFIGURED, HOST_PRESENTATION_METAL,
    HOST_PRESENTATION_UNSUPPORTED, HOST_PRESENTATION_SIMULATOR, HOST_PRESENTATION_VISIONOS };
struct host_benchmark_presentation_token { uint64_t generation; size_t index; };
void host_benchmark_presentation_configure(enum host_presentation_capability capability,
    int display_synchronized, int allows_tearing);
/* scene_is_cutscene comes from the current renderer frame context, not the
   legacy benchmark window. A zero generation means no callback is needed. */
struct host_benchmark_presentation_token host_benchmark_presentation_reserve(double submitted, int scene_is_cutscene);
/* Observe each primary present attempt, even without a drawable. Only a
   gameplay run's cutscene closes a segment; eligible missing frames remain gaps. */
void host_benchmark_presentation_scene(int scene_is_cutscene);
/* Deliberately excluded transition: break adjacency without reserving a frame. */
void host_benchmark_presentation_exclude(void);
/* Process-lifetime entry point: callbacks capture only the scalar token. */
void host_benchmark_presentation_complete(struct host_benchmark_presentation_token token, double presented);

/* at launch, before host_install_signal_handlers: the crash marker's signal handlers, which do nothing until a
   suite runs */
void host_benchmark_install(void);
/* Actual primary scene/final scaling paths observed in one frame, independent of the request. */
enum host_upscaler_path { HOST_UPSCALER_NONE = 1, HOST_UPSCALER_BILINEAR = 2,
    HOST_UPSCALER_METALFX = 4, HOST_UPSCALER_UNKNOWN = 8 };
void host_benchmark_upscaler(const char *requested, unsigned effective_paths);
/* Bounded typed configuration from the guest resolver during an open measurement window. */
void host_benchmark_graphics(const char *json);

/* the Metal backend, at each present: seconds since the last one, the frame's CPU and GPU seconds (negative:
   unknown), the display's refresh period (0: unknown), and the parts of the CPU time spent running the
   simulation's ticks, rendering and, within the render, translating and encoding, with the ticks it ran
   (host_frame_cpu_split) */
void host_benchmark_frame(double interval, double cpu, double gpu, double refresh_period, double simulation_cpu,
    double render_cpu, double translation_cpu, int ticks);
/* the game's marks around game_time_update and its render (main.c), through guest_host.h: phase 0 and 1 begin and
   end the ticks (ticks: how many ran), 2 and 3 the render; 4, at the render's end, the D3D8 device's translation
   and encoding time since the last (ticks: in nanoseconds, negative where unknown) */
enum { HOST_FRAME_SIMULATION_BEGIN, HOST_FRAME_SIMULATION_END, HOST_FRAME_RENDER_BEGIN, HOST_FRAME_RENDER_END,
    HOST_FRAME_TRANSLATION };
void host_frame_cpu_phase(int phase, int ticks);
/* at a present: the seconds spent in ticks, in the render and in translation since the last present, and the
   ticks run, then cleared; negative when the game marked none (translation: when it could not time it) */
/* debug.frame_cpu_probe: after host_benchmark_frame, on the drawing thread, its readings for that frame (nothing
   when the setting is off) */
void host_benchmark_frame_probe(void);
void host_frame_cpu_split(double *simulation, double *render, double *translation, int *ticks);
/* the Metal backend, for each frame it could not show (why: 0 no drawable, 1 no picture): counted so a run that
   measured nothing can say whether the display handed out no drawables */
void host_benchmark_frame_unshown(int why);
/* the Metal backend, for each shader library, specialization or pipeline compiled; any thread */
void host_benchmark_compile(double seconds);
/* 1 while a suite runs */
int host_benchmark_running(void);
/* debug.benchmark_offscreen (host_benchmark_offscreen.h): the guest's first clock read (xbox_kernel.c) asks, with
   the setting and whether debug.benchmark_suite names a suite; 1 when the mode is honored, and the guest then
   forces debug.fixed_timestep. Logged once either way. */
int host_benchmark_offscreen_request(int setting, int suite_named);
/* 1 when the mode was honored at launch */
int host_benchmark_offscreen_requested(void);
/* the guest's suite (benchmark_suite.c), for a presented reality check inside an offscreen suite and for the game
   after it: on, the frames are shown and paced by the display; off, offscreen again. 1 when the launch was honored
   offscreen, and the guest's clock then follows (platform_clock_realtime) */
int host_benchmark_presented(int on);
/* gpu_metal.m: the same for the renderer, which a fixed-timestep launch started unpaced */
void gpu_metal_benchmark_realtime(int on);
/* gpu_metal.m: the command buffers whose GPU times were used and refused since launch, and the last refused, as text */
void gpu_metal_gpu_spans_summary(char *text, size_t size);
#include "host_benchmark_progress.h"
/* Guest phase changes only; the native UI samples progress at 4 Hz. */
void host_benchmark_phase(int phase);
/* Hide synchronously before switching the renderer to presented work. */
void host_benchmark_overlay_presented(int presented);
void host_benchmark_progress(struct host_benchmark_progress *progress);
/* 1 while this frame is drawn offscreen: honored, and the suite running */
int host_benchmark_offscreen_active(void);

/* Tester entry screen. "full-unified" starts the signed baseline pass; legacy
   journal modes can resume. NULL chooses Play. */
const char *host_benchmark_menu(void);
void host_benchmark_menu_status(const char *message);
/* The UI only sets a latch; the guest owns partial sealing and replay/match teardown. */
void host_benchmark_request_finish(void);
int host_benchmark_finish_requested(void);
struct benchmark_live_snapshot;
/* Host-only, bounded submission telemetry; CPU work and completed GPU busy
   increments overlap and must not be added as a frame budget. */
void host_benchmark_live(struct benchmark_live_snapshot *snapshot);
void host_benchmark_live_reset(void);
void host_benchmark_overlay_show(int visible);


/* Queue an immutable report and attempt automatic CloudKit delivery for at most
   twelve seconds. NULL report_path retries pending reports. Failed attempts stay
   queued; the message describes confirmed delivery or pending local data. */
int host_benchmark_send(const char *report_path, const char *summary, char *message, unsigned long size);

/* host_benchmark_page.m: a still screen between runs, for up to seconds or until `until` (when given) returns
   nonzero: what comes next, and that Back stops the benchmark. 1 if Back was pressed */
int host_benchmark_notice(const char *heading, const char *detail, double seconds, int (*until)(void));

/* host_benchmark_page.m: the finished report on a page the device serves on the local network, with a screen
   that shows the page's address as a QR code, until Back or Menu is pressed or the minutes run out */
void host_benchmark_page(const char *report_path, const char *summary, const char *code, int minutes);

#endif
