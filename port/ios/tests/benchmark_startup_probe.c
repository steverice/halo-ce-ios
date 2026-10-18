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
static int mp_begins, proof_begins, proof_failed, mp_ends, mp_probe_enabled, mp_probe_resets, suite_suspends, suite_restores;
static int suspension_allowed = 1;
static int mp_request, window_opens, window_closes, run_ends;
static int fatal_reports;
static char last_end_status[32];
static struct benchmark_mp_state observed_mp;
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
    if (mp_request) snprintf(text, size, "scene=%s map=chillout difficulty=normal kind=multiplayer_synthetic lead=15 seconds=60",
        mp_request == 2 ? "mp-chillout-2-proof" : "mp-chillout-4");
    else snprintf(text, size, "scene=startup map=a10 difficulty=normal kind=gameplay lead=100 seconds=10");
    return 1;
}
void host_benchmark_window(int open, int width, int height) {
    (void)width; (void)height; if (open) window_opens++; else window_closes++;
}
void host_benchmark_end(const char *status, const char *note) {
    (void)note; run_ends++; snprintf(last_end_status, sizeof(last_end_status), "%s", status);
}
void host_benchmark_fault(const char *note) { (void)note; fatal_reports++; }
int input_replay_suite_suspend(void) { suite_suspends++; return suspension_allowed; }
void input_replay_suite_restore(void) { suite_restores++; }
int network_test_suite_begin_chillout(void) { mp_begins++; return 1; }
int network_test_suite_begin_two_player_proof(void) { proof_begins++; return 1; }
int network_test_suite_two_player_proof_failed(void) { return proof_failed; }
void network_test_suite_end(void) { mp_ends++; }
void mp_benchmark_probe_set_suite_requested(int requested) { mp_probe_enabled = requested; }
void mp_benchmark_probe_reset_controllers(void) { mp_probe_resets++; }
int input_replay_begin(const char *name, int lip_sync) { (void)name; (void)lip_sync; return 1; }
int input_replay_ended(void) { return 0; }
long input_replay_segment(void) { return 0; }
double input_replay_length_ticks(void) { return 9000; }
double input_replay_timeline(void) { timeline_reads++; return 0; }
void benchmark_scenario_load(const char *map, const char *difficulty) {
    (void)difficulty; loads++; unsafe_loads += worker_active != 0; at_menu = !map || !*map;
}
unsigned char benchmark_scenario_at_menu(void) { return at_menu; }
unsigned char benchmark_scenario_player_position(float *position) { (void)position; return 0; }
void benchmark_scenario_multiplayer_state(struct benchmark_mp_state *state) { *state = observed_mp; }
int halo_scene_class(void) { return CONFIG_SCENE_GAMEPLAY; }
void halo_benchmark_overrides(long lines, long shadow, long anisotropy) { (void)lines; (void)shadow; (void)anisotropy; }
void halo_model_lod_set(int max) { (void)max; }

static void reset(void) {
    memset(&suite, 0, sizeof(suite));
    now = 0; mode = "quick"; at_menu = 1; worker_active = 0;
    readiness_reads = timeline_reads = starts = requests = loads = unsafe_loads = 0;
    mp_begins = proof_begins = proof_failed = mp_ends = mp_probe_enabled = mp_probe_resets = suite_suspends = suite_restores = 0;
    suspension_allowed = 1;
    mp_request = window_opens = window_closes = run_ends = fatal_reports = 0;
    last_end_status[0] = 0;
    memset(&observed_mp, 0, sizeof(observed_mp));
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

    reset(); mode = "full"; suspension_allowed = 0;
    CHECK(frame(0) == -1 && suite.state == suite_off);
    CHECK(starts == 0 && requests == 0 && loads == 0 && suite_suspends == 1 && suite_restores == 0);
    CHECK(strstr(last_log, "active recording") != NULL);
    CHECK(frame(100000) == -1 && starts == 0 && requests == 0);

    reset(); suite.state = suite_off; suite.pending_action = suite_action_mp_begin;
    CHECK(frame(0) == -1 && mp_begins == 0);
    benchmark_suite_mainloop_update();
    CHECK(mp_begins == 1 && mp_probe_enabled == 1 && mp_probe_resets == 1);
    suite.pending_action = suite_action_mp_end;
    CHECK(frame(1) == -1 && mp_ends == 0);
    benchmark_suite_mainloop_update();
    CHECK(mp_ends == 1 && mp_probe_enabled == 0);

    reset(); mode = "full"; mp_request = 1;
    frame(0); frame(3500); frame(3516);
    CHECK(suite.mp_scene && mp_begins == 0 && window_opens == 0);
    frame(3532); frame(6533);
    CHECK(mp_begins == 0 && suite.pending_action == suite_action_mp_begin);
    benchmark_suite_mainloop_update();
    CHECK(mp_begins == 1 && mp_probe_enabled == 1 && mp_probe_resets == 1);
    at_menu = 0;
    observed_mp = (struct benchmark_mp_state){ .tick = 30, .multiplayer = 1, .map_ok = 1, .slayer = 1,
        .score_limit = 1000, .local_players = 4, .roster = 4, .spawned = 4, .view_count = 1,
        .observer0 = 1, .probe_active = 1, .can_score = 1 };
    for (int player = 0; player < 4; player++) {
        observed_mp.actors[player].present = observed_mp.actors[player].spawned = 1;
        observed_mp.actors[player].weapon_definition = 1;
        observed_mp.actors[player].ammo = 100;
        observed_mp.actors[player].grenades[0] = 1;
        observed_mp.actors[player].shield = 1.0f;
        observed_mp.actors[player].throttle = 0.88f;
    }
    frame(12000);
    CHECK(window_opens == 0 && suite.state == suite_loading);
    observed_mp.tick = 60;
    frame(13000);
    CHECK(suite.state == suite_lead && window_opens == 0);
    observed_mp.tick = 510;
    frame(28000);
    CHECK(suite.state == suite_measuring && window_opens == 1);
    observed_mp.tick = 750;
    observed_mp.actors[0].spawned = 0;
    observed_mp.actors[0].deaths = 1;
    frame(36000);
    observed_mp.tick = 780;
    observed_mp.actors[0].spawned = 1;
    frame(37000);
    for (int step = 1; step <= 6; step++) {
        observed_mp.tick = 780 + step * 255;
        for (int player = 0; player < 4; player++)
            observed_mp.actors[player].position[0] = (float)(step * 2);
        if (step == 6) {
            for (int player = 0; player < 4; player++) {
                observed_mp.actors[player].fire_requests = 1;
                observed_mp.actors[player].ammo = 90;
            }
            observed_mp.actors[0].jump_requests = 1;
            observed_mp.actors[0].grenade_requests = 1;
            observed_mp.actors[0].grenades[0] = 0;
            observed_mp.actors[1].shield = 0.5f;
        }
        frame(37000 + step * 8500);
    }
    CHECK(run_ends == 1 && !strcmp(last_end_status, "ok") && window_closes == 1 &&
        suite.state == suite_wait_mp_teardown && mp_ends == 0);
    benchmark_suite_mainloop_update();
    CHECK(mp_ends == 1 && mp_probe_enabled == 0);
    at_menu = 1;
    frame(88016);
    CHECK(suite.state == suite_next);

    reset(); mode = "full"; mp_request = 2;
    frame(0); frame(3500); frame(3516); frame(3532); frame(6533);
    benchmark_suite_mainloop_update();
    CHECK(proof_begins == 1 && mp_begins == 0);
    at_menu = 0;
    observed_mp = (struct benchmark_mp_state){ .tick = 30, .multiplayer = 1, .map_ok = 1, .slayer = 1,
        .score_limit = 1000, .local_players = 1, .roster = 2, .spawned = 2, .view_count = 1,
        .observer0 = 1, .probe_active = 1, .synthetic_queue_ready = 1, .can_score = 1 };
    for (int player = 0; player < 2; player++) {
        observed_mp.actors[player].present = observed_mp.actors[player].spawned = 1;
        observed_mp.actors[player].weapon_definition = 1;
        observed_mp.actors[player].ammo = 100;
        observed_mp.actors[player].shield = 1.0f;
        observed_mp.actors[player].throttle = 0.75f;
    }
    frame(12000);
    observed_mp.tick = 60; frame(13000);
    observed_mp.tick = 510; frame(28000);
    CHECK(suite.state == suite_measuring && window_opens == 1);
    observed_mp.tick = 600; observed_mp.actors[1].spawned = 0; observed_mp.actors[1].deaths = 1;
    observed_mp.synthetic_queue_ready = 0; /* a dead unit may not have a dequeued action */
    frame(31000);
    observed_mp.tick = 630; observed_mp.actors[1].spawned = 1;
    observed_mp.synthetic_queue_ready = 1;
    frame(32000);
    for (int step = 1; step <= 20; step++) {
        observed_mp.tick = 630 + step * 90;
        for (int player = 0; player < 2; player++)
            observed_mp.actors[player].position[0] = (float)(step * 2);
        observed_mp.actors[1].ammo = 100 - step;
        observed_mp.actors[1].shield = 0.75f;
        observed_mp.projectiles_alive = step == 1;
        frame(32000 + step * 3000);
    }
    CHECK(run_ends == 1 && !strcmp(last_end_status, "ok") && window_closes == 1);

    reset(); suite.mp_scene = 1; strcpy(suite.scene, "mp-chillout-2-proof");
    suite.mp_distance[0] = suite.mp_distance[1] = 10.0;
    suite.mp_ammo_spent = suite.mp_damage_changes = suite.mp_deaths = suite.mp_respawns = 1;
    { char note[512];
        CHECK(!mp_evidence_note(note, sizeof(note)) && strstr(note, "synthetic") != NULL);
        suite.mp_projectile_seen = 1;
        CHECK(!mp_evidence_note(note, sizeof(note)));
        suite.mp_ammo_spent_actor[1] = 1;
        CHECK(mp_evidence_note(note, sizeof(note)) && strstr(note, "local1 synthetic1") != NULL);
    }

    reset(); suite.state = suite_measuring; suite.mp_scene = 1;
    benchmark_suite_multiplayer_fault("guest fatal assertion");
    benchmark_suite_multiplayer_fault("duplicate assertion");
    CHECK(fatal_reports == 1 && suite.state == suite_off);
    CHECK(frame(100000) == -1 && requests == 0 && run_ends == 0);
    reset(); suite.state = suite_measuring;
    benchmark_suite_multiplayer_fault("unrelated solo fault");
    CHECK(fatal_reports == 0 && suite.state == suite_measuring);

    reset(); suite.state = suite_loading; suite.mp_scene = suite.mp_host_started = 1;
    observed_mp.multiplayer = 1; observed_mp.tick = 30;
    frame(1000);
    CHECK(run_ends == 1 && !strcmp(last_end_status, "failed") && suite.state == suite_wait_mp_teardown);

    reset(); suite.state = suite_loading; suite.mp_scene = suite.mp_host_started = 1;
    snprintf(suite.scene, sizeof(suite.scene), "mp-chillout-2-proof");
    suite.mp_first_tick = suite.mp_last_tick = -1;
    observed_mp = (struct benchmark_mp_state){ .tick = 30, .multiplayer = 1, .map_ok = 1, .slayer = 1,
        .score_limit = 1000, .local_players = 1, .roster = 2, .spawned = 2, .view_count = 1,
        .observer0 = 1, .probe_active = 1, .can_score = 1, .synthetic_queue_ready = 1 };
    observed_mp.actors[0].present = observed_mp.actors[0].spawned = 1;
    observed_mp.actors[1].present = observed_mp.actors[1].spawned = 1;
    frame(1000);
    CHECK(suite.mp_first_tick == 30 && suite.state == suite_loading);
    observed_mp.tick = 60;
    frame(2000);
    CHECK(suite.state == suite_lead && suite.mp_ready_tick == 60);

    reset(); suite.state = suite_loading; suite.mp_scene = suite.mp_host_started = 1;
    snprintf(suite.scene, sizeof(suite.scene), "mp-chillout-2-proof");
    suite.mp_first_tick = suite.mp_last_tick = -1;
    observed_mp = (struct benchmark_mp_state){ .tick = 30, .multiplayer = 1, .map_ok = 1, .slayer = 1,
        .score_limit = 1000, .local_players = 1, .roster = 2, .spawned = 2, .view_count = 1,
        .observer0 = 1, .probe_active = 1, .can_score = 1 };
    observed_mp.actors[0].present = observed_mp.actors[0].spawned = 1;
    observed_mp.actors[1].present = observed_mp.actors[1].spawned = 1;
    frame(1000);
    CHECK(suite.state == suite_loading && suite.mp_first_tick == -1);

    reset(); suite.state = suite_loading; suite.mp_scene = suite.mp_host_started = 1;
    observed_mp = (struct benchmark_mp_state){ .tick = 30, .multiplayer = 1, .map_ok = 1, .slayer = 1,
        .score_limit = 1000, .local_players = 4, .roster = 3, .spawned = 3, .view_count = 1,
        .observer0 = 1, .probe_active = 1, .can_score = 1 };
    frame(LOAD_LIMIT_MILLISECONDS + 1);
    CHECK(run_ends == 1 && !strcmp(last_end_status, "failed") && suite.state == suite_wait_mp_teardown);

    reset(); suite.state = suite_lead; suite.mp_scene = suite.mp_host_started = 1;
    observed_mp = (struct benchmark_mp_state){ .tick = 30, .multiplayer = 1, .map_ok = 1, .slayer = 1,
        .score_limit = 1000, .local_players = 4, .roster = 4, .spawned = 4, .view_count = 1,
        .observer0 = 1, .probe_active = 1, .can_score = 1 };
    suite.mp_last_tick = 30; suite.mp_advanced_at = 0;
    frame(10001);
    CHECK(run_ends == 1 && !strcmp(last_end_status, "failed") && window_opens == 0);

    reset(); suite.state = suite_measuring; suite.mp_scene = suite.mp_host_started = 1;
    suite.seconds = 60; suite.opened_ticks = 30;
    observed_mp = (struct benchmark_mp_state){ .tick = 1830, .multiplayer = 1, .map_ok = 1, .slayer = 1,
        .score_limit = 1000, .local_players = 4, .roster = 4, .spawned = 4, .view_count = 1,
        .observer0 = 1, .probe_active = 1, .can_score = 1 };
    suite.mp_last_tick = 30;
    frame(60000);
    CHECK(run_ends == 1 && !strcmp(last_end_status, "failed") && window_closes == 1);

    reset(); suite.state = suite_loading; suite.mp_scene = 1;
    snprintf(suite.scene, sizeof(suite.scene), "mp-chillout-2-proof");
    suite.pending_action = suite_action_mp_begin;
    benchmark_suite_mainloop_update();
    CHECK(proof_begins == 1 && mp_begins == 0 && suite.mp_host_started == 1);
    proof_failed = 1;
    frame(1000);
    CHECK(run_ends == 1 && !strcmp(last_end_status, "failed") && suite.state == suite_wait_mp_teardown);

    if (failures) fprintf(stderr, "benchmark startup: %d failed\n", failures);
    else puts("benchmark startup: PASS (busy worker, timeout, next-frame handoff, ready and ordinary startup)");
    return failures != 0;
}
