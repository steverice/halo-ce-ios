/* debug.benchmark_offscreen's rules (port/ios/host/host_benchmark_offscreen.h): tools/ios_test.py runs every case,
   tools/test_benchmark_offscreen.py one at a time by name. */
#include "host_benchmark_offscreen.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; \
    fprintf(stderr, "benchmark_offscreen_probe: line %d: %s\n", __LINE__, #condition); } } while (0)

/* the setting on, and no suite: every frame is shown */
static void ignored_outside_suite(void) {
    enum host_benchmark_offscreen decided = host_benchmark_offscreen_decide(1, 0, 0);

    CHECK(decided == HOST_BENCHMARK_OFFSCREEN_NO_SUITE);
    CHECK(!host_benchmark_offscreen_frame(decided, 0));
    CHECK(!host_benchmark_offscreen_frame(decided, 1));   /* a suite started later from the menu */
    /* a suite named at launch, before it starts and after it ends */
    CHECK(!host_benchmark_offscreen_frame(host_benchmark_offscreen_decide(1, 1, 0), 0));
    /* the setting off */
    CHECK(host_benchmark_offscreen_decide(0, 1, 0) == HOST_BENCHMARK_OFFSCREEN_OFF);
    CHECK(!host_benchmark_offscreen_frame(host_benchmark_offscreen_decide(0, 1, 0), 1));
    CHECK(!strcmp(host_benchmark_timing_basis(0), "submission_timing"));
}

/* the setting on with a suite named at launch: offscreen while the suite runs */
static void active_in_suite(void) {
    enum host_benchmark_offscreen decided = host_benchmark_offscreen_decide(1, 1, 0);

    CHECK(decided == HOST_BENCHMARK_OFFSCREEN_ON);
    CHECK(host_benchmark_offscreen_frame(decided, 1));
    CHECK(!strcmp(host_benchmark_timing_basis(1), "offscreen_uncapped"));
}

/* Vision Pro's stereo path keeps real time */
static void ignored_on_stereo(void) {
    enum host_benchmark_offscreen decided = host_benchmark_offscreen_decide(1, 1, 1);

    CHECK(decided == HOST_BENCHMARK_OFFSCREEN_STEREO);
    CHECK(!host_benchmark_offscreen_frame(decided, 1));
}

int main(int argc, char **argv) {
    static const struct { const char *name; void (*run)(void); } cases[] = {
        { "ignored_outside_suite", ignored_outside_suite }, { "active_in_suite", active_in_suite },
        { "ignored_on_stereo", ignored_on_stereo },
    };
    size_t index, ran = 0;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
        if (argc < 2 || !strcmp(argv[1], cases[index].name)) { cases[index].run(); ran++; }
    if (!ran) { fprintf(stderr, "benchmark_offscreen_probe: no case %s\n", argv[1]); return 2; }
    if (failures) { fprintf(stderr, "benchmark_offscreen_probe: %d of %d checks failed\n", failures, checks); return 1; }
    printf("benchmark_offscreen_probe: %d checks passed\n", checks);
    return 0;
}
