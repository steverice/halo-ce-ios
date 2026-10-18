/* Production multiplayer benchmark probe contracts, without launching a match. */
#include "mp_benchmark_probe.h"

#include <stdio.h>
#include <string.h>

unsigned long mp_benchmark_probe_device_mask(unsigned long connected, int requested);

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
	if (failures < 20) fprintf(stderr, "mp_benchmark_probe: line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)

static void test_view_count(void)
{
	int active = mp_benchmark_probe_enabled(1, 1, 1, 4, 1);
	int menu = mp_benchmark_probe_enabled(1, 1, 0, 4, 1);
	int replay = mp_benchmark_probe_enabled(1, 1, 1, 4, 0);

	CHECK(active);
	CHECK(!menu && !replay);
	CHECK(!mp_benchmark_probe_enabled(0, 1, 1, 4, 1));
	CHECK(!mp_benchmark_probe_enabled(1, 0, 1, 4, 1));
	CHECK(!mp_benchmark_probe_enabled(1, 1, 1, 1, 1));
	CHECK(mp_benchmark_probe_windows(4, 0, active) == 1);
	CHECK(mp_benchmark_probe_windows(4, 0, menu) == 4);
	CHECK(mp_benchmark_probe_windows(4, 0, replay) == 4);
	CHECK(mp_benchmark_probe_windows(4, 1, active) == 1); /* cinematic/postgame */
	CHECK(mp_benchmark_probe_windows(1, 0, 0) == 1);
	CHECK(mp_benchmark_probe_windows(0, 0, 0) == 0); /* caller's clamp is authoritative */
	CHECK(mp_benchmark_probe_windows(7, 0, 0) == 7);
	CHECK(mp_benchmark_probe_listener_enabled(0, active));
	CHECK(!mp_benchmark_probe_listener_enabled(1, active));
	CHECK(!mp_benchmark_probe_listener_enabled(3, active));
	CHECK(mp_benchmark_probe_listener_enabled(3, menu));
	CHECK(mp_benchmark_probe_map_matches("levels\\test\\chillout\\chillout"));
	CHECK(mp_benchmark_probe_map_matches("chillout"));
	CHECK(!mp_benchmark_probe_map_matches("levels\\test\\bloodgulch\\bloodgulch"));
	CHECK(!mp_benchmark_probe_map_matches("chilloutclone"));
	CHECK(!mp_benchmark_probe_map_matches(""));
	CHECK(mp_benchmark_probe_policy(0, 0) == mp_benchmark_probe_ordinary_input);
	CHECK(mp_benchmark_probe_policy(0, 1) == mp_benchmark_probe_ordinary_input);
	CHECK(mp_benchmark_probe_policy(1, 0) == mp_benchmark_probe_neutral_input);
	CHECK(mp_benchmark_probe_policy(1, 1) == mp_benchmark_probe_scripted_input);
}

static void test_distinct_tick_input(void)
{
	struct mp_benchmark_probe_pad first[4], repeat;
	int port, displays;
	long tick;

	for (tick = 0; tick < 420; tick++) {
		for (port = 0; port < 4; port++) {
			mp_benchmark_probe_input(tick, port, &first[port]);
			for (displays = 1; displays <= 4; displays++) {
				mp_benchmark_probe_input(tick, port, &repeat);
				CHECK(!memcmp(&first[port], &repeat, sizeof(repeat)));
			}
		}
		CHECK(memcmp(&first[0], &first[1], sizeof(first[0])));
	}
	{
		int movement[4] = { 0 }, firing[4] = { 0 }, jumps[4] = { 0 }, grenades[4] = { 0 };
		for (tick = 0; tick < 900; tick++) for (port = 0; port < 4; port++) {
			mp_benchmark_probe_input(tick, port, &repeat);
			movement[port] += repeat.left_x != 0 || repeat.left_y != 0;
			firing[port] += repeat.fire != 0;
			jumps[port] += repeat.jump != 0;
			grenades[port] += repeat.grenade != 0;
		}
		for (port = 0; port < 4; port++) {
			CHECK(movement[port] > 600);
			CHECK(firing[port] > 30);
			CHECK(jumps[port] > 0);
			CHECK(grenades[port] > 0);
		}
	}
}

static void test_virtual_controller_connection(void)
{
	CHECK(mp_benchmark_probe_device_mask(1UL, 0) == 1UL);
	CHECK(mp_benchmark_probe_device_mask(3UL, 0) == 3UL);
	CHECK(mp_benchmark_probe_device_mask(1UL, 1) == 15UL);
	CHECK(mp_benchmark_probe_device_mask(3UL, 1) == 15UL);
}

int main(void)
{
	test_view_count();
	test_distinct_tick_input();
	test_virtual_controller_connection();
	if (failures) fprintf(stderr, "mp_benchmark_probe: %d failures\n", failures);
	return failures ? 1 : 0;
}
