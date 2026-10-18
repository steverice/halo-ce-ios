#include "mp_benchmark_probe.h"

#include <string.h>

unsigned long mp_benchmark_probe_device_mask(unsigned long connected, int requested)
{
	return requested ? connected | 0x0eUL : connected;
}

int mp_benchmark_probe_enabled(int requested, int network_host, int multiplayer_map, int local_players,
	int recording_off)
{
	return requested && network_host && multiplayer_map && local_players == 4 && recording_off;
}

short mp_benchmark_probe_windows(short local_players, int forced_single_screen, int enabled)
{
	if (forced_single_screen || enabled)
		return 1;
	return local_players;
}

int mp_benchmark_probe_listener_enabled(short local_player_index, int enabled)
{
	return !enabled || local_player_index == 0;
}

int mp_benchmark_probe_map_matches(const char *map)
{
	const char *name = map;

	if (!map)
		return 0;
	for (; *map; map++)
		if (*map == '\\' || *map == '/')
			name = map + 1;
	return !strcmp(name, "chillout");
}

enum mp_benchmark_probe_input_policy mp_benchmark_probe_policy(int requested, int active)
{
	if (!requested)
		return mp_benchmark_probe_ordinary_input;
	return active ? mp_benchmark_probe_scripted_input : mp_benchmark_probe_neutral_input;
}

void mp_benchmark_probe_input(long tick, short port, struct mp_benchmark_probe_pad *pad)
{
	static const short moves[8][2] = {
		{ 0, 30000 }, { 18000, 26000 }, { 30000, 0 }, { 18000, -26000 },
		{ 0, -30000 }, { -18000, -26000 }, { -30000, 0 }, { -18000, 26000 }
	};
	unsigned long phase;
	int direction;

	memset(pad, 0, sizeof(*pad));
	if (tick < 0 || port < 0 || port >= 4)
		return;
	phase = (unsigned long)tick + (unsigned long)port * 53UL;
	direction = (int)(((unsigned long)tick / 60UL + (unsigned long)port * 2UL) % 8UL);
	pad->left_x = moves[direction][0];
	pad->left_y = moves[direction][1];
	pad->right_x = (short)((long)(phase % 180UL) - 90L) * 120;
	pad->right_y = (short)((long)((phase / 3UL) % 60UL) - 30L) * 90;
	pad->fire = phase % 90UL < 24UL;
	pad->jump = phase % 150UL < 3UL;
	pad->grenade = phase % 210UL < 3UL;
}
