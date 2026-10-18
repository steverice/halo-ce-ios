/* Private benchmark match control. Unsupported requests leave the host untouched. */
#ifndef NETWORK_TEST_SUITE_H
#define NETWORK_TEST_SUITE_H
int network_test_suite_begin(const char *simulation, const char *map, const char *game_mode,
	int players, int views, int score_limit, int time_limit_minutes);
int network_test_suite_two_player_proof_active(void);
int network_test_suite_failed(void);
void network_test_suite_end(void);
#endif
