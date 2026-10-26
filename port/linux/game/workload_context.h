/*
WORKLOAD_CONTEXT.H

Whether a map's workload is predictable or variable, for the device
profile's [variable.display] (profiles/README.md, "Workload contexts").
Predictable is single-player campaign at standard settings, and a small LAN
co-op game at standard settings: a fixed render size, or a small dynamic
resolution window. Variable is everything whose load follows the map, the
settings and the players: multiplayer maps, online co-op, LAN co-op of more
than WORKLOAD_LAN_PLAYERS, split screen, and any enemy multiplier above 1x.
scene_class.c gathers the inputs as each map loads; this decides, with no
game state of its own, so a probe can cover every case.

Split screen is variable for now. Predictable split-screen contexts by
player count (2, 3 and 4) on devices that hold them are left for later; the
local player count is among the inputs for that.
*/

#ifndef WORKLOAD_CONTEXT_H
#define WORKLOAD_CONTEXT_H

#include <stdio.h>

/* the most players a LAN co-op game may have and stay predictable */
#define WORKLOAD_LAN_PLAYERS 4

struct workload_inputs
{
	/* the scenario is a multiplayer one (_scenario_type_multiplayer) */
	int multiplayer_map;
	/* the main menu's own scenario: neither context, predictable */
	int main_menu;
	/* a network game, hosted or joined (_game_connection_network_server or _client) */
	int networked;
	/* internet play's tunnel reaches a machine (p2p_connected_peers): the game is online, not LAN */
	int online;
	/* the network game's players, every machine's; 0 outside a network game */
	int network_players;
	/* this device's players: more than one is split screen */
	int local_players;
	/* the enemies' multiplier (coop_enemies_solo_multiplier, at least 1) */
	double enemy_multiplier;
	/* a co-op host's extra enemies (network.coop_enemies_mode other than "none") */
	int coop_extra_enemies;
};

/* 1 for variable, 0 for predictable; reasons (may be NULL) names why, for the log */
static inline int workload_variable(const struct workload_inputs *in, char *reasons, size_t size)
{
	const char *why = NULL;
	int variable = 1;

	if (in->main_menu)
		variable = 0, why = "main menu";
	else if (in->multiplayer_map)
		why = "multiplayer map";
	else if (in->local_players > 1)
		why = "split screen";
	else if (in->enemy_multiplier > 1.0)
		why = "enemy multiplier";
	else if (in->networked && in->coop_extra_enemies)
		why = "co-op extra enemies";
	else if (in->networked && in->online)
		why = "online co-op";
	else if (in->networked && in->network_players > WORKLOAD_LAN_PLAYERS)
		why = "LAN co-op beyond four players";
	else if (in->networked)
		variable = 0, why = "LAN co-op at standard settings";
	else
		variable = 0, why = "single-player campaign at standard settings";
	if (reasons && size)
	{
		if (in->networked || in->local_players > 1)
			snprintf(reasons, size, "%s; %d players, %d on this device", why, in->network_players > in->local_players ?
				in->network_players : in->local_players, in->local_players);
		else
			snprintf(reasons, size, "%s", why);
	}
	return variable;
}

#endif
