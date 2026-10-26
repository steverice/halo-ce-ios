/* Dynamic resolution's actuation through host_perf_hooks.c without graphics:
 * the guest asks for lines between frames (host_dynamic_resolution_lines),
 * renders them, and the frames it presents come back tagged with them.
 * A sustained fight lowers the lines; its end raises them back to the
 * ceiling one level at a time; a refusal returns to the ceiling at once.
 * "pin" and "sweep" (argv[1]) check the debug overrides; "profile" turns it on from a
 * device profile alone, installed after the first question; "notiming" is a GPU
 * without stage-boundary timing, where it stays off. */
#include "host_perf_hooks.h"
#include "host_perf_reports.h"
#include "dynamic_resolution.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static double load = 1.0;
static unsigned rows, drops, recoveries;
static int profile_installed = 1, profile_on, stage_timing = 1, ineligible;
int host_config_profile_installed(void) { return profile_installed; }
int host_perf_metal_stage_supported(void) { return stage_timing; }
void host_config_string(const char *name, const char *fallback, char *out, size_t size) {
    /* the device profile's value, once installed */
    if (profile_on && profile_installed && !strcmp(name, "display.dynamic_resolution")) { snprintf(out, size, "true"); return; }
    snprintf(out, size, "%s", fallback);
}
void host_perf_reports_frame(const game_perf_frame *frame) {
    rows++;
    if (frame->drs.action == DYNAMIC_RESOLUTION_DROP || frame->drs.action == DYNAMIC_RESOLUTION_PANIC) drops++;
    if (frame->drs.action == DYNAMIC_RESOLUTION_RECOVER) recoveries++;
}
void host_perf_reports_event(const game_perf_event *event) { (void)event; }
/* The GPU: a fixed 2 ms plus 11 ms a ceiling's worth of pixels, times the
 * load; upscale and HUD 1 ms. Late when over the 16.7 ms budget. */
void host_perf_metal_join(game_perf_frame *frame) {
    double share = (double)frame->drs.actual_lines / 1080.0;
    uint64_t scene = (uint64_t)((2000.0 + 11000.0 * share * share) * load);
    frame->drs.available = 1 | 2 | 8;
    frame->drs.scene_us = scene; frame->drs.upscale_us = 400; frame->drs.hud_us = 600;
    frame->drs.presentation_us = scene + 1000 > 16667 ? 33333 : 16667;
    frame->drs.late = scene + 1000 > 16667;
}
static unsigned long long now = 1000000;
static int frame(int ceiling) {
    int lines = host_dynamic_resolution_lines(ceiling);
    int rendered = lines > 0 ? lines : ceiling;
    host_perf_hooks_phase_at(7, 0, now);
    host_perf_hooks_present_begin_at(now + 100);
    host_perf_hooks_dynamic_config(1, 1920, 1080, rendered, 540, 0, ceiling > 0 && !ineligible);
    host_perf_hooks_budget(16667);
    host_perf_hooks_cpu(3000);
    host_perf_hooks_present_end_at(now + 200);
    now += 16667;
    return lines;
}
int main(int count, char **arguments) {
    const char *mode = count > 1 ? arguments[1] : "plan";
    int lines = 0, lowest = 1080, previous, i;
    if (!strcmp(mode, "pin")) assert(!setenv("HALO_DYNAMIC_RESOLUTION_LINES", "700", 1));
    else if (!strcmp(mode, "sweep")) assert(!setenv("HALO_DYNAMIC_RESOLUTION_SWEEP", "true", 1));
    else if (!strcmp(mode, "profile")) {
        /* asked before the profile is installed: not settled yet, and off */
        profile_installed = 0; profile_on = 1;
        assert(!host_perf_hooks_dynamic_actuates() && host_dynamic_resolution_lines(1080) == 0);
        profile_installed = 1;
    } else assert(!setenv("HALO_DYNAMIC_RESOLUTION", "true", 1));
    assert(host_perf_hooks_dynamic_actuates());
    if (!strcmp(mode, "notiming")) {
        stage_timing = 0;
        assert(host_dynamic_resolution_lines(-1) == 0);
        for (i = 0; i < 60; i++) assert(frame(1080) == 0);
        /* (and benchmark reports carry no lines distribution) */
        assert(host_perf_hooks_dynamic_lines() == 0);
        return 0;
    }
    assert(host_dynamic_resolution_lines(-1) == 1);
    /* the first frames at a ceiling render it, until one is measured */
    assert(frame(1080) == 1080);
    for (i = 0; i < 120; i++) lines = frame(1080);
    if (!strcmp(mode, "pin")) {
        assert(lines == 700);
        /* (a new ceiling renders itself until it is measured) */
        assert(host_dynamic_resolution_lines(600) == 600);
        return 0;
    }
    if (!strcmp(mode, "sweep")) {
        int seen_floor = 0, seen_ceiling = 0, changes = 0;
        previous = lines;
        for (i = 0; i < 1200; i++) {
            lines = frame(1080);
            assert(lines >= 540 && lines <= 1080);
            seen_floor |= lines == 540; seen_ceiling |= lines == 1080;
            if (lines != previous) { assert(abs(lines - previous) <= 41); changes++; }
            previous = lines;
        }
        assert(seen_floor && seen_ceiling && changes >= 28);
        return 0;
    }
    /* an easy scene holds the ceiling */
    assert(lines == 1080 && drops == 0);
    /* a fight: 1.6 times the work, late at the ceiling */
    load = 1.6;
    for (i = 0; i < 60; i++) { lines = frame(1080); if (lines < lowest) lowest = lines; }
    assert(drops >= 1 && lowest < 1080 && lowest >= 540);
    /* once lowered, the fight's frames are on time again */
    {
        double share = lowest / 1080.0;
        assert((2000.0 + 11000.0 * share * share) * load + 1000 <= 16667);
    }
    /* the fight ends: back to the ceiling one level at a time, within seconds */
    load = 1.0;
    previous = lines;
    for (i = 0; i < 600 && lines < 1080; i++) {
        lines = frame(1080);
        assert(lines >= previous || lines == previous);
        assert(lines - previous <= 41);
        previous = lines;
    }
    assert(lines == 1080 && recoveries >= 1);
    /* a refusal (a stereo frame, a menu) is the ceiling at once, and resets */
    load = 1.6;
    for (i = 0; i < 60; i++) lines = frame(1080);
    assert(lines < 1080);
    assert(frame(0) == 0);
    assert(frame(1080) == 1080);
    /* split screen: frames the planner cannot measure return to the ceiling */
    for (i = 0; i < 60; i++) lines = frame(1080);
    assert(lines < 1080);
    ineligible = 1;
    frame(1080);
    assert(frame(1080) == 1080);
    for (i = 0; i < 30; i++) assert(frame(1080) == 1080);
    ineligible = 0;
    /* another ceiling (a scene class's): the planner starts over there */
    assert(frame(900) == 900);
    printf("dynamic resolution actuation: %u rows, %u drops, %u recoveries, lowest %d\n", rows, drops, recoveries, lowest);
    return 0;
}
