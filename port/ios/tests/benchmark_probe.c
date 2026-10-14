/* The device benchmark's metrics, plan and journal (port/runtime/benchmark_*.c), with made-up frames and
   made-up devices: tools/ios_test.py builds and runs this. */
#include "benchmark_metrics.h"
#include "benchmark_plan.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; \
    fprintf(stderr, "benchmark_probe: line %d: %s\n", __LINE__, #condition); } } while (0)
#define NEAR(a, b, tolerance) (fabs((double)(a) - (double)(b)) <= (tolerance))

/* ---------- metrics */

static void test_metrics(void) {
    struct benchmark_metrics metrics = { 0 };
    struct benchmark_summary summary;
    char *json, *encoded;
    int index;

    benchmark_metrics_reset(&metrics, 60.0);
    benchmark_metrics_summarize(&metrics, &summary);
    CHECK(summary.frames == 0 && summary.frames_a_second == 0.0);

    /* 90 frames on time, 8 held for two refreshes, one for three, one stall of 120 ms */
    for (index = 0; index < 90; index++) benchmark_metrics_frame(&metrics, 16.7, 5.0, 8.0);
    for (index = 0; index < 8; index++) benchmark_metrics_frame(&metrics, 33.3, 12.0, 20.0);
    benchmark_metrics_frame(&metrics, 50.0, 30.0, 25.0);
    benchmark_metrics_frame(&metrics, 120.0, 100.0, 9.0);
    benchmark_metrics_summarize(&metrics, &summary);
    CHECK(summary.frames == 100);
    CHECK(summary.shown[1] == 90 && summary.shown[2] == 8 && summary.shown[3] == 1 && summary.shown[5] == 1);
    CHECK(NEAR(summary.on_time_full, 0.90, 1e-9) && NEAR(summary.on_time_half, 0.98, 1e-9));
    CHECK(NEAR(summary.interval.median, 16.7, 0.05));
    CHECK(NEAR(summary.interval.high, 33.3, 0.05));      /* the 95th of 100 is the 95th value */
    CHECK(NEAR(summary.interval.higher, 50.0, 0.05));    /* the 99th */
    CHECK(NEAR(summary.interval.worst, 120.0, 0.05));
    CHECK(NEAR(summary.cpu.median, 5.0, 0.05) && NEAR(summary.gpu.high, 20.0, 0.05) && NEAR(summary.gpu.worst, 25.0, 0.05));
    CHECK(NEAR(summary.seconds, (90 * 16.7 + 8 * 33.3 + 50.0 + 120.0) / 1000.0, 1e-6));
    CHECK(NEAR(summary.frames_a_second, 100.0 / summary.seconds, 1e-6));
    CHECK(summary.stalls == 2 && metrics.stall_count == 2 && NEAR(metrics.stalls[1].milliseconds, 120.0, 1e-9));
    CHECK(NEAR(summary.interval.mean, summary.seconds * 1000.0 / 100.0, 0.01));

    /* a frame too long for 16 bits is kept at the largest value, and unknown times as zero */
    benchmark_metrics_frame(&metrics, 9000.0, -1.0, -1.0);
    CHECK(metrics.interval[100] == 65535 && metrics.cpu[100] == 0 && metrics.gpu[100] == 0);

    /* samples: memory extremes, thermal changes */
    benchmark_metrics_sample(&metrics, 500, 900, BENCHMARK_THERMAL_NOMINAL);
    benchmark_metrics_sample(&metrics, 640, 760, BENCHMARK_THERMAL_NOMINAL);
    benchmark_metrics_sample(&metrics, -1, -1, BENCHMARK_THERMAL_SERIOUS);
    benchmark_metrics_sample(&metrics, 600, 800, BENCHMARK_THERMAL_FAIR);
    CHECK(metrics.footprint_first == 500 && metrics.footprint_peak == 640 && metrics.footprint_last == 600);
    CHECK(metrics.left_least == 760);
    CHECK(metrics.thermal_first == BENCHMARK_THERMAL_NOMINAL && metrics.thermal_worst == BENCHMARK_THERMAL_SERIOUS &&
        metrics.thermal_last == BENCHMARK_THERMAL_FAIR && metrics.thermal_change_count == 2);
    benchmark_metrics_compile(&metrics, 12.0);
    benchmark_metrics_compile(&metrics, 40.0);
    CHECK(metrics.compiles == 2 && NEAR(metrics.compile_longest, 40.0, 1e-9) && NEAR(metrics.compile_milliseconds, 52.0, 1e-9));

    json = benchmark_metrics_json(&metrics);
    CHECK(json && strstr(json, "\"frames\":101") && strstr(json, "\"shown_refreshes\":[90,8,1,0,2]") &&
        strstr(json, "\"left_least\":760") && strstr(json, "\"frame_interval\":\"") && strstr(json, "\"count\":2"));
    /* the first frame, 16.7 ms: 167 = 0xa7 0x00, twice over begins "pwCn" */
    CHECK(json && strstr(json, "\"frame_interval\":\"pwCnAK"));
    free(json);

    encoded = benchmark_base64("any carnal pleas", 16);
    CHECK(encoded && !strcmp(encoded, "YW55IGNhcm5hbCBwbGVhcw=="));
    free(encoded);
    encoded = benchmark_base64("", 0);
    CHECK(encoded && !*encoded);
    free(encoded);

    /* a 120 Hz display: 8.3 ms is one refresh, 16.7 two */
    benchmark_metrics_reset(&metrics, 120.0);
    CHECK(metrics.frames == 0 && metrics.left_least == -1 && metrics.thermal_worst == BENCHMARK_THERMAL_UNKNOWN);
    for (index = 0; index < 20000; index++) benchmark_metrics_frame(&metrics, index % 2 ? 8.3 : 16.7, 3.0, 4.0);
    benchmark_metrics_summarize(&metrics, &summary);
    CHECK(summary.frames == 20000 && summary.shown[1] == 10000 && summary.shown[2] == 10000);
    CHECK(benchmark_refreshes(0.5, 60.0) == 1 && benchmark_refreshes(41.0, 60.0) == 2 && benchmark_refreshes(42.0, 60.0) == 3);
    benchmark_metrics_free(&metrics);
}

/* ---------- the suite the plan is walked with: three scenes that play by themselves and two recorded routes */

static const struct benchmark_scene probe_scenes[] = {
    { "menu", "", "normal", NULL, 5.0, 30.0, BENCHMARK_SCENE_MENU, 0 },
    { "a10-opening", "a10", "normal", NULL, 2.0, 60.0, BENCHMARK_SCENE_CUTSCENE, 0 },
    { "a10-crew", "a10", "hard", "a10-bench", 0.0, 0.0, BENCHMARK_SCENE_GAMEPLAY, 1 },
    { "b30-beach", "b30", "easy", NULL, 85.0, 60.0, BENCHMARK_SCENE_GAMEPLAY, 1 },
    { "c40-bridge", "c40", "easy", "c40-bench", 0.0, 0.0, BENCHMARK_SCENE_GAMEPLAY, 1 },
};
#define PROBE_SCENE_COUNT ((int)(sizeof(probe_scenes) / sizeof(probe_scenes[0])))

static void test_routes(void) {
    struct benchmark_route route;

    CHECK(benchmark_scene_from_route("scene = a10-crew\nmap = a10\ndifficulty = hard\nfirst_tick = 0\nplayers = 1\nlead = 12.5\n"
        "window = 40\n", "a10-bench", &route));
    CHECK(!strcmp(route.scene.id, "a10-crew") && !strcmp(route.scene.map, "a10") && !strcmp(route.scene.difficulty, "hard") &&
        !strcmp(route.scene.recording, "a10-bench") && route.scene.lead == 12.5 && route.scene.seconds == 40.0 &&
        route.scene.kind == BENCHMARK_SCENE_GAMEPLAY && route.scene.heavy);
    /* a take as the recorder writes it: quoted values, other keys, no scene name (the recording's name stands in) */
    CHECK(benchmark_scene_from_route("map = a10\r\ndifficulty = impossible\r\nentered = start\nfirst_tick = 1\n"
        "display.model_lod = \"auto\"\nplayers = 1\nsegments = 3\n", "take-a10-20261013-213502", &route));
    CHECK(!strcmp(route.scene.id, "take-a10-20261013-213502") && !strcmp(route.scene.difficulty, "impossible") &&
        route.scene.lead == 0.0 && route.scene.seconds == 0.0);
    CHECK(benchmark_scene_from_route("map=c40\ndifficulty=easy\nkind = cutscene\n", "x", &route) &&
        route.scene.kind == BENCHMARK_SCENE_CUTSCENE && !route.scene.heavy);
    CHECK(benchmark_scene_from_route("map = c40\ndifficulty = easy\nheavy = false\n", "x", &route) && !route.scene.heavy);
    /* not usable: begun from a save or past the start, more than one player, no map, a difficulty that is no name */
    CHECK(!benchmark_scene_from_route("map = a10\ndifficulty = hard\nentered = saved\n", "x", &route));
    CHECK(!benchmark_scene_from_route("map = a10\ndifficulty = hard\nfirst_tick = 4500\n", "x", &route));
    CHECK(!benchmark_scene_from_route("map = a10\ndifficulty = hard\nplayers = 2\n", "x", &route));
    CHECK(!benchmark_scene_from_route("difficulty = hard\n", "x", &route));
    CHECK(!benchmark_scene_from_route("map = a10\ndifficulty = legendary\n", "x", &route));
    CHECK(!benchmark_scene_from_route("mapping = a10\ndifficulty = hard\n", "x", &route));
    CHECK(!benchmark_scene_from_route("", "x", &route));
}

/* ---------- a made-up device, to walk the plan with */

struct pretend {
    double gpu_at_1080;      /* GPU ms a frame in play at 1080 lines with 16x filtering and 512 shadows */
    double cpu;
    double hertz;
    long base_lines;         /* what the profile renders gameplay at: a run with 0 lines draws at this */
    int fail_index;          /* this run ends as fail_status (-1: none) */
    enum benchmark_status fail_status;
};

static struct benchmark_outcome pretend_run(const struct pretend *device, const struct benchmark_run *run) {
    const struct benchmark_scene *scene = &probe_scenes[run->scene];
    struct benchmark_outcome outcome = { BENCHMARK_OK, 0, 0, 0, device->hertz, 900 };
    long lines = run->lines ? run->lines : device->base_lines;
    double gpu = device->gpu_at_1080 * (double)(lines * lines) / (1080.0 * 1080.0), period = 1000.0 / device->hertz;

    gpu *= run->anisotropy >= 16 ? 1.0 : run->anisotropy >= 4 ? 0.8 : 0.75;
    gpu *= run->shadow_size >= 512 ? 1.0 : run->shadow_size >= 256 ? 0.93 : 0.9;
    gpu *= run->model_lod_max ? 1.0 : 0.95;
    if (scene->kind != BENCHMARK_SCENE_GAMEPLAY) gpu *= 0.4;
    if (!strcmp(scene->id, "b30-beach")) gpu *= 1.2;
    outcome.gpu_p95 = gpu;
    outcome.cpu_p95 = device->cpu;
    /* under vsync a frame takes whole refreshes */
    outcome.interval_p99 = ceil((gpu > device->cpu ? gpu : device->cpu) / (period * 0.85)) * period;
    if (run->index == device->fail_index) outcome.status = device->fail_status;
    return outcome;
}

/* walks a whole suite; returns the run count, and the last run of each stage through `last` (indexed by letter) */
static int walk_suite(enum benchmark_mode mode, const struct benchmark_device *facts, const struct pretend *device,
    struct benchmark_run *runs, struct benchmark_outcome *outcomes) {
    struct benchmark_plan plan = { mode, probe_scenes, PROBE_SCENE_COUNT, *facts };
    int count = 0;

    while (count < BENCHMARK_RUNS_MAXIMUM && benchmark_plan_next(&plan, outcomes, count, &runs[count])) {
        CHECK(runs[count].index == count);
        outcomes[count] = pretend_run(device, &runs[count]);
        count++;
    }
    return count;
}

static const struct benchmark_run *last_of_stage(const struct benchmark_run *runs, int count, char stage) {
    const struct benchmark_run *found = NULL;
    int index;

    for (index = 0; index < count; index++)
        if (runs[index].stage == stage) found = &runs[index];
    return found;
}

static int count_stage(const struct benchmark_run *runs, int count, char stage) {
    int index, total = 0;

    for (index = 0; index < count; index++) total += runs[index].stage == stage;
    return total;
}

static void test_plan(void) {
    struct benchmark_run runs[BENCHMARK_RUNS_MAXIMUM];
    struct benchmark_outcome outcomes[BENCHMARK_RUNS_MAXIMUM];
    struct benchmark_outcome outcome = { BENCHMARK_OK, 16.7, 9.0, 12.0, 59.94, 900 };
    const struct benchmark_run *run;
    int count;

    /* what "holds the target" means */
    CHECK(benchmark_outcome_meets(&outcome, 1));
    outcome.gpu_p95 = 14.5;                          /* over 85% of 16.7 */
    CHECK(!benchmark_outcome_meets(&outcome, 1) && benchmark_outcome_meets(&outcome, 2));
    outcome.gpu_p95 = 12.0; outcome.interval_p99 = 18.0;
    CHECK(!benchmark_outcome_meets(&outcome, 1));
    outcome.interval_p99 = 16.7; outcome.left_least = 250;
    CHECK(!benchmark_outcome_meets(&outcome, 1));
    outcome.left_least = -1;                         /* unknown memory does not fail a run */
    CHECK(benchmark_outcome_meets(&outcome, 1));
    outcome.status = BENCHMARK_THERMAL;
    CHECK(!benchmark_outcome_meets(&outcome, 1) && !benchmark_outcome_meets(NULL, 1));

    /* quick: the baseline, every scene once, at the profile's values */
    {
        struct benchmark_device facts = { 1080, 540, 4, 256, 0 };
        struct pretend device = { 60.0, 9.0, 59.94, 540, -1, BENCHMARK_OK };

        count = walk_suite(BENCHMARK_QUICK, &facts, &device, runs, outcomes);
        CHECK(count == PROBE_SCENE_COUNT && count_stage(runs, count, 'A') == count);
        /* a baseline run leaves the render size to the profile */
        CHECK(runs[0].lines == 0 && runs[0].anisotropy == 4 && runs[0].shadow_size == 256 && !runs[0].model_lod_max);
        CHECK(!strcmp(runs[2].key, "A a10-crew 0 4 256 auto"));
    }

    /* a slow device whose estimate was too hopeful: 1080 misses, the sweep steps down, nothing above the baseline
       is tried, the confirmation runs at the size that held */
    {
        struct benchmark_device facts = { 1080, 1080, 16, 512, 1 };
        struct pretend device = { 30.0, 9.0, 59.94, 1080, -1, BENCHMARK_OK };   /* holds at 540 only with cheaper settings */

        count = walk_suite(BENCHMARK_FULL, &facts, &device, runs, outcomes);
        CHECK(count > PROBE_SCENE_COUNT && count < BENCHMARK_RUNS_MAXIMUM);
        run = last_of_stage(runs, count, 'B');
        CHECK(run && run->lines < 1080);
        run = last_of_stage(runs, count, 'E');
        CHECK(run && run->lines == 540 && count_stage(runs, count, 'E') == PROBE_SCENE_COUNT);
        /* nothing in stage C costlier than the profile's, since the target was missed */
        for (run = runs; run < runs + count; run++)
            CHECK(run->stage != 'C' || (run->anisotropy <= 16 && run->shadow_size <= 512 && run->lines == 540));
        CHECK(count_stage(runs, count, 'F') == 0);
    }

    /* a fast device at a 4K display with a 1080 estimate: the sweep climbs, at most three sizes */
    {
        struct benchmark_device facts = { 2160, 1080, 4, 256, 0 };
        struct pretend device = { 2.5, 4.0, 59.94, 1080, -1, BENCHMARK_OK };

        count = walk_suite(BENCHMARK_FULL, &facts, &device, runs, outcomes);
        run = last_of_stage(runs, count, 'E');
        CHECK(run && run->lines == 2160 && run->anisotropy == 16 && run->shadow_size == 512 && run->model_lod_max);
        /* with room to spare, stage C tried only costlier values */
        for (run = runs; run < runs + count; run++)
            CHECK(run->stage != 'C' || (run->anisotropy >= 4 && run->shadow_size >= 256));
        /* nothing is above the display's own height, so the menu and cutscene have no stage D */
        CHECK(count_stage(runs, count, 'D') == 0 && count < BENCHMARK_RUNS_MAXIMUM);
    }

    /* a device whose estimate is exactly right: no size change, and the menu and cutscene are tried one size up */
    {
        struct benchmark_device facts = { 2160, 1080, 16, 512, 1 };
        struct pretend device = { 11.0, 6.0, 59.94, 1080, -1, BENCHMARK_OK };

        count = walk_suite(BENCHMARK_FULL, &facts, &device, runs, outcomes);
        CHECK(count_stage(runs, count, 'D') >= 2);
        run = last_of_stage(runs, count, 'D');
        CHECK(run && run->lines > 1080 && probe_scenes[run->scene].kind != BENCHMARK_SCENE_GAMEPLAY);
        /* the baseline was already the answer: no confirmation pass repeats it */
        CHECK(count_stage(runs, count, 'E') == 0);
    }

    /* a 120 Hz display: 60 is held with a frame every two refreshes, and one run asks what 120 would take */
    {
        struct benchmark_device facts = { 1440, 1440, 16, 512, 1 };
        struct pretend device = { 5.0, 5.0, 120.0, 1440, -1, BENCHMARK_OK };   /* 8.9 ms of GPU at 1440: 60 yes, 120 no */

        count = walk_suite(BENCHMARK_FULL, &facts, &device, runs, outcomes);
        CHECK(count_stage(runs, count, 'F') == 1);
        run = last_of_stage(runs, count, 'F');
        CHECK(run && run->lines == 1080);
        CHECK(count_stage(runs, count, 'B') == 0);   /* 1440 is the display's height: nothing above to try */
    }

    /* a baseline run spoiled by heat is run once more at the end */
    {
        struct benchmark_device facts = { 1080, 1080, 16, 512, 1 };
        struct pretend device = { 8.0, 6.0, 59.94, 1080, 1, BENCHMARK_THERMAL };

        count = walk_suite(BENCHMARK_FULL, &facts, &device, runs, outcomes);
        CHECK(count_stage(runs, count, 'R') == 1 && runs[count - 1].stage == 'R' && runs[count - 1].scene == 1);
    }

    /* scenes that cannot run here (no recording) are left out of every later stage */
    {
        struct benchmark_device facts = { 1080, 1080, 16, 512, 1 };
        struct benchmark_plan plan = { BENCHMARK_FULL, probe_scenes, PROBE_SCENE_COUNT, facts };
        struct pretend device = { 30.0, 9.0, 59.94, 1080, -1, BENCHMARK_OK };

        count = 0;
        while (count < BENCHMARK_RUNS_MAXIMUM && benchmark_plan_next(&plan, outcomes, count, &runs[count])) {
            outcomes[count] = pretend_run(&device, &runs[count]);
            if (probe_scenes[runs[count].scene].recording) {
                CHECK(runs[count].stage == 'A' || runs[count].stage == 'B');
                outcomes[count].status = BENCHMARK_SKIPPED;
            }
            count++;
        }
        CHECK(count < BENCHMARK_RUNS_MAXIMUM && last_of_stage(runs, count, 'E'));
        for (run = runs; run < runs + count; run++)
            CHECK(run->stage == 'A' || run->stage == 'B' || !probe_scenes[run->scene].recording);
    }

    /* every device from hopeless to idle ends, within the run limit */
    {
        double gpu;

        for (gpu = 0.5; gpu < 200.0; gpu *= 1.3) {
            struct benchmark_device facts = { 2160, 1080, 16, 512, 1 };
            struct pretend device = { gpu, 7.0, 59.94, 1080, -1, BENCHMARK_OK };

            count = walk_suite(BENCHMARK_FULL, &facts, &device, runs, outcomes);
            CHECK(count >= PROBE_SCENE_COUNT && count < BENCHMARK_RUNS_MAXIMUM);
        }
    }
}

/* ---------- journal */

static char journal_path[256];

static long file_size(const char *path) {
    FILE *file = fopen(path, "r");
    long size;

    if (!file) return -1;
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fclose(file);
    return size;
}

/* runs a suite through the journal as the app does, stopping (as a kill would) after `events` journal writes;
   returns 1 if the suite reached its end */
static int journaled_suite(const struct benchmark_device *facts, const struct pretend *device, int events, int crash) {
    struct benchmark_plan plan = { BENCHMARK_FULL, probe_scenes, PROBE_SCENE_COUNT, *facts };
    struct benchmark_journal journal;
    struct benchmark_run run;

    benchmark_journal_load(journal_path, &journal);
    if (!journal.valid || journal.finished) {
        if (events-- <= 0) return 0;
        benchmark_journal_start(journal_path, BENCHMARK_FULL, "TESTCODE", facts, 0x1234abcdUL);
        benchmark_journal_load(journal_path, &journal);
    } else if (journal.cut_off) {
        /* the app's first act on a resume: the cut-off run gets its end line */
        benchmark_journal_end(journal_path, journal.count - 1, &journal.outcomes[journal.count - 1]);
    }
    while (benchmark_plan_next(&plan, journal.outcomes, journal.count, &run)) {
        struct benchmark_outcome outcome = pretend_run(device, &run);

        if (events-- <= 0) return 0;
        benchmark_journal_begin(journal_path, run.index, run.key);
        if (events-- <= 0) {
            if (crash) {
                int marker = benchmark_journal_marker_open(journal_path);

                benchmark_journal_mark_crash(marker, 11);
                close(marker);
            }
            return 0;
        }
        benchmark_journal_memory(journal_path, 500, 420);
        if (events-- <= 0) return 0;
        benchmark_journal_end(journal_path, run.index, &outcome);
        journal.outcomes[journal.count++] = outcome;
    }
    benchmark_journal_done(journal_path);
    return 1;
}

static void test_journal(void) {
    struct benchmark_device facts = { 2160, 1080, 16, 512, 1 };
    struct pretend device = { 30.0, 9.0, 59.94, 1080, -1, BENCHMARK_OK };
    struct benchmark_journal whole, journal;
    struct benchmark_outcome outcome = { BENCHMARK_OK, 16.68, 9.25, 12.5, 59.94, 904 };
    char *bytes;
    long size, cut;
    int events, kills;
    FILE *file;

    snprintf(journal_path, sizeof(journal_path), "%s/visr-benchmark-probe-%d.journal", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp",
        (int)getpid());
    unlink(journal_path);
    benchmark_journal_load(journal_path, &journal);
    CHECK(!journal.valid);

    /* what is written comes back */
    CHECK(benchmark_journal_start(journal_path, BENCHMARK_FULL, "K7Q2M9XD", &facts, 0x5f3a9c01UL));
    CHECK(benchmark_journal_begin(journal_path, 0, "A menu 1080 16 512 max"));
    CHECK(benchmark_journal_memory(journal_path, 512, 904));
    CHECK(benchmark_journal_end(journal_path, 0, &outcome));
    benchmark_journal_load(journal_path, &journal);
    CHECK(journal.valid && journal.mode == BENCHMARK_FULL && !strcmp(journal.id, "K7Q2M9XD") && journal.count == 1 &&
        !journal.cut_off && !journal.finished);
    CHECK(journal.device.drawable_lines == 2160 && journal.device.base_lines == 1080 && journal.device.anisotropy == 16 &&
        journal.device.shadow_size == 512 && journal.device.model_lod_max == 1);
    CHECK(journal.signature == 0x5f3a9c01UL);
    {
        /* the signature follows the scenes: the same suite gives the same number, another scene list another */
        struct benchmark_plan same = { BENCHMARK_FULL, probe_scenes, PROBE_SCENE_COUNT, facts };
        struct benchmark_plan fewer = { BENCHMARK_FULL, probe_scenes, PROBE_SCENE_COUNT - 1, facts };

        CHECK(benchmark_plan_signature(&same) == benchmark_plan_signature(&same));
        CHECK(benchmark_plan_signature(&same) != benchmark_plan_signature(&fewer));
    }
    CHECK(journal.outcomes[0].status == BENCHMARK_OK && NEAR(journal.outcomes[0].interval_p99, 16.68, 1e-9) &&
        NEAR(journal.outcomes[0].cpu_p95, 9.25, 1e-9) && NEAR(journal.outcomes[0].gpu_p95, 12.5, 1e-9) &&
        NEAR(journal.outcomes[0].refresh_hertz, 59.94, 1e-9) && journal.outcomes[0].left_least == 904);

    /* a run that began and never ended: the system ended the app; its last memory line is kept */
    CHECK(benchmark_journal_begin(journal_path, 1, "A a10-opening 1080 16 512 max"));
    CHECK(benchmark_journal_memory(journal_path, 1400, 35));
    benchmark_journal_load(journal_path, &journal);
    CHECK(journal.valid && journal.count == 2 && journal.cut_off && journal.outcomes[1].status == BENCHMARK_KILLED &&
        journal.last_left == 35 && journal.last_footprint == 1400 && journal.outcomes[1].left_least == 35);
    /* with a signal's mark it crashed */
    {
        int marker = benchmark_journal_marker_open(journal_path);

        CHECK(marker >= 0);
        benchmark_journal_mark_crash(marker, 11);
        close(marker);
    }
    benchmark_journal_load(journal_path, &journal);
    CHECK(journal.cut_off && journal.outcomes[1].status == BENCHMARK_CRASHED && journal.crash_signal == 11);
    /* the resume writes the cut-off run's end; the journal then reads as two finished runs */
    CHECK(benchmark_journal_end(journal_path, 1, &journal.outcomes[1]));
    benchmark_journal_load(journal_path, &journal);
    CHECK(journal.valid && journal.count == 2 && !journal.cut_off && journal.outcomes[1].status == BENCHMARK_CRASHED);
    CHECK(benchmark_journal_done(journal_path));
    benchmark_journal_load(journal_path, &journal);
    CHECK(journal.finished);

    /* a foreign file, and a begin out of order, are not trusted */
    file = fopen(journal_path, "w");
    fputs("something else\nsuite full X 1 1 1 1 1 0\n", file);
    fclose(file);
    benchmark_journal_load(journal_path, &journal);
    CHECK(!journal.valid);
    benchmark_journal_start(journal_path, BENCHMARK_QUICK, "X", &facts, 1UL);
    benchmark_journal_begin(journal_path, 3, "A menu 1080 16 512 max");
    benchmark_journal_load(journal_path, &journal);
    CHECK(!journal.valid);

    /* a whole suite, uninterrupted */
    unlink(journal_path);
    CHECK(journaled_suite(&facts, &device, 1 << 20, 0));
    benchmark_journal_load(journal_path, &whole);
    CHECK(whole.valid && whole.finished && whole.count > PROBE_SCENE_COUNT);

    /* the file cut at every byte: never a crash, never more runs than the whole, and the finished ones agree */
    size = file_size(journal_path);
    bytes = malloc((size_t)size);
    file = fopen(journal_path, "r");
    CHECK(fread(bytes, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    for (cut = 0; cut <= size; cut++) {
        int index, finished;

        file = fopen(journal_path, "w");
        fwrite(bytes, 1, (size_t)cut, file);
        fclose(file);
        benchmark_journal_load(journal_path, &journal);
        if (!journal.valid) { CHECK(journal.count == 0); continue; }
        CHECK(journal.count <= whole.count);
        finished = journal.count - journal.cut_off;
        for (index = 0; index < finished; index++)
            CHECK(journal.outcomes[index].status == whole.outcomes[index].status &&
                NEAR(journal.outcomes[index].gpu_p95, whole.outcomes[index].gpu_p95, 1e-9));
    }
    free(bytes);

    /* the app killed after every possible journal write, once each, then reopened until the suite ends: it always
       ends, each kill costs exactly one run, and a kill between runs costs none */
    for (events = 0; events < 3 * whole.count + 3; events++) {
        int rounds = 0;

        unlink(journal_path);
        kills = journaled_suite(&facts, &device, events, events % 2) ? 0 : 1;
        while (!journaled_suite(&facts, &device, 1 << 20, 0) && ++rounds < 4) {}
        benchmark_journal_load(journal_path, &journal);
        CHECK(journal.valid && journal.finished && rounds < 4);
        if (kills) {
            int index, lost = 0;

            for (index = 0; index < journal.count; index++)
                lost += journal.outcomes[index].status == BENCHMARK_CRASHED || journal.outcomes[index].status == BENCHMARK_KILLED;
            CHECK(lost <= 1);
        }
    }
    unlink(journal_path);
}

int main(void) {
    test_metrics();
    test_routes();
    test_plan();
    test_journal();
    if (failures) {
        fprintf(stderr, "benchmark_probe: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("benchmark_probe: %d checks passed\n", checks);
    return 0;
}
