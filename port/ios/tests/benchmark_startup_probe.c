/* Execute the complete production benchmark state machine. The UI worker is
   deliberately held active while clocks advance and between startup frames. */
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HALO_IOS 1
#define __HALO_LINUX_PLATFORM_H
void platform_log(const char *format, ...);
static _Noreturn void probe_exit(int status);
#define exit probe_exit
#include "benchmark_suite.c"
#undef exit

static double now;
static const char *mode;
static unsigned char at_menu, worker_active;
static int readiness_reads, timeline_reads, starts, requests, loads, unsafe_loads, failures;
static int exit_status, exit_armed;
static jmp_buf exit_target;
static char last_log[512];
#define CHECK(condition) do { if (!(condition)) { failures++; \
    fprintf(stderr, "benchmark startup line %d: %s\n", __LINE__, #condition); } } while (0)

void platform_log(const char *format, ...) {
    va_list args; va_start(args, format); vsnprintf(last_log, sizeof(last_log), format, args); va_end(args);
}
static _Noreturn void probe_exit(int status) {
    assert(exit_armed);
    exit_status = status;
    longjmp(exit_target, 1);
}
const char *config_string(const char *name) {
    return !strcmp(name, "debug.benchmark_suite") ? mode : "auto";
}
long config_integer(const char *name) { (void)name; return 0; }
double config_scene_real(const char *name, int scene) { (void)name; (void)scene; return 1; }
long config_scene_integer(const char *name, int scene) { (void)name; (void)scene; return 0; }
void platform_video_drawable_size(int *width, int *height) { *width = 1280; *height = 720; }
double halo_frame_trace_milliseconds(void) { return now; }
unsigned char filesystem_check_thread_is_active(void) { readiness_reads++; return worker_active; }
int host_benchmark_start(const char *name, int drawable, int base, int anisotropy, int shadow, int lod) {
    (void)name; (void)drawable; (void)base; (void)anisotropy; (void)shadow; (void)lod; starts++; return 1;
}
int host_benchmark_next(char *text, unsigned int size) {
    requests++;
    snprintf(text, size, "scene=startup map=a10 difficulty=normal kind=gameplay lead=100 seconds=10"); return 1;
}
void host_benchmark_window(int open, int width, int height) { (void)open; (void)width; (void)height; }
void host_benchmark_end(const char *status, const char *note) { (void)status; (void)note; }
int input_replay_begin(const char *name, int lip_sync) { (void)name; (void)lip_sync; return 1; }
int input_replay_ended(void) { return 0; }
long input_replay_segment(void) { return 0; }
double input_replay_length_ticks(void) { return 9000; }
double input_replay_timeline(void) { timeline_reads++; return 0; }
void benchmark_scenario_load(const char *map, const char *difficulty) {
    (void)map; (void)difficulty; loads++; unsafe_loads += worker_active != 0; at_menu = 0;
}
unsigned char benchmark_scenario_at_menu(void) { return at_menu; }
unsigned char benchmark_scenario_player_position(float *position) { (void)position; return 0; }
int halo_scene_class(void) { return CONFIG_SCENE_GAMEPLAY; }
void halo_benchmark_overrides(long lines, long shadow, long anisotropy) { (void)lines; (void)shadow; (void)anisotropy; }
void halo_model_lod_set(int max) { (void)max; }

static void reset(void) {
    memset(&suite, 0, sizeof(suite));
    now = 0; mode = "quick"; at_menu = 1; worker_active = 0;
    readiness_reads = timeline_reads = starts = requests = loads = unsafe_loads = 0;
    last_log[0] = 0;
}
static int frame(double milliseconds) {
    now = milliseconds; exit_status = -1; exit_armed = 1;
    if (!setjmp(exit_target)) benchmark_suite_frame(1280, 720);
    exit_armed = 0; return exit_status;
}
int main(void) {
    reset(); worker_active = 1;
    CHECK(frame(0) == -1 && starts == 1);
    CHECK(frame(3500) == -1 && suite.state == suite_starting);
    CHECK(frame(4000) == -1 && requests == 0 && loads == 0);
    CHECK(frame(START_LIMIT_MILLISECONDS + 1) == EXIT_FAILURE);
    CHECK(requests == 0 && loads == 0 && unsafe_loads == 0 && timeline_reads == 0);
    CHECK(strstr(last_log, "filesystem") != NULL);

    reset(); worker_active = 1; at_menu = 0;
    frame(0);
    CHECK(frame(START_LIMIT_MILLISECONDS + 1) == EXIT_FAILURE);
    CHECK(requests == 0 && loads == 0); /* timeout cannot bypass readiness without a menu either */

    reset(); worker_active = 1; frame(0); frame(3500);
    worker_active = 0; /* the normal UI pump joined and cleared the worker */
    CHECK(frame(3516) == -1 && suite.state == suite_next && requests == 0);
    CHECK(frame(3532) == -1 && loads == 1 && requests == 1 && starts == 1);
    frame(3548);
    CHECK(loads == 1 && unsafe_loads == 0);

    reset(); frame(0); frame(3500);
    CHECK(suite.state == suite_next);
    worker_active = 1; /* initialization starts after the first readiness observation */
    CHECK(frame(3516) == -1 && suite.state == suite_next && loads == 0 && requests == 0);
    worker_active = 0;
    CHECK(frame(3532) == -1 && loads == 1 && requests == 1 && unsafe_loads == 0);

    reset(); frame(0); frame(3500); worker_active = 1;
    CHECK(frame(3500 + START_LIMIT_MILLISECONDS + 1) == EXIT_FAILURE);
    CHECK(requests == 0 && loads == 0);

    reset(); at_menu = 0; frame(0);
    CHECK(frame(START_LIMIT_MILLISECONDS + 1) == -1 && suite.state == suite_next);
    CHECK(frame(START_LIMIT_MILLISECONDS + 2) == -1 && loads == 1); /* existing direct-map fallback */

    reset(); worker_active = 1; mode = "";
    CHECK(frame(0) == -1 && suite.state == suite_off);
    frame(100000);
    CHECK(starts == 0 && requests == 0 && loads == 0 && readiness_reads == 0 && timeline_reads == 0);
    reset(); worker_active = 1; mode = "invalid";
    CHECK(frame(0) == -1 && suite.state == suite_off && readiness_reads == 0);

    if (failures) fprintf(stderr, "benchmark startup: %d failed\n", failures);
    else puts("benchmark startup: PASS (busy worker, timeout, next-frame handoff, ready and ordinary startup)");
    return failures != 0;
}
