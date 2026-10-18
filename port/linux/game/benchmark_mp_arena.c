/* Scenario-derived staging and ordinary queued input for signed benchmark arenas. */
#ifndef BENCHMARK_MP_ARENA_PROBE
#include "cseries.h"
#include "game/game.h"
#include "game/players.h"
#include "input/input_abstraction.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_files.h"
#include "network_test_suite.h"
#endif
#include "benchmark_mp_arena.h"
#include <math.h>
#include <string.h>

void platform_log(const char *format, ...);

static struct
{
	struct benchmark_mp_arena_config config;
	int enabled, ready, failed, players, combat;
	real_point3d points[32], previous[32];
	long units[32], stuck_ticks[32], last_tick;
	unsigned long repositions[32];
} arena;

void benchmark_mp_arena_reset(void)
{
	memset(&arena, 0, sizeof(arena));
}

int benchmark_mp_arena_configure(const struct benchmark_mp_arena_config *config)
{
	int players = network_test_suite_network_players(), i;

	if (arena.enabled || players < 2 || players > 32 || !benchmark_mp_arena_valid(config))
		return 0;
	arena.config = *config;
	arena.players = players;
	arena.enabled = 1;
	arena.last_tick = NONE;
	for (i = 0; i < players; i++) arena.units[i] = NONE;
	return 1;
}

int benchmark_mp_arena_enabled(void)
{
	return arena.enabled;
}

int benchmark_mp_arena_ready(void)
{
	return !arena.enabled || (arena.ready && !arena.failed);
}

int benchmark_mp_arena_combat_ready(void)
{
	return !arena.enabled || (arena.ready && arena.combat && !arena.failed);
}

int benchmark_mp_arena_begin_combat(void)
{
	if (!arena.enabled) return 1;
	if (!arena.ready || arena.failed) return 0;
	if (!arena.combat)
		platform_log("network test: arena combat enabled after host readiness at tick %ld", arena.last_tick);
	arena.combat = 1;
	return 1;
}

unsigned long benchmark_mp_arena_repositions(int actor)
{
	return actor >= 0 && actor < arena.players ? arena.repositions[actor] : 0;
}

static struct player_datum *arena_player(int actor)
{
	long *list = machine_get_player_list(actor);
	struct player_datum *player = list && list[0] != NONE && list[1] == NONE ? player_try_and_get(list[0]) : NULL;

	return player && player->network_player_data.machine_index == actor &&
		player->local_player_index == (actor ? NONE : 0) ? player : NULL;
}

static int arena_ground(real_point3d *position, int *rejections)
{
	real_point3d above = *position, center;
	real_vector3d down = { 0.0f, 0.0f, -6.0f }, height = { 0.0f, 0.0f, 0.35f };
	struct collision_result collision;
	struct location feet, head;

	above.z += 2.0f;
	if (!collision_test_vector(FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit),
		&above, &down, NONE, &collision))
	{
		rejections[0]++;
		return 0;
	}
	if (collision.plane.n.k < 0.75f)
	{
		rejections[1]++;
		return 0;
	}
	*position = collision.point;
	position->z += 0.05f;
	center = *position;
	center.z += 0.25f;
	if (collision_test_pill(_collision_test_for_line_of_sight_flags, &center, &height, 0.25f, NONE, &collision))
	{
		rejections[2]++;
		return 0;
	}
	above = *position;
	above.z += 0.7f;
	scenario_location_from_point(&feet, position);
	scenario_location_from_point(&head, &above);
	if (feet.cluster_index == NONE || head.cluster_index == NONE)
	{
		rejections[3] += feet.cluster_index == NONE;
		rejections[4] += head.cluster_index == NONE;
		return 0;
	}
	return 1;
}

static int arena_choose(void)
{
	struct scenario *scenario = global_scenario_try_and_get();
	real_point3d points[32] = { 0 };
	int start, candidates = 0, ground_rejections[5] = { 0 }, sight_rejections = 0, first_rejected_actor = NONE;

	if (!scenario || scenario->players.count <= 0)
	{
		platform_log("network test: arena geometry exhausted: scenario has no player starts");
		return 0;
	}
	/* Bound setup work even for a malformed or unusually large scenario. */
	for (start = 0; start < scenario->players.count && start < 128; start++)
	{
		struct player_starting_location *location = TAG_BLOCK_GET_ELEMENT(&scenario->players, start, struct player_starting_location);
		int direction;

		if (!isfinite(location->position.x) || !isfinite(location->position.y) || !isfinite(location->position.z) || !isfinite(location->facing))
			continue;
		for (direction = 0; direction < 4; direction++)
		{
			float facing = location->facing + direction * 1.57079633f;
			float forward_x = cosf(facing), forward_y = sinf(facing);
			int valid = 1, actor;

			candidates++;

			for (actor = 0; actor < arena.players && valid; actor++)
			{
				float offset[2];
				real_point3d eye;
				real_vector3d sight;
				struct collision_result collision;

				if (!benchmark_mp_arena_offset(&arena.config, arena.players, actor, forward_x, forward_y, offset))
					return 0;
				points[actor] = location->position;
				points[actor].x += offset[0];
				points[actor].y += offset[1];
				if (!arena_ground(&points[actor], ground_rejections))
				{
					if (first_rejected_actor == NONE) first_rejected_actor = actor;
					valid = 0; break;
				}
				if (!actor) continue;
				eye = points[0]; eye.z += 0.7f;
				sight.i = points[actor].x - eye.x;
				sight.j = points[actor].y - eye.y;
				sight.k = points[actor].z + 0.7f - eye.z;
				if (collision_test_vector(_collision_test_for_line_of_sight_flags, &eye, &sight, NONE, &collision))
				{
					sight_rejections++;
					if (first_rejected_actor == NONE) first_rejected_actor = actor;
					valid = 0;
				}
			}
			if (valid)
			{
				memcpy(arena.points, points, sizeof(points));
				platform_log("network test: arena fits %d participants at scenario start %d heading %.2f; columns %d spacing %.3f distance %.3f radius %.3f",
					arena.players, start, facing, arena.config.columns, arena.config.spacing, arena.config.distance, arena.config.movement_radius);
				return 1;
			}
		}
	}
	platform_log("network test: arena geometry exhausted: %d candidates, floor misses %d steep floors %d blocked clearance %d invalid feet clusters %d invalid head clusters %d sight rejections %d first rejected actor %d",
		candidates, ground_rejections[0], ground_rejections[1], ground_rejections[2], ground_rejections[3], ground_rejections[4], sight_rejections, first_rejected_actor);
	return 0;
}

static void arena_place(int actor, struct player_datum *player)
{
	object_set_position(player->unit_index, &arena.points[actor], NULL, NULL);
	arena.units[actor] = player->unit_index;
	arena.previous[actor] = arena.points[actor];
	arena.stuck_ticks[actor] = 0;
	arena.repositions[actor]++;
	platform_log("network test: arena reposition actor %d unit %ld count %lu", actor, player->unit_index, arena.repositions[actor]);
}

int benchmark_mp_arena_tick(long tick)
{
	int i;
	if (!arena.enabled) return 1;
	if (arena.failed)
	{
		platform_log("network test: arena already failed: previous %ld current %ld", arena.last_tick, tick);
		return -1;
	}
	if (tick < 0 || tick < arena.last_tick)
	{
		platform_log("network test: arena clock regression: previous %ld current %ld", arena.last_tick, tick);
		return -1;
	}
	if (tick == arena.last_tick) return arena.ready;
	arena.last_tick = tick;
	if (!arena.ready)
	{
		for (i = 0; i < arena.players; i++)
		{
			struct player_datum *player = arena_player(i);

			if (!player || player->unit_index == NONE)
			{
				if (tick > 450)
				{
					platform_log("network test: arena readiness timeout: tick %ld actor %d player present %d unit %ld",
						tick, i, player != NULL, player ? player->unit_index : NONE);
					arena.failed = 1; return -1;
				}
				return 0;
			}
		}
		if (!arena_choose()) { arena.failed = 1; return -1; }
		for (i = 0; i < arena.players; i++) arena_place(i, arena_player(i));
		arena.ready = 1;
		return 1;
	}
	for (i = 0; i < arena.players; i++)
	{
		struct player_datum *player = arena_player(i);
		real_point3d position;
		float dx, dy, moved_x, moved_y, limit = arena.config.movement_radius * 3.0f + 1.0f;

		if (!player)
		{
			platform_log("network test: arena lost player: tick %ld actor %d", tick, i);
			arena.failed = 1; return -1;
		}
		if (player->unit_index == NONE) continue;
		if (player->unit_index != arena.units[i]) { arena_place(i, player); continue; }
		object_get_origin(player->unit_index, &position);
		if (!isfinite(position.x) || !isfinite(position.y) || !isfinite(position.z))
		{
			platform_log("network test: arena invalid position: tick %ld actor %d unit %ld", tick, i, player->unit_index);
			arena.failed = 1; return -1;
		}
		dx = position.x - arena.points[i].x; dy = position.y - arena.points[i].y;
		moved_x = position.x - arena.previous[i].x; moved_y = position.y - arena.previous[i].y;
		arena.stuck_ticks[i] = moved_x * moved_x + moved_y * moved_y < 0.000004f ? arena.stuck_ticks[i] + 1 : 0;
		arena.previous[i] = position;
		if (dx * dx + dy * dy > limit * limit || arena.stuck_ticks[i] >= 90 || fabsf(position.z - arena.points[i].z) > 2.0f)
			arena_place(i, player);
	}
	return 1;
}

int benchmark_mp_arena_action(int actor, long tick, struct player_action *action)
{
	struct player_datum *player, *target;
	real_point3d position, aim;
	float yaw, phase, dx, dy, magnitude;
	int opponent;

	if (!arena.enabled || !arena.ready || arena.failed || actor <= 0 || actor >= arena.players || !action)
		return 0;
	player = arena_player(actor);
	if (!player || player->unit_index == NONE) return 0;
	opponent = arena.players == 2 ? 0 : actor == arena.players - 1 ? 1 : actor + 1;
	target = arena_player(opponent);
	object_get_origin(player->unit_index, &position);
	aim = arena.points[opponent];
	if (target && target->unit_index != NONE) object_get_origin(target->unit_index, &aim);
	yaw = atan2f(aim.y - position.y, aim.x - position.x);
	action->desired_facing.yaw = yaw;
	action->desired_facing.pitch = atan2f(aim.z - position.z, hypotf(aim.x - position.x, aim.y - position.y));
	phase = (float)(tick % 900) * 0.04f + (float)actor;
	dx = arena.points[actor].x + cosf(phase) * arena.config.movement_radius - position.x;
	dy = arena.points[actor].y + sinf(phase) * arena.config.movement_radius - position.y;
	magnitude = hypotf(dx, dy);
	/* Keep actual walking active near a moving waypoint. Placement counters
	remain separate; a stalled body still contributes zero measured distance. */
	if (magnitude > 0.02f)
	{
		float throttle = arena.config.movement_radius * 1.5f;
		if (throttle > 0.75f) throttle = 0.75f;
		dx *= throttle / magnitude; dy *= throttle / magnitude;
	}
	else dx = dy = 0.0f;
	action->throttle.i = -sinf(yaw) * dx + cosf(yaw) * dy;
	action->throttle.j = cosf(yaw) * dx + sinf(yaw) * dy;
	action->primary_trigger = arena.combat && tick % 90 < 24 ? 1.0f : 0.0f;
	return 1;
}

int benchmark_mp_arena_gamepad(short port, long tick, struct mp_benchmark_probe_pad *pad)
{
	struct game_input_preferences preferences;
	struct player_datum *player;
	real_point3d position, target = { 0.0f, 0.0f, 0.0f };
	real_vector3d facing;
	float yaw, pitch;
	int i;

	if (!arena.enabled || arena.failed || port != 0 || !game_time_initialized() || !game_in_progress() ||
		!mp_benchmark_probe_active())
		return 0;
	if (!arena.ready || !(player = arena_player(0)) || player->unit_index == NONE)
	{
		int handled = benchmark_mp_arena_observer_pad(0.0f, 0.0f, 0, tick, pad);
		if (handled) pad->fire = 0;
		return handled;
	}
	input_abstraction_get_local_player_preferences(0, &preferences);
	if (preferences.joystick_controls != 0) { arena.failed = 1; memset(pad, 0, sizeof(*pad)); return 1; }
	object_get_origin(player->unit_index, &position);
	for (i = 1; i < arena.players; i++)
	{
		target.x += arena.points[i].x / (arena.players - 1);
		target.y += arena.points[i].y / (arena.players - 1);
		target.z += arena.points[i].z / (arena.players - 1);
	}
	player_control_get_facing_direction(0, &facing);
	yaw = atan2f(target.y - position.y, target.x - position.x) - atan2f(facing.j, facing.i);
	pitch = atan2f(target.z - position.z, hypotf(target.x - position.x, target.y - position.y)) - atan2f(facing.k, hypotf(facing.i, facing.j));
	i = benchmark_mp_arena_observer_pad(yaw, pitch, preferences.invert_look, tick, pad);
	if (i && !arena.combat) pad->fire = 0;
	return i;
}
