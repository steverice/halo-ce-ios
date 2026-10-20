/* Private benchmark match control. Unsupported requests leave the host untouched. */
#ifndef NETWORK_TEST_SUITE_H
#define NETWORK_TEST_SUITE_H
#include <stdint.h>
#include <math.h>
#include "benchmark_mp_limits.h"
/* Some battery weapons have a dummy magazine that consumes zero rounds. */
static inline int network_test_suite_energy_trigger(int magazine, int magazines, int rounds, float age)
{
	return __builtin_isfinite(age) && age > 0.0f && magazines >= 0 && magazines <= 2 && (magazine == -1 ||
		(magazine >= 0 && magazine < magazines && rounds == 0));
}
void network_test_suite_projectile_created(long owner_unit, int energy);
uint32_t network_test_suite_energy_projectiles(int actor);
int network_test_suite_begin(const char *simulation, const char *map, const char *game_mode,
	int players, int views, int score_limit, int time_limit_minutes);
int network_test_suite_network_players(void);
int network_test_suite_map_matches(const char *map);
unsigned long network_test_suite_repositions(int actor);
void network_test_suite_scripted_counts(unsigned long *hits, unsigned long *kills);
int network_test_suite_failed(void);
void network_test_suite_end(void);
#endif
