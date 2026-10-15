/* The device benchmark's host side (host_benchmark.m): which run comes next
   (port/runtime/benchmark_plan.c), every frame's time, memory and heat while a
   run's measured window is open (port/runtime/benchmark_metrics.c), the journal
   that lets a cut-off suite carry on, the checks that say whether a run can be
   trusted, and the report. The game image's side is
   port/linux/src/benchmark_suite.c, which calls host_benchmark_start, _next,
   _window and _end through guest_host.h. */
#ifndef HALO_HOST_BENCHMARK_H
#define HALO_HOST_BENCHMARK_H

/* at launch, before host_install_signal_handlers: the crash marker's signal handlers, which do nothing until a
   suite runs */
void host_benchmark_install(void);
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

/* host_benchmark_menu.m, tester builds only: the first screen. "quick" or "full" when a benchmark was chosen (or one
   that was cut off is to carry on), NULL to play */
const char *host_benchmark_menu(void);

/* host_benchmark_send.m, builds that can send (HALO_BENCHMARK_CLOUD names the iCloud container): shows what the
   report holds, asks, and on Send saves it to the container's public database. 1 if it was sent; message gets a
   sentence for the screen either way */
int host_benchmark_send(const char *report_path, const char *summary, char *message, unsigned long size);

/* host_benchmark_page.m: a still screen between runs, for up to seconds or until `until` (when given) returns
   nonzero: what comes next, and that Back stops the benchmark. 1 if Back was pressed */
int host_benchmark_notice(const char *heading, const char *detail, double seconds, int (*until)(void));

/* host_benchmark_page.m: the finished report on a page the device serves on the local network, with a screen
   that shows the page's address as a QR code, until Back or Menu is pressed or the minutes run out */
void host_benchmark_page(const char *report_path, const char *summary, const char *code, int minutes);

#endif
