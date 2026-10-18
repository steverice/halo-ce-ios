/* Generic bounded arena mechanics. Workload dimensions come from signed data. */
#ifndef BENCHMARK_MP_ARENA_H
#define BENCHMARK_MP_ARENA_H
#include "mp_benchmark_probe.h"
#include <math.h>
#include <string.h>

struct benchmark_mp_arena_config
{
	int columns;
	float spacing, distance, movement_radius;
};

static inline int benchmark_mp_arena_valid(const struct benchmark_mp_arena_config *config)
{
	return config && config->columns >= 1 && config->columns <= 8 &&
		config->spacing >= 0.8f && config->spacing <= 3.0f &&
		config->distance >= 2.0f && config->distance <= 12.0f &&
		config->movement_radius >= 0.15f && config->movement_radius <= 1.0f &&
		/* Signed integer milliunits remain authoritative. Allow only derived
		float roundoff, far below the next valid or invalid integer step. */
		config->movement_radius * 3.0f <= config->spacing + 0.000001f;
}

static inline int benchmark_mp_arena_offset(const struct benchmark_mp_arena_config *config,
	int players, int actor, float forward_x, float forward_y, float *position)
{
	float length = forward_x * forward_x + forward_y * forward_y;
	int columns;
	float distance, lateral;

	if (!benchmark_mp_arena_valid(config) || !position || players < 2 || players > 32 || actor < 0 || actor >= players ||
		!(length >= 0.99f && length <= 1.01f))
		return 0;
	if (!actor)
	{
		position[0] = position[1] = 0.0f;
		return 1;
	}
	columns = config->columns < players - 1 ? config->columns : players - 1;
	distance = config->distance + ((actor - 1) / columns) * config->spacing;
	lateral = ((actor - 1) % columns - (columns - 1) * 0.5f) * config->spacing;
	position[0] = forward_x * distance - forward_y * lateral;
	position[1] = forward_y * distance + forward_x * lateral;
	return 1;
}

/* Encode only the synthetic observer's intent through the Xbox axis dead zone. */
static inline short benchmark_mp_arena_observer_axis(float wanted)
{
	int raw;
	if (wanted == 0.0f) return 0;
	raw = (int)(9000.0f + (32767.0f - 9000.0f) * fabsf(wanted) + 0.5f);
	return (short)(wanted < 0.0f ? -raw : raw);
}

static inline int benchmark_mp_arena_observer_pad(float yaw_error, float pitch_error, int invert_look,
	long tick, struct mp_benchmark_probe_pad *pad)
{
	float magnitude, maximum, scale;
	if (!pad || !(yaw_error >= -100.0f && yaw_error <= 100.0f) ||
		!(pitch_error >= -3.2f && pitch_error <= 3.2f) || tick < 0)
		return 0;
	while (yaw_error > 3.14159265f) yaw_error -= 6.28318530f;
	while (yaw_error < -3.14159265f) yaw_error += 6.28318530f;
	if (yaw_error > 0.65f) yaw_error = 0.65f;
	if (yaw_error < -0.65f) yaw_error = -0.65f;
	if (pitch_error > 0.65f) pitch_error = 0.65f;
	if (pitch_error < -0.65f) pitch_error = -0.65f;
	memset(pad, 0, sizeof(*pad));
	/* input_abstraction's default layout negates the right stick's yaw. */
	/* input_abstraction expands the stick's circle to a square. Encode its
	desired result before the normal per-axis dead zone and that expansion. */
	magnitude = hypotf(yaw_error, pitch_error);
	maximum = fmaxf(fabsf(yaw_error), fabsf(pitch_error));
	scale = magnitude > 0.0f ? maximum / magnitude : 0.0f;
	pad->right_x = benchmark_mp_arena_observer_axis(-yaw_error * scale);
	pad->right_y = benchmark_mp_arena_observer_axis((invert_look ? -pitch_error : pitch_error) * scale);
	/* Reverse each second to reduce observer drift between arena checks. */
	pad->left_x = tick % 60 < 30 ? 16000 : -16000;
	pad->fire = tick % 90 < 24;
	return 1;
}
struct player_action;
void benchmark_mp_arena_reset(void);
int benchmark_mp_arena_configure(const struct benchmark_mp_arena_config *config);
int benchmark_mp_arena_enabled(void);
int benchmark_mp_arena_ready(void);
int benchmark_mp_arena_begin_combat(void);
int benchmark_mp_arena_combat_ready(void);
int benchmark_mp_arena_tick(long tick);
int benchmark_mp_arena_action(int actor, long tick, struct player_action *action);
int benchmark_mp_arena_gamepad(short port, long tick, struct mp_benchmark_probe_pad *pad);
unsigned long benchmark_mp_arena_repositions(int actor);
#endif
