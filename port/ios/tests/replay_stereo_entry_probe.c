/* Execute the real replay state and stereo accounting in one frame loop.
   Rename only colliding environment stubs from the replay fixture. */
#define main replay_fixture_main
#define config_boolean replay_config_boolean
#define config_integer replay_config_integer
#define config_string replay_config_string
#define config_folder replay_config_folder
#define config_profile_settings_text replay_config_profile_settings_text
#define platform_log replay_platform_log
#define halo_frame_trace_milliseconds replay_trace_milliseconds
#include "input_replay_probe.c"
#undef main
#undef config_boolean
#undef config_integer
#undef config_string
#undef config_folder
#undef config_profile_settings_text
#undef platform_log
#undef halo_frame_trace_milliseconds
#define main stereo_fixture_main
#include "stereo_head_probe.c"
#undef main

static void input_frame(float facing[2])
{
	float yaw, pitch;
	/* Same session gate used by get_local_player_input_blob for action slot zero. */
	if (input_replay_action_camera_active())
		halo_stereo_replay_look(0, 0.0f);
	else if (halo_stereo_head_look(0, facing[1], &yaw, &pitch)) {
		facing[0] += yaw;
		facing[1] += pitch;
	}
}

int main(void)
{
	char folder[] = "/tmp/visr-replay-head-XXXXXX";
	assert(mkdtemp(folder) && !chdir(folder));
	FILE *file = fopen("camera.input", "w"); assert(file);
	fputs("halo input recording 1\n0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", file); fclose(file);
	file = fopen("camera.actions", "w"); assert(file);
	fputs("halo action recording 1\n0 10 0 0 0 0 0 0 0 0\n0 12 0 0 0 0 0 0 0 0\n", file); fclose(file);
	assert(input_replay_begin("./camera.input", 0));
	clock_ready = 1; clock_tick = 10;
	restart("snap", 30, 120, 0);
	float facing[2] = {0}, view_yaw, view_pitch;
	short zoom;
	unsigned int action[ACTION_WORDS] = {0};
	/* Head motion is pending before the first recorded action is injected. */
	set_pose(5 * DEGREES, 0, 0); halo_stereo_frame_begin();
	input_frame(facing);
	input_replay_tick_action(action);
	assert(input_replay_apply_control(0, facing, &zoom));
	halo_stereo_camera_head_yaw(halo_stereo_head_yaw_taken(), 1, "probe");
	oriented(facing[0], facing[1], &view_yaw, &view_pitch);
	check(fabsf(view_yaw - 5) < 0.01f, "first replay action preserves pending head yaw");
	/* A missing action releases frame-facing ownership, but cannot consume the
	   head motion that the next recorded pose would overwrite. */
	clock_tick = 11; input_replay_tick_action(action);
	assert(!input_replay_controls_facing(0));
	set_pose(10 * DEGREES, 0, 0); halo_stereo_frame_begin();
	input_frame(facing);
	clock_tick = 12; input_replay_tick_action(action);
	assert(input_replay_apply_control(0, facing, &zoom));
	halo_stereo_camera_head_yaw(halo_stereo_head_yaw_taken(), 1, "probe");
	oriented(facing[0], facing[1], &view_yaw, &view_pitch);
	check(fabsf(view_yaw - 10) < 0.01f, "reacquired replay action preserves accumulated head yaw");
	/* Ending the replay hands pending motion back to ordinary control once. */
	clock_ready = 0; input_replay_begin(NULL, 0); clock_ready = 1;
	input_frame(facing);
	halo_stereo_camera_head_yaw(halo_stereo_head_yaw_taken(), 1, "probe");
	oriented(facing[0], facing[1], &view_yaw, &view_pitch);
	check(fabsf(view_yaw - 10) < 0.01f, "replay release retains eye orientation after entry and gap");
	clock_ready = 0; input_replay_begin(NULL, 0);
	unlink("camera.input"); unlink("camera.actions"); assert(!chdir("/") && !rmdir(folder));
	return failures ? 1 : 0;
}
