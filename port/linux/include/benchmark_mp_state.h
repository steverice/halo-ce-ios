/* Guest-only observation of a private synthetic multiplayer match. */
#ifndef HALO_BENCHMARK_MP_STATE_H
#define HALO_BENCHMARK_MP_STATE_H
#include <stdint.h>

#define BENCHMARK_MP_ACTORS 32

struct benchmark_mp_actor
{
	int present, spawned;
	float position[3], body, shield;
	int grenades[2], ammo, weapon_definition, kills, deaths;
	float throttle;
	unsigned long fire_requests, jump_requests, grenade_requests, repositions;
	uint32_t energy_projectiles;
};

struct benchmark_mp_state
{
	long tick;
	int multiplayer, map_ok, slayer, score_limit, time_limit_minutes;
	int local_players, roster, spawned, view_count, observer0, probe_active, can_score, postgame;
	int synthetic_queue_ready, projectiles_alive;
	unsigned long scripted_hits, scripted_kills;
	struct benchmark_mp_actor actors[BENCHMARK_MP_ACTORS];
};

void benchmark_scenario_multiplayer_state(struct benchmark_mp_state *state);

#endif
