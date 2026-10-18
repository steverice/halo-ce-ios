/* The complete replay implementation, with the platform and game at its edges stubbed out.
   Benchmark mouth animation must stop when replay ends or the suite clears a scene. */
#include <assert.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

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
static const char *record_name = "";
void platform_log(const char *format, ...) { (void)format; }
int config_boolean(const char *name) { return (!strcmp(name, "debug.benchmark_lip_sync") && lip_sync) ||
	(!strcmp(name, "debug.benchmark") && timed_benchmark); }
long config_integer(const char *name) { (void)name; return 0; }
const char *config_string(const char *name) {
	if (!strcmp(name, "debug.input_record")) return record_name;
	return !strcmp(name, "debug.input_replay") ? replay_name : "";
}
void config_folder(char *path, size_t size) { snprintf(path, size, "./"); }
void config_profile_settings_text(char *text, size_t size) { snprintf(text, size, ""); }
static unsigned char clock_ready, menu_loaded;
static long clock_tick;
unsigned char game_time_initialized(void) { return clock_ready; }
long game_time_get(void) { return clock_tick; }
float game_time_get_tick_fraction(void) { return 0; }
unsigned char game_time_get_paused(void) { return 0; }
unsigned char input_replay_main_menu_loaded(void) { return menu_loaded; }
unsigned char input_replay_skip_cinematic(void) { return 0; }
void input_replay_level(char *map, unsigned int size, long *difficulty, long *players, unsigned long *seed)
{ snprintf(map, size, "a10"); *difficulty = *players = 1; *seed = 0; }
unsigned long input_replay_random_seed(const unsigned long *seed) { return seed ? *seed : 0; }
void input_replay_random_seeds(unsigned long seeds[2], unsigned char set) { (void)set; seeds[0] = seeds[1] = 0; }
unsigned char input_replay_player_position(float position[3]) { (void)position; return 0; }
unsigned char input_replay_fight_state(float *body, float *shield, long *actors, long *dead_objects)
{ (void)body; (void)shield; (void)actors; (void)dead_objects; return 0; }
double halo_frame_trace_milliseconds(void) { return 0; }

static void check_control_pose(void)
{
	float facing[2] = { -2.0f, 0.7f };
	short zoom = -1;
	unsigned int action[ACTION_WORDS] = {0};
	FILE *file = fopen("camera.input", "w");
	assert(file);
	fputs("halo input recording 1\n0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", file);
	fclose(file);
	file = fopen("camera.actions", "w");
	assert(file);
	/* yaw 1.0, pitch -0.5, zoom 2; an intentional missing tick before the final action. */
	fputs("halo action recording 1\n0 10 0 3f800000 bf000000 0 0 0 0 00000002\n"
		"0 12 0 40c00000 3e800000 0 0 0 0 00000001\n", file);
	fclose(file);
	assert(input_replay_begin("./camera.input", 0));
	clock_ready = 1;
	clock_tick = 10;
	assert(input_replay_action_camera_active());
	input_replay_tick_action(action);
	assert(action[1] == 0x3f800000 && action[2] == 0xbf000000);
	/* This is the actual parsed action/control boundary, starting from drifted raw input. */
	assert(!input_replay_apply_control(-1, facing, &zoom));
	assert(input_replay_apply_control(1, facing, &zoom));
	assert(facing[0] == 1.0f && facing[1] == -0.5f && zoom == 2);
	assert(input_replay_controls_facing(1));
	assert(!input_replay_controls_facing(0));
	/* A post-injection impulse must survive repeated camera/input reads. */
	facing[0] += 0.125f;
	assert(!input_replay_apply_control(1, facing, &zoom));
	assert(facing[0] == 1.125f);
	clock_tick = 11;
	assert(input_replay_controls_facing(1)); /* game clock advances after the injected tick */
	input_replay_tick_action(action);
	assert(!input_replay_controls_facing(1));
	assert(input_replay_action_camera_active());
	assert(!input_replay_apply_control(1, facing, &zoom));
	clock_tick = 12;
	input_replay_tick_action(action);
	assert(replay.replay_finished);
	assert(input_replay_apply_control(1, facing, &zoom));
	assert(facing[0] == 6.0f && facing[1] == 0.25f && zoom == 1);
	assert(input_replay_controls_facing(1)); /* final action still owns its rendered frame */
	assert(input_replay_action_camera_active());
	clock_tick = 13;
	assert(input_replay_controls_facing(1));
	input_replay_tick_action(action);
	assert(!input_replay_controls_facing(1));
	/* Replacement, load failure, raw-only replay, and recording never retain ownership. */
	clock_ready = 0;
	assert(input_replay_begin("./camera.input", 0));
	clock_ready = 1;
	clock_tick = 10;
	input_replay_tick_action(action);
	assert(input_replay_apply_control(1, facing, &zoom));
	menu_loaded = 1;
	assert(!input_replay_action_camera_active());
	assert(!input_replay_controls_facing(1));
	menu_loaded = 0;
	assert(!input_replay_controls_facing(1));
	clock_ready = 0;
	assert(input_replay_begin("./camera.input", 0));
	clock_ready = 1;
	clock_tick = 10;
	input_replay_tick_action(action);
	assert(input_replay_apply_control(1, facing, &zoom));
	clock_tick = 1;
	assert(!input_replay_controls_facing(1));
	clock_tick = 10;
	assert(!input_replay_controls_facing(1));
	clock_ready = 0;
	input_replay_begin(NULL, 0);
	assert(!input_replay_controls_facing(1));
	assert(!input_replay_apply_control(1, facing, &zoom));
	/* A readable actions sibling cannot make a failed input load own the camera. */
	unlink("camera.input");
	assert(!input_replay_begin("./camera.input", 0));
	clock_ready = 1;
	clock_tick = 10;
	input_replay_tick_action(action);
	assert(!input_replay_apply_control(1, facing, &zoom));
	assert(!input_replay_controls_facing(1));
	clock_ready = 0;
	input_replay_begin(NULL, 0);
	assert(!input_replay_action_camera_active());
	unlink("camera.actions");
	file = fopen("camera.input", "w");
	assert(file);
	fputs("halo input recording 1\n0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", file);
	fclose(file);
	assert(input_replay_begin("./camera.input", 0));
	clock_ready = 1;
	assert(!input_replay_action_camera_active()); /* controller-only legacy replay */
	clock_ready = 0;
	input_replay_begin(NULL, 0);
	unlink("camera.input");
}

static void remove_recording_fixture_files(void);

static int check_auto_record_suite_camera(int expect_auto_record_collision)
{
	char folder[] = "/tmp/visr-auto-camera-XXXXXX";
	float facing[2] = { -2.0f, 0.7f };
	short zoom = -1;
	unsigned int action[ACTION_WORDS] = {0};
	XINPUT_GAMEPAD pad = {0};
	FILE *file;

	assert(mkdtemp(folder) && !chdir(folder));
	assert(!mkdir("recordings", 0700));
	file = fopen("probe.input", "w");
	assert(file);
	fputs("halo input recording 1\n0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", file);
	fclose(file);
	file = fopen("probe.actions", "w");
	assert(file);
	fputs("halo action recording 1 lip_sync=simulated stereo=off\n"
		"0 10 0 3f800000 bf000000 0 0 0 0 00000002\n"
		"0 12 0 40c00000 3e800000 0 0 0 0 00000001\n", file);
	fclose(file);

	record_name = "auto";
	replay_name = "";
	replay.configured = FALSE;
	take.automatic = FALSE;
	menu_loaded = 1;
	configure();
	assert(take.automatic);
	menu_loaded = 0;
#ifndef REPLAY_PROBE_OMIT_SUITE_SUSPEND
	assert(input_replay_suite_suspend());
#endif
	assert(input_replay_suite_suspended() != expect_auto_record_collision);
	assert(take.automatic == expect_auto_record_collision);
	assert(input_replay_begin("./probe.input", 1));
	clock_ready = 1;
	clock_tick = 10;
	input_replay_filter(&pad);
	input_replay_tick_action(action);
	{
		int control_applied = input_replay_apply_control(0, facing, &zoom);
		int camera_active = input_replay_action_camera_active();

		if (expect_auto_record_collision)
		{
			assert(*replay.record_actions_path);
			assert(!camera_active && !control_applied);
			fprintf(stderr, "RED reproduced: auto recording owns the action path; camera=%d control=%d\n",
				camera_active, control_applied);
			input_replay_suite_restore();
			take_close("probe");
			input_replay_begin(NULL, 0);
			unlink("probe.input");
			unlink("probe.actions");
			remove_recording_fixture_files();
			assert(!rmdir("recordings"));
			assert(!chdir("/"));
			assert(!rmdir(folder));
			return 0;
		}
		assert(!*replay.record_actions_path);
		assert(camera_active && control_applied);
		assert(facing[0] == 1.0f && facing[1] == -0.5f && zoom == 2);
	}
	assert(input_replay_action_camera_active());

	input_replay_suite_restore();
	clock_tick = 0;
	input_replay_filter(&pad);
	assert(take.automatic && !input_replay_suite_suspended());
	assert(take.open && replay.record_file);
	{
		FILE *recording_file = replay.record_file;
		long size_before = ftell(recording_file);

		assert(size_before > 0);
		assert(!input_replay_suite_suspend());
		assert(take.open && take.automatic && !input_replay_suite_suspended());
		assert(replay.record_file == recording_file && ftell(recording_file) == size_before);
	}
	take_close("probe");
	input_replay_begin(NULL, 0);
	unlink("probe.input");
	unlink("probe.actions");
	remove_recording_fixture_files();
	assert(!rmdir("recordings"));
	assert(!chdir("/"));
	assert(!rmdir(folder));
	puts("auto-record suite camera ownership: PASS");
	return 0;
}

static void remove_recording_fixture_files(void)
{
	DIR *directory = opendir("recordings");
	struct dirent *entry;
	char path[600];

	assert(directory);
	while ((entry = readdir(directory)) != NULL)
	{
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		snprintf(path, sizeof(path), "recordings/%s", entry->d_name);
		assert(!remove(path));
	}
	assert(!closedir(directory));
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--auto-record-suite-camera"))
		return check_auto_record_suite_camera(0);
	if (argc == 2 && !strcmp(argv[1], "--auto-record-suite-camera-baseline"))
		return check_auto_record_suite_camera(1);
	char folder[] = "/tmp/visr-replay-probe-XXXXXX";
	assert(mkdtemp(folder));
	assert(!chdir(folder));
	check_control_pose();
	clock_tick = 0;
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
	clock_ready = 1;
	input_replay_tick_action(action); /* action tick zero is the take's final action */
	assert(replay.replay_finished && !touch_replay_active);
	input_replay_begin(NULL, 0);
	assert(touch_changes >= 4);
	clock_ready = 0;
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
	/* Suite ownership keeps touch suppressed when a scene has no replay and
	   when its replay finishes; the configured replay reloads after restore. */
	replay_name = "./probe.input";
	replay.configured = FALSE;
	configure();
	assert(touch_replay_active);
	assert(input_replay_suite_suspend());
	assert(input_replay_suite_suspended() && touch_replay_active);
	assert(input_replay_begin("./probe.input", 0));
	assert(touch_replay_active);
	clock_ready = 1;
	menu_loaded = 0;
	clock_tick = REPLAY_TAIL_TICKS + 1;
	XINPUT_GAMEPAD suite_pad = {0};
	input_replay_filter(&suite_pad);
	assert(replay.replay_finished && touch_replay_active);
	input_replay_suite_restore();
	assert(!input_replay_suite_suspended() && !touch_replay_active);
	input_replay_filter(&suite_pad);
	assert(*replay.replay_path && touch_replay_active);
	input_replay_begin(NULL, 0);
	assert(!touch_replay_active);
	replay_name = "";
	clock_ready = 0;
	menu_loaded = 1;
	/* A suite suspends the effective auto-take without changing its configured value. */
	record_name = "auto";
	replay.configured = FALSE;
	take.automatic = FALSE;
	menu_loaded = 1;
	configure();
	assert(take.automatic);
	take.open = TRUE;
	assert(!input_replay_suite_suspend());
	assert(take.open && take.automatic && !input_replay_suite_suspended());
	take.open = FALSE;
	assert(input_replay_suite_suspend());
	assert(input_replay_suite_suspended() && !take.automatic && replay.suite);
	input_replay_suite_restore();
	assert(!input_replay_suite_suspended() && !replay.suite && !take.automatic);
	XINPUT_GAMEPAD resumed = {0};
	input_replay_filter(&resumed);
	assert(take.automatic && !strcmp(record_name, "auto"));
	record_name = "";
	take.automatic = FALSE;
	unlink("probe.input");
	unlink("probe.actions");
	assert(!chdir("/"));
	assert(!rmdir(folder));
	puts("input replay mouth mode: passed");
	return 0;
}
