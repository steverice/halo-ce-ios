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

int halo_controller_unit_state(float position[3], float *facing_yaw, float *aim_yaw)
{ (void)position; (void)facing_yaw; (void)aim_yaw; return 0; }
int halo_controller_run_aimed(void) { return 0; }
static int controller_configured;
static const char *replay_probe_stereo_mode = "";
int halo_controller_aim_configured(void) { return controller_configured; }

static int touch_replay_active;
static int touch_changes;
void host_touch_replay_active(int active) {
	if (touch_replay_active != active) touch_changes++;
	touch_replay_active = active;
}

static int lip_sync;
static int enhanced_animations = 1;
static unsigned long environment_config_changes = 1;
static int timed_benchmark;
static const char *replay_name = "";
static const char *record_name = "";
void platform_log(const char *format, ...) { (void)format; }
int config_boolean(const char *name) { return !strcmp(name, "debug.scenario_help") ||
	(!strcmp(name, "debug.benchmark_lip_sync") && lip_sync) ||
	(!strcmp(name, "debug.benchmark") && timed_benchmark) ||
	(!strcmp(name, "game.enhanced_animations") && enhanced_animations); }
long config_integer(const char *name) { (void)name; return 0; }
const char *config_string(const char *name) {
	if (!strcmp(name,"display.stereo")) return replay_probe_stereo_mode;
	if (!strcmp(name, "debug.input_record")) return record_name;
	return !strcmp(name, "debug.input_replay") ? replay_name : "";
}
/* coop_enemies.c's debug.enemy_multiplier and a replayed take's own */
static double probe_multiplier = 1.0, probe_override = -1.0;
double coop_enemies_solo_multiplier(void) { return probe_override >= 1.0 ? probe_override : probe_multiplier; }
void coop_enemies_solo_override(double multiplier) { probe_override = multiplier >= 1.0 ? multiplier : -1.0; }
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

static void check_controller_action_format(void)
{
	struct replay_environment environment;
	assert(replay_environment_parse("halo action recording 1 enhanced_animations=1", &environment));
	assert(environment.action_version == 1 && !environment.controller_aim && environment.enhanced_animations);
	assert(replay_environment_parse("halo action recording 2 stereo=head aim=controller lip_sync=simulated enhanced_animations=1", &environment));
	assert(environment.action_version == 2 && environment.controller_aim && environment.enhanced_animations);
	const char *bad[] = {"halo action recording 2", "halo action recording 2 stereo=head aim=controllerX",
		"halo action recording 2 stereo=head aim=controller aim=controller", "halo action recording 12",
		"halo action recording 2 stereo=off aim=controller", "halo action recording 1 aim=controller",
		"halo action recording 2 stereo=head aim=controller unknown=1",
		"halo action recording 2 stereo=head aim=controller enhanced_animations=2"};
	for (unsigned i=0; i<sizeof(bad)/sizeof(*bad); i++) assert(!replay_environment_parse(bad[i], &environment));
	const char *headers[] = {"halo action recording 1", "halo action recording 2 stereo=head aim=controller"};
	for (int version=1; version<=2; version++) {
		FILE *file=fopen("controller.input", "w"); assert(file);
		fputs("halo input recording 1\n0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", file); fclose(file);
		file=fopen("controller.actions", "w"); assert(file);
		fprintf(file,"%s\n0 10 0 3f800000 bf000000 0 0 0 0 ffff0002\n",headers[version-1]); fclose(file);
		clock_ready=0;
		assert(input_replay_begin("./controller.input",0));
		assert(replay.action_count==1);
		assert(replay.actions[0].words[7] == (version==1 ? 2u : 0xffff0002u));
		input_replay_begin(NULL,0);
	}
	FILE *file=fopen("controller.actions", "w"); assert(file);
	fputs("halo action recording 2 stereo=head aim=controller\n0 10 0 3f800000 bf000000 0 0 0 0 00020002\n",file); fclose(file);
	input_replay_begin("./controller.input",0);
	assert(replay.environment_invalid);
	assert(!input_replay_action_camera_active());
	input_replay_begin(NULL,0);
	file=fopen("controller.actions", "w"); assert(file);
	fputs("halo action recording 2 stereo=head aim=controller",file);
	for (int i=0;i<300;i++) fputc(' ',file);
	fputc('\n',file); fclose(file);
	input_replay_begin("./controller.input",0);
	assert(replay.environment_invalid);
	input_replay_begin(NULL,0);
	unlink("controller.actions"); unlink("controller.input");
	puts("controller action format: PASS");
}

static void check_recorded_action_metadata(void)
{
    for (int version=1;version<=2;version++) {
        unsigned int action[ACTION_WORDS]={0x03428001u,0x3f800000u,0,0,0,0,0,2};
        XINPUT_GAMEPAD pad={0};
        char header[256];long segment,tick;unsigned int words[ACTION_WORDS];
        input_replay_begin(NULL,0);clock_ready=1;clock_tick=0;
        replay.await_level=0;controller_configured=version==2;replay_probe_stereo_mode=version==2?"head":"";
        strcpy(replay.record_path,"metadata.input");strcpy(replay.record_actions_path,"metadata.actions");
        if(version==2)action[7]=0xc0010002u;
        record(&pad,0);input_replay_tick_action(action);record_end("metadata probe");
        FILE *file=fopen("metadata.actions","r");assert(file && fgets(header,sizeof(header),file));
        assert(fscanf(file,"%ld %ld %x %x %x %x %x %x %x %x",&segment,&tick,
            &words[0],&words[1],&words[2],&words[3],&words[4],&words[5],&words[6],&words[7])==10);fclose(file);
        /* Preserve v1's existing on-disk tutorial slot; v2 must carry both independently. */
        assert(words[0]==(version==1?0x8001u:0x03428001u));
        assert(words[7]==(version==1?0x03420002u:0xc0010002u));
        clock_ready=0;assert(input_replay_begin("./metadata.input",0));
        assert(replay.action_count==1 && replay.actions[0].words[0]==0x03428001u);
        assert(replay.actions[0].words[7]==(version==1?2u:0xc0010002u));
        clock_ready=1;clock_tick=0;memset(action,0,sizeof(action));input_replay_tick_action(action);
        assert(action[0]==0x03428001u && action[7]==(version==1?2u:0xc0010002u));
        input_replay_begin(NULL,0);
    }
    controller_configured=0;replay_probe_stereo_mode="";clock_ready=0;
    unlink("metadata.input");unlink("metadata.actions");unlink("metadata.seeds");
    puts("controller and tutorial metadata: PASS");
}

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

/* PRODUCTION_ENHANCED_CONSUMER */
static void check_environment_mode(int expected)
{
    if (input_replay_enhanced_animations(enhanced_animations) != expected)
        fprintf(stderr, "mode expected=%d actual=%d tick=%ld owned=%d finished=%d await=%d states=%ld\n",
            expected, input_replay_enhanced_animations(enhanced_animations), clock_tick,
            replay.environment_owned, replay.replay_finished, replay.await_level, replay.state_count);
    assert(input_replay_enhanced_animations(enhanced_animations) == expected);
#ifdef REPLAY_ENVIRONMENT_CONSUMER
    assert(unit_animation_enhanced(0) == expected);
#endif
}

static void environment_files(const char *metadata)
{
    FILE *file = fopen("environment.input", "w");
    assert(file);
    fputs("halo input recording 1\n0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", file);
    fclose(file);
    file = fopen("environment.actions", "w");
    assert(file);
    fprintf(file, "halo action recording 1%s\n0 0 0 0 0 0 0 0 0 0\n0 2 0 0 0 0 0 0 0 0\n1 0 1 0 0 0 0 0 0 0\n1 2 2 0 0 0 0 0 0 0\n", metadata);
    fclose(file);
}

static void check_replayed_environment(void)
{
    unsigned int action[ACTION_WORDS] = {0};
    const char *invalid[] = {" enhanced_animations=2", " enhanced_animations=true",
        " enhanced_animations=0 enhanced_animations=1", " stereo=head stereo=off", " unknown=1"};
    unsigned int i;
    enhanced_animations = 1;
    clock_ready = 1;
    clock_tick = 500;
    environment_files("");
    assert(input_replay_begin("./environment.input", 0));
    check_environment_mode(1); /* old level still owns simulation */
    input_replay_tick_begin();
    check_environment_mode(1);
    clock_tick = 0;
    input_replay_tick_begin(); /* before AI and before the first action hook */
    check_environment_mode(0);
    input_replay_tick_action(action);
    input_replay_tick_end();
    check_environment_mode(0);
    clock_tick = 1;
    input_replay_tick_begin();
    input_replay_tick_action(action);
    input_replay_tick_end();
    check_environment_mode(0); /* action gap */
    clock_tick = 0;
    input_replay_tick_begin();
    check_environment_mode(0); /* checkpoint/seek retains ownership */
    input_replay_tick_action(action);
    assert(action[0] == 1 && replay.segment == 1);
    input_replay_tick_end();
    clock_tick = 2;
    input_replay_tick_begin();
    input_replay_tick_action(action);
    assert(action[0] == 2);
    assert(replay.replay_finished); /* unchanged replay finish timing */
    check_environment_mode(0); /* units still consume the final action this tick */
    input_replay_tick_end();
    check_environment_mode(1);
    for (i = 0; i < 2; i++) {
        clock_ready = 0;
        if (i) { enhanced_animations = 0; environment_config_changes++; }
        environment_files(i ? " enhanced_animations=1" : " enhanced_animations=0");
        assert(input_replay_begin("./environment.input", 0));
        clock_ready = 1;
        clock_tick = 0;
        input_replay_tick_begin();
        check_environment_mode(i);
        input_replay_begin(NULL, 0);
        check_environment_mode(i ? 0 : 1); /* next scene/MP: current configured mode */
        if (i) { enhanced_animations = 1; environment_config_changes++; }
        check_environment_mode(1);
    }
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        environment_files(invalid[i]);
        assert(!input_replay_begin("./environment.input", 0));
        input_replay_tick_begin();
        check_environment_mode(1);
    }
    environment_files(" lip_sync=original enhanced_animations=0");
    assert(!input_replay_begin("./environment.input", 1));
    input_replay_tick_begin();
    check_environment_mode(1);
    remove("environment.actions");
    clock_ready = 0;
    assert(input_replay_begin("./environment.input", 0)); /* optional legacy controller-only take */
    clock_ready = 1;
    input_replay_tick_begin();
    check_environment_mode(0);
    replay_finish();
    check_environment_mode(1); /* controller-only tail ends immediately */
    remove("environment.input");
    assert(!input_replay_begin("./environment.input", 0));
    input_replay_tick_begin();
    check_environment_mode(1);
    environment_files("");
    { FILE *file = fopen("environment.input", "w");
      assert(file); fputs("not an input recording\n", file); fclose(file); }
    assert(!input_replay_begin("./environment.input", 0));
    memset(action, 0, sizeof(action));
    input_replay_tick_begin();
    input_replay_tick_action(action);
    assert(action[0] == 0 && !replay.camera_sample.valid);
    check_environment_mode(1);
    input_replay_suite_restore();
    check_environment_mode(1);
    assert(input_replay_suite_suspend());
    input_replay_tick_begin();
    check_environment_mode(1); /* suite ownership alone, including MP, is not replay ownership */
    input_replay_suite_restore();
    environment_files(" enhanced_animations=0");
    replay_name = "./environment.input";
    clock_ready = 0;
    replay.configured = 0;
    input_replay_tick_begin(); /* direct configured entry, before the first simulation tick */
    check_environment_mode(1);
    clock_ready = 1;
    clock_tick = 0;
    input_replay_tick_begin();
    check_environment_mode(0);
    input_replay_suite_restore(); /* not suite-owned: ordinary replay continues */
    check_environment_mode(0);
    input_replay_begin(NULL, 0); /* explicit ordinary replay cancellation */
    replay_name = "";
    check_environment_mode(1);
}

static void check_recorded_environment(void)
{
    unsigned int action[ACTION_WORDS] = {0};
    char header[256];
    XINPUT_GAMEPAD pad = {0};
    FILE *file;
    clock_ready = 1;
    clock_tick = 0;
    input_replay_begin(NULL, 0);
    replay.await_level = 0;
    strcpy(replay.record_path, "environment.input");
    strcpy(replay.record_actions_path, "environment.actions");
    enhanced_animations = 1;
    record(&pad, 0);
    input_replay_tick_action(action);
    record_end("probe");
    file = fopen("environment.actions", "r");
    assert(file && fgets(header, sizeof(header), file));
    fclose(file);
    assert(strstr(header, " enhanced_animations=1"));
    assert(replay_environment_parse(header, &replay.environment));
    assert(replay.environment.enhanced_recorded && replay.environment.enhanced_animations);
    clock_ready = 0;
    assert(input_replay_begin("./environment.input", 0)); /* actual writer/reader round trip */
    clock_ready = 1;
    input_replay_tick_begin();
    assert(input_replay_enhanced_animations(0));
    input_replay_begin(NULL, 0);
    replay.await_level = 0;
    strcpy(replay.record_path, "environment.input");
    strcpy(replay.record_actions_path, "environment.actions");
    enhanced_animations = 0;
    input_replay_tick_action(action);
    assert(replay.actions_written == 1);
    enhanced_animations = 1;
    input_replay_tick_begin(); /* close before a changed-mode simulation tick */
    input_replay_tick_action(action);
    assert(replay.record_ended && !replay.actions_file && replay.actions_written == 1);
    file = fopen("environment.actions", "r");
    assert(file && fgets(header, sizeof(header), file));
    fclose(file);
    assert(replay_environment_parse(header, &replay.environment));
    assert(replay.environment.enhanced_recorded && !replay.environment.enhanced_animations);
    input_replay_begin(NULL, 0);
    replay.await_level = 0;
    assert(!mkdir("recordings", 0700));
    take.automatic = TRUE;
    input_replay_tick_action(action);
    assert(take.open && replay.actions_file);
    enhanced_animations = 0;
    input_replay_tick_begin();
    assert(!take.open && !take.automatic && replay.record_ended);
    input_replay_tick_action(action);
    assert(!take.open && !replay.actions_file); /* no same-second overwrite/restart */
    remove_recording_fixture_files();
    assert(!rmdir("recordings"));
    enhanced_animations = 1;
    input_replay_begin(NULL, 0);
}

extern int input_replay_scenario_help_suppressed(void);
extern int input_replay_scenario_help_fixed(void);
static void check_enemy_multiplier_metadata(void)
{
	unsigned int action[ACTION_WORDS] = { 0x00000001u, 0x3f800000u, 0, 0, 0, 0, 0, 0 };
	XINPUT_GAMEPAD pad = { 0 };
	char line[128] = "";
	FILE *file;

	/* a recording made with debug.enemy_multiplier 2 notes it in NAME.route */
	input_replay_begin(NULL, 0); clock_ready = 1; clock_tick = 0;
	replay.await_level = 0;
	probe_multiplier = 2.0;
	strcpy(replay.record_path, "multiplier.input"); strcpy(replay.record_actions_path, "multiplier.actions");
	record(&pad, 0); input_replay_tick_action(action); record_end("multiplier probe");
	file = fopen("multiplier.route", "r"); assert(file && fgets(line, sizeof(line), file)); fclose(file);
	assert(!strcmp(line, "enemy_multiplier = 2.000\n"));
	input_replay_begin(NULL, 0);
	/* its replay uses the recorded value whatever the setting is */
	probe_multiplier = 1.0; clock_ready = 0;
	assert(input_replay_begin("./multiplier.input", 0) && probe_override == 2.0);
	input_replay_begin(NULL, 0); assert(probe_override < 0.0);
	probe_multiplier = 3.0;
	assert(input_replay_begin("./multiplier.input", 0) && probe_override == 2.0);
	input_replay_begin(NULL, 0);
	/* a setting equal to the recorded value needs no override; a replay that records a new take keeps the setting */
	probe_multiplier = 2.0;
	assert(input_replay_begin("./multiplier.input", 0) && probe_override < 0.0);
	input_replay_begin(NULL, 0);
	probe_multiplier = 3.0; record_name = "other";
	assert(input_replay_begin("./multiplier.input", 0) && probe_override < 0.0);
	input_replay_begin(NULL, 0); record_name = "";
	/* in-game help dialogs: shown unless the replayed route says "scenario_help = off" */
	file = fopen("multiplier.route", "w"); assert(file); fputs("map = a10\n", file); fclose(file);
	assert(input_replay_begin("./multiplier.input", 0) && !input_replay_scenario_help_suppressed());
	file = fopen("multiplier.route", "w"); assert(file); fputs("map = a10\nscenario_help = off\n", file); fclose(file);
	assert(input_replay_begin("./multiplier.input", 0) && input_replay_scenario_help_suppressed());
	/* a replay under way decides the dialogs in place of the player's profile; none once it is gone */
	assert(input_replay_scenario_help_fixed());
	input_replay_begin(NULL, 0); assert(!input_replay_scenario_help_suppressed() && !input_replay_scenario_help_fixed());
	/* a route without the line was recorded at 1: the setting does not apply to it */
	file = fopen("multiplier.route", "w"); assert(file); fputs("map = a10\n", file); fclose(file);
	assert(input_replay_begin("./multiplier.input", 0) && probe_override == 1.0 && coop_enemies_solo_multiplier() == 1.0);
	input_replay_begin(NULL, 0);
	/* and a setting of 1 writes no route */
	unlink("multiplier.route"); probe_multiplier = 1.0;
	input_replay_begin(NULL, 0); clock_ready = 1; clock_tick = 0; replay.await_level = 0;
	strcpy(replay.record_path, "multiplier.input"); strcpy(replay.record_actions_path, "multiplier.actions");
	record(&pad, 0); input_replay_tick_action(action); record_end("multiplier probe");
	assert(access("multiplier.route", F_OK));
	input_replay_begin(NULL, 0); clock_ready = 0; probe_multiplier = 1.0;
	unlink("multiplier.input"); unlink("multiplier.actions"); unlink("multiplier.seeds");
	puts("enemy multiplier metadata: PASS");
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
	if (argc == 2 && !strcmp(argv[1], "--replay-environment")) {
        check_replayed_environment();
        check_recorded_environment();
        unlink("environment.input");
        unlink("environment.actions");
        unlink("environment.seeds");
        assert(!chdir("/"));
        assert(!rmdir(folder));
        puts("replay environment: PASS");
        return 0;
    }
	check_recorded_action_metadata();
	check_enemy_multiplier_metadata();
	check_controller_action_format();
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
	assert(input_replay_suite_suspend());
	assert(input_replay_suite_suspended() && touch_replay_active);
	input_replay_suite_restore();
	assert(!input_replay_suite_suspended() && !touch_replay_active);
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
	clock_tick = 0; /* the restored controller-only replay starts before its tail, without a stale suite wait */
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
