/* debug.benchmark_offscreen's rules (port/ios/host/host_benchmark_offscreen.h): tools/ios_test.py runs every case,
   tools/test_benchmark_offscreen.py one at a time by name. */
#include "host_benchmark_offscreen.h"
#include "host_benchmark_progress.h"

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

static void progress_format(void) {
    struct host_benchmark_progress p = { .mode = "cost:shadows", .number = 2, .total = 8,
        .elapsed = 125.9, .measured_fraction = -1, .offscreen = 1 };
    char text[512];
    host_benchmark_progress_detail(&p, text, sizeof(text));
    CHECK(strstr(text, "Settings cost\n") != NULL);
    CHECK(strstr(text, "Case 2 of 8") != NULL);
    CHECK(strstr(text, "Loading · Elapsed 2:05") != NULL);
    CHECK(strstr(text, "Measured window") == NULL);
    p.phase = HOST_BENCHMARK_LEAD_IN;
    host_benchmark_progress_detail(&p, text, sizeof(text));
    CHECK(strstr(text, "Lead-in") != NULL);
    p.phase = HOST_BENCHMARK_MEASURING;
    p.measured_fraction = 0.375;
    host_benchmark_progress_detail(&p, text, sizeof(text));
    CHECK(strstr(text, "Measuring") != NULL);
    CHECK(strstr(text, "Measured window: 38%") != NULL);
    p.measured_fraction = 1.5;
    host_benchmark_progress_detail(&p, text, sizeof(text));
    CHECK(strstr(text, "Measured window: 100%") != NULL);
    p.measured_fraction = -1;
    host_benchmark_progress_detail(&p, text, sizeof(text));
    CHECK(strstr(text, "Measured window") == NULL);
    p.presented = 1;
    host_benchmark_progress_detail(&p, text, sizeof(text));
    CHECK(strstr(text, "Presented check") != NULL);
    /* the phone's screen: level name alone as the title, the case id once after the kind, no percentage */
    snprintf(p.title, sizeof(p.title), "The Maw\nd40-maw-fight-base");
    snprintf(p.mode, sizeof(p.mode), "cost:d40-maw-fight-base");
    p.presented = 0; p.measured_fraction = 0.5;
    host_benchmark_progress_level(&p, text, sizeof(text));
    CHECK(!strcmp(text, "The Maw"));
    host_benchmark_progress_text(&p, text, sizeof(text), 1, 0);
    CHECK(strstr(text, "Settings cost · d40-maw-fight-base\nCase 2 of 8") == text);
    CHECK(strstr(text, "cost:") == NULL && strstr(text, "offscreen") == NULL && strstr(text, "Measured window") == NULL);
}

static void progress_visibility(void) {
    struct host_benchmark_progress p = {0};
    CHECK(!host_benchmark_progress_visible(&p));
    p.offscreen = 1;
    CHECK(host_benchmark_progress_visible(&p));
    /* A stage P case still has an offscreen lead before presentation begins. */
    p.reality_check = 1;
    CHECK(host_benchmark_progress_visible(&p));
    p.presented = 1;
    CHECK(!host_benchmark_progress_visible(&p));
    p.presented = 0;
    CHECK(host_benchmark_progress_visible(&p));
    p.offscreen = 0;
    CHECK(!host_benchmark_progress_visible(&p));
}

int main(int argc, char **argv) {
    static const struct { const char *name; void (*run)(void); } cases[] = {
        { "ignored_outside_suite", ignored_outside_suite }, { "active_in_suite", active_in_suite },
        { "ignored_on_stereo", ignored_on_stereo },
        { "progress_format", progress_format }, { "progress_visibility", progress_visibility },
    };
    size_t index, ran = 0;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++)
        if (argc < 2 || !strcmp(argv[1], cases[index].name)) { cases[index].run(); ran++; }
    if (!ran) { fprintf(stderr, "benchmark_offscreen_probe: no case %s\n", argv[1]); return 2; }
    if (failures) { fprintf(stderr, "benchmark_offscreen_probe: %d of %d checks failed\n", failures, checks); return 1; }
    printf("benchmark_offscreen_probe: %d checks passed\n", checks);
    return 0;
}
