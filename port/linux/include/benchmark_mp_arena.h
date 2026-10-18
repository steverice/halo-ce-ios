/* Generic bounded arena mechanics. Workload dimensions come from signed data. */
#ifndef BENCHMARK_MP_ARENA_H
#define BENCHMARK_MP_ARENA_H
#include "mp_benchmark_probe.h"
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
		config->movement_radius <= config->spacing / 3.0f;
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

static inline int benchmark_mp_arena_observer_pad(float yaw_error, float pitch_error, int invert_look,
	long tick, struct mp_benchmark_probe_pad *pad)
{
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
	pad->right_x = (short)(-yaw_error * 32767.0f);
	pad->right_y = (short)((invert_look ? -pitch_error : pitch_error) * 32767.0f);
	pad->left_x = tick % 120 < 60 ? 11000 : -11000;
	pad->fire = tick % 90 < 24;
	return 1;
}
#endif
