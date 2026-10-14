/* The device benchmark's plan (benchmark_plan.h): which run comes next. */
#include "benchmark_plan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Suite 1's scenes that nobody plays: they run by themselves. The app adds one scene for each recorded route it
   carries (benchmark_scene_from_route; port/benchmark/recordings/NAME.route). The probe's own copy of the whole
   suite is in benchmark_probe.c */
const struct benchmark_scene benchmark_suite_scenes[] = {
    { "menu", "", "normal", NULL, 5.0, 30.0, BENCHMARK_SCENE_MENU, 0 },
    { "a10-opening", "a10", "normal", NULL, 2.0, 60.0, BENCHMARK_SCENE_CUTSCENE, 0 },
    /* the landing cutscene is about 75 s; then the beach, standing still */
    { "b30-beach", "b30", "easy", NULL, 85.0, 60.0, BENCHMARK_SCENE_GAMEPLAY, 1 },
};
const int benchmark_suite_scene_count = (int)(sizeof(benchmark_suite_scenes) / sizeof(benchmark_suite_scenes[0]));

/* the value of "key = value" in a route file's text, quotes and spaces trimmed; 0 if the key is not there */
static int route_value(const char *text, const char *key, char *value, size_t size) {
    size_t length = strlen(key);
    const char *line = text;

    while (line && *line) {
        const char *end = strchr(line, '\n'), *at = line + length;

        if (!strncmp(line, key, length) && (*at == ' ' || *at == '=' || *at == '\t')) {
            size_t count;

            while (*at == ' ' || *at == '\t') at++;
            if (*at == '=') {
                at++;
                while (*at == ' ' || *at == '\t' || *at == '"') at++;
                count = end ? (size_t)(end - at) : strlen(at);
                while (count && (at[count - 1] == ' ' || at[count - 1] == '\r' || at[count - 1] == '"' || at[count - 1] == '\t')) count--;
                if (count >= size) count = size - 1;
                memcpy(value, at, count);
                value[count] = 0;
                return 1;
            }
        }
        line = end ? end + 1 : NULL;
    }
    *value = 0;
    return 0;
}

int benchmark_scene_from_route(const char *text, const char *recording, struct benchmark_route *route) {
    static const char *const difficulties[] = { "easy", "normal", "hard", "impossible" };
    char number[32], kind[16];
    int index, known = 0;

    memset(route, 0, sizeof(*route));
    snprintf(route->recording, sizeof(route->recording), "%s", recording);
    if (!route_value(text, "map", route->map, sizeof(route->map)) || !*route->map) return 0;
    if (!route_value(text, "difficulty", route->difficulty, sizeof(route->difficulty))) return 0;
    for (index = 0; index < 4; index++) known |= !strcmp(route->difficulty, difficulties[index]);
    if (!known) return 0;
    /* a route that did not begin at its level's beginning cannot be replayed on another device */
    if (route_value(text, "first_tick", number, sizeof(number)) && atol(number) > 1) return 0;
    if (route_value(text, "entered", number, sizeof(number)) && !strcmp(number, "saved")) return 0;
    if (route_value(text, "players", number, sizeof(number)) && atol(number) != 1) return 0;
    if (!route_value(text, "scene", route->id, sizeof(route->id)) || !*route->id)
        snprintf(route->id, sizeof(route->id), "%s", recording);
    route->scene.lead = route_value(text, "lead", number, sizeof(number)) ? atof(number) : 0.0;
    route->scene.seconds = route_value(text, "window", number, sizeof(number)) ? atof(number) : 0.0;
    route->scene.kind = route_value(text, "kind", kind, sizeof(kind)) && !strcmp(kind, "cutscene") ? BENCHMARK_SCENE_CUTSCENE :
        BENCHMARK_SCENE_GAMEPLAY;
    route->scene.heavy = route->scene.kind == BENCHMARK_SCENE_GAMEPLAY &&
        !(route_value(text, "heavy", number, sizeof(number)) && !strcmp(number, "false"));
    route->scene.id = route->id;
    route->scene.map = route->map;
    route->scene.difficulty = route->difficulty;
    route->scene.recording = route->recording;
    return 1;
}

static const long ladder_lines[] = { 540, 720, 1080, 1440, 2160 };
#define LADDER_COUNT ((int)(sizeof(ladder_lines) / sizeof(ladder_lines[0])))
#define SIZE_STEPS_MAXIMUM 3
static const long anisotropy_values[] = { 16, 4, 2 };
static const long shadow_values[] = { 512, 256, 128 };

const char *benchmark_status_name(enum benchmark_status status) {
    static const char *const names[BENCHMARK_STATUS_COUNT] = { "ok", "skipped", "diverged", "thermal", "interrupted", "crashed",
        "killed", "failed" };

    return (unsigned)status < BENCHMARK_STATUS_COUNT ? names[status] : "failed";
}

const char *benchmark_mode_name(enum benchmark_mode mode) {
    return mode == BENCHMARK_FULL ? "full" : "quick";
}

unsigned long benchmark_plan_signature(const struct benchmark_plan *plan) {
    unsigned long hash = 2166136261UL + BENCHMARK_SUITE_VERSION;
    char text[256];
    int scene;
    const char *at;

    for (scene = 0; scene < plan->scene_count; scene++) {
        const struct benchmark_scene *one = &plan->scenes[scene];

        snprintf(text, sizeof(text), "%s|%s|%s|%s|%.2f|%.2f|%d|%d;", one->id, one->map, one->difficulty,
            one->recording ? one->recording : "", one->lead, one->seconds, (int)one->kind, one->heavy);
        for (at = text; *at; at++) hash = ((hash ^ (unsigned char)*at) * 16777619UL) & 0xffffffffUL;
    }
    return hash;
}

int benchmark_outcome_meets(const struct benchmark_outcome *outcome, int refreshes) {
    double period;

    if (!outcome || outcome->status != BENCHMARK_OK || outcome->refresh_hertz <= 0.0) return 0;
    period = 1000.0 * (double)refreshes / outcome->refresh_hertz;
    /* The GPU time is reported and not judged: on an Apple TV HD it read 29.5 ms at the 95th percentile for a menu
       that showed every frame for one refresh (2026-10-13), so it spans more than one frame's own work there.
       What the display got decides; the CPU's margin is kept because that time is the frame's own */
    return outcome->interval_p99 <= period * 1.05 && outcome->cpu_p95 <= period * 0.85 &&
        (outcome->left_least < 0 || outcome->left_least >= BENCHMARK_MEMORY_FLOOR);
}

/* The targets (the user's rule, 2026-10-13): 60 frames a second or better in play and in the menu, 30 in cutscenes.
   On a display that refreshes faster than 60 a frame may stay up for more than one refresh and still hold 60 */
static int holds(const struct benchmark_outcome *outcome, double frames_a_second) {
    long refreshes;

    if (!outcome || outcome->refresh_hertz <= 0.0) return 0;
    refreshes = (long)(outcome->refresh_hertz / frames_a_second + 0.5);
    return benchmark_outcome_meets(outcome, refreshes < 1 ? 1 : (int)refreshes);
}
#define PLAY_TARGET 60.0
#define CUTSCENE_TARGET 30.0
#define FAST_REFRESH 100.0

/* how hard a run pressed the device: the larger of its CPU and GPU times */
static double load(const struct benchmark_outcome *outcome) {
    return outcome->cpu_p95 > outcome->gpu_p95 ? outcome->cpu_p95 : outcome->gpu_p95;
}

struct walk {
    const struct benchmark_plan *plan;
    const struct benchmark_outcome *finished;
    int count, index, stopped;
    struct benchmark_run *next;
};

/* the outcome of the walk's next run if it has one; otherwise the run becomes the answer and the walk stops */
static const struct benchmark_outcome *step(struct walk *walk, char stage, int scene, long lines, long anisotropy,
    long shadow_size, int model_lod_max) {
    if (walk->stopped) return NULL;
    if (walk->index >= BENCHMARK_RUNS_MAXIMUM) { walk->stopped = 2; return NULL; }
    if (walk->index < walk->count) return &walk->finished[walk->index++];
    walk->next->index = walk->index;
    walk->next->stage = stage;
    walk->next->scene = scene;
    walk->next->lines = lines;
    walk->next->anisotropy = anisotropy;
    walk->next->shadow_size = shadow_size;
    walk->next->model_lod_max = model_lod_max;
    snprintf(walk->next->key, sizeof(walk->next->key), "%c %s %ld %ld %ld %s", stage, walk->plan->scenes[scene].id, lines,
        anisotropy, shadow_size, model_lod_max ? "max" : "auto");
    walk->stopped = 1;
    return NULL;
}

/* every heavy scene at one size; 1 if all met the target. Stops at the first that does not: the answer is known.
   *reference is the outcome of the scene `worst`, where it ran */
static int size_step(struct walk *walk, long lines, int worst, const struct benchmark_outcome **reference) {
    const struct benchmark_device *device = &walk->plan->device;
    int scene;

    *reference = NULL;
    for (scene = 0; scene < walk->plan->scene_count; scene++) {
        const struct benchmark_outcome *outcome;

        if (!walk->plan->scenes[scene].heavy) continue;
        outcome = step(walk, 'B', scene, lines, device->anisotropy, device->shadow_size, device->model_lod_max);
        if (!outcome) return 0;
        if (outcome->status == BENCHMARK_SKIPPED) continue;
        if (scene == worst) *reference = outcome;
        if (!holds(outcome, PLAY_TARGET)) return 0;
    }
    return 1;
}

int benchmark_plan_next(const struct benchmark_plan *plan, const struct benchmark_outcome *finished, int count,
    struct benchmark_run *next) {
    struct walk walk = { plan, finished, count, 0, 0, next };
    const struct benchmark_device *device = &plan->device;
    const struct benchmark_outcome *baseline[BENCHMARK_SCENES_MAXIMUM] = { 0 }, *reference = NULL;
    long ladder[LADDER_COUNT + 1], base_lines, lines, anisotropy = device->anisotropy, shadow_size = device->shadow_size;
    int ladder_count = 0, base = 0, chosen, scene, worst = -1, base_meets = 1, index, model_lod_max = device->model_lod_max;
    int heavier_only, lighter_only;

    if (plan->scene_count > BENCHMARK_SCENES_MAXIMUM) return 0;
    base_lines = device->base_lines > 0 && device->base_lines < device->drawable_lines ? device->base_lines : device->drawable_lines;

    /* A: every scene as the device's profile has it */
    for (scene = 0; scene < plan->scene_count; scene++) {
        /* (0 lines: the render size is left to the device's profile, which may have its own for the menu and for
           cutscenes; base_lines is what it gives gameplay, where the sweep starts) */
        baseline[scene] = step(&walk, 'A', scene, 0, device->anisotropy, device->shadow_size, device->model_lod_max);
        if (!baseline[scene]) return walk.stopped == 1;
        if (!plan->scenes[scene].heavy || baseline[scene]->status == BENCHMARK_SKIPPED) continue;
        if (!holds(baseline[scene], PLAY_TARGET)) base_meets = 0;
        if (baseline[scene]->status == BENCHMARK_OK && (worst < 0 || load(baseline[scene]) > load(baseline[worst]))) worst = scene;
    }
    if (plan->mode == BENCHMARK_QUICK) return 0;
    /* (no heavy scene gave frames: any of them stands in, and its runs will say why) */
    for (scene = 0; worst < 0 && scene < plan->scene_count; scene++)
        if (plan->scenes[scene].heavy && baseline[scene]->status != BENCHMARK_SKIPPED) worst = scene;
    if (worst < 0) return 0;

    /* B: the render size. The ladder's steps up to the display's own height, which is always one of them */
    for (index = 0; index < LADDER_COUNT; index++)
        if (ladder_lines[index] < device->drawable_lines) ladder[ladder_count++] = ladder_lines[index];
    ladder[ladder_count++] = device->drawable_lines;
    for (index = 0; index < ladder_count; index++)
        if (ladder[index] <= base_lines) base = index;
    chosen = base;
    reference = baseline[worst];
    if (base_meets) {
        for (index = base + 1; index < ladder_count && index <= base + SIZE_STEPS_MAXIMUM; index++) {
            const struct benchmark_outcome *at_size;

            if (!size_step(&walk, ladder[index], worst, &at_size)) {
                if (walk.stopped) return walk.stopped == 1;
                break;
            }
            chosen = index;
            reference = at_size;
        }
    } else {
        for (index = base - 1; index >= 0 && index >= base - SIZE_STEPS_MAXIMUM; index--) {
            const struct benchmark_outcome *at_size;
            int meets = size_step(&walk, ladder[index], worst, &at_size);

            if (walk.stopped) return walk.stopped == 1;
            chosen = index;
            reference = at_size;
            if (meets) break;
        }
    }
    lines = ladder[chosen];

    /* C: one setting at a time on the scene that pressed hardest, at the chosen size. With plenty of room only the
       costlier values can change the answer; with the target missed, only the cheaper ones. Each setting ends at
       the best value that held the target, or at the cheapest one tried if none did */
    heavier_only = holds(reference, PLAY_TARGET) && load(reference) < 0.5 * 1000.0 / PLAY_TARGET;
    lighter_only = !holds(reference, PLAY_TARGET);
    {
        long best = holds(reference, PLAY_TARGET) ? device->anisotropy : 0, cheapest = device->anisotropy;

        for (index = 0; index < 3; index++) {
            long value = anisotropy_values[index];
            const struct benchmark_outcome *outcome;

            if (value == device->anisotropy || (heavier_only && value < device->anisotropy) ||
                (lighter_only && value > device->anisotropy)) continue;
            outcome = step(&walk, 'C', worst, lines, value, device->shadow_size, device->model_lod_max);
            if (!outcome) return walk.stopped == 1;
            if (value < cheapest) cheapest = value;
            if (holds(outcome, PLAY_TARGET) && value > best) best = value;
        }
        anisotropy = best ? best : cheapest;
    }
    {
        long best = holds(reference, PLAY_TARGET) ? device->shadow_size : 0, cheapest = device->shadow_size;

        for (index = 0; index < 3; index++) {
            long value = shadow_values[index];
            const struct benchmark_outcome *outcome;

            if (value == device->shadow_size || (heavier_only && value < device->shadow_size) ||
                (lighter_only && value > device->shadow_size)) continue;
            outcome = step(&walk, 'C', worst, lines, device->anisotropy, value, device->model_lod_max);
            if (!outcome) return walk.stopped == 1;
            if (value < cheapest) cheapest = value;
            if (holds(outcome, PLAY_TARGET) && value > best) best = value;
        }
        shadow_size = best ? best : cheapest;
    }
    if (!(heavier_only && device->model_lod_max) && !(lighter_only && !device->model_lod_max)) {
        const struct benchmark_outcome *outcome = step(&walk, 'C', worst, lines, device->anisotropy, device->shadow_size,
            !device->model_lod_max);

        if (!outcome) return walk.stopped == 1;
        /* full detail where it held; the game's own where full detail was the profile's and did not */
        if (outcome->status != BENCHMARK_SKIPPED &&
            (device->model_lod_max ? !holds(reference, PLAY_TARGET) : holds(outcome, PLAY_TARGET)))
            model_lod_max = !device->model_lod_max;
    }

    /* D: the menu one size above play's (it must hold 60 there to keep it); a cutscene up to two sizes above,
       while it holds 30 */
    for (scene = 0; scene < plan->scene_count; scene++) {
        int steps = plan->scenes[scene].kind == BENCHMARK_SCENE_CUTSCENE ? 2 : 1;

        if (plan->scenes[scene].kind == BENCHMARK_SCENE_GAMEPLAY || baseline[scene]->status == BENCHMARK_SKIPPED) continue;
        for (index = chosen + 1; index < ladder_count && index <= chosen + steps; index++) {
            const struct benchmark_outcome *outcome = step(&walk, 'D', scene, ladder[index], anisotropy, shadow_size,
                model_lod_max);

            if (!outcome) return walk.stopped == 1;
            if (!holds(outcome, plan->scenes[scene].kind == BENCHMARK_SCENE_CUTSCENE ? CUTSCENE_TARGET : PLAY_TARGET)) break;
        }
    }

    /* F: a display that refreshes at 100 Hz or more: if the chosen settings do not already show a frame every
       refresh, how far off is that one size lower? Reported as an option, never chosen here */
    if (reference && reference->refresh_hertz >= FAST_REFRESH && !benchmark_outcome_meets(reference, 1) && chosen > 0)
        if (!step(&walk, 'F', worst, ladder[chosen - 1], anisotropy, shadow_size, model_lod_max)) return walk.stopped == 1;

    /* E: every scene at what the stages chose, unless that is the baseline over again */
    if (lines != base_lines || anisotropy != device->anisotropy || shadow_size != device->shadow_size ||
        model_lod_max != device->model_lod_max)
        for (scene = 0; scene < plan->scene_count; scene++) {
            if (baseline[scene]->status == BENCHMARK_SKIPPED) continue;
            if (!step(&walk, 'E', scene, lines, anisotropy, shadow_size, model_lod_max)) return walk.stopped == 1;
        }

    /* R: a baseline run that heat or an interruption spoiled, once more */
    for (scene = 0; scene < plan->scene_count; scene++)
        if (baseline[scene]->status == BENCHMARK_THERMAL || baseline[scene]->status == BENCHMARK_INTERRUPTED)
            if (!step(&walk, 'R', scene, 0, device->anisotropy, device->shadow_size, device->model_lod_max))
                return walk.stopped == 1;
    return 0;
}
