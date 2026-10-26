/* The planner consumes actual levels. In shadow mode suggested levels never
 * become measurements: without actuation, recovery and clock behavior are
 * predictions. With actuation (host_dynamic_resolution_lines) each frame is
 * tagged with the level it was rendered at, which may trail the plan's. */
#pragma once
#include "dynamic_resolution.h"
#include "game_perf.h"
#include <stdlib.h>
struct dynamic_resolution_shadow {
    struct dynamic_resolution plan;
    int width, height, floor;
    unsigned pending_ignore;
    int actuate;
};
/* the level whose lines are nearest to the frame's actual lines */
static inline int dynamic_resolution_shadow_level(const struct dynamic_resolution *plan, int lines)
{
    int best = 0;
    for (int i = 1; i < plan->levels; i++)
        if (abs(plan->lines[i] - lines) < abs(plan->lines[best] - lines)) best = i;
    return best;
}
static inline void dynamic_resolution_shadow_frame(struct dynamic_resolution_shadow *shadow,
    game_perf_frame *value, int width)
{
    game_perf_dynamic_resolution *d = &value->drs;
    d->available &= ~4u;
    d->action = DYNAMIC_RESOLUTION_HOLD;
    /* A loading screen may itself present longer than the ignore delay. */
    if (value->flags & GAME_PERF_LOADING) d->ignore |= DYNAMIC_RESOLUTION_LOAD;
    shadow->pending_ignore |= d->ignore;
    if (width < 1 || !d->ceiling_lines || !d->actual_lines) return;
    if (shadow->width != width || shadow->height != (int)d->ceiling_lines || shadow->floor != (int)d->floor_lines) {
        shadow->width = width; shadow->height = (int)d->ceiling_lines; shadow->floor = (int)d->floor_lines;
        dynamic_resolution_init(&shadow->plan, width, shadow->height, shadow->floor, 0.0, 0.0);
    }
    d->chosen_lines = shadow->plan.lines[shadow->plan.level];
    if ((d->available & (2|8)) != (2|8) || (value->flags & GAME_PERF_PRESENT_FAILED)) return;
    struct dynamic_resolution_frame frame = {0};
    frame.frame_id = value->frame_id;
    /* Shadow mode always renders the ceiling: preserve this tag after virtual
     * drops. Actuation renders the frame's actual lines. */
    frame.level = shadow->actuate ? dynamic_resolution_shadow_level(&shadow->plan, (int)d->actual_lines) : 0;
    frame.elapsed = d->presentation_us / 1e6;
    frame.scene = d->scene_us / 1e6; frame.upscale = d->upscale_us / 1e6; frame.hud = d->hud_us / 1e6;
    frame.cpu = value->cpu_us == GAME_PERF_UNAVAILABLE ? INFINITY : value->cpu_us / 1e6;
    frame.budget = value->budget_us == GAME_PERF_UNAVAILABLE ? 0.0 : value->budget_us / 1e6;
    frame.gpu_available = (d->available & 1) != 0;
    frame.late = d->late;
    frame.thermal = (enum dynamic_resolution_thermal)d->thermal;
    frame.ignore = shadow->pending_ignore;
    shadow->pending_ignore = 0;
    d->action = dynamic_resolution_present(&shadow->plan, &frame);
    d->ignore |= shadow->plan.ignore_reason;
    d->chosen_lines = shadow->plan.lines[shadow->plan.level];
    d->available |= 4;
}
