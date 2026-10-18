#ifndef MP_BENCHMARK_PROBE_H
#define MP_BENCHMARK_PROBE_H

struct mp_benchmark_probe_pad
{
	short left_x, left_y, right_x, right_y;
	unsigned char fire, jump, grenade;
};

enum mp_benchmark_probe_input_policy {
	mp_benchmark_probe_ordinary_input,
	mp_benchmark_probe_neutral_input,
	mp_benchmark_probe_scripted_input
};

int mp_benchmark_probe_enabled(int requested, int network_host, int multiplayer_map, int local_players,
	int recording_off);
short mp_benchmark_probe_windows(short local_players, int forced_single_screen, int enabled);
int mp_benchmark_probe_listener_enabled(short local_player_index, int enabled);
int mp_benchmark_probe_map_matches(const char *map);
enum mp_benchmark_probe_input_policy mp_benchmark_probe_policy(int requested, int active);
void mp_benchmark_probe_input(long tick, short port, struct mp_benchmark_probe_pad *pad);

/* The game-state adapter is false outside the explicit Chill Out test match. */
int mp_benchmark_probe_active(void);
int mp_benchmark_probe_requested(void);
enum mp_benchmark_probe_input_policy mp_benchmark_probe_current_policy(void);
long mp_benchmark_probe_tick(void);

#endif
