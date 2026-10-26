/* The device profile's workload context (port/linux/game/workload_context.h): which games are predictable
 * (single-player campaign and small LAN co-op at standard settings) and which variable, with the reason the log
 * gives. Steve's rules of 2026-10-26: split screen is variable for now; LAN co-op is predictable at 1x with four
 * players or fewer; online co-op, multiplayer maps and any enemy multiplier are variable. */
#include "workload_context.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int decide(struct workload_inputs in, const char *expected_reason)
{
	char reasons[160];
	int variable;

	if (in.enemy_multiplier == 0.0)
		in.enemy_multiplier = 1.0;
	variable = workload_variable(&in, reasons, sizeof(reasons));
	if (!strstr(reasons, expected_reason))
	{
		fprintf(stderr, "reasons \"%s\" lack \"%s\"\n", reasons, expected_reason);
		assert(0);
	}
	return variable;
}

int main(void)
{
	char reasons[160];

	/* single-player campaign */
	assert(!decide((struct workload_inputs){ .local_players = 1 }, "single-player campaign at standard settings"));
	assert(decide((struct workload_inputs){ .local_players = 1, .enemy_multiplier = 1.5 }, "enemy multiplier"));
	assert(decide((struct workload_inputs){ .local_players = 1, .enemy_multiplier = 2.0 }, "enemy multiplier"));
	/* (the host's extra enemies count only in a network game) */
	assert(!decide((struct workload_inputs){ .local_players = 1, .coop_extra_enemies = 1 }, "single-player"));
	/* LAN co-op: predictable at 1x with 2, 3 or 4 players */
	for (int players = 2; players <= 4; players++)
		assert(!decide((struct workload_inputs){ .networked = 1, .network_players = players, .local_players = 1 },
			"LAN co-op at standard settings"));
	assert(decide((struct workload_inputs){ .networked = 1, .network_players = 5, .local_players = 1 },
		"LAN co-op beyond four players"));
	assert(decide((struct workload_inputs){ .networked = 1, .network_players = 16, .local_players = 1 },
		"LAN co-op beyond four players"));
	assert(decide((struct workload_inputs){ .networked = 1, .network_players = 2, .local_players = 1,
		.enemy_multiplier = 1.5 }, "enemy multiplier"));
	assert(decide((struct workload_inputs){ .networked = 1, .network_players = 2, .local_players = 1,
		.coop_extra_enemies = 1 }, "co-op extra enemies"));
	/* online co-op, at any size */
	assert(decide((struct workload_inputs){ .networked = 1, .online = 1, .network_players = 2, .local_players = 1 },
		"online co-op"));
	/* split screen: alone and inside LAN co-op */
	assert(decide((struct workload_inputs){ .local_players = 2 }, "split screen"));
	assert(decide((struct workload_inputs){ .networked = 1, .network_players = 3, .local_players = 2 }, "split screen"));
	/* multiplayer maps: local, LAN, online, one player */
	assert(decide((struct workload_inputs){ .multiplayer_map = 1, .local_players = 1 }, "multiplayer map"));
	assert(decide((struct workload_inputs){ .multiplayer_map = 1, .local_players = 4 }, "multiplayer map"));
	assert(decide((struct workload_inputs){ .multiplayer_map = 1, .networked = 1, .network_players = 2, .local_players = 1 },
		"multiplayer map"));
	assert(decide((struct workload_inputs){ .multiplayer_map = 1, .networked = 1, .online = 1, .network_players = 16,
		.local_players = 1 }, "multiplayer map; 16 players, 1 on this device"));
	/* film playback: not a network game; a replayed take's multiplier is its recording's */
	assert(!decide((struct workload_inputs){ .local_players = 1 }, "single-player"));
	assert(decide((struct workload_inputs){ .local_players = 1, .enemy_multiplier = 2.0 }, "enemy multiplier"));
	/* the main menu */
	assert(!decide((struct workload_inputs){ .main_menu = 1, .multiplayer_map = 0, .local_players = 1 }, "main menu"));
	/* (no reasons buffer) */
	assert(workload_variable(&(struct workload_inputs){ .multiplayer_map = 1, .enemy_multiplier = 1.0 }, NULL, 0));
	assert(!workload_variable(&(struct workload_inputs){ .enemy_multiplier = 1.0, .local_players = 1 }, reasons, 1) &&
		!reasons[0]);
	puts("PASS: workload contexts: solo, multiplier, LAN co-op by size, online co-op, split screen, multiplayer maps");
	return 0;
}
