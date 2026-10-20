/* The existing stereo probe's real head and camera code, with the frame
order that `main.c` uses. The dead camera does not take the look delta. */
#ifdef HALO_STEREO_SOURCE_DEAD_CAMERA
typedef int boolean;
#define FALSE 0
#define TRUE 1
#define NONE (-1)
#define MAXIMUM_NUMBER_OF_LOCAL_PLAYERS 4
typedef void (*director_camera_update_proc)(void *, void *, void *);
struct director { director_camera_update_proc camera_proc; };
struct player_datum { long unit_index; };
static struct director source_director;
static struct player_datum source_player;
static int source_network, source_player_present = 1;
static void dead_camera_update(void *camera, void *command, void *result)
{ (void)camera; (void)command; (void)result; }
static void other_camera_update(void *camera, void *command, void *result)
{ (void)camera; (void)command; (void)result; }
static struct director *director_get(short local) { (void)local; return &source_director; }
static long local_player_get_player_index(short local) { (void)local; return source_player_present ? 1 : NONE; }
static struct player_datum *player_get(long player) { (void)player; return &source_player; }
static int coop_spectating(void) { return source_network; }
#include "director_dead.inc"
#include "coop_dead.inc"
#endif
#define main stereo_fixture_main
#include "stereo_head_probe.c"
#undef main

static void ten_head_frames(const char *name, int third_person, int facing_takes_look)
{
	float camera_yaw = 0.0f, eye_yaw = 0.0f, eye_pitch = 0.0f;
	int frame;

	game_third_person = third_person;
	#ifdef HALO_STEREO_SOURCE_DEAD_CAMERA
	source_network = 1;
	source_player.unit_index = facing_takes_look || third_person ? 2 : NONE;
	source_director.camera_proc = !third_person && !facing_takes_look ? dead_camera_update : other_camera_update;
	#else
	game_dead_camera = !third_person && !facing_takes_look;
	#endif
	game_scripted_camera = game_letterbox = game_look_disabled = 0;
	game_player_look = facing_takes_look;
	third_person_head = 0;
	third_person_yaw = 0.0f;
	head_pitch_known = 0;
	restart("snap", 30.0, 120.0, 0);
	for (frame = 1; frame <= 10; frame++) {
		float look_yaw = 0.0f, look_pitch = 0.0f;

		if (halo_stereo_head_look(0, 0.0f, &look_yaw, &look_pitch) && facing_takes_look)
			camera_yaw += look_yaw;
		set_pose((float)frame * DEGREES, 0.0f, 0.0f);
		halo_stereo_frame_begin();
		/* render_interpolation_camera notes taken yaw even when the
		observer is not posed from player facing (the dead camera). */
		halo_stereo_camera_head_yaw(halo_stereo_head_yaw_taken(), 1.0f, "live");
		halo_stereo_camera_posed(facing_takes_look);
		oriented(camera_yaw, 0.0f, &eye_yaw, &eye_pitch);
	}
	printf("%s: head 10.000 deg, view %.3f deg, taken %.3f deg, pending %.3f deg, third-person %d\n",
		name, eye_yaw, halo_stereo_head_yaw_taken() / DEGREES,
		head_pending_yaw / DEGREES, third_person_head);
	check(fabsf(eye_yaw - 10.0f) < 0.01f, name);
}

#ifdef HALO_STEREO_SOURCE_DEAD_CAMERA
static void camera_ownership(void)
{
	source_network = source_player_present = 1;
	source_player.unit_index = NONE;
	source_director.camera_proc = dead_camera_update;
	check(halo_coop_dead_camera(), "local co-op corpse uses the dead picture camera");
	check(!director_dead_camera_active(-1) && !director_dead_camera_active(MAXIMUM_NUMBER_OF_LOCAL_PLAYERS),
		"invalid local camera indices are excluded");
	check(halo_coop_dead_camera(), "the same dead procedure covers a teammate target and first-spawn wait");
	source_player.unit_index = 2;
	check(!halo_coop_dead_camera(), "respawn releases the dead picture camera");
	source_player.unit_index = NONE;
	source_director.camera_proc = other_camera_update;
	check(!halo_coop_dead_camera(), "scripted host or debug camera is excluded");
	source_director.camera_proc = dead_camera_update;
	source_network = 0;
	check(!halo_coop_dead_camera(), "local campaign death is excluded");
	source_network = 1;
	source_player_present = 0;
	check(!halo_coop_dead_camera(), "absent local player is excluded");
	source_player_present = 1;
}

static void camera_transitions(void)
{
	float eye_yaw, eye_pitch, look_yaw, look_pitch;
	int frame, action_stayed_local = 1;

	source_network = source_player_present = 1;
	source_player.unit_index = NONE;
	source_director.camera_proc = dead_camera_update;
	game_third_person = game_scripted_camera = game_letterbox = game_look_disabled = game_player_look = 0;
	third_person_head = 0;
	third_person_yaw = 0.0f;
	head_pitch_known = 0;
	restart("snap", 30.0, 120.0, 0);
	for (frame = 1; frame <= 10; frame++) {
		look_yaw = look_pitch = 0.0f;
		action_stayed_local &= !halo_stereo_head_look(0, 0.0f, &look_yaw, &look_pitch) && look_yaw == 0.0f;
		set_pose((float)frame * DEGREES, 0.0f, 0.0f);
		halo_stereo_frame_begin();
		halo_stereo_camera_head_yaw(halo_stereo_head_yaw_taken(), 1.0f, "unposed");
		halo_stereo_camera_posed(0);
		oriented(0.0f, 0.0f, &eye_yaw, &eye_pitch);
	}
	check(action_stayed_local, "dead view never hands headset yaw to the player action");
	check(fabsf(eye_yaw - 10.0f) < 0.01f, "corpse-to-teammate dead camera keeps accumulated head turn");
	/* The director switches back to player first person; the existing
	third-person exit keeps the local offset until the camera holds facing. */
	source_player.unit_index = 2;
	source_director.camera_proc = other_camera_update;
	game_player_look = 1;
	halo_stereo_frame_begin();
	halo_stereo_camera_posed(0);
	oriented(0.0f, 0.0f, &eye_yaw, &eye_pitch);
	check(fabsf(eye_yaw - 10.0f) < 0.01f, "respawn glide keeps the head's local view");
	halo_stereo_camera_posed(1);
	look_yaw = look_pitch = 0.0f;
	halo_stereo_head_look(0, 0.0f, &look_yaw, &look_pitch);
	halo_stereo_frame_begin();
	halo_stereo_camera_head_yaw(halo_stereo_head_yaw_taken(), 1.0f, "live");
	oriented(look_yaw, 0.0f, &eye_yaw, &eye_pitch);
	check(fabsf(eye_yaw - 10.0f) < 0.01f, "respawn hands the offset to first-person facing without a step");
	/* A borrowed host camera is scripted, so it cannot keep the local
	dead-camera picture path. */
	source_player.unit_index = NONE;
	source_director.camera_proc = other_camera_update;
	game_scripted_camera = 1;
	halo_stereo_frame_begin();
	check(!third_person_head, "scripted host camera leaves the dead head picture path");
	game_scripted_camera = 0;
}

static void paused_and_unanchored(void)
{
	float eye_yaw, eye_pitch;
	int frame;

	source_network = source_player_present = 1;
	source_player.unit_index = NONE;
	source_director.camera_proc = dead_camera_update;
	game_third_person = game_scripted_camera = game_letterbox = game_look_disabled = game_player_look = 0;
	third_person_head = 0;
	third_person_yaw = 0.0f;
	head_pitch_known = 0;
	restart("snap", 30.0, 120.0, 0);
	/* A paused game has no input update, but head frames still draw. */
	for (frame = 1; frame <= 5; frame++) {
		set_pose((float)frame * DEGREES, 0.0f, 0.0f);
		halo_stereo_frame_begin();
		oriented(0.0f, 0.0f, &eye_yaw, &eye_pitch);
	}
	check(fabsf(eye_yaw - 5.0f) < 0.01f && halo_stereo_head_yaw_taken() == 0.0f,
		"paused dead view still follows five degrees of local head motion");
	host_unanchored = 1;
	set_pose(10.0f * DEGREES, 0.0f, 0.0f);
	halo_stereo_frame_begin();
	oriented(0.0f, 0.0f, &eye_yaw, &eye_pitch);
	check(fabsf(eye_yaw - 5.0f) < 0.01f,
		"without a head anchor the dead view does not invent another turn");
	host_unanchored = 0;
}
#endif

int main(void)
{
	ten_head_frames("dead neutral, no unit to apply look", 0, 0);
	ten_head_frames("living first person, unit applies look", 0, 1);
	ten_head_frames("following camera, picture takes head", 1, 0);
	#ifdef HALO_STEREO_SOURCE_DEAD_CAMERA
	camera_ownership();
	camera_transitions();
	paused_and_unanchored();
	#endif
	#ifndef HALO_STEREO_SOURCE_DEAD_CAMERA
	game_dead_camera = 0;
	#endif
	return failures ? 1 : 0;
}
