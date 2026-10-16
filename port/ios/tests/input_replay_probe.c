/* The complete replay implementation, with the platform and game at its edges stubbed out.
   Benchmark mouth animation must stop when replay ends or the suite clears a scene. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define HALO_IOS 1
#define __HALO_LINUX_PLATFORM_H
typedef int BOOL;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef int16_t SHORT;
#define TRUE 1
#define FALSE 0
#define XINPUT_GAMEPAD_BACK 0x20
#define XINPUT_GAMEPAD_START 0x10
typedef struct {
	WORD wButtons;
	BYTE bAnalogButtons[8];
	int16_t sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XINPUT_GAMEPAD;
void platform_log(const char *format, ...);

#include "input_replay.c"

static int touch_replay_active;
static int touch_changes;
void host_touch_replay_active(int active) {
	if (touch_replay_active != active) touch_changes++;
	touch_replay_active = active;
}

static int lip_sync;
static int timed_benchmark;
static const char *replay_name = "";
void platform_log(const char *format, ...) { (void)format; }
int config_boolean(const char *name) { return (!strcmp(name, "debug.benchmark_lip_sync") && lip_sync) ||
	(!strcmp(name, "debug.benchmark") && timed_benchmark); }
long config_integer(const char *name) { (void)name; return 0; }
const char *config_string(const char *name) { return !strcmp(name, "debug.input_replay") ? replay_name : ""; }
void config_folder(char *path, size_t size) { snprintf(path, size, "./"); }
void config_profile_settings_text(char *text, size_t size) { snprintf(text, size, ""); }
static int game_initialized;
unsigned char game_time_initialized(void) { return game_initialized; }
long game_time_get(void) { return 0; }
float game_time_get_tick_fraction(void) { return 0; }
unsigned char game_time_get_paused(void) { return 0; }
unsigned char input_replay_main_menu_loaded(void) { return 0; }
unsigned char input_replay_skip_cinematic(void) { return 0; }
void input_replay_level(char *map, unsigned int size, long *difficulty, long *players, unsigned long *seed)
{ snprintf(map, size, "a10"); *difficulty = *players = 1; *seed = 0; }
unsigned long input_replay_random_seed(const unsigned long *seed) { return seed ? *seed : 0; }
void input_replay_random_seeds(unsigned long seeds[2], unsigned char set) { (void)set; seeds[0] = seeds[1] = 0; }
unsigned char input_replay_player_position(float position[3]) { (void)position; return 0; }
unsigned char input_replay_fight_state(float *body, float *shield, long *actors, long *dead_objects)
{ (void)body; (void)shield; (void)actors; (void)dead_objects; return 0; }
double halo_frame_trace_milliseconds(void) { return 0; }

int main(void)
{
	char folder[] = "/tmp/visr-replay-probe-XXXXXX";
	assert(mkdtemp(folder));
	assert(!chdir(folder));
	lip_sync = 1;
	assert(!input_replay_benchmark_lip_sync()); /* opt-in alone never changes gameplay */
	strcpy(replay.record_actions_path, "take.actions");
	lip_sync = 0;
	assert(input_replay_sim_isolated());
	assert(!input_replay_benchmark_lip_sync()); /* RNG isolation stays on with original mouths */
	lip_sync = 1;
	assert(input_replay_benchmark_lip_sync());
	record_end("probe");
	assert(!input_replay_benchmark_lip_sync());
	input_replay_begin(NULL, 0);
	assert(!input_replay_benchmark_lip_sync());
	/* Legacy takes used simulated mouths. The suite opts in without touching persistent config. */
	FILE *file = fopen("probe.input", "w");
	assert(file);
	fputs("halo input recording 1\n0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", file);
	fclose(file);
	file = fopen("probe.actions", "w");
	assert(file);
	fputs("halo action recording 1\n0 0 0 0 0 0 0 0 0 0\n", file);
	fclose(file);
	lip_sync = 0;
	assert(input_replay_begin("./probe.input", 1));
	assert(input_replay_benchmark_lip_sync());
	replay.await_level = TRUE;
	assert(!input_replay_benchmark_lip_sync()); /* still on the previous scene */
	replay.await_level = FALSE;
	replay.replay_finished = TRUE;
	assert(!input_replay_benchmark_lip_sync());
	replay.replay_finished = FALSE;
	assert(input_replay_benchmark_lip_sync());
	input_replay_begin(NULL, 0);
	assert(!input_replay_benchmark_lip_sync()); /* no mode leaks between suite scenes */
	assert(!lip_sync); /* the suite never writes the config */
	assert(input_replay_begin("./probe.input", 0));
	assert(!input_replay_benchmark_lip_sync()); /* old replay still needs opt-in */
	lip_sync = 1;
	assert(input_replay_benchmark_lip_sync());
	input_replay_begin(NULL, 0);
	file = fopen("probe.actions", "w");
	assert(file);
	fputs("halo action recording 1 lip_sync=simulated stereo=off\n0 0 0 0 0 0 0 0 0 0\n", file);
	fclose(file);
	assert(input_replay_begin("./probe.input", 1));
	assert(input_replay_benchmark_lip_sync()); /* new explicit metadata matches the legacy behavior */
	input_replay_begin(NULL, 0);
	/* Direct timed replays must opt in too, rather than silently measuring another mode. */
	pid_t child = fork();
	assert(child >= 0);
	if (!child) {
		lip_sync = 0;
		timed_benchmark = 1;
		replay_name = "./probe.input";
		configure();
		_exit(99);
	}
	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE);
	file = fopen("probe.actions", "w");
	assert(file);
	fputs("halo action recording 1 lip_sync=original\n0 0 0 0 0 0 0 0 0 0\n", file);
	fclose(file);
	assert(!input_replay_begin("./probe.input", 1)); /* original mixer cannot run in a silent benchmark */
	input_replay_begin(NULL, 0);
	assert(input_replay_begin("./probe.input", 0));
	assert(!input_replay_benchmark_lip_sync()); /* a non-benchmark original replay warns instead */
	input_replay_begin(NULL, 0);
	replay_name = "./probe.input";
	configure();
	assert(replay.action_count == 1 && !replay.replay_finished);
	assert(!input_replay_benchmark_lip_sync()); /* same behavior through direct config */
	input_replay_begin(NULL, 0);
	/* Both direct config and suite entry points publish actual loaded replay lifetime. */
	assert(!touch_replay_active);
	assert(!input_replay_begin("./missing.input", 0));
	assert(!touch_replay_active);
	assert(input_replay_begin("./probe.input", 0));
	assert(touch_replay_active);
	input_replay_begin(NULL, 0);
	assert(!touch_replay_active);
	replay_name = "./probe.input";
	configure();
	assert(touch_replay_active);
	unsigned int action[8] = {0};
	game_initialized = 1;
	input_replay_tick_action(action); /* action tick zero is the take's final action */
	assert(replay.replay_finished && !touch_replay_active);
	input_replay_begin(NULL, 0);
	assert(touch_changes >= 4);
	game_initialized = 0;
	/* A controller-only take completes through its tail, without tick actions. */
	unlink("probe.actions");
	assert(input_replay_begin("./probe.input", 0));
	assert(touch_replay_active);
	XINPUT_GAMEPAD pad = {0};
	play(&pad, REPLAY_TAIL_TICKS + 1);
	assert(replay.replay_finished && !touch_replay_active);
	/* Invalid and empty files cannot leave suppression latched. */
	file = fopen("invalid.input", "w");
	assert(file); fputs("invalid header\n", file); fclose(file);
	assert(!input_replay_begin("./invalid.input", 0));
	assert(!touch_replay_active);
	file = fopen("invalid.input", "w");
	assert(file); fputs("halo input recording 1\n", file); fclose(file);
	assert(!input_replay_begin("./invalid.input", 0));
	assert(!touch_replay_active);
	unlink("invalid.input");
	input_replay_begin(NULL, 0);
	replay_name = "";
	configure();
	assert(!touch_replay_active);
	unlink("probe.input");
	unlink("probe.actions");
	assert(!chdir("/"));
	assert(!rmdir(folder));
	puts("input replay mouth mode: passed");
	return 0;
}
