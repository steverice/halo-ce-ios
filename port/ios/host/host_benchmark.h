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
   unknown), and the display's refresh period (0: unknown) */
void host_benchmark_frame(double interval, double cpu, double gpu, double refresh_period);
/* the Metal backend, for each frame it could not show (why: 0 no drawable, 1 no picture): counted so a run that
   measured nothing can say whether the display handed out no drawables */
void host_benchmark_frame_unshown(int why);
/* the Metal backend, for each shader library, specialization or pipeline compiled; any thread */
void host_benchmark_compile(double seconds);
/* 1 while a suite runs */
int host_benchmark_running(void);

/* Tester entry screen. "full-unified" starts the signed baseline pass; legacy
   journal modes can resume. NULL chooses Play. */
const char *host_benchmark_menu(void);
void host_benchmark_menu_status(const char *message);
/* The UI only sets a latch; the guest owns partial sealing and replay/match teardown. */
void host_benchmark_request_finish(void);
int host_benchmark_finish_requested(void);
void host_benchmark_live(double *frames_per_second);
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
