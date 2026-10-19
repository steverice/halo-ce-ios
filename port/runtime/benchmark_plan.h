/* The device benchmark's plan and journal.

   The plan is a pure function: given the device, the mode and the outcomes of
   the runs finished so far, it names the next run, or says the suite is over.
   So a suite that was cut off (a crash, the system ending the app for memory,
   the player leaving) carries on by reading the outcomes back from the journal
   and asking again; nothing else is saved.

   The journal is a text file that only grows, one line per event, flushed to
   disk as each is written, so a kill at any moment loses at most the run in
   progress. Plain C, tested by port/ios/tests/benchmark_probe.c. */
#ifndef HALO_BENCHMARK_PLAN_H
#define HALO_BENCHMARK_PLAN_H

#include <stddef.h>

#define BENCHMARK_RUNS_MAXIMUM 64
#define BENCHMARK_SCENES_MAXIMUM 16
/* megabytes that must stay free before the system's limit for a run to count as meeting its target */
#define BENCHMARK_MEMORY_FLOOR 300

enum benchmark_mode { BENCHMARK_QUICK, BENCHMARK_FULL, BENCHMARK_UNIFIED };

enum benchmark_status {
    BENCHMARK_OK,
    BENCHMARK_SKIPPED,       /* the scene cannot run here (no recording, no such map) */
    BENCHMARK_DIVERGED,      /* the replay left its route */
    BENCHMARK_THERMAL,       /* the device throttled during the run */
    BENCHMARK_INTERRUPTED,   /* the app left the foreground */
    BENCHMARK_CRASHED,       /* a signal ended the app during the run */
    BENCHMARK_KILLED,        /* the app ended during the run with no signal seen: the system, most likely for memory */
    BENCHMARK_FAILED,        /* the run never produced frames (a map that did not load, a timeout) */
    BENCHMARK_PARTIAL,       /* Finish ended this case; available measurements are retained, never qualified */
    BENCHMARK_STATUS_COUNT
};

enum benchmark_scene_kind { BENCHMARK_SCENE_MENU, BENCHMARK_SCENE_CUTSCENE, BENCHMARK_SCENE_GAMEPLAY,
    BENCHMARK_SCENE_MULTIPLAYER };

struct benchmark_scene {
    const char *id, *map;
    /* the console's difficulty names: easy, normal, hard, impossible */
    const char *difficulty;
    /* a recording's name (debug.input_record), or NULL: nothing drives the scene */
    const char *recording;
    /* seconds of game time before the measured window opens, and the window's length (0: to the recording's end) */
    double lead, seconds;
    enum benchmark_scene_kind kind;
    /* part of the resolution sweep */
    int heavy;
    /* Multiplayer workload parameters; zero or NULL for other scene kinds. */
    const char *simulation, *game_mode;
    int players, views, score_limit, time_limit_minutes;
    int arena_columns;
    double arena_spacing, arena_distance, arena_movement_radius;
};

/* what the plan needs to know about the device and its profile's values */
struct benchmark_device {
    long drawable_lines;   /* the display's height in pixels */
    long base_lines;       /* the render height the profile gives gameplay (0: the drawable's) */
    long anisotropy, shadow_size;
    int model_lod_max;     /* display.model_lod is "max" */
};

struct benchmark_outcome {
    enum benchmark_status status;
    double interval_p99, cpu_p95, gpu_p95, refresh_hertz;
    long left_least;       /* megabytes; negative: unknown */
};

struct benchmark_run {
    int index;
    /* A baseline, B resolution, C one setting at a time, D menu and cutscene, F one size lower toward 120,
       E confirmation, R a repeat */
    char stage;
    int scene;
    /* lines 0: the render size is left to the device's profile (the baseline) */
    long lines, anisotropy, shadow_size;
    int model_lod_max;
    /* names the run in the journal and the report: "A a10-crew 540 16 512 max" */
    char key[96];
};

struct benchmark_plan {
    enum benchmark_mode mode;
    const struct benchmark_scene *scenes;
    int scene_count;
    struct benchmark_device device;
};

/* A recorded route as a scene: the text of its NAME.route file ("key = value" lines; port/benchmark/README.md)
   and the recording's name. The scene's strings point into the route, which must outlive the plan. 0 for a route
   that cannot be a scene: no map or difficulty, not begun at its level's beginning, not single-player */
struct benchmark_route {
    struct benchmark_scene scene;
    char id[48], map[32], difficulty[16], recording[64];
};
int benchmark_scene_from_route(const char *text, const char *recording, struct benchmark_route *route);
/* Select an ordered subset of a validated signed catalog. No scenes are supplied by the engine. */
int benchmark_scenes_select(enum benchmark_mode mode, const struct benchmark_scene *catalog, int catalog_count,
    const char *const *only, int only_count, struct benchmark_scene *output, int capacity);
#define BENCHMARK_SUITE_VERSION 3

/* 1 and *next: the run after the count finished ones; 0: the suite is over */
int benchmark_plan_next(const struct benchmark_plan *plan, const struct benchmark_outcome *finished, int count,
    struct benchmark_run *next);
/* whether an outcome holds a target of one frame every `refreshes` refreshes */
int benchmark_outcome_meets(const struct benchmark_outcome *outcome, int refreshes);
/* a number that changes when the suite's scenes do, so a journal written for one set of scenes is never carried
   on with another */
unsigned long benchmark_plan_signature(const struct benchmark_plan *plan);
const char *benchmark_status_name(enum benchmark_status status);
const char *benchmark_mode_name(enum benchmark_mode mode);

/* ---------- the journal */

struct benchmark_journal {
    int valid;               /* a journal of this version with a suite line */
    enum benchmark_mode mode;
    char id[16];             /* the report's code */
    unsigned long signature; /* benchmark_plan_signature of the suite it was written for */
    struct benchmark_device device;
    struct benchmark_outcome outcomes[BENCHMARK_RUNS_MAXIMUM];
    int count;               /* finished runs, a cut-off one included */
    int cut_off;             /* the last outcome is a run that began and never ended (crashed or killed) */
    int crash_signal;        /* with cut_off: the signal the marker recorded, or 0 */
    long last_footprint, last_left;   /* the last memory line before the cut, megabytes; -1 unknown */
    int finished;            /* the suite's "done" line is there */
};

/* starts a journal over; 0 if the file cannot be written */
int benchmark_journal_start(const char *path, enum benchmark_mode mode, const char *id, const struct benchmark_device *device,
    unsigned long signature);
int benchmark_journal_begin(const char *path, int index, const char *key);
int benchmark_journal_end(const char *path, int index, const struct benchmark_outcome *outcome);
int benchmark_journal_memory(const char *path, long footprint, long left);
int benchmark_journal_done(const char *path);
/* for a signal handler: a descriptor to keep open, and a mark made with write() alone */
int benchmark_journal_marker_open(const char *path);
void benchmark_journal_mark_crash(int descriptor, int signal_number);
/* reads a journal back; a missing or foreign file gives valid = 0 */
void benchmark_journal_load(const char *path, struct benchmark_journal *journal);

#endif
