/* Head-tracked stereo's look (port/linux/game/stereo.c with
port/ios/host/host_stereo_head.c), frame by frame as the game runs it:
the player's look takes in the head's turn, the next frame begins with the
head's new pose, and the render orients its camera. The world the camera
shows must stay put in the room: its up is the room's up, and its forward
doesn't move, while the head pans level at any pitch. A world that drifts
in roll while the head pans is the bug from headset session 1.

And the right stick's turn (input.turn): snaps by default, smooth turning at
input.smooth_turn_speed whatever the frame rate, none at all for "off", and
the comfort vignette's easing. stereo.c is included, not linked, so each
check can read the settings afresh.

And the film's (a cutscene on the screen): the mapping it shares with the
3D TV (halo_stereo_tv_eyes) puts infinity a share of the eye separation
behind the screen and the convergence distance on its surface, and the
held reason keeps the cutscene's framing through the hold; any script fade
over the picture covers the cut.

And a vehicle's third-person camera: the head turns the picture, relative
to its pose when the camera began (no jump on taking a seat), and never the
player's facing; the stick keeps the game's own turn and pitch. With
display.stereo_vehicle_screen the camera goes on the screen instead.

And the head's yaw at render time: paused (no look between frames) the eye
cameras still turn with the head, and the look takes the whole turn when it
runs again, without a jump; and with the game's camera blended between 30 Hz
ticks (render_interpolation.c), the eye cameras' yaw is the head's every
frame, plus the body's own.

And split screen (more than one player window): it keeps the mono path, as
render.c's eye loop does, so the frame has no eyes for any hook to take: no
head-driven look, no 3D film, no screen framing. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "halo_stereo.h"
#include "host_stereo_head.h"
#include "host_stereo_vignette.h"
#include "halo_stereo_window.h"
#include "halo_stereo_veil.h"

/* the eye shader's vignette mask, as C */
HOST_STEREO_VIGNETTE_SOURCE

/* the settings each check reads (NULL: the setting is absent) */
static const char *setting_turn = "smooth";
static double setting_snap_angle = 30.0, setting_smooth_turn_speed = 120.0;
static int setting_comfort_vignette;
static int unrecognized_logged;
static char last_log[256];
/* the film's settings, and what the game shows: the letterbox, a scripted
camera */
static double setting_film_depth_share = 0.25, setting_film_convergence = 1.75;
/* display.stereo, and the window's drawable size (the side-by-side view's) */
static const char *setting_stereo = "head";
static int probe_drawable[2];
static int game_letterbox, game_scripted_camera, game_third_person;
/* the cutscene's camera: first person (the director's or a scripted one in
first-person mode), the look taken away in it, and whether the camera has
reached the player's eyes (the observer settled) */
static int game_first_person, game_look_disabled, game_settled = 1;
/* the player's own first-person camera with the look enabled (the
director's first person, player_camera_control on): 0 unless a case sets it,
so every other case's first-person camera is one without the player's look */
static int game_player_look;
/* the film's "expands out to the full view" lines logged, and its "didn't
reach the player's eyes" lines */
static int expansion_logs, settle_gave_up_logs;
/* the last "the eyes keep the head's" line (a third-person camera's end) and
the last "the look asks for the" line (the fold that follows it), kept apart
from last_log, which debug.head_yaw_log's line overwrites in the same frame */
static char eyes_keep_log[256], look_asks_log[256];

/* stereo.c's imports */
const char *config_string(const char *name)
{
	if (!strcmp(name, "display.stereo"))
		return setting_stereo;
	if (!strcmp(name, "input.turn"))
		return setting_turn;
	if (!strcmp(name, "display.screen_framing"))
		return "band";
	return "";
}
double config_real(const char *name)
{
	if (!strcmp(name, "display.lod_scale"))
		return 1.0;
	if (!strcmp(name, "display.hud_resolution"))
		return 1.0;
	if (!strcmp(name, "input.smooth_turn_speed"))
		return setting_smooth_turn_speed;
	if (!strcmp(name, "display.film_depth_share"))
		return setting_film_depth_share;
	if (!strcmp(name, "display.film_convergence"))
		return setting_film_convergence;
	if (!strcmp(name, "display.screen_depth_share"))
		return 0.3;
	if (!strcmp(name, "display.screen_convergence"))
		return 1.0;
	if (!strcmp(name, "debug.screen_lean"))
		return 0.0;
	if (!strcmp(name, "display.seat_look_fade_start"))
		return 5.0;
	if (!strcmp(name, "display.seat_look_fade_black"))
		return 45.0;
	return setting_snap_angle;
}
int config_boolean(const char *name) { return !strcmp(name, "input.comfort_vignette") && setting_comfort_vignette; }
void platform_video_drawable_size(int *width, int *height) { *width = probe_drawable[0]; *height = probe_drawable[1]; }
void platform_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(last_log, sizeof(last_log), format, arguments);
	va_end(arguments);
	if (strstr(last_log, "is not recognized; using snap"))
		unrecognized_logged++;
	if (strstr(last_log, "expands out to the full view"))
		expansion_logs++;
	if (strstr(last_log, "didn't reach the player's eyes"))
		settle_gave_up_logs++;
	if (strstr(last_log, "the eyes keep the head's"))
		memcpy(eyes_keep_log, last_log, sizeof(eyes_keep_log));
	if (strstr(last_log, "the look asks for the"))
		memcpy(look_asks_log, last_log, sizeof(look_asks_log));
}
int halo_cinematic_screen(void) { return game_letterbox; }
int halo_scripted_camera(void) { return game_scripted_camera; }
int halo_scripted_director_camera(void) { return 0; }
int halo_level_first_tick_pending(void) { return 0; }
int halo_level_start_campaign(void) { return 0; }
int platform_fixed_timestep(void) { return 1; }
/* the frame clock (debug.fixed_timestep: 1/30 s a frame), a frame each
time stereo.c reads it (once a frame) */
static unsigned long probe_clock;
unsigned long platform_clock_frames(void) { return ++probe_clock; }
double halo_frame_trace_milliseconds(void) { return 0.0; }
int halo_third_person_camera(void) { return game_third_person; }
/* The focused dead-camera probe can replace this with the production
camera procedure and network predicate. */
#ifndef HALO_STEREO_SOURCE_DEAD_CAMERA
static int game_dead_camera;
int halo_coop_dead_camera(void) { return game_dead_camera; }
#endif
int halo_cutscene_camera_first_person(void) { return game_first_person; }
/* the cutscene camera's horizontal field of view (Task 12k): 70 degrees unless a case sets it */
static float game_field_of_view = 1.2217305f;
float halo_cutscene_camera_field_of_view(void) { return game_field_of_view; }
int halo_look_disabled_first_person(void) { return game_look_disabled; }
int halo_player_camera_first_person(void) { return game_player_look; }
/* the director holding the facing (a seat's entry or exit animation) */
static int game_inhibited_facing;
int halo_director_inhibited_facing(void) { return game_inhibited_facing; }
int halo_cutscene_camera_settled(void) { return game_settled; }
void halo_cutscene_state(struct halo_cutscene_state *state)
{
	memset(state, 0, sizeof(*state));
	state->letterbox = game_letterbox;
	state->look_disabled = game_look_disabled;
	state->player_first_person = game_player_look;
	state->observer_finished = game_settled;
	state->distance = game_settled ? 0.0f : 1.0f;
}
void halo_screen_commit_stereo_scale(void) {}
/* main.c's player windows: 1 unless a case splits the screen */
static short probe_windows = 1;
short main_get_window_count(void) { return probe_windows; }

#include "../../linux/game/stereo.c"

/* the frames the film shows after a cutscene once the camera is at the
eyes, in HEAD mode: its ease into a window onto the world, 30 thirtieths of
a second, then the frame showing the window whole */
#define PORTAL_FRAMES 31

#define DEGREES (3.14159265f / 180.0f)

/* the head's yaw off the screen's axis, which turns the mock's SCREEN eyes */
static float probe_screen_yaw;

/* the device's pose this frame, columns right, up, back (ARKit's axes) */
static float pose[3][3];
static struct host_stereo_head head;
static int host_closed;
static int host_unanchored;
/* the Compositor frames the game asked for */
static unsigned long host_frames;

/* host_stereo.m's host_stereo_frame, reduced to the head */
void host_stereo_frame(struct halo_stereo_frame *frame)
{
	host_frames++;
	frame->head_yaw = frame->head_pitch = frame->head_roll = 0.0f;
	/* the space closed: no eyes, and the host forgets the head's pose */
	if (host_closed) {
		frame->eye_count = 0;
		head.known = 0;
		return;
	}
	frame->eye_count = 2;
	if (host_unanchored)
		host_stereo_head_hold(&head, frame);
	else
		host_stereo_head_turn(&head, pose[0], pose[1], pose[2], frame);
	/* SCREEN eyes (the film in HEAD mode): 64 mm apart, 4 m behind the
	camera, frusta through the edges of a screen 4.6 m wide */
	if (frame->mode == HALO_STEREO_SCREEN) {
		int eye;

		for (eye = 0; eye < 2; eye++) {
			struct halo_stereo_eye *e = &frame->eyes[eye];
			/* (the eyes turned with the head by probe_screen_yaw, left positive,
			about their midpoint) */
			float side = (eye == 0 ? -0.032f : 0.032f) / 3.048f, half = 2.309f / 3.048f;
			float x = side * cosf(probe_screen_yaw), distance = 4.0f / 3.048f - side * sinf(probe_screen_yaw);

			e->offset[0] = x;
			e->offset[1] = 0.0f;
			e->offset[2] = distance;
			e->left = (half + x) / distance;
			e->right = (half - x) / distance;
			e->up = e->down = 0.75f * half / distance;
		}
	}
}

static void rotate_axis(float v[3], int axis, float angle)
{
	float c = cosf(angle), s = sinf(angle), x = v[0], y = v[1], z = v[2];

	if (axis == 0) { v[1] = c * y - s * z; v[2] = s * y + c * z; }
	if (axis == 1) { v[0] = c * x + s * z; v[2] = -s * x + c * z; }
	if (axis == 2) { v[0] = c * x - s * y; v[1] = s * x + c * y; }
}

/* yaw about the room's up (left positive), then pitch about the head's right
(up positive), then roll about its back (left ear down positive) */
static void set_pose(float yaw, float pitch, float roll)
{
	int column, step;

	for (column = 0; column < 3; column++)
	{
		float *v = pose[column];

		v[0] = column == 0; v[1] = column == 1; v[2] = column == 2;
		for (step = 0; step < 3; step++)
		{
			if (step == 0) rotate_axis(v, 2, roll);
			if (step == 1) rotate_axis(v, 0, pitch);
			if (step == 2) rotate_axis(v, 1, yaw);
		}
	}
}

/* the room's vector for a world vector, through the camera (the game's
axes: x forward, y left, z up) and the device */
static void world_in_room(const float forward[3], const float up[3], const float world[3], float room[3])
{
	float right[3] = {
		forward[1] * up[2] - forward[2] * up[1],
		forward[2] * up[0] - forward[0] * up[2],
		forward[0] * up[1] - forward[1] * up[0],
	};
	float x = right[0] * world[0] + right[1] * world[1] + right[2] * world[2];
	float y = up[0] * world[0] + up[1] * world[1] + up[2] * world[2];
	float z = -(forward[0] * world[0] + forward[1] * world[1] + forward[2] * world[2]);
	int i;

	for (i = 0; i < 3; i++)
		room[i] = x * pose[0][i] + y * pose[1][i] + z * pose[2][i];
}

static int failures;

/* pans the head level from 0 to 60 degrees left, then to 60 right, at a pitch
and roll, starting with the game's look level */
static void pan(const char *name, float pitch, float roll)
{
	float game_yaw = 0.0f, game_pitch = 0.0f, first_forward[3] = { 0 };
	float worst_tilt = 0.0f, worst_drift = 0.0f, worst_sideways = 0.0f;
	int frame;

	memset(&head, 0, sizeof(head));
	for (frame = 0; frame <= 90; frame++)
	{
		float yaw = (frame <= 30 ? frame * 2.0f : 60.0f - (frame - 30) * 2.0f) * DEGREES;
		float look_yaw, look_pitch;
		float forward[3], up[3], world_up[3] = { 0, 0, 1 }, world_forward[3] = { 1, 0, 0 };
		float room_up[3], room_forward[3];
		float tilt, drift;

		/* player_control: the turn the last frame read */
		if (halo_stereo_head_look(0, game_pitch, &look_yaw, &look_pitch))
		{
			game_yaw += look_yaw;
			game_pitch += look_pitch;
		}
		set_pose(yaw, pitch, roll);
		halo_stereo_frame_begin();
		/* the render's camera, from the look, oriented by the head */
		forward[0] = cosf(game_pitch) * cosf(game_yaw);
		forward[1] = cosf(game_pitch) * sinf(game_yaw);
		forward[2] = sinf(game_pitch);
		up[0] = -sinf(game_pitch) * cosf(game_yaw);
		up[1] = -sinf(game_pitch) * sinf(game_yaw);
		up[2] = cosf(game_pitch);
		halo_stereo_head_orient(forward, up);
		world_in_room(forward, up, world_up, room_up);
		world_in_room(forward, up, world_forward, room_forward);
		if (frame == 0)
			memcpy(first_forward, room_forward, sizeof(first_forward));
		/* the world's up against the room's, and the world's forward against
		where it was: both must hold */
		tilt = acosf(fminf(1.0f, room_up[1])) / DEGREES;
		drift = acosf(fminf(1.0f, room_forward[0] * first_forward[0] + room_forward[1] * first_forward[1] +
			room_forward[2] * first_forward[2])) / DEGREES;
		if (tilt > worst_tilt)
			worst_tilt = tilt;
		if (drift > worst_drift)
			worst_drift = drift;
		/* how far the world's up leans from the room's across the head's view:
		roll as the eye sees it */
		{
			float sideways = fabsf(room_up[0] * pose[0][0] + (room_up[1] - 1.0f) * pose[0][1] +
				room_up[2] * pose[0][2]);

			sideways = asinf(fminf(1.0f, sideways)) / DEGREES;
			if (sideways > worst_sideways)
				worst_sideways = sideways;
		}
	}
	printf("%-36s world tilt %.3f deg, drift %.3f deg, sideways %.3f deg\n", name, worst_tilt, worst_drift,
		worst_sideways);
	if (worst_tilt > 0.05f || worst_drift > 0.05f || worst_sideways > 0.05f)
	{
		printf("  FAIL: the world moves in the room while the head pans\n");
		failures++;
	}
}

/* the settings read afresh, as if the game had just started */
static void restart(const char *turn, double snap_angle, double smooth_turn_speed, int comfort_vignette)
{
	setting_turn = turn;
	setting_snap_angle = snap_angle;
	setting_smooth_turn_speed = smooth_turn_speed;
	setting_comfort_vignette = comfort_vignette;
	turn_mode = -1;
	stereo_mode = -1;
	snap_pending = smooth_yaw = 0.0f;
	snap_armed = 1;
	vignette_strength = 0.0f;
	unrecognized_logged = 0;
	memset(&head, 0, sizeof(head));
	head_pending_yaw = 0.0f;
	head_yaw_now = head_yaw_taken = 0.0f;
	head_request = head_seat_leftover = head_fold = 0.0f;
	seat_cut_requested = 0;
	camera_held_facing = 0;
	fold_wait_frames = 0;
	seat_limited = 0;
	seat_look_fade_now = 0.0f;
	seat_look_fade_logged = 0;
	set_pose(0.0f, 0.0f, 0.0f);
	/* a frame with the Compositor's eyes: the head drives the view */
	halo_stereo_frame_begin();
}

/* the game's response curve (globals.globals' look_function), as
player_control.c evaluates it */
static float response(float stick)
{
	static const float curve[6] = { 0.0f, 0.05f, 0.1f, 0.25f, 0.58f, 1.0f };
	float x = fminf(fabsf(stick), 1.0f) * 5.0f;
	int low = (int)x < 5 ? (int)x : 5, high = low < 5 ? low + 1 : 5;
	float value = (curve[high] - curve[low]) * (x - low) + curve[low];

	return stick < 0.0f ? -value : value;
}

/* holds the stick at a yaw for a time at a frame rate, as the game's loop
does (the stick, then the look, then the next frame); returns the look's
turn in degrees, and the vignette's strongest */
static float hold_stick(float stick, float seconds, float frames_per_second, float *vignette_peak)
{
	float turned = 0.0f, peak = 0.0f;
	int frame, frames = (int)lroundf(seconds * frames_per_second);

	for (frame = 0; frame < frames; frame++) {
		float yaw = stick, pitch = 0.3f, look_yaw, look_pitch;

		halo_stereo_stick_look(0, response(stick), 1.0f / frames_per_second, &yaw, &pitch);
		if (yaw != 0.0f || pitch != 0.0f) {
			printf("  FAIL: the game's own stick turn wasn't zeroed (yaw %f, pitch %f)\n", yaw, pitch);
			failures++;
		}
		if (halo_stereo_head_look(0, 0.0f, &look_yaw, &look_pitch))
			turned += look_yaw;
		halo_stereo_frame_begin();
		if (halo_stereo_vignette() > peak)
			peak = halo_stereo_vignette();
	}
	if (vignette_peak)
		*vignette_peak = peak;
	return turned / DEGREES;
}

static void check(int passed, const char *what)
{
	printf("  %s: %s\n", passed ? "ok" : "FAIL", what);
	if (!passed)
		failures++;
}

static void turning(void)
{
	float turned, peak, rate;

	printf("right-stick turning (input.turn):\n");
	restart(NULL, 30.0, 120.0, 0);
	check(turn_mode == TURN_SNAP, "absent: snap");
	restart("", 30.0, 120.0, 0);
	check(turn_mode == TURN_SNAP, "empty: snap");
	restart("spin", 30.0, 120.0, 0);
	check(turn_mode == TURN_SNAP && unrecognized_logged == 1, "unrecognized: snap, logged once");
	check(strstr(last_log, "input.turn snap, input.snap_angle 30.0, input.smooth_turn_speed 120.0, "
		"input.comfort_vignette false") != NULL, "the settings line logs the effective settings");
	hold_stick(1.0f, 0.1f, 90.0f, NULL);
	check(unrecognized_logged == 1, "unrecognized: not logged again");

	/* snap: one flick, one snap, however long it's held; back inside the
	release and out again, another */
	restart("snap", 30.0, 120.0, 0);
	turned = hold_stick(1.0f, 1.0f, 90.0f, NULL);
	check(fabsf(turned - 30.0f) < 0.001f, "snap: a held flick turns 30 degrees once");
	hold_stick(0.0f, 0.1f, 90.0f, NULL);
	turned = hold_stick(-0.8f, 0.5f, 30.0f, NULL);
	check(fabsf(turned + 30.0f) < 0.001f, "snap: a flick the other way turns -30 degrees");
	turned = hold_stick(0.6f, 0.5f, 90.0f, NULL);
	check(turned == 0.0f, "snap: under the flick threshold, no turn");
	restart("snap", 1.0, 120.0, 0);
	check(fabsf(hold_stick(1.0f, 0.1f, 90.0f, NULL) - 5.0f) < 0.001f, "snap: input.snap_angle 1 clamps to 5");
	restart("snap", 400.0, 120.0, 0);
	check(fabsf(hold_stick(1.0f, 0.1f, 90.0f, NULL) - 180.0f) < 0.001f, "snap: input.snap_angle 400 clamps to 180");

	restart("off", 30.0, 120.0, 0);
	turned = hold_stick(1.0f, 1.0f, 90.0f, NULL) + hold_stick(0.0f, 0.1f, 90.0f, NULL) + hold_stick(-1.0f, 1.0f, 90.0f, NULL);
	check(turned == 0.0f, "off: the stick never turns the look");

	/* smooth: input.smooth_turn_speed a second at full deflection, at 30, 60
	and 90 frames a second; the curve's share of it part way over */
	restart("smooth", 30.0, 120.0, 0);
	rate = hold_stick(1.0f, 1.0f, 90.0f, &peak);
	check(fabsf(rate - 120.0f) < 1.2f, "smooth: full deflection turns 120 degrees in a second at 90 Hz");
	check(peak == 0.0f, "smooth: no vignette unless input.comfort_vignette");
	check(fabsf(hold_stick(-1.0f, 1.0f, 30.0f, NULL) + 120.0f) < 1.2f, "smooth: -120 degrees in a second at 30 Hz");
	check(fabsf(hold_stick(1.0f, 1.0f, 60.0f, NULL) - 120.0f) < 1.2f, "smooth: 120 degrees in a second at 60 Hz");
	check(fabsf(hold_stick(0.5f, 1.0f, 90.0f, NULL) - 120.0f * response(0.5f)) < 0.1f,
		"smooth: half way over turns the curve's share (0.175)");
	restart("smooth", 30.0, 45.0, 0);
	check(fabsf(hold_stick(1.0f, 1.0f, 90.0f, NULL) - 45.0f) < 0.45f, "smooth: input.smooth_turn_speed 45");
	printf("  smooth at 120 deg/s: %.3f degrees in a second at 90 Hz\n", rate);

	/* the vignette: only smooth with input.comfort_vignette, as strong as
	the turn, gone with the turn and for snaps */
	restart("smooth", 30.0, 120.0, 1);
	hold_stick(1.0f, 0.5f, 90.0f, &peak);
	check(fabsf(peak - 1.0f) < 0.001f && fabsf(halo_stereo_vignette() - 1.0f) < 0.001f,
		"vignette: full while turning at full speed");
	hold_stick(0.0f, 0.3f, 90.0f, NULL);
	check(halo_stereo_vignette() == 0.0f, "vignette: gone 0.3 s after the turn stops");
	hold_stick(0.5f, 0.5f, 90.0f, &peak);
	check(fabsf(peak - response(0.5f)) < 0.001f, "vignette: half way over, the turn's share");
	restart("snap", 30.0, 120.0, 1);
	hold_stick(1.0f, 0.5f, 90.0f, &peak);
	check(peak == 0.0f, "vignette: none for snaps");
	restart("smooth", 30.0, 45.0, 1);
	hold_stick(1.0f, 0.5f, 90.0f, &peak);
	check(fabsf(peak - 45.0f / 120.0f) < 0.001f, "vignette: at 45 deg/s full over, 45/120 strong");
	restart("smooth", 30.0, 360.0, 1);
	hold_stick(1.0f, 0.5f, 90.0f, &peak);
	check(fabsf(peak - 1.0f) < 0.001f, "vignette: at 360 deg/s, full");
}

/* the vignette's easing, alone */
static void vignette_easing(void)
{
	float strength = 0.0f;
	int step;

	printf("comfort vignette easing:\n");
	for (step = 0; step < 8; step++)
		strength = halo_stereo_vignette_ease(strength, 1.0f, 1.0f / 90.0f);
	check(fabsf(strength - 8.0f / 9.0f) < 0.001f, "in: 8/9 after 8 frames at 90 Hz");
	strength = halo_stereo_vignette_ease(strength, 1.0f, 1.0f / 90.0f);
	check(strength > 0.999f, "in: full after 0.1 s");
	strength = halo_stereo_vignette_ease(strength, 1.0f, 1.0f / 90.0f);
	strength = halo_stereo_vignette_ease(strength, 1.0f, 1.0f / 90.0f);
	check(strength == 1.0f, "in: never past the target");
	strength = halo_stereo_vignette_ease(1.0f, 0.0f, 0.1f);
	check(fabsf(strength - 0.5f) < 0.0001f, "out: half after 0.1 s");
	strength = halo_stereo_vignette_ease(strength, 0.0f, 0.1f);
	check(strength == 0.0f, "out: gone after 0.2 s");
	check(fabsf(halo_stereo_vignette_ease(0.0f, 0.4f, 1.0f) - 0.4f) < 0.0001f, "in: to the turn's share, no further");
	check(fabsf(halo_stereo_vignette_ease(1.0f, 0.4f, 0.05f) - 0.75f) < 0.0001f, "out: toward the turn's share");
	check(halo_stereo_vignette_ease(0.3f, 2.0f, 0.0f) == 0.3f, "no time: no change");
	check(halo_stereo_vignette_ease(0.0f, 2.0f, 1.0f) == 1.0f, "a share over 1: full");
}

/* nearly straight up, forward's yaw is ill-conditioned: a head at 89.9
degrees that turns 30 degrees about the room's up must turn the look 30
degrees, read from its right, not by whatever forward's yaw swings to */
static void pole(void)
{
	struct halo_stereo_frame frame;

	memset(&head, 0, sizeof(head));
	memset(&frame, 0, sizeof(frame));
	set_pose(0.0f, 89.9f * DEGREES, 0.0f);
	host_stereo_head_turn(&head, pose[0], pose[1], pose[2], &frame);
	set_pose(30.0f * DEGREES, 89.9f * DEGREES, 0.0f);
	host_stereo_head_turn(&head, pose[0], pose[1], pose[2], &frame);
	printf("%-36s head yaw %.3f deg\n", "a 30 deg turn at 89.9 deg pitch", frame.head_yaw / DEGREES);
	if (fabsf(frame.head_yaw / DEGREES - 30.0f) > 0.01f)
	{
		printf("  FAIL: the turn at the pole isn't 30 degrees\n");
		failures++;
	}
}

/* the mask (host_stereo_vignette.h) at a direction, azimuth right positive
and elevation up positive in radians, through an eye's frustum tangents
(left, right, up, down); -1 if the eye doesn't see it */
static float mask_at(const float tangents[4], float azimuth, float elevation, float inner, float outer)
{
	float x = tanf(azimuth), y = tanf(elevation) / cosf(azimuth);
	float u = (x + tangents[0]) / (tangents[0] + tangents[1]);
	float v = (tangents[2] - y) / (tangents[2] + tangents[3]);

	if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
		return -1.0f;
	return host_stereo_vignette_edge(u, v, tangents[0], tangents[1], tangents[2], tangents[3], inner, outer);
}

/* the mask both eyes see: the Vision Pro's measured frusta (task-7b-report.md),
the right eye's mirroring the left's */
static void vignette_binocular(void)
{
	static const float left_eye[4] = { 1.757f, 1.014f, 1.014f, 1.209f };
	static const float right_eye[4] = { 1.014f, 1.757f, 1.014f, 1.209f };
	float outer = atanf(1.014f), inner = HOST_STEREO_VIGNETTE_CLEAR_SHARE * outer;
	float worst = 0.0f;
	int both = 0, a, e;

	printf("comfort vignette in both eyes:\n");
	for (a = -60; a <= 60; a += 2) {
		for (e = -40; e <= 40; e += 2) {
			float left = mask_at(left_eye, a * DEGREES, e * DEGREES, inner, outer);
			float right = mask_at(right_eye, a * DEGREES, e * DEGREES, inner, outer);

			if (left < 0.0f || right < 0.0f)
				continue;
			both++;
			if (fabsf(left - right) > worst)
				worst = fabsf(left - right);
		}
	}
	printf("  %d directions seen by both eyes: the masks differ by at most %.6f\n", both, worst);
	check(both > 500 && worst < 0.0001f, "a direction both eyes see is darkened alike in each");
	check(mask_at(left_eye, 0.0f, 0.0f, inner, outer) == 0.0f && mask_at(right_eye, 0.0f, 0.0f, inner, outer) == 0.0f,
		"straight ahead is clear in both eyes");
	check(mask_at(left_eye, 25.0f * DEGREES, 0.0f, inner, outer) == 0.0f &&
		mask_at(left_eye, -25.0f * DEGREES, 0.0f, inner, outer) == 0.0f,
		"the left eye is clear 25 degrees to either side (centered on straight ahead)");
	check(fabsf(mask_at(left_eye, 40.0f * DEGREES, 0.0f, inner, outer) -
		mask_at(right_eye, 40.0f * DEGREES, 0.0f, inner, outer)) < 0.0001f &&
		mask_at(left_eye, 40.0f * DEGREES, 0.0f, inner, outer) > 0.5f,
		"40 degrees right: the same in both eyes, over half dark");
	check(fabsf(mask_at(left_eye, -50.0f * DEGREES, 0.0f, inner, outer) - 1.0f) < 0.0001f,
		"50 degrees out (past the nearest edge's angle): full");
}

/* a tilted head nodding across the 85-degree line, with no turn about the
room's up, must not turn the look (the yaw's source changes there) */
static void pole_crossing(void)
{
	struct halo_stereo_frame frame;
	float worst = 0.0f;
	int step;

	memset(&head, 0, sizeof(head));
	for (step = 0; step <= 40; step++)
	{
		/* 80 to 90 degrees and back, with 10 degrees of roll */
		float pitch = (step <= 20 ? 80.0f + step * 0.5f : 90.0f - (step - 20) * 0.5f) * DEGREES;

		memset(&frame, 0, sizeof(frame));
		set_pose(20.0f * DEGREES, fminf(pitch, 89.9f * DEGREES), 10.0f * DEGREES);
		host_stereo_head_turn(&head, pose[0], pose[1], pose[2], &frame);
		if (fabsf(frame.head_yaw) > worst)
			worst = fabsf(frame.head_yaw);
	}
	printf("%-36s worst head yaw %.3f deg\n", "a tilted nod across 85 deg", worst / DEGREES);
	if (worst / DEGREES > 0.5f)
	{
		printf("  FAIL: nodding with the head tilted turned the look\n");
		failures++;
	}
}

/* where something at (x, y, z) in the camera's frame (right, up, ahead;
world units) lands on a screen of half width w, as an eye of the mapping
sees it: its tangents from the eye, placed in the eye's frustum, which spans
the screen */
static void on_screen(const struct halo_stereo_eye *e, float w, float aspect, const float point[3], float at[2])
{
	float tx = (point[0] - e->offset[0]) / point[2], ty = (point[1] - e->offset[1]) / point[2];

	at[0] = -w + 2.0f * w * (tx + e->left) / (e->left + e->right);
	at[1] = -w / aspect + 2.0f * (w / aspect) * (ty + e->down) / (e->up + e->down);
}

static void film_mapping_check(void)
{
	const float share = 0.25f, convergence = 1.75f, separation = 0.064f / 3.048f, w = 2.309f / 3.048f;
	const float vertical = 0.335f, aspect = 16.0f / 9.0f, horizontal = vertical * aspect;
	const float c = convergence / 3.048f, lean[2] = { 0.01f, -0.02f };
	struct halo_stereo_eye eyes[2];
	float worst = 0.0f;
	int k;

	printf("the film's mapping:\n");
	halo_stereo_tv_eyes(share, convergence, separation, w, vertical, NULL, eyes);
	check(fabsf(eyes[0].left - eyes[1].right) < 1e-6f && fabsf(eyes[0].right - eyes[1].left) < 1e-6f &&
		eyes[0].up == eyes[0].down && fabsf(eyes[0].offset[0] + eyes[1].offset[0]) < 1e-7f,
		"the eyes mirror each other");
	/* parallax (right eye's place minus the left's) against share e (1 - C / z), at
	any field of view, with and without a lean */
	for (k = 0; k < 2; k++) {
		const float *with = k ? lean : NULL;
		float distances[] = { c * 0.5f, c, c * 2.0f, c * 8.0f, 1e5f }, v;
		int d;

		for (v = 0.2f; v < 0.8f; v += 0.29f) {
			halo_stereo_tv_eyes(share, convergence, separation, w, v, with, eyes);
			for (d = 0; d < 5; d++) {
				float z = distances[d], point[3] = { 0.3f * z * v, -0.2f * z * v, z }, left[2], right[2];
				float expected = share * separation * (1.0f - c / z);

				on_screen(&eyes[0], w, aspect, point, left);
				on_screen(&eyes[1], w, aspect, point, right);
				worst = fmaxf(worst, fabsf((right[0] - left[0]) - expected) / (share * separation));
				worst = fmaxf(worst, fabsf(right[1] - left[1]) / (share * separation));
			}
		}
	}
	printf("  worst parallax error %.2e of infinity's\n", worst);
	check(worst < 1e-3f, "infinity share * e behind the screen, C on it, nearer in front, level");
	/* at the convergence where the camera's view is as wide as the screen, the
	eyes are share * e apart */
	halo_stereo_tv_eyes(share, w / horizontal * 3.048f, separation, w, vertical, NULL, eyes);
	check(fabsf((eyes[1].offset[0] - eyes[0].offset[0]) - share * separation) < 1e-6f,
		"s = share * e at C = w / T");
	/* a lean right and down moves both eyes so, and skews their frusta the
	other way: the picture's rectangle at C stays put */
	halo_stereo_tv_eyes(share, convergence, separation, w, vertical, lean, eyes);
	check(eyes[0].offset[0] + eyes[1].offset[0] > 0.0f && eyes[0].offset[1] < 0.0f &&
		eyes[0].left + eyes[1].left > eyes[0].right + eyes[1].right && eyes[0].up > eyes[0].down &&
		fabsf(eyes[0].up - (vertical - lean[1] / c)) < 1e-6f &&
		fabsf(eyes[1].left - (horizontal + eyes[1].offset[0] / c)) < 1e-6f, "the lean's signs");
}

/* frames of the game after the letterbox goes, each with a script fade of
this intensity over the picture (render.c's halo_stereo_set_fade); returns
how many frames the film's letterbox reason held, and whether every one of
them covered the cut */
static int frames_held(float fade_intensity, int *covered)
{
	const float fade[4] = { 1.0f, 1.0f, 1.0f, fade_intensity };
	int frame;

	*covered = 1;
	game_letterbox = 0;
	for (frame = 0; frame < 3 * PORTAL_FRAMES; frame++) {
		halo_stereo_frame_begin();
		halo_stereo_set_fade(fade);
		if (!halo_stereo_film_letterbox())
			break;
		*covered &= halo_stereo_cut_covered();
	}
	return frame;
}

static void film_hold_reason(void)
{
	int held, covered;

	printf("the film's held reason:\n");
	restart("snap", 30.0, 120.0, 0);
	game_letterbox = 1;
	halo_stereo_frame_begin();
	check(halo_stereo_film() && halo_stereo_film_letterbox() && halo_stereo_frame()->mode == HALO_STEREO_SCREEN,
		"the letterbox puts the film on the screen, for a cutscene");
	check(!halo_stereo_cut_covered(), "no script fade: a cut isn't covered");
	held = frames_held(0.0f, &covered);
	printf("  held %d frames after the letterbox went\n", held);
	check(held == PORTAL_FRAMES, "the cutscene's framing holds through the hold and the film's ease into a window");
	check(!halo_stereo_film() && halo_stereo_frame()->mode == HALO_STEREO_HEAD, "then the full view, head-tracked");
	check(!halo_stereo_cut_covered(), "a fade at zero covers no cut");

	/* a10's two-tick gap between cutscenes, under a white fade: the framing
	doesn't change */
	game_letterbox = 1;
	halo_stereo_frame_begin();
	game_letterbox = 0;
	halo_stereo_frame_begin();
	halo_stereo_frame_begin();
	held = halo_stereo_film_letterbox();
	game_letterbox = 1;
	halo_stereo_frame_begin();
	check(held && halo_stereo_film_letterbox(), "a two-frame gap in the letterbox keeps the cutscene's framing");

	/* a colored fade-in still under way when the hold ends: no black inside it */
	held = frames_held(0.3f, &covered);
	check(held == PORTAL_FRAMES && covered && halo_stereo_cut_covered(),
		"a fade over the picture holds the film as long, and covers the cut");

	/* a scripted camera is the film, without the cutscene's framing */
	game_scripted_camera = 1;
	halo_stereo_frame_begin();
	check(halo_stereo_film() && !halo_stereo_film_letterbox(), "a scripted camera's film has no letterbox reason");
	game_scripted_camera = 0;
	for (held = 0; held < 2 * PORTAL_FRAMES; held++)
		halo_stereo_frame_begin();
}

/* the yaw and pitch (degrees) of a camera facing (yaw, pitch) radians after
the render orients it by the head */
static void oriented(float yaw, float pitch, float *oriented_yaw, float *oriented_pitch)
{
	float forward[3] = { cosf(pitch) * cosf(yaw), cosf(pitch) * sinf(yaw), sinf(pitch) };
	float up[3] = { -sinf(pitch) * cosf(yaw), -sinf(pitch) * sinf(yaw), cosf(pitch) };

	halo_stereo_head_orient(forward, up);
	*oriented_yaw = atan2f(forward[1], forward[0]) / DEGREES;
	*oriented_pitch = asinf(fmaxf(-1.0f, fminf(1.0f, forward[2]))) / DEGREES;
}

/* one frame of the game's loop with the head at (yaw, pitch) degrees and the
stick at (stick_yaw, stick_pitch): the stick, then the look (which adds to
the facing what the head and stick hand it), then the frame and the render's
camera, which faces the facing (the chase camera points along it) and is
reported as holding it (halo_stereo_camera_posed). Returns
the stick's yaw and pitch as the game keeps them, and the view's angles */
struct loop_frame { float stick_yaw, stick_pitch, view_yaw, view_pitch; int look_turned; };

static struct loop_frame loop(float *facing_yaw, float *facing_pitch, float yaw, float pitch, float stick)
{
	struct loop_frame result;
	float look_yaw, look_pitch;

	result.stick_yaw = stick;
	result.stick_pitch = 0.3f;
	halo_stereo_stick_look(0, response(stick), 1.0f / 90.0f, &result.stick_yaw, &result.stick_pitch);
	result.look_turned = halo_stereo_head_look(0, *facing_pitch, &look_yaw, &look_pitch);
	if (result.look_turned) {
		*facing_yaw += look_yaw;
		*facing_pitch += look_pitch;
	}
	set_pose(yaw * DEGREES, pitch * DEGREES, 0.0f);
	halo_stereo_frame_begin();
	/* (the camera faces the facing, posed from it) */
	halo_stereo_camera_posed(1);
	oriented(*facing_yaw, *facing_pitch, &result.view_yaw, &result.view_pitch);
	return result;
}

static float degrees_apart(float a, float b)
{
	return fabsf(remainderf(a - b, 360.0f));
}

static void third_person(void)
{
	float facing_yaw = 0.0f, facing_pitch = 0.0f, entry_yaw, entry_pitch, worst_jump = 0.0f, worst_facing = 0.0f;
	float seated_view_yaw, seated_facing_yaw;
	struct loop_frame f;
	int frame, passed, snapped;

	printf("a vehicle's third-person camera:\n");
	restart("smooth", 30.0, 120.0, 1);
	game_third_person = 0;
	/* first person: the head turns 40 degrees left and pitches 15 down, and
	the look follows it */
	for (frame = 1; frame <= 20; frame++)
		loop(&facing_yaw, &facing_pitch, 2.0f * frame, -0.75f * frame, 0.0f);
	f = loop(&facing_yaw, &facing_pitch, 40.0f, -15.0f, 0.0f);
	printf("  first person, the head at 40 deg left, 15 deg down: facing %.3f, %.3f deg\n", facing_yaw / DEGREES,
		facing_pitch / DEGREES);
	check(fabsf(facing_yaw / DEGREES - 40.0f) < 0.01f && fabsf(facing_pitch / DEGREES + 15.0f) < 0.01f,
		"first person: the look follows the head");

	/* take a seat with the head still where it was */
	game_third_person = 1;
	entry_yaw = facing_yaw;
	entry_pitch = facing_pitch;
	f = loop(&facing_yaw, &facing_pitch, 40.0f, -15.0f, 0.0f);
	worst_jump = fmaxf(degrees_apart(f.view_yaw, entry_yaw / DEGREES), fabsf(f.view_pitch - entry_pitch / DEGREES));
	printf("  the seat's first frame: the view %.4f deg from the chase camera's\n", worst_jump);
	check(worst_jump < 0.01f && halo_stereo_frame()->mode == HALO_STEREO_HEAD && !halo_stereo_film(),
		"taking a seat with the head turned doesn't jump the view, and it stays head-tracked");

	/* the head turns 30 degrees left and pitches 10 up over a third of a
	second, with the stick held part way over */
	passed = 1;
	for (frame = 1; frame <= 30; frame++) {
		f = loop(&facing_yaw, &facing_pitch, 40.0f + frame, -15.0f + frame / 3.0f, 0.5f);
		worst_facing = fmaxf(worst_facing, fmaxf(fabsf(facing_yaw - entry_yaw), fabsf(facing_pitch - entry_pitch)));
		passed &= !f.look_turned && f.stick_yaw == 0.5f && f.stick_pitch == 0.3f;
	}
	printf("  the head 30 deg left, 10 up: the view %.4f, %.4f deg from the chase camera's; the facing moved %.6f deg\n",
		f.view_yaw - entry_yaw / DEGREES, f.view_pitch - entry_pitch / DEGREES, worst_facing / DEGREES);
	check(fabsf(f.view_yaw - entry_yaw / DEGREES - 30.0f) < 0.01f, "a 30 degree head yaw turns the eye cameras 30 degrees");
	check(fabsf(f.view_pitch - entry_pitch / DEGREES - 10.0f) < 0.01f, "a 10 degree head pitch pitches them 10 degrees");
	check(worst_facing == 0.0f && passed, "the facing never moves: the look takes no turn from the head");
	check(passed, "the stick keeps the game's own turn and pitch (it swings the boom)");
	check(halo_stereo_vignette() == 0.0f, "no comfort vignette for the stick's turn");
	check(!halo_stereo_head_drives_look(0), "the head doesn't drive the look (the autolevel is the game's)");
	/* the seat's gun aims along the game's camera (the facing): the
	crosshair goes where that points in the head-turned picture, 30 degrees
	right of the view's center and below it, at the angle between them */
	{
		float reticle[3], length, game[3], view[3], apart;

		halo_stereo_reticle(reticle);
		length = sqrtf(reticle[0] * reticle[0] + reticle[1] * reticle[1] + reticle[2] * reticle[2]);
		game[0] = cosf(facing_pitch) * cosf(facing_yaw);
		game[1] = cosf(facing_pitch) * sinf(facing_yaw);
		game[2] = sinf(facing_pitch);
		view[0] = cosf(f.view_pitch * DEGREES) * cosf(f.view_yaw * DEGREES);
		view[1] = cosf(f.view_pitch * DEGREES) * sinf(f.view_yaw * DEGREES);
		view[2] = sinf(f.view_pitch * DEGREES);
		apart = acosf(fminf(1.0f, game[0] * view[0] + game[1] * view[1] + game[2] * view[2])) / DEGREES;
		printf("  the seat's crosshair: %.4f %.4f %.4f in the eyes' frame, %.3f deg off center (the aim and the "
			"view %.3f deg apart)\n", reticle[0], reticle[1], reticle[2], acosf(-reticle[2] / length) / DEGREES,
			apart);
		check(fabsf(length - 1.0f) < 1e-4f && reticle[0] > 0.4f && reticle[1] < -0.1f &&
			fabsf(acosf(-reticle[2] / length) / DEGREES - apart) < 0.01f,
			"a seat's crosshair points where the gun aims: right of and below the head-turned view's center");
	}

	/* the stick swings the camera (the facing, which the chase camera
	follows): the head's turn stays on top of it */
	facing_yaw += 20.0f * DEGREES;
	f = loop(&facing_yaw, &facing_pitch, 70.0f, -5.0f, 0.0f);
	check(fabsf(f.view_yaw - facing_yaw / DEGREES - 30.0f) < 0.01f, "the head's turn rides on the stick's swing");

	/* back in first person with the head still (the game has glided its
	camera from the boom to the eyes, along the facing): the view's yaw
	doesn't move, the look takes the head's 30 degrees in the seat once, and
	the look's pitch is the head's own again */
	game_third_person = 0;
	seated_view_yaw = f.view_yaw;
	seated_facing_yaw = facing_yaw;
	f = loop(&facing_yaw, &facing_pitch, 70.0f, -5.0f, 0.0f);
	/* the camera's end: the eyes keep the seat's 30 degrees until the look
	gives them to the facing, so the crosshair stays on the aim, as a seat's */
	{
		float reticle[3], length, game[3], view[3], apart;

		halo_stereo_reticle(reticle);
		length = sqrtf(reticle[0] * reticle[0] + reticle[1] * reticle[1] + reticle[2] * reticle[2]);
		game[0] = cosf(facing_pitch) * cosf(facing_yaw);
		game[1] = cosf(facing_pitch) * sinf(facing_yaw);
		game[2] = sinf(facing_pitch);
		view[0] = cosf(f.view_pitch * DEGREES) * cosf(f.view_yaw * DEGREES);
		view[1] = cosf(f.view_pitch * DEGREES) * sinf(f.view_yaw * DEGREES);
		view[2] = sinf(f.view_pitch * DEGREES);
		apart = acosf(fminf(1.0f, game[0] * view[0] + game[1] * view[1] + game[2] * view[2])) / DEGREES;
		printf("  the camera's end: the crosshair %.4f %.4f %.4f, %.3f deg off center (the aim and the view %.3f deg "
			"apart, the yaw %.3f)\n", reticle[0], reticle[1], reticle[2], acosf(-reticle[2] / length) / DEGREES, apart,
			f.view_yaw - facing_yaw / DEGREES);
		check(fabsf(length - 1.0f) < 1e-4f && reticle[0] > 0.4f &&
			fabsf(acosf(-reticle[2] / length) / DEGREES - apart) < 0.01f &&
			fabsf(f.view_yaw - facing_yaw / DEGREES - 30.0f) < 0.01f,
			"the camera's end: the crosshair on the aim, 30 degrees right of the view's center, as a seat's");
	}
	printf("  first person again: the view %.4f deg from the seat's last\n", degrees_apart(f.view_yaw, seated_view_yaw));
	check(degrees_apart(f.view_yaw, seated_view_yaw) < 0.01f && fabsf(f.view_pitch + 5.0f) < 0.01f,
		"leaving: the view's yaw holds (the seat's head turn kept), the pitch the head's");
	f = loop(&facing_yaw, &facing_pitch, 72.0f, -5.0f, 0.0f);
	{
		float reticle[3];

		halo_stereo_reticle(reticle);
		check(reticle[0] == 0.0f && reticle[1] == 0.0f && reticle[2] == -1.0f,
			"on foot the crosshair is head-locked, straight ahead");
	}
	check(f.look_turned && fabsf(facing_pitch / DEGREES + 5.0f) < 0.01f &&
		degrees_apart(facing_yaw / DEGREES, seated_facing_yaw / DEGREES + 30.0f) < 0.01f &&
		degrees_apart(facing_yaw / DEGREES, f.view_yaw - 2.0f) < 0.01f,
		"the look takes the seat's 30 degrees once, then follows the head from there");
	f = loop(&facing_yaw, &facing_pitch, 72.0f, -5.0f, 0.0f);
	check(degrees_apart(facing_yaw / DEGREES, seated_facing_yaw / DEGREES + 32.0f) < 0.01f &&
		degrees_apart(f.view_yaw, seated_view_yaw + 2.0f) < 0.01f, "and not again");

	/* snaps: the stick held over in the seat doesn't snap on leaving it */
	restart("snap", 30.0, 120.0, 0);
	facing_yaw = facing_pitch = 0.0f;
	game_third_person = 1;
	/* (the stick's read before the frame begins: the seat's first frame
	still has the first person's look) */
	loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 0.0f);
	loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 1.0f);
	loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 1.0f);
	game_third_person = 0;
	for (frame = 0; frame < 10; frame++)
		loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 1.0f);
	snapped = facing_yaw != 0.0f;
	loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 0.0f);
	loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 1.0f);
	loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 1.0f);
	check(!snapped && fabsf(facing_yaw / DEGREES - 30.0f) < 0.01f,
		"a stick held over from the seat doesn't snap; a fresh flick does");

	/* display.stereo_vehicle_screen: the camera goes on the screen */
	vehicle_screen = 1;
	game_third_person = 1;
	halo_stereo_frame_begin();
	check(halo_stereo_film() && film_reason == 3 && halo_stereo_frame()->mode == HALO_STEREO_SCREEN,
		"display.stereo_vehicle_screen: the third-person camera is the film");
	vehicle_screen = 0;
	game_third_person = 0;
	for (frame = 0; frame < 2 * FILM_HOLD_FRAMES; frame++)
		halo_stereo_frame_begin();
}

/* a seat begun with the head moving on that frame, after the film, and
after the space reopens: the view on the seat's first frame is the head's
turn since the pose the facing has, which is the last frame's after
head-tracked first person and the head's now otherwise */
static void third_person_entries(void)
{
	float facing_yaw, facing_pitch;
	struct loop_frame f;
	int frame;

	printf("a third-person camera's first frame:\n");
	/* the head moving: the facing has the last frame's pose, so the view is
	turned by this frame's change */
	restart("snap", 30.0, 120.0, 0);
	game_third_person = 0;
	facing_yaw = facing_pitch = 0.0f;
	for (frame = 1; frame <= 20; frame++)
		loop(&facing_yaw, &facing_pitch, 2.0f * frame, -0.75f * frame, 0.0f);
	game_third_person = 1;
	f = loop(&facing_yaw, &facing_pitch, 42.0f, -14.0f, 0.0f);
	printf("  the head 2 deg left and 1 up on the seat's first frame: the view %.4f, %.4f deg from the facing\n",
		f.view_yaw - facing_yaw / DEGREES, f.view_pitch - facing_pitch / DEGREES);
	check(fabsf(facing_yaw / DEGREES - 40.0f) < 0.01f && fabsf(facing_pitch / DEGREES + 15.0f) < 0.01f &&
		fabsf(f.view_yaw - facing_yaw / DEGREES - 2.0f) < 0.01f && fabsf(f.view_pitch - facing_pitch / DEGREES - 1.0f) < 0.01f,
		"moving into the seat: the view moves by the frame's change only, no jump");
	game_third_person = 0;
	for (frame = 0; frame < 3; frame++)
		loop(&facing_yaw, &facing_pitch, 42.0f, -14.0f, 0.0f);

	/* after the film: a cutscene in which the head turns 50 degrees and
	looks down, then the seat once the film's hold ends */
	restart("snap", 30.0, 120.0, 0);
	facing_yaw = 10.0f * DEGREES;
	facing_pitch = 5.0f * DEGREES;
	game_letterbox = 1;
	for (frame = 0; frame <= 20; frame++)
		loop(&facing_yaw, &facing_pitch, 2.5f * frame, -1.0f * frame, 0.0f);
	game_letterbox = 0;
	game_third_person = 1;
	for (frame = 0; frame < PORTAL_FRAMES; frame++)
		loop(&facing_yaw, &facing_pitch, 50.0f, -20.0f, 0.0f);
	f = loop(&facing_yaw, &facing_pitch, 51.0f, -20.0f, 0.0f);
	printf("  the seat after the film: the view %.4f, %.4f deg from the chase camera's\n",
		f.view_yaw - facing_yaw / DEGREES, f.view_pitch - facing_pitch / DEGREES);
	check(!halo_stereo_film() && fabsf(facing_yaw / DEGREES - 10.0f) < 0.01f &&
		degrees_apart(f.view_yaw, facing_yaw / DEGREES) < 0.01f && fabsf(f.view_pitch - facing_pitch / DEGREES) < 0.01f,
		"after the film: the head's pose then is the reference, no jump");
	game_third_person = 0;
	for (frame = 0; frame < 3; frame++)
		loop(&facing_yaw, &facing_pitch, 51.0f, -20.0f, 0.0f);

	/* after the space reopens: closed while the head turned 60 degrees and
	looked down, reopened in the seat */
	restart("snap", 30.0, 120.0, 0);
	facing_yaw = facing_pitch = 0.0f;
	loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 0.0f);
	host_closed = 1;
	game_third_person = 1;
	for (frame = 0; frame <= 10; frame++)
		loop(&facing_yaw, &facing_pitch, 6.0f * frame, -2.0f * frame, 0.0f);
	host_closed = 0;
	f = loop(&facing_yaw, &facing_pitch, 61.0f, -20.0f, 0.0f);
	printf("  the seat after the space reopens: the view %.4f, %.4f deg from the chase camera's\n",
		f.view_yaw - facing_yaw / DEGREES, f.view_pitch - facing_pitch / DEGREES);
	check(halo_stereo_frame()->eye_count == 2 && degrees_apart(f.view_yaw, facing_yaw / DEGREES) < 0.01f &&
		fabsf(f.view_pitch - facing_pitch / DEGREES) < 0.01f,
		"after the space reopens: the head's pose then is the reference, no jump");
	f = loop(&facing_yaw, &facing_pitch, 71.0f, -10.0f, 0.0f);
	check(fabsf(f.view_yaw - facing_yaw / DEGREES - 10.0f) < 0.01f && fabsf(f.view_pitch - facing_pitch / DEGREES - 10.0f) < 0.01f,
		"then the head turns it from there");
	game_third_person = 0;
	for (frame = 0; frame < 3; frame++)
		loop(&facing_yaw, &facing_pitch, 71.0f, -10.0f, 0.0f);
}

/* paused: three frames with the head turning 5 degrees each and no look
between them (player_control's camera control is off while the game is
paused, so halo_stereo_head_look doesn't run), then the look again */
static void paused(void)
{
	float facing_yaw = 0.0f, facing_pitch = 0.0f, view_yaw, view_pitch, look_yaw, look_pitch, paused_view = 0.0f;
	struct loop_frame f;
	int frame, passed = 1;

	printf("paused (no look between frames):\n");
	restart("snap", 30.0, 120.0, 0);
	for (frame = 0; frame < 3; frame++)
		loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 0.0f);
	for (frame = 1; frame <= 3; frame++) {
		set_pose(5.0f * frame * DEGREES, 0.0f, 0.0f);
		halo_stereo_frame_begin();
		oriented(facing_yaw, facing_pitch, &view_yaw, &view_pitch);
		printf("  paused frame %d: the head %.1f deg, the eye cameras %.4f deg\n", frame, 5.0f * frame, view_yaw);
		passed &= fabsf(view_yaw - 5.0f * frame) < 0.01f;
		paused_view = view_yaw;
	}
	check(passed, "paused, the eye cameras turn with the head: 5, 10 and 15 degrees");
	check(halo_stereo_head_look(0, facing_pitch, &look_yaw, &look_pitch) &&
		fabsf(look_yaw / DEGREES - 15.0f) < 0.01f, "the look takes the paused frames' 15 degrees at once");
	facing_yaw += look_yaw;
	check(halo_stereo_head_look(0, facing_pitch, &look_yaw, &look_pitch) == 0 && look_yaw == 0.0f,
		"and clears them");
	set_pose(15.0f * DEGREES, 0.0f, 0.0f);
	halo_stereo_frame_begin();
	oriented(facing_yaw, facing_pitch, &view_yaw, &view_pitch);
	printf("  unpaused: the facing %.4f deg, the eye cameras %.4f deg (paused last at %.4f)\n",
		facing_yaw / DEGREES, view_yaw, paused_view);
	check(fabsf(view_yaw - facing_yaw / DEGREES) < 0.01f && fabsf(view_yaw - paused_view) < 0.01f,
		"the next frame's eye cameras match the look: unpausing doesn't jump the view");
	f = loop(&facing_yaw, &facing_pitch, 17.0f, 0.0f, 0.0f);
	check(fabsf(f.view_yaw - 17.0f) < 0.01f, "and the head turns it from there");

	/* the film between the pause and the look: its turns are dropped, as
	before (the film's hold ends on a head-tracked frame) */
	restart("snap", 30.0, 120.0, 0);
	facing_yaw = facing_pitch = 0.0f;
	loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 0.0f);
	set_pose(10.0f * DEGREES, 0.0f, 0.0f);
	halo_stereo_frame_begin();
	game_letterbox = 1;
	halo_stereo_frame_begin();
	game_letterbox = 0;
	for (frame = 0; frame < PORTAL_FRAMES + 1; frame++)
		halo_stereo_frame_begin();
	check(halo_stereo_head_look(0, facing_pitch, &look_yaw, &look_pitch) == 0 && look_yaw == 0.0f,
		"a film in between drops the paused turn");
}

/* An action replay owns base aim while the eyes still follow the live head. */
static void replay_look(void)
{
	float facing_yaw = 0.0f, facing_pitch = 0.0f, view_yaw, view_pitch, yaw, pitch;
	int frame;
	restart("snap", 30.0, 120.0, 1);
	for (frame = 0; frame < 3; frame++)
		loop(&facing_yaw, &facing_pitch, 0.0f, 0.0f, 0.0f);
	for (frame = 1; frame <= 3; frame++) {
		snap_pending = 0.5f;
		smooth_yaw = 0.2f;
		head_request = 0.1f;
		head_fold = 0.1f;
		vignette_strength = 0.8f;
		halo_stereo_replay_look(0, 1.0f);
		check(snap_pending == 0.0f && smooth_yaw == 0.0f && head_request == 0.0f &&
			head_fold == 0.0f && vignette_strength == 0.0f, "replay drops only suppressed turn transients");
		set_pose(5.0f * frame * DEGREES, 0.0f, 0.0f);
		halo_stereo_frame_begin();
		oriented(facing_yaw, facing_pitch, &view_yaw, &view_pitch);
		check(fabsf(view_yaw - 5.0f * frame) < 0.01f, "replay eyes follow accumulated live head yaw");
	}
	check(!snap_armed, "held replay stick cannot fire a snap on release");
	check(halo_stereo_head_look(0, facing_pitch, &yaw, &pitch) &&
		fabsf(yaw / DEGREES - 15.0f) < 0.01f, "releasing replay transfers pending head yaw once");
	facing_yaw += yaw;
	set_pose(15.0f * DEGREES, 0.0f, 0.0f);
	halo_stereo_frame_begin();
	oriented(facing_yaw, facing_pitch, &view_yaw, &view_pitch);
	check(fabsf(view_yaw - 15.0f) < 0.01f, "replay release preserves eye orientation");
	head_seat_leftover = 0.25f;
	halo_stereo_replay_look(0, 0.0f);
	check(head_seat_leftover == 0.25f && snap_armed, "replay preserves seat residual and neutral input rearms snap");
	head_seat_leftover = 0.0f;
}

/* the game's camera in the render with interpolation (render_interpolation.c):
the facing is posed into the camera every frame (first_person_camera_update),
kept after each tick with the head yaw the look had taken
(halo_stereo_head_yaw_taken), and the frame draws the last two kept, blended
by how far the clock is into the next tick (nlerp of the forward). The
earlier one is first turned about the world's up by the head yaw the later
holds and it doesn't, so the blend holds the later one's head yaw exactly
(render_interpolation_blended_camera), and the render is told so
(halo_stereo_camera_head_yaw) */
struct ticked_camera
{
	float previous[3], latest[3];
	float previous_head, latest_head;
	/* posed from the facing (the director's settled first person), not a
	glide or another camera (camera_facing_posed) */
	int previous_posed, latest_posed;
	int kept;
};

/* a tick's camera: facing a yaw, posed from the facing or not */
static void camera_keep_posed(struct ticked_camera *camera, float yaw, int posed)
{
	memcpy(camera->previous, camera->latest, sizeof(camera->latest));
	camera->previous_head = camera->latest_head;
	camera->previous_posed = camera->latest_posed;
	camera->latest[0] = cosf(yaw);
	camera->latest[1] = sinf(yaw);
	camera->latest[2] = 0.0f;
	camera->latest_head = halo_stereo_head_yaw_taken();
	camera->latest_posed = posed;
	if (!camera->kept++) {
		memcpy(camera->previous, camera->latest, sizeof(camera->latest));
		camera->previous_head = camera->latest_head;
		camera->previous_posed = posed;
	}
}

static void camera_keep(struct ticked_camera *camera, float facing_yaw)
{
	camera_keep_posed(camera, facing_yaw, 1);
}

/* the blend's yaw as render_interpolation.c makes it: each camera posed from
the facing turned by the head yaw the look took since it was kept, so it
holds the head yaw as of now (a glide stays as it is), and what it holds
reported as the look's now */
static float camera_blended_yaw(const struct ticked_camera *camera, float t)
{
	float previous[3], latest[3], blended[3];
	float taken = halo_stereo_head_yaw_taken();
	int i;

	memcpy(previous, camera->previous, sizeof(previous));
	memcpy(latest, camera->latest, sizeof(latest));
	if (camera->previous_posed)
		rotate_axis(previous, 2, remainderf(taken - camera->previous_head, TWO_PI));
	if (camera->latest_posed)
		rotate_axis(latest, 2, remainderf(taken - camera->latest_head, TWO_PI));
	for (i = 0; i < 3; i++)
		blended[i] = previous[i] + (latest[i] - previous[i]) * t;
	halo_stereo_camera_head_yaw(taken, t, camera->previous_posed && camera->latest_posed ? "blended" : "edge");
	return atan2f(blended[1], blended[0]);
}

/* a steady head turn at 90 frames a second while the game ticks at 30 (three
frames a tick, the blend 0, 1/3, 2/3), with the body turned by the stick too
(smooth turning) in one case: the eye cameras' yaw must be the head's plus the
body's own, which the game's camera shows blended a tick behind, every frame */
static void blended_turn(const char *name, float head_degrees_per_frame, float stick, int frames_per_tick)
{
	struct ticked_camera camera = { { 0 }, { 0 }, 0.0f, 0.0f, 0 };
	float facing_yaw = 0.0f, facing_pitch = 0.0f, look_yaw, look_pitch, head_yaw = 0.0f, worst = 0.0f;
	/* the body's own yaw (the facing less the head's turn the look took) at
	each tick kept, the first at the start */
	float body[64] = { 0.0f };
	int frame, ticks = 1;

	restart("smooth", 30.0, 120.0, 0);
	camera_keep(&camera, facing_yaw);
	for (frame = 1; frame <= 60; frame++) {
		float yaw_in = stick, pitch_in = 0.3f, t, view, expected, error, earlier, later;

		/* player_control: the stick, then the look */
		halo_stereo_stick_look(0, response(stick), 1.0f / 90.0f, &yaw_in, &pitch_in);
		if (halo_stereo_head_look(0, facing_pitch, &look_yaw, &look_pitch))
			facing_yaw += look_yaw;
		/* the tick (game_time_update), then the frame, the blend that far
		into the next tick */
		if (frame % frames_per_tick == 0) {
			camera_keep(&camera, facing_yaw);
			body[ticks % 64] = remainderf(facing_yaw - halo_stereo_head_yaw_taken(), TWO_PI);
			ticks++;
		}
		t = (float)(frame % frames_per_tick) / (float)frames_per_tick;
		head_yaw += head_degrees_per_frame * DEGREES;
		set_pose(head_yaw, 0.0f, 0.0f);
		halo_stereo_frame_begin();
		oriented(camera_blended_yaw(&camera, t), 0.0f, &view, &look_pitch);
		earlier = body[(ticks >= 2 ? ticks - 2 : 0) % 64];
		later = body[(ticks - 1) % 64];
		expected = (head_yaw + earlier + remainderf(later - earlier, TWO_PI) * t) / DEGREES;
		error = degrees_apart(view, expected);
		if (error > worst)
			worst = error;
	}
	printf("  %-44s the eye cameras %.4f deg at worst from the head's yaw plus the body's\n", name, worst);
	check(worst < 0.02f, name);
}

/* leaving a seat: the director's glide toward the eyes, a camera not posed
from the facing, over 6 ticks at 3 frames a tick, between first-person
cameras posed from it; the glide starts at the facing's pose then and
closes on the facing's pose as it moves (as the observer chases its
command), reaching it on its last tick. During the glide the eye cameras are
the glide's blend turned by only the head yaw the look hasn't taken (its
turn since the last frame): no step at a tick from head yaw the glide never
had. And at its edges, with the head turning or still, the body's yaw (the
view's less the head's) moves without a step: no frame turns it by more
than a frame of the head's turn plus a frame of the glide's own */
static void glide(void)
{
	struct ticked_camera camera;
	float facing_yaw, facing_pitch = 0.0f, look_yaw, look_pitch, head_yaw, glide_from = 0.0f;
	float worst_glide = 0.0f, worst_step[2] = { 0.0f, 0.0f }, last_body = 0.0f;
	int frame, still;

	printf("a glide between cameras (the director's, leaving a seat), the game's camera blended between ticks:\n");
	for (still = 0; still < 2; still++) {
		float head_step = still ? 0.0f : 1.0f * DEGREES;

		restart("snap", 30.0, 120.0, 0);
		memset(&camera, 0, sizeof(camera));
		facing_yaw = head_yaw = 0.0f;
		camera_keep(&camera, facing_yaw);
		/* 6 ticks posed, 6 gliding, 6 posed again */
		for (frame = 1; frame <= 54; frame++) {
			int tick = frame % 3 == 0, tick_index = frame / 3;
			float t = (float)(frame % 3) / 3.0f, camera_yaw, view, view_pitch, pending, body;

			if (halo_stereo_head_look(0, facing_pitch, &look_yaw, &look_pitch))
				facing_yaw += look_yaw;
			if (tick) {
				if (tick_index == 6)
					glide_from = facing_yaw;
				if (tick_index >= 6 && tick_index < 12)
					camera_keep_posed(&camera, glide_from + (facing_yaw - glide_from) * (float)(tick_index - 5) / 6.0f,
						0);
				else
					camera_keep(&camera, facing_yaw);
			}
			head_yaw += head_step;
			set_pose(head_yaw, 0.0f, 0.0f);
			halo_stereo_frame_begin();
			pending = head_pending_yaw;
			camera_yaw = camera_blended_yaw(&camera, t);
			oriented(camera_yaw, 0.0f, &view, &view_pitch);
			/* both cameras the glide's: its own yaw plus the turn not yet
			taken, nothing more */
			if (!camera.previous_posed && !camera.latest_posed)
				worst_glide = fmaxf(worst_glide, degrees_apart(view, (camera_yaw + pending) / DEGREES));
			/* the body's yaw from frame to frame, past the start */
			body = view - head_yaw / DEGREES;
			if (frame > 1)
				worst_step[still] = fmaxf(worst_step[still], fabsf(remainderf(body - last_body, 360.0f)));
			last_body = body;
		}
	}
	printf("  gliding with the head turning: the eye cameras %.4f deg at worst from the glide's yaw plus the turn "
		"not yet taken\n", worst_glide);
	check(worst_glide < 0.01f, "a glide gets no head yaw it never had: no step at its ticks");
	/* a frame's turn of the head (1 degree) and of the glide's own (its 6
	ticks close on the facing, which the head turns 3 degrees a tick: at
	most about 1 degree a frame) */
	printf("  into the glide and out of it, the body's yaw steps %.4f deg at worst in a frame with the head turning "
		"1 deg a frame, %.4f with it still\n", worst_step[0], worst_step[1]);
	check(worst_step[0] <= 2.05f && worst_step[1] < 0.01f, "into a glide and out of it, no hitch, turning or still");
}

/* a first-person seat's yaw limit (player_control_modify_desired_angles,
halo_stereo_seat_yaw_clamp): the seat's marker and its bounds off it */
static float seat_marker, seat_minimum = -45.0f * DEGREES, seat_maximum = 45.0f * DEGREES;
static int seated;
/* a second turn in the frame after the look's (a damage effect's camera
impulse, player_control_permanent_impulse), in radians; applied once */
static float seat_impulse;
static int seat_impulse_due;

/* the game's yaw through a turn: the turn added, then the seat's clamp
(stereo's, or the game's own nearest-bound rule when it declines) while
seated, then the game's wrap to 0..2 pi */
static void seat_modify(float *facing_yaw, float delta_yaw)
{
	float before = *facing_yaw;

	*facing_yaw += delta_yaw;
	if (seated && !halo_stereo_seat_yaw_clamp(0, facing_yaw, before, delta_yaw, seat_marker, seat_minimum,
		seat_maximum))
		*facing_yaw = seat_marker + seat_nearest_bound(remainderf(*facing_yaw - seat_marker, TWO_PI), seat_minimum,
			seat_maximum);
	while (*facing_yaw < 0.0f)
		*facing_yaw += TWO_PI;
	while (*facing_yaw > TWO_PI)
		*facing_yaw -= TWO_PI;
}

/* one frame of the seat at 90 Hz, three frames a tick: the stick (a snap
when stick is past the flick), the look, the seat's clamp, the tick's camera
(posed, or the exit glide's), the head's pose (degrees), the frame, the
camera's report (halo_stereo_camera_posed) and the eye cameras' yaw
(degrees). Returns the look's yaw (degrees) */
struct seat_run
{
	struct ticked_camera camera;
	float facing_yaw, view, body;
	int frame;
	/* the exit glide: from the seat camera's yaw, its ticks so far (0: none,
	-1: over) */
	float glide_from;
	int glide_tick;
	/* a vehicle's chase camera: each tick's camera kept at the facing, not
	posed from it (a third-person camera); and the direct camera
	(display.direct_camera, on foot): the frame's camera is the facing as of
	now, reported as holding it */
	int chase, direct;
};

static float seat_step(struct seat_run *run, float head_degrees, float stick)
{
	float yaw_in = stick, pitch_in = 0.0f, look_yaw = 0.0f, look_pitch = 0.0f, t, view_pitch;

	halo_stereo_stick_look(0, response(stick), 1.0f / 90.0f, &yaw_in, &pitch_in);
	halo_stereo_head_look(0, 0.0f, &look_yaw, &look_pitch);
	/* (the game takes no turn while the director holds the facing) */
	if (!game_inhibited_facing)
		seat_modify(&run->facing_yaw, look_yaw);
	if (seat_impulse_due) {
		seat_modify(&run->facing_yaw, seat_impulse);
		seat_impulse_due = 0;
	}
	run->frame++;
	if (run->frame % 3 == 0) {
		if (run->glide_tick > 0 && run->glide_tick <= 6) {
			camera_keep_posed(&run->camera, run->glide_from +
				remainderf(run->facing_yaw - run->glide_from, TWO_PI) * (float)run->glide_tick / 6.0f, 0);
			run->glide_tick = run->glide_tick < 6 ? run->glide_tick + 1 : -1;
		} else
			camera_keep_posed(&run->camera, run->facing_yaw, !run->chase);
	}
	t = (float)(run->frame % 3) / 3.0f;
	/* the observer settles once the glide's last camera has gone by */
	if (run->glide_tick < 0 && run->camera.latest_posed && run->camera.previous_posed)
		game_settled = 1;
	set_pose(head_degrees * DEGREES, 0.0f, 0.0f);
	halo_stereo_frame_begin();
	/* (the game's rule: a blend holds the facing only with both its cameras
	posed; the direct camera always does) */
	halo_stereo_camera_posed(run->direct || (run->camera.latest_posed && run->camera.previous_posed));
	if (run->direct) {
		halo_stereo_camera_head_yaw(halo_stereo_head_yaw_taken(), t, "direct");
		oriented(run->facing_yaw, 0.0f, &run->view, &view_pitch);
	} else
		oriented(camera_blended_yaw(&run->camera, t), 0.0f, &run->view, &view_pitch);
	run->body = remainderf(run->view - head_degrees, 360.0f);
	return look_yaw / DEGREES;
}

static void seat_start(struct seat_run *run, const char *turn)
{
	restart(turn, 30.0, 120.0, 0);
	memset(run, 0, sizeof(*run));
	seat_marker = 0.0f;
	seated = 1;
	game_settled = 1;
	game_inhibited_facing = 0;
	seat_impulse_due = 0;
	camera_keep(&run->camera, run->facing_yaw);
}

/* the head turned from where it is to a yaw at 1 degree a frame, then held
for a few frames, the camera settled; the worst change of the body's yaw
from body on the way */
static float seat_turn_to(struct seat_run *run, float *head, float to, float body)
{
	float worst = 0.0f;

	while (fabsf(to - *head) > 0.5f) {
		*head += to > *head ? 1.0f : -1.0f;
		seat_step(run, *head, 0.0f);
		worst = fmaxf(worst, fabsf(remainderf(run->body - body, 360.0f)));
	}
	*head = to;
	return worst;
}

static void seat_hold(struct seat_run *run, float head, int frames)
{
	while (frames-- > 0)
		seat_step(run, head, 0.0f);
}

/* the logged left (debug.head_yaw_log's line, degrees), and its pending */
static int logged_left(float *left, float *pending)
{
	const char *at = strstr(last_log, "pending ");

	return strstr(last_log, "stereo: head yaw:") && at && sscanf(at, "pending %f, left %f", pending, left) == 2;
}

static void seat_exit(float past, float *worst_step, int *cut_right, int *cut_seen, float *first_look)
{
	struct seat_run run;
	float head = 0.0f, last_body;
	int frame;

	seat_start(&run, "snap");
	seat_turn_to(&run, &head, 45.0f + past, 0.0f);
	seat_hold(&run, head, 9);
	/* the seat ends on a tick: the next frame's tick is the glide's first */
	while ((run.frame + 1) % 3 != 0)
		seat_step(&run, head, 0.0f);
	seated = 0;
	game_settled = 0;
	run.glide_from = run.facing_yaw;
	run.glide_tick = 1;
	last_body = run.body;
	*worst_step = 0.0f;
	*cut_right = 1;
	*cut_seen = 0;
	for (frame = 0; frame < 36; frame++) {
		float look = seat_step(&run, head, 0.0f);

		if (frame == 0)
			*first_look = look;
		*worst_step = fmaxf(*worst_step, fabsf(remainderf(run.body - last_body, 360.0f)));
		last_body = run.body;
		*cut_seen |= halo_stereo_cut_requested();
		if (halo_stereo_cut_requested() != !game_settled)
			*cut_right = 0;
	}
	game_settled = 1;
	seated = 0;
}

/* a first-person seat with a yaw limit (a10's pod, b30's Warthog passenger):
the seat's marker at 0 with bounds of 45 degrees either way, the game's
camera kept per tick (90 Hz, three frames a tick). Past a bound the aim
stops at it while the eye cameras follow the head exactly: the world holds
still in the room, and what the bound refused (the leftover) comes back as
the head returns. A snap at the bound is dropped as in mono, the seat's own
turn drags the aim at the bound, the crosshair follows the aim, the
leftover is held to 180 degrees with its flip past the far side, and
leaving the seat folds it into the facing, a glide's step covered by the
cut, a third-person camera's by its turn */
static void seat_yaw_limit(void)
{
	struct seat_run run;
	float head = 0.0f, worst_body = 0.0f, worst_facing = 0.0f, worst_left = 0.0f, worst_logged = 0.0f;
	float body0, facing0, view0, left0, flip_head = 0.0f, worst_step, first_look;
	float direction[3], length;
	int passed_logged = 1, logged = 0, cut_right, cut_seen, frame;

	printf("a first-person seat's yaw limit (45 deg either way), the game's camera blended between ticks:\n");
	seat_start(&run, "snap");
	head_yaw_log = 1;
	seat_hold(&run, head, 3);
	body0 = run.body;
	/* the sweep: 0 to +70 to -70 to 0 at 1 deg a frame */
	{
		static const float stops[3] = { 70.0f, -70.0f, 0.0f };
		int stop;

		for (stop = 0; stop < 3; stop++) {
			while (fabsf(stops[stop] - head) > 0.5f) {
				float taken_head = head, left, pending;

				head += stops[stop] > head ? 1.0f : -1.0f;
				seat_step(&run, head, 0.0f);
				worst_body = fmaxf(worst_body, fabsf(remainderf(run.body - body0, 360.0f)));
				/* the head the look took this frame (the last frame's) past
				the bound: the aim at it, the leftover the rest */
				if (taken_head >= 45.0f) {
					worst_facing = fmaxf(worst_facing, degrees_apart(run.facing_yaw / DEGREES, 45.0f));
					worst_left = fmaxf(worst_left, fabsf(head_seat_leftover / DEGREES - (taken_head - 45.0f)));
					if (logged_left(&left, &pending)) {
						logged++;
						worst_logged = fmaxf(worst_logged, fabsf(left - (head - pending - 45.0f)));
					} else
						passed_logged = 0;
				}
			}
		}
	}
	printf("  the sweep: the body's yaw %.4f deg at worst from its start; past +45 deg the facing %.4f deg at worst "
		"from 45, the leftover %.4f from the head's less 45, the logged left %.4f (%d lines)\n", worst_body,
		worst_facing, worst_left, worst_logged, logged);
	check(worst_body < 0.01f, "past the limit and back, the body's yaw holds: the world never moves");
	check(worst_facing < 0.01f && worst_left < 0.01f, "past the bound the aim stays at 45 deg, the leftover the rest");
	check(passed_logged && logged > 0 && worst_logged < 0.01f,
		"the logged left is the head's yaw less 45 (less the frame's pending turn)");
	seat_hold(&run, head, 6);
	printf("  after the sweep: the facing %.4f deg, the leftover %.4f, the body %.4f (%.4f before)\n",
		remainderf(run.facing_yaw / DEGREES, 360.0f), head_seat_leftover / DEGREES, run.body, body0);
	check(degrees_apart(run.facing_yaw / DEGREES, head) < 0.01f && head_seat_leftover == 0.0f &&
		fabsf(remainderf(run.body - body0, 360.0f)) < 0.01f,
		"after the sweep the facing is the head's and the body's yaw is what it was");
	halo_stereo_reticle(direction);
	check(fabsf(direction[0]) < 1e-4f && fabsf(direction[1]) < 1e-4f && direction[2] < 0.0f,
		"no leftover: the crosshair straight ahead, as on foot");
	head_yaw_log = 0;

	/* snaps with the head 20 deg past the bound */
	seat_turn_to(&run, &head, 65.0f, 0.0f);
	seat_hold(&run, head, 9);
	halo_stereo_reticle(direction);
	length = sqrtf(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
	printf("  20 deg of leftover: the crosshair at (%.4f, %.4f, %.4f)\n", direction[0] / length,
		direction[1] / length, direction[2] / length);
	check(fabsf(direction[0] / length - sinf(20.0f * DEGREES)) < 1e-3f && fabsf(direction[1] / length) < 1e-3f &&
		fabsf(direction[2] / length + cosf(20.0f * DEGREES)) < 1e-3f,
		"20 deg of leftover: the crosshair on the aim, 20 deg right of the head's forward");
	facing0 = run.facing_yaw;
	view0 = run.view;
	left0 = head_seat_leftover;
	seat_step(&run, head, 1.0f);
	seat_hold(&run, head, 9);
	printf("  a +30 deg snap at the bound: the facing %.4f deg, the eyes %.4f (were %.4f, %.4f)\n",
		run.facing_yaw / DEGREES, run.view, facing0 / DEGREES, view0);
	check(degrees_apart(run.facing_yaw / DEGREES, facing0 / DEGREES) < 0.01f && degrees_apart(run.view, view0) < 0.01f &&
		fabsf(head_seat_leftover - left0) < 1e-4f, "a snap past the bound is dropped: the facing and the eyes stay");
	seat_step(&run, head, -1.0f);
	printf("  a -30 deg snap: the facing %.4f deg on the snap's frame\n", remainderf(run.facing_yaw / DEGREES, 360.0f));
	check(degrees_apart(run.facing_yaw / DEGREES, 15.0f) < 0.01f, "a snap back inside the bounds: the facing at 15 deg");
	seat_hold(&run, head, 9);
	printf("  then the eyes %.4f deg (were %.4f), the facing %.4f, the leftover %.4f\n", run.view, view0,
		remainderf(run.facing_yaw / DEGREES, 360.0f), head_seat_leftover / DEGREES);
	check(degrees_apart(run.view, view0 - 30.0f) < 0.01f, "and the eyes turn by -30 deg, as the snap turns the body");
	check(degrees_apart(run.facing_yaw / DEGREES, 35.0f) < 0.01f && head_seat_leftover == 0.0f,
		"then the aim takes the leftover inside the bounds: the facing at the eyes, 35 deg");

	/* the seat's own turn under a still head 20 deg past the bound */
	seat_start(&run, "snap");
	head = 0.0f;
	seat_turn_to(&run, &head, 65.0f, 0.0f);
	seat_hold(&run, head, 9);
	facing0 = run.facing_yaw;
	view0 = run.view;
	left0 = head_seat_leftover;
	seat_marker = -10.0f * DEGREES;
	seat_hold(&run, head, 9);
	printf("  the marker turning -10 deg with the head 20 deg past: the facing %.4f deg, the eyes %.4f, the leftover "
		"%.4f (were %.4f, %.4f, %.4f)\n", run.facing_yaw / DEGREES, run.view, head_seat_leftover / DEGREES,
		facing0 / DEGREES, view0, left0 / DEGREES);
	check(degrees_apart(run.facing_yaw / DEGREES, facing0 / DEGREES - 10.0f) < 0.01f &&
		degrees_apart(run.view, view0 - 10.0f) < 0.01f && fabsf(head_seat_leftover - left0) < 1e-4f,
		"at the bound the seat's turn drags the aim and the eyes by it, as in mono; the leftover stays");
	seat_marker = 0.0f;
	seat_hold(&run, head, 9);
	printf("  the marker back to 0 (toward the head): the facing %.4f deg, the eyes %.4f, the leftover %.4f\n",
		run.facing_yaw / DEGREES, run.view, head_seat_leftover / DEGREES);
	check(degrees_apart(run.facing_yaw / DEGREES, facing0 / DEGREES) < 0.01f &&
		degrees_apart(run.view, view0 - 10.0f) < 0.01f && fabsf(head_seat_leftover / DEGREES - 10.0f) < 0.01f,
		"turning toward the head it frees the aim, which follows the bound; the eyes stay");
	seat_start(&run, "snap");
	head = 0.0f;
	seat_turn_to(&run, &head, 20.0f, 0.0f);
	seat_hold(&run, head, 9);
	facing0 = run.facing_yaw;
	view0 = run.view;
	seat_marker = 10.0f * DEGREES;
	seat_hold(&run, head, 9);
	seat_marker = -10.0f * DEGREES;
	seat_hold(&run, head, 9);
	check(degrees_apart(run.facing_yaw / DEGREES, facing0 / DEGREES) < 0.01f && degrees_apart(run.view, view0) < 0.01f &&
		head_seat_leftover == 0.0f, "inside the bounds the marker's turn moves nothing");

	/* far past: the leftover held to 180 deg */
	seat_start(&run, "snap");
	head = 0.0f;
	body0 = run.body;
	worst_body = seat_turn_to(&run, &head, 220.0f, body0);
	printf("  the head at +220 deg: the facing %.4f deg, the leftover %.4f\n", run.facing_yaw / DEGREES,
		head_seat_leftover / DEGREES);
	check(degrees_apart(run.facing_yaw / DEGREES, 45.0f) < 0.01f, "the head 220 deg round: the aim still at +45 deg");
	for (flip_head = 0.0f; head < 230.0f - 0.5f;) {
		head += 1.0f;
		seat_step(&run, head, 0.0f);
		worst_body = fmaxf(worst_body, fabsf(remainderf(run.body - body0, 360.0f)));
		if (flip_head == 0.0f && degrees_apart(run.facing_yaw / DEGREES, -45.0f) < 0.01f)
			flip_head = head;
	}
	printf("  the aim flips to -45 deg with the head at %.1f deg\n", flip_head);
	check(flip_head > 225.0f && flip_head <= 228.0f && degrees_apart(run.facing_yaw / DEGREES, -45.0f) < 0.01f,
		"past 225 deg the aim moves to the other bound, -45 deg, within a tick");
	worst_body = fmaxf(worst_body, seat_turn_to(&run, &head, 140.0f, body0));
	check(degrees_apart(run.facing_yaw / DEGREES, -45.0f) < 0.01f, "back to 140 deg: the aim still at -45 deg");
	for (flip_head = 0.0f; head > 130.0f + 0.5f;) {
		head -= 1.0f;
		seat_step(&run, head, 0.0f);
		worst_body = fmaxf(worst_body, fabsf(remainderf(run.body - body0, 360.0f)));
		if (flip_head == 0.0f && degrees_apart(run.facing_yaw / DEGREES, 45.0f) < 0.01f)
			flip_head = head;
	}
	printf("  the aim flips back to +45 deg with the head at %.1f deg; the body's yaw %.4f deg at worst throughout\n",
		flip_head, worst_body);
	check(flip_head < 135.0f && flip_head >= 132.0f && degrees_apart(run.facing_yaw / DEGREES, 45.0f) < 0.01f,
		"below 135 deg it comes back to +45 deg within a tick");
	check(worst_body < 0.02f, "far past the bound and through both flips the body's yaw holds");

	/* leaving the seat into the director's glide */
	seat_exit(20.0f, &worst_step, &cut_right, &cut_seen, &first_look);
	printf("  leaving with 20 deg of leftover: the first look %.4f deg; through the glide the body steps %.4f deg at "
		"worst in a frame (under the cut)\n", first_look, worst_step);
	check(fabsf(first_look - 20.0f) < 0.01f, "leaving the seat, the next look takes the 20 deg leftover");
	check(cut_seen && cut_right, "the cut holds from the fold until the camera settles, and ends then");
	seat_exit(1.0f, &worst_step, &cut_right, &cut_seen, &first_look);
	printf("  leaving with 1 deg of leftover: the body steps %.4f deg at worst in a frame (uncovered)\n", worst_step);
	check(!cut_seen && fabsf(first_look - 1.0f) < 0.01f, "1 deg of leftover: no cut");

	/* a third-person camera beginning with a leftover */
	seat_start(&run, "snap");
	head = 0.0f;
	seat_turn_to(&run, &head, 65.0f, 0.0f);
	seat_hold(&run, head, 9);
	view0 = run.view;
	game_third_person = 1;
	seat_step(&run, head, 0.0f);
	printf("  a third-person camera with 20 deg of leftover: the view %.4f deg (was %.4f)\n", run.view, view0);
	check(degrees_apart(run.view, view0) < 0.01f, "a third-person camera begins where the view was: no jump");
	game_third_person = 0;
	for (frame = 0; frame < 3; frame++)
		seat_step(&run, head, 0.0f);
	seated = 0;
}

/* the leftover kept where the game takes no head yaw: a second turn in the
frame (a damage effect's camera impulse, after the look's), frames with the
director holding the facing (a seat's exit animation) and the first frame on
foot after them; and the zoom, which looks along the gun while the leftover
holds the aim back */
static void seat_leftover_kept(void)
{
	struct seat_run run;
	float head = 0.0f, body0, facing0, worst = 0.0f, aim_yaw, zoom_yaw, forward[3], up[3];
	int frame, kept = 1;

	printf("a seat's leftover kept through other turns and held facing:\n");
	seat_start(&run, "snap");
	seat_turn_to(&run, &head, 65.0f, 0.0f);
	seat_hold(&run, head, 9);
	body0 = run.body;
	facing0 = run.facing_yaw;
	/* camera impulses: into the bound (refused), and none at all */
	for (frame = 0; frame < 12; frame++) {
		seat_impulse = frame < 6 ? 2.0f * DEGREES : 0.0f;
		seat_impulse_due = frame % 3 == 0;
		seat_step(&run, head, 0.0f);
		worst = fmaxf(worst, fabsf(remainderf(run.body - body0, 360.0f)));
		kept &= fabsf(head_seat_leftover / DEGREES - 20.0f) < 0.01f;
	}
	printf("  second turns in the frame: the leftover %.4f deg, the facing %.4f, the body %.4f deg at worst from "
		"before\n", head_seat_leftover / DEGREES, run.facing_yaw / DEGREES, worst);
	check(kept && worst < 0.01f && degrees_apart(run.facing_yaw / DEGREES, facing0 / DEGREES) < 0.01f,
		"a camera impulse after the look's turn keeps the leftover: the eyes and the aim stay");
	/* the zoom: along the aim while the leftover holds it back */
	aim_yaw = 45.0f * DEGREES;
	forward[0] = cosf(aim_yaw); forward[1] = sinf(aim_yaw); forward[2] = 0.0f;
	up[0] = 0.0f; up[1] = 0.0f; up[2] = 1.0f;
	halo_stereo_zoom_orient(forward, up);
	zoom_yaw = atan2f(forward[1], forward[0]) / DEGREES;
	printf("  zoomed with 20 deg of leftover: the zoom's yaw %.4f deg (the aim 45, the eyes %.4f)\n", zoom_yaw,
		run.view);
	check(degrees_apart(zoom_yaw, 45.0f) < 0.01f, "with a leftover the zoom looks along the gun's aim");
	/* the director holding the facing for 30 frames, then on foot */
	worst = 0.0f;
	kept = 1;
	game_inhibited_facing = 1;
	for (frame = 0; frame < 30; frame++) {
		seat_step(&run, head, 0.0f);
		worst = fmaxf(worst, fabsf(remainderf(run.body - body0, 360.0f)));
		kept &= fabsf(head_seat_leftover / DEGREES - 20.0f) < 0.01f;
	}
	game_inhibited_facing = 0;
	printf("  30 frames with the facing held: the leftover %.4f deg, the body %.4f deg at worst from before\n",
		head_seat_leftover / DEGREES, worst);
	check(kept && worst < 0.01f, "while the director holds the facing the leftover stays and the world holds");
	seated = 0;
	for (frame = 0; frame < 12; frame++) {
		seat_step(&run, head, 0.0f);
		worst = fmaxf(worst, fabsf(remainderf(run.body - body0, 360.0f)));
	}
	printf("  then on foot: the facing %.4f deg, the leftover %.4f, the body %.4f deg at worst from before\n",
		remainderf(run.facing_yaw / DEGREES, 360.0f), head_seat_leftover / DEGREES, worst);
	check(degrees_apart(run.facing_yaw / DEGREES, 65.0f) < 0.01f && head_seat_leftover == 0.0f && worst < 0.01f,
		"the first look the game takes folds the leftover in: the aim at the eyes, the world still");
	forward[0] = cosf(run.facing_yaw); forward[1] = sinf(run.facing_yaw); forward[2] = 0.0f;
	up[0] = 0.0f; up[1] = 0.0f; up[2] = 1.0f;
	halo_stereo_zoom_orient(forward, up);
	zoom_yaw = atan2f(forward[1], forward[0]) / DEGREES;
	check(degrees_apart(zoom_yaw, run.view) < 0.01f, "without one the zoom follows the eyes, as on foot");
	seated = 0;
}

/* one vehicle camera's end, entering or leaving a seat: a third-person camera
(the chase camera, kept at the facing and not posed from it) through which
the head turns rate degrees a frame for 30 frames, then the camera's end,
the glide to the eyes or the seat's camera (six ticks not posed from the
facing, then posed ticks), or on foot the direct camera; still_turning keeps
the head turning 1 degree a frame through the end and the fold. bounded: the
seat at the end has the 45 degree limit. The world must never move: the
body's yaw steps by no more than the head's turn that frame (and the glide's
own closing on a facing the head turns, 2.05 degrees, glide's bound); the
eyes keep the head's turn until the camera holds the facing, then the look
asks for it once */
static void third_person_end(const char *name, int exiting, float rate, int still_turning, int bounded, int direct)
{
	struct seat_run run;
	float head = 0.0f, from_head, to_head, last_body, last_head, worst = 0.0f, worst_glide = 0.0f, kept = 0.0f;
	float asked = 0.0f, facing0, body0, left, pending, turn = still_turning ? 1.0f : 0.0f;
	int frame, frames = 0, worst_glide_frame = -1, cut = 0, steady, at_bound, glide, was_glide = 0;
	char check_name[192];

	printf("  %s:\n", name);
	seat_start(&run, "snap");
	seated = exiting;
	head_yaw_log = 1;
	eyes_keep_log[0] = look_asks_log[0] = '\0';
	seat_hold(&run, head, 3);
	last_body = run.body;
	last_head = head;
	from_head = head;
	game_third_person = 1;
	run.chase = 1;
	for (frame = 0; frame < 30 + 9 || (run.frame + 1) % 3 != 0; frame++) {
		head += frame < 30 ? rate : turn;
		seat_step(&run, head, 0.0f);
		worst = fmaxf(worst, fabsf(remainderf(run.body - last_body, 360.0f)) - fabsf(head - last_head));
		cut |= halo_stereo_cut_requested();
		last_body = run.body;
		last_head = head;
	}
	to_head = head;
	/* the camera's end: the next frame is first person, its tick the
	glide's first (or the direct camera's) */
	game_third_person = 0;
	run.chase = 0;
	seated = exiting ? 0 : bounded;
	if (direct)
		run.direct = 1;
	else {
		game_settled = 0;
		run.glide_from = run.facing_yaw;
		run.glide_tick = 1;
	}
	for (frame = 0; frame < 45; frame++) {
		float step;

		head += turn;
		seat_step(&run, head, 0.0f);
		step = fabsf(remainderf(run.body - last_body, 360.0f));
		/* (a step from the glide's last frame into a held camera is the
		glide's too) */
		glide = !direct && !(run.camera.latest_posed && run.camera.previous_posed);
		if (still_turning && (glide || was_glide)) {
			if (step > worst_glide) {
				worst_glide = step;
				worst_glide_frame = frame;
			}
		} else
			worst = fmaxf(worst, step - fabsf(head - last_head));
		was_glide = glide;
		cut |= halo_stereo_cut_requested();
		last_body = run.body;
		last_head = head;
	}
	/* the head still again: the facing settles where the fold put it */
	seat_hold(&run, head, 6);
	worst = fmaxf(worst, fabsf(remainderf(run.body - last_body, 360.0f)));
	if (strstr(eyes_keep_log, "the eyes keep the head's "))
		sscanf(strstr(eyes_keep_log, "the eyes keep the head's ") + strlen("the eyes keep the head's "), "%f", &kept);
	if (strstr(look_asks_log, "the look asks for the "))
		sscanf(strstr(look_asks_log, "the look asks for the ") + strlen("the look asks for the "),
			"%f degrees the eyes kept, after %d frames", &asked, &frames);
	at_bound = seated && to_head > 45.0f;
	printf("    the body steps %.4f deg at worst past the head's turn%s", worst, still_turning ? "" : "\n");
	if (still_turning)
		printf("; through the glide %.4f deg at worst (frame %d after the end)\n", worst_glide, worst_glide_frame);
	printf("    logged: the eyes keep %.1f deg (the head turned %.1f), the look asks for %.1f after %d frames; the "
		"facing %.4f deg, the head %.1f, the leftover %.4f\n", kept, to_head - from_head, asked, frames,
		remainderf(run.facing_yaw / DEGREES, 360.0f), head, head_seat_leftover / DEGREES);
	snprintf(check_name, sizeof(check_name), "%s: the world holds still at the camera's end and at the fold", name);
	check(worst < 0.02f && (!still_turning || worst_glide <= 2.05f), check_name);
	snprintf(check_name, sizeof(check_name), "%s: no cut", name);
	check(!cut, check_name);
	snprintf(check_name, sizeof(check_name), "%s: the eyes keep the head's turn, the look asks for it %s", name,
		direct ? "after 1 frame" : "after the glide, within two ticks more");
	check(fabsf(kept - (to_head - from_head)) < 0.06f && fabsf(asked - kept) < 0.06f &&
		(direct ? frames == 1 : frames >= 18 && frames <= 24), check_name);
	snprintf(check_name, sizeof(check_name), at_bound ? "%s: the aim at the seat's bound, the leftover the rest" :
		"%s: the facing is the head's", name);
	check(at_bound ? degrees_apart(run.facing_yaw / DEGREES, 45.0f) < 0.01f &&
		fabsf(head_seat_leftover / DEGREES - (head - 45.0f)) < 0.01f :
		degrees_apart(run.facing_yaw / DEGREES, head) < 0.01f && head_seat_leftover == 0.0f, check_name);
	/* a degree more of the head */
	facing0 = run.facing_yaw;
	body0 = run.body;
	head += 1.0f;
	seat_step(&run, head, 0.0f);
	seat_hold(&run, head, 1);
	steady = fabsf(remainderf(run.body - body0, 360.0f)) < 0.01f;
	if (at_bound) {
		seat_step(&run, head + 1.0f, 0.0f);
		snprintf(check_name, sizeof(check_name), "%s: then a degree more: the aim stays, the logged left the head's "
			"less 45", name);
		check(steady && degrees_apart(run.facing_yaw / DEGREES, facing0 / DEGREES) < 0.01f &&
			logged_left(&left, &pending) && fabsf(left - (head + 1.0f - pending - 45.0f)) < 0.01f, check_name);
		head += 1.0f;
	} else {
		snprintf(check_name, sizeof(check_name), "%s: then a degree more: the facing by 1 deg, the world still",
			name);
		check(steady && fabsf(remainderf((run.facing_yaw - facing0) / DEGREES, 360.0f) - 1.0f) < 0.01f, check_name);
	}
	head_yaw_log = 0;
	game_settled = 1;
	seated = 0;
}

static void third_person_ends(void)
{
	static const float turns[3] = { 30.0f, 90.0f, 150.0f };
	char name[96];
	int i;

	printf("a vehicle's third-person camera ending (entry and exit), 90 Hz, three frames a tick:\n");
	for (i = 0; i < 3; i++) {
		snprintf(name, sizeof(name), "the exit, %.0f deg turned, the glide", turns[i]);
		third_person_end(name, 1, turns[i] / 30.0f, 0, 0, 0);
		snprintf(name, sizeof(name), "the exit, %.0f deg turned, the direct camera", turns[i]);
		third_person_end(name, 1, turns[i] / 30.0f, 0, 0, 1);
		snprintf(name, sizeof(name), "the entry, %.0f deg turned, a 45 deg seat", turns[i]);
		third_person_end(name, 0, turns[i] / 30.0f, 0, 1, 0);
		snprintf(name, sizeof(name), "the entry, %.0f deg turned, a seat without bounds", turns[i]);
		third_person_end(name, 0, turns[i] / 30.0f, 0, 0, 0);
	}
	third_person_end("the exit, 90 deg turned, the head still turning", 1, 3.0f, 1, 0, 0);
}

static void interpolated_turns(void)
{
	printf("the head's yaw at render time, the game's camera blended between ticks:\n");
	blended_turn("a steady turn, 1 deg a frame, 3 frames a tick", 1.0f, 0.0f, 3);
	blended_turn("a fast turn, 3 deg a frame, 3 frames a tick", 3.0f, 0.0f, 3);
	blended_turn("a turn, 2 frames a tick", 1.5f, 0.0f, 2);
	blended_turn("a turn with the stick turning the body", 1.0f, 0.6f, 3);
}

/* one frame of a cutscene check: the clock moves a thirtieth of a second */
static void cutscene_frame(void)
{
	halo_stereo_frame_begin();
}

static int immersive(void)
{
	return !halo_stereo_film() && halo_stereo_frame()->mode == HALO_STEREO_HEAD;
}

static int on_film(void)
{
	return halo_stereo_film() && halo_stereo_frame()->mode == HALO_STEREO_SCREEN;
}

/* frames until the film is gone (at most limit) */
static int frames_to_full_view(int limit)
{
	int frame;

	for (frame = 0; frame < limit && halo_stereo_film(); frame++)
		cutscene_frame();
	return frame;
}

/* the session-4 rule: in HEAD mode the film is only for a cutscene's camera
that isn't first person, from the cutscene's first such camera to its end */
static void first_person_cutscenes(void)
{
	int frame, passed;

	printf("first-person cutscene cameras:\n");
	restart("snap", 30.0, 120.0, 0);
	game_first_person = 0;
	game_settled = 1;
	/* a letterbox with a third-person scripted camera: the film */
	game_letterbox = 1;
	cutscene_frame();
	check(on_film(), "a cutscene's third-person camera is on the film");
	game_letterbox = 0;
	frames_to_full_view(4 * FILM_HOLD_FRAMES);
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();

	/* a letterbox with a first-person camera, throughout: immersive throughout */
	game_letterbox = 1;
	game_first_person = 1;
	passed = 1;
	for (frame = 0; frame < 60; frame++) {
		cutscene_frame();
		passed &= immersive();
	}
	game_letterbox = 0;
	for (frame = 0; frame < 2 * FILM_HOLD_FRAMES; frame++) {
		cutscene_frame();
		passed &= immersive();
	}
	check(passed, "a cutscene first person throughout stays immersive throughout");

	/* a scripted first-person camera without the letterbox: immersive too */
	game_scripted_camera = 1;
	cutscene_frame();
	check(immersive(), "a scripted camera in first person stays immersive");
	game_scripted_camera = 0;
	for (frame = 0; frame < 2 * FILM_HOLD_FRAMES; frame++)
		cutscene_frame();

	/* b30's intro, in the other order too: first person, then a third-person
	shot, then first person again; the film from the first third-person shot
	to the cutscene's end, never back */
	game_letterbox = 1;
	game_first_person = 1;
	for (frame = 0; frame < 10; frame++)
		cutscene_frame();
	check(immersive(), "a cutscene that begins first person begins immersive");
	game_first_person = 0;
	cutscene_frame();
	check(on_film(), "its first third-person shot puts it on the film");
	game_first_person = 1;
	passed = 1;
	for (frame = 0; frame < 60; frame++) {
		cutscene_frame();
		passed &= on_film();
	}
	game_first_person = 0;
	cutscene_frame();
	game_first_person = 1;
	for (frame = 0; frame < 60; frame++) {
		cutscene_frame();
		passed &= on_film();
	}
	check(passed, "a first-person shot without the player's look after it stays on the film");
	/* a10's two-frame gap in the letterbox, then a first-person stretch:
	still the same cutscene */
	game_letterbox = 0;
	cutscene_frame();
	cutscene_frame();
	game_letterbox = 1;
	cutscene_frame();
	check(on_film(), "a two-frame gap in the letterbox keeps one cutscene on the film");
	game_letterbox = 0;
	frame = frames_to_full_view(4 * FILM_HOLD_FRAMES);
	/* (the frame that ends it counted) */
	check(frame == PORTAL_FRAMES + 1, "the cutscene's end: the film through its hold and its ease, then the full view");
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();
	/* the next cutscene starts afresh */
	game_letterbox = 1;
	cutscene_frame();
	check(immersive(), "the next cutscene, first person, is immersive again");
	game_letterbox = 0;
	game_first_person = 0;
	for (frame = 0; frame < 2 * FILM_HOLD_FRAMES; frame++)
		cutscene_frame();
}

/* SB-b: inside a cutscene that has shown a third-person camera, the
player's own first-person camera with the look enabled (b30's ramp shot on
the Pelican bench) leaves the film through the film's own end path (the
hold, the ease into a window, the expansion); from then the head drives the
look under the letterbox, as the Warthog passenger's does. Any other camera
in the same stretch goes back to the film */
static void released_cutscene(void)
{
	float facing_yaw = 0.0f, facing_pitch = 0.0f, entry_yaw, progress = 0.0f, bars = 0.0f;
	struct loop_frame f;
	int frame, passed, held, expanding;

	printf("the player's own first-person camera with the look, inside a cutscene:\n");
	restart("snap", 30.0, 120.0, 0);
	game_first_person = game_player_look = game_look_disabled = 0;
	game_settled = 1;
	/* b30's Pelicans: a third-person camera under the letterbox, the head
	20 degrees off the screen's axis */
	probe_screen_yaw = 20.0f * DEGREES;
	game_letterbox = 1;
	passed = 1;
	for (frame = 0; frame < 10; frame++) {
		loop(&facing_yaw, &facing_pitch, 20.0f, 0.0f, 0.0f);
		passed &= on_film();
	}
	check(passed, "b30's Pelicans, a third-person camera under the letterbox: the film");
	/* the ramp shot: the director's first person with the look enabled, the
	letterbox still up */
	game_first_person = game_player_look = 1;
	entry_yaw = facing_yaw;
	expansion_logs = 0;
	passed = 1;
	f = loop(&facing_yaw, &facing_pitch, 20.0f, 0.0f, 0.0f);
	for (held = 1; held < 100 && halo_stereo_film(); held++) {
		passed &= facing_yaw == entry_yaw;
		f = loop(&facing_yaw, &facing_pitch, 20.0f, 0.0f, 0.0f);
	}
	expanding = halo_stereo_expansion(&progress, &bars);
	printf("  the ramp shot: the film held %d frames; the full view's first frame: expanding %d at %.4f, the head's "
		"direction shows the world at %.4f deg (the facing %.4f, was %.4f)\n", held, expanding, progress, f.view_yaw,
		facing_yaw / DEGREES, entry_yaw / DEGREES);
	check(passed && held == PORTAL_FRAMES + 1 && immersive() && expanding && progress == 0.0f,
		"the ramp shot: the film through its hold and its ease, then the full view, expanding, the letterbox still up");
	check(degrees_apart(f.view_yaw, entry_yaw / DEGREES + 20.0f) < 0.01f,
		"the world the screen showed stays put: the head, 20 deg off it, sees the world 20 deg off the facing");
	f = loop(&facing_yaw, &facing_pitch, 20.0f, 0.0f, 0.0f);
	check(f.look_turned && degrees_apart(facing_yaw / DEGREES, entry_yaw / DEGREES + 20.0f) < 0.01f &&
		degrees_apart(f.view_yaw, entry_yaw / DEGREES + 20.0f) < 0.01f,
		"the look takes the 20 degrees off the screen's axis once, the view holding");
	/* the head turns 10 degrees further left: the look follows it */
	passed = 1;
	for (frame = 1; frame <= 10; frame++) {
		f = loop(&facing_yaw, &facing_pitch, 20.0f + frame, 0.0f, 0.0f);
		passed &= immersive();
	}
	f = loop(&facing_yaw, &facing_pitch, 30.0f, 0.0f, 0.0f);
	printf("  the head 10 deg further left: the facing %.4f deg from the cutscene's\n",
		facing_yaw / DEGREES - entry_yaw / DEGREES);
	check(passed && degrees_apart(facing_yaw / DEGREES, entry_yaw / DEGREES + 30.0f) < 0.01f &&
		degrees_apart(f.view_yaw, entry_yaw / DEGREES + 30.0f) < 0.01f,
		"released, under the letterbox: the head drives the look (halo_stereo_head_look takes its turn)");
	for (frame = 0; frame < 30; frame++)
		cutscene_frame();
	check(expansion_logs == 1 && !halo_stereo_expansion(&progress, &bars) && immersive(),
		"the film expands out to the full view once, then the plain full view");
	probe_screen_yaw = 0.0f;

	/* a third-person camera again in the same letterbox: the film the next
	frame */
	game_first_person = game_player_look = 0;
	cutscene_frame();
	check(on_film(), "a third-person camera again in the same cutscene: on the film the next frame");
	for (frame = 0; frame < 10; frame++)
		cutscene_frame();
	/* first person with the look again: released again; the predicate
	drops for one frame during the hold (a cut between two first-person
	cameras), and the film holds through it and expands once */
	game_first_person = game_player_look = 1;
	expansion_logs = 0;
	passed = 1;
	for (frame = 0; frame < 5; frame++) {
		cutscene_frame();
		passed &= on_film();
	}
	game_player_look = 0;
	cutscene_frame();
	passed &= on_film();
	game_player_look = 1;
	held = frames_to_full_view(100);
	printf("  released again, the predicate dropping a frame in the hold: the film held %d frames after it\n", held);
	check(passed && held == PORTAL_FRAMES + 1 && immersive() && expansion_logs == 1,
		"first person with the look again: released again; a one-frame drop in the hold keeps the film, one expansion");
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();
	/* the letterbox ends while released: no second expansion */
	game_letterbox = 0;
	expansion_logs = 0;
	passed = 1;
	for (frame = 0; frame < 2 * FILM_HOLD_FRAMES; frame++) {
		cutscene_frame();
		passed &= immersive();
	}
	check(passed && expansion_logs == 0 && !halo_stereo_expansion(&progress, &bars),
		"the letterbox ending while released: the full view throughout, no second expansion");
	/* the latch cleared: a first-person camera without the player's look
	under the next letterbox is immersive */
	game_player_look = 0;
	game_letterbox = 1;
	cutscene_frame();
	check(immersive(), "the latch cleared with the cutscene: the next cutscene starts afresh");
	game_letterbox = 0;
	game_first_person = 0;
	for (frame = 0; frame < 2 * FILM_HOLD_FRAMES; frame++)
		cutscene_frame();

	/* first person with the look disabled inside the latch: the film */
	game_letterbox = 1;
	cutscene_frame();
	game_first_person = game_look_disabled = 1;
	passed = 1;
	for (frame = 0; frame < 60; frame++) {
		cutscene_frame();
		passed &= on_film();
	}
	check(passed, "first person with the look disabled after a third-person shot: the film, under the latch");
	game_first_person = game_look_disabled = 0;
	game_letterbox = 0;
	frames_to_full_view(200);
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();

	/* a10's shape: third person, a two-frame gap in the letterbox with the
	camera away from the eyes for a while after it, then first person with
	the look under the letterbox, then the camera settles */
	game_letterbox = 1;
	for (frame = 0; frame < 10; frame++)
		cutscene_frame();
	game_letterbox = 0;
	game_settled = 0;
	expansion_logs = 0;
	passed = 1;
	for (frame = 0; frame < 2; frame++) {
		cutscene_frame();
		passed &= on_film();
	}
	game_letterbox = 1;
	game_first_person = game_player_look = 1;
	for (frame = 0; frame < 20; frame++) {
		cutscene_frame();
		passed &= on_film();
	}
	game_settled = 1;
	held = frames_to_full_view(100);
	printf("  a10's shape: the film held %d frames after the camera settled\n", held);
	check(passed && held == PORTAL_FRAMES + 1 && halo_stereo_expansion(&progress, &bars) && expansion_logs == 1,
		"a10's shape: the hold bridges the gap, the release waits on the settle, then expands once");
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();
	game_letterbox = 0;
	for (frame = 0; frame < 2 * FILM_HOLD_FRAMES; frame++)
		cutscene_frame();
	check(expansion_logs == 1, "its letterbox's end: no second expansion");
	game_first_person = game_player_look = 0;
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();

	/* the same, the camera never settling: the film ends through black after
	FILM_SETTLE_SECONDS, with no expansion */
	game_letterbox = 1;
	for (frame = 0; frame < 10; frame++)
		cutscene_frame();
	game_letterbox = 0;
	game_settled = 0;
	expansion_logs = settle_gave_up_logs = 0;
	for (held = 0; held < 200 && halo_stereo_film(); held++) {
		if (held == 2) {
			game_letterbox = 1;
			game_first_person = game_player_look = 1;
		}
		cutscene_frame();
	}
	printf("  never settling: the film held %d frames\n", held);
	check(held == 75 && settle_gave_up_logs == 1 && expansion_logs == 0 && !halo_stereo_expansion(&progress, &bars),
		"the camera never settling: the film ends through black after 2.5 s, with no expansion (settle_gave_up)");
	game_settled = 1;
	game_letterbox = 0;
	for (frame = 0; frame < 2 * FILM_HOLD_FRAMES; frame++)
		cutscene_frame();
	game_first_person = game_player_look = 0;
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();

	/* Task 12k's spike (debug.cutscene_immersive): the Pelicans immersive,
	the ramp frame on the film (the immersive cutscene ends on the film), the
	full view after the portal; the released frames never immersive */
	cutscene_immersive_setting = 1;
	cutscene_immersive_min_fov = 40.0f * DEGREES;
	cutscene_outside_dim = 0.6f;
	game_field_of_view = 70.0f * DEGREES;
	game_letterbox = 1;
	for (frame = 0; frame < 10; frame++)
		cutscene_frame();
	check(halo_stereo_cutscene_immersive(), "with debug.cutscene_immersive, the Pelicans: immersive");
	game_first_person = game_player_look = 1;
	expansion_logs = 0;
	cutscene_frame();
	check(on_film() && !halo_stereo_cutscene_immersive(), "the ramp frame: on the film, through the cut");
	passed = 1;
	for (held = 0; held < 100 && halo_stereo_film(); held++) {
		cutscene_frame();
		passed &= !halo_stereo_cutscene_immersive();
	}
	for (frame = 0; frame < 40; frame++) {
		cutscene_frame();
		passed &= !halo_stereo_cutscene_immersive() && immersive();
	}
	printf("  the spike: the film held %d frames after the ramp frame\n", held);
	check(passed && held == PORTAL_FRAMES + 1 && expansion_logs == 1,
		"then the full view after the portal, one expansion, and no released frame immersive");
	game_letterbox = 0;
	for (frame = 0; frame < 2 * FILM_HOLD_FRAMES; frame++)
		cutscene_frame();
	cutscene_immersive_setting = 0;
	game_first_person = game_player_look = 0;
	set_pose(0.0f, 0.0f, 0.0f);
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();
}

/* a first-person moment with the look taken away (a10's cryo pod, as the
spec has it): immersive, the head turning the picture only, its turn handed
to the look on the way out */
static void look_disabled(void)
{
	float facing_yaw = 0.0f, facing_pitch = 0.0f, entry_yaw, worst_facing = 0.0f, last_view_yaw;
	struct loop_frame f;
	int frame, turned = 0, passed = 1;

	printf("a first-person camera with the look taken away:\n");
	restart("snap", 30.0, 120.0, 0);
	game_first_person = 1;
	for (frame = 1; frame <= 10; frame++)
		loop(&facing_yaw, &facing_pitch, 1.0f * frame, 0.0f, 0.0f);
	/* (the look takes the last frame's turn) */
	loop(&facing_yaw, &facing_pitch, 10.0f, 0.0f, 0.0f);
	game_scripted_camera = 1;
	game_look_disabled = 1;
	entry_yaw = facing_yaw;
	f = loop(&facing_yaw, &facing_pitch, 10.0f, 0.0f, 0.0f);
	check(immersive(), "the look taken away in first person: immersive, not the film");
	check(degrees_apart(f.view_yaw, entry_yaw / DEGREES) < 0.01f, "its first frame doesn't move the view");
	for (frame = 1; frame <= 30; frame++) {
		f = loop(&facing_yaw, &facing_pitch, 10.0f + frame, 0.0f, 0.0f);
		turned |= f.look_turned;
		worst_facing = fmaxf(worst_facing, fabsf(facing_yaw - entry_yaw));
		passed &= immersive();
	}
	printf("  the head 30 deg left: the view %.4f deg from the camera's, the facing moved %.6f deg\n",
		f.view_yaw - entry_yaw / DEGREES, worst_facing / DEGREES);
	check(passed && fabsf(f.view_yaw - entry_yaw / DEGREES - 30.0f) < 0.01f,
		"the head turns the picture: 30 degrees for 30");
	check(!turned && worst_facing == 0.0f, "halo_stereo_head_look returns no turn: the look takes none of it");
	/* the look back: the view holds, and the look takes the 30 degrees once */
	game_scripted_camera = 0;
	game_look_disabled = 0;
	last_view_yaw = f.view_yaw;
	f = loop(&facing_yaw, &facing_pitch, 40.0f, 0.0f, 0.0f);
	check(degrees_apart(f.view_yaw, last_view_yaw) < 0.01f, "the look back: the view doesn't jump");
	f = loop(&facing_yaw, &facing_pitch, 40.0f, 0.0f, 0.0f);
	check(f.look_turned && degrees_apart(facing_yaw / DEGREES, entry_yaw / DEGREES + 30.0f) < 0.01f &&
		degrees_apart(f.view_yaw, last_view_yaw) < 0.01f, "the look takes the moment's head turn once, as a seat's");
	game_first_person = 0;
}

/* the cutscene's end: the film holds until the camera reaches the eyes,
at most 2.5 s */
static void cutscene_end(void)
{
	int frame, held;

	printf("a cutscene's end:\n");
	restart("snap", 30.0, 120.0, 0);
	game_first_person = 0;
	/* the observer still gliding to the eyes when the letterbox goes */
	game_letterbox = 1;
	game_settled = 1;
	cutscene_frame();
	game_letterbox = 0;
	game_settled = 0;
	for (held = 0; held < 40 && halo_stereo_film(); held++)
		cutscene_frame();
	printf("  the camera away from the eyes: the film held %d frames\n", held);
	check(held == 40 && on_film() && halo_stereo_film_letterbox(),
		"the film holds while the camera hasn't reached the eyes, past the 10-frame hold");
	game_settled = 1;
	held = frames_to_full_view(100);
	check(held == PORTAL_FRAMES + 1 && immersive(), "once it has: the film's ease into a window, then the full view");
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();

	/* a camera that never reaches the eyes: the hold gives up after 2.5 s,
	and the film ends through black, as before */
	game_letterbox = 1;
	cutscene_frame();
	game_letterbox = 0;
	game_settled = 0;
	held = frames_to_full_view(200);
	printf("  never settling: the film held %d frames\n", held);
	check(held == 75, "a camera that never reaches the eyes: the film holds 2.5 s, no longer");
	game_settled = 1;
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();
}

/* after the hold, the film's rectangle expands out to the full view at the
bars' rate, carrying the film's bars */
static void cutscene_expansion(void)
{
	int frame, held, passed, expanding;
	float progress = 0.0f, bars = 0.0f, last_progress = -1.0f, worst_step = 0.0f, worst_bars = 0.0f;

	printf("the cutscene window's expansion:\n");
	restart("snap", 30.0, 120.0, 0);
	game_first_person = 0;
	game_letterbox = 1;
	game_settled = 1;
	cutscene_frame();
	halo_stereo_set_title_bars(0.4f);
	game_letterbox = 0;
	game_settled = 0;
	for (held = 0; held < 20 && halo_stereo_film(); held++) {
		cutscene_frame();
		halo_stereo_set_title_bars(0.4f);
	}
	game_settled = 1;
	while (halo_stereo_film()) {
		cutscene_frame();
		halo_stereo_set_title_bars(0.4f);
	}
	expanding = halo_stereo_expansion(&progress, &bars);
	printf("  the first full-view frame: expanding %d, progress %.4f, bars %.4f\n", expanding, progress, bars);
	check(immersive() && expanding && progress == 0.0f && fabsf(bars - 0.4f) < 1e-6f,
		"the film's rectangle starts to expand, carrying the film's bars");
	passed = 1;
	for (frame = 1; frame < 40 && halo_stereo_expansion(&progress, &bars); frame++) {
		last_progress = progress;
		cutscene_frame();
		if (!halo_stereo_expansion(&progress, &bars))
			break;
		worst_step = fmaxf(worst_step, fabsf(progress - last_progress - 1.0f / 30.0f));
		worst_bars = fmaxf(worst_bars, fabsf(bars - fmaxf(0.0f, 0.4f - progress)));
		passed &= immersive();
	}
	printf("  expanded over %d frames; its progress %.2e from a thirtieth a frame at worst, the bars %.2e from "
		"0.4 less a second's rate\n", frame, worst_step, worst_bars);
	check(passed && frame == 30 && worst_step < 1e-4f && worst_bars < 1e-4f,
		"over a second, linear, the bars going on out at one a second, immersive throughout");
	cutscene_frame();
	check(!halo_stereo_expansion(&progress, &bars) && immersive(), "then the plain full view");

	/* a camera that never reaches the eyes: no expansion */
	game_letterbox = 1;
	cutscene_frame();
	game_letterbox = 0;
	game_settled = 0;
	frames_to_full_view(200);
	check(!halo_stereo_expansion(&progress, &bars), "a film that gave up on the camera cuts, with no expansion");
	game_settled = 1;
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();

	/* the film coming back stops it */
	game_letterbox = 1;
	cutscene_frame();
	game_letterbox = 0;
	frames_to_full_view(4 * FILM_HOLD_FRAMES);
	cutscene_frame();
	game_letterbox = 1;
	cutscene_frame();
	check(on_film() && !halo_stereo_expansion(&progress, &bars), "a new cutscene during it stops the expansion");
	game_letterbox = 0;
	frames_to_full_view(4 * FILM_HOLD_FRAMES);
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();

	/* a vehicle's film (display.stereo_vehicle_screen) cuts, as before */
	vehicle_screen = 1;
	game_third_person = 1;
	cutscene_frame();
	game_third_person = 0;
	frames_to_full_view(4 * FILM_HOLD_FRAMES);
	check(!halo_stereo_expansion(&progress, &bars), "a vehicle's film doesn't expand");
	vehicle_screen = 0;
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();
}

/* the title bars: out while the script fade shows, in at one a second,
out with the title's fade */
static void title_bars(void)
{
	float bars = 0.0f;
	int frame;

	printf("the film's title bars:\n");
	for (frame = 0; frame < 30; frame++)
		bars = halo_stereo_title_bars_ease(bars, 1.0f, 0.5f, 1.0f / 30.0f);
	check(bars == 0.0f, "none while the white fade shows");
	for (frame = 0; frame < 15; frame++)
		bars = halo_stereo_title_bars_ease(bars, 1.0f, 0.0f, 1.0f / 30.0f);
	printf("  half a second after the fade: %.4f\n", bars);
	check(fabsf(bars - 0.5f) < 1e-4f, "then in at one a second");
	for (frame = 0; frame < 30; frame++)
		bars = halo_stereo_title_bars_ease(bars, 1.0f, 0.0f, 1.0f / 30.0f);
	check(bars == 1.0f, "all the way in");
	bars = halo_stereo_title_bars_ease(bars, 0.3f, 0.0f, 1.0f / 30.0f);
	check(fabsf(bars - 0.3f) < 1e-6f, "following the title's fade-out");
	bars = halo_stereo_title_bars_ease(bars, 0.0f, 0.0f, 1.0f / 30.0f);
	check(bars == 0.0f, "out with it");
}

/* the window's geometry (halo_stereo_window.h) and the side-by-side model */
static void expansion_window(void)
{
	const float eye[3] = { -0.032f, 0.05f, 4.0f }, half_width = 2.309f, half_height = 2.309f * 9.0f / 16.0f;
	const float identity[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
	const float tangents[4] = { 1.2f, 0.9f, 1.0f, 1.1f };
	float start[4], end[4], window[4], last[4], misfit = 0.0f;
	struct halo_stereo_window_model model;
	int k, inside, outside;

	printf("the cutscene window:\n");
	check(halo_stereo_screen_window(eye, half_width, half_height, start), "an eye in front of the screen");
	/* points on the screen's plane just inside and just outside its edges */
	inside = outside = 0;
	for (k = 0; k < 400; k++) {
		float u = -1.2f + 2.4f * (k % 20) / 19.0f, v = -1.2f + 2.4f * (k / 20) / 19.0f;
		float point[3] = { u * half_width, v * half_height, 0.0f };
		float d[3] = { point[0] - eye[0], point[1] - eye[1], point[2] - eye[2] };
		int on_screen = fabsf(u) <= 1.0f && fabsf(v) <= 1.0f;
		int in_window = halo_stereo_window_test(d[0], d[1], d[2], start[0], start[1], start[2], start[3], 0.0f) != 0;

		inside += on_screen && in_window;
		outside += !on_screen && !in_window;
		misfit += on_screen != in_window;
	}
	printf("  the start window against the screen's quad: %d in, %d out, %.0f misfits\n", inside, outside, misfit);
	check(misfit == 0.0f, "the window begins as the screen exactly, from an eye off its axis");
	halo_stereo_window_empty(end);
	halo_stereo_window_include_view(end, tangents, identity);
	check(end[0] < -atanf(1.2f) && end[1] > atanf(0.9f) && end[2] < -atanf(1.1f) && end[3] > atanf(1.0f),
		"the end window covers the view");
	halo_stereo_window_at(start, end, 0.0f, NULL, window);
	check(!memcmp(window, start, sizeof(window)), "progress 0: the screen");
	halo_stereo_window_at(start, end, 0.5f, NULL, window);
	check(fabsf(window[1] - 0.5f * (start[1] + end[1])) < 1e-6f, "progress 0.5: half way, linear");
	halo_stereo_window_at(start, end, 1.0f, NULL, window);
	check(!memcmp(window, end, sizeof(window)), "progress 1: the end");
	memcpy(last, end, sizeof(last));
	halo_stereo_window_at(start, end, 0.2f, last, window);
	check(!memcmp(window, end, sizeof(window)), "never smaller than the last frame's");
	{
		const float turned[3][3] = { { 0, 0, 1 }, { 0, 1, 0 }, { -1, 0, 0 } };

		halo_stereo_window_empty(end);
		halo_stereo_window_include_view(end, tangents, turned);
		check(end[0] <= -3.14f && end[1] >= 3.14f, "a view turned from the screen: the end is everything");
	}
	check(halo_stereo_window_test(0.0f, 0.99f * tanf(start[3]) * 4.0f, -4.0f, start[0], start[1], start[2], start[3],
		0.5f) == 2 && halo_stereo_window_test(0.0f, 0.0f, -4.0f, start[0], start[1], start[2], start[3], 0.5f) == 1,
		"the bars sit at the window's top and bottom");

	/* the side-by-side view's model: the film on the theater's default
	screen (60 degrees across, 4 m ahead), then the expansion from it */
	setting_stereo = "side_by_side";
	probe_drawable[0] = 1920;
	probe_drawable[1] = 1080;
	restart("snap", 30.0, 120.0, 0);
	game_letterbox = 1;
	cutscene_frame();
	k = halo_stereo_side_by_side_window(0, &model);
	printf("  side by side, the film: kind %d, the screen's tangents %.4f %.4f %.4f %.4f, the eye's %.2f %.2f %.2f %.2f\n",
		k, model.screen[0], model.screen[1], model.screen[2], model.screen[3], model.tangents[0], model.tangents[1],
		model.tangents[2], model.tangents[3]);
	check(k == 1 && model.kind == 1 && fabsf(model.screen[1] - model.screen[0] - 2.0f * 2.309f / 4.0f) < 1e-3f &&
		fabsf((model.screen[3] - model.screen[2]) * 16.0f / 9.0f - 2.0f * 2.309f / 4.0f) < 1e-3f &&
		model.screen[0] + model.screen[1] > 0.0f && model.tangents[0] == SIDE_BY_SIDE_TANGENT,
		"the film: the default screen as the left eye sees it, a little to its right");
	game_letterbox = 0;
	frames_to_full_view(4 * FILM_HOLD_FRAMES);
	k = halo_stereo_side_by_side_window(0, &model);
	check(k == 2 && fabsf(model.window[0] - atanf(model.screen[0])) < 1e-5f &&
		fabsf(model.window[3] - atanf(model.screen[3])) < 1e-5f, "the expansion begins at the film's screen");
	for (k = 0; k < 15; k++)
		cutscene_frame();
	halo_stereo_side_by_side_window(0, &model);
	check(model.kind == 2 && model.window[1] > atanf(model.screen[1]) && model.window[1] < atanf(0.8f) + 0.06f,
		"half way, between the screen and the view's edge");
	for (k = 0; k < 15; k++)
		cutscene_frame();
	check(halo_stereo_side_by_side_window(0, &model) == 0, "after a second, nothing to model: the full view");
	setting_stereo = "head";
	probe_drawable[0] = probe_drawable[1] = 0;
	restart("snap", 30.0, 120.0, 0);
}

/* split screen: two player windows keep the mono path (render.c draws the
eyes only with one), so the frame has no eyes and no hook acts on them,
while the Compositor's frame is still asked for (its picture goes on the
UI's quad, as for a frame without eyes) */
static void split_screen(void)
{
	float yaw, pitch;
	unsigned long asked;
	int frame;

	printf("split screen:\n");
	restart("snap", 30.0, 120.0, 0);
	check(halo_stereo_frame()->eye_count == 2 && halo_stereo_head_drives_look(0),
		"one window: the eyes, and the head drives the look");

	probe_windows = 2;
	asked = host_frames;
	set_pose(30.0f * DEGREES, 10.0f * DEGREES, 0.0f);
	halo_stereo_frame_begin();
	check(halo_stereo_frame()->eye_count == 0 && halo_stereo_frame()->mode == HALO_STEREO_HEAD && host_frames == asked + 1,
		"HEAD, two windows: the Compositor's frame asked for, but no eyes");
	check(!halo_stereo_head_drives_look(0) && !halo_stereo_head_look(0, 0.0f, &yaw, &pitch) && yaw == 0.0f &&
		pitch == 0.0f, "HEAD, two windows: the head turned, but it doesn't drive the look");
	yaw = 1.0f;
	pitch = 0.5f;
	halo_stereo_stick_look(0, 1.0f, 1.0f / 90.0f, &yaw, &pitch);
	check(yaw == 1.0f && pitch == 0.5f, "HEAD, two windows: the stick keeps the game's own turn and pitch");
	game_letterbox = 1;
	halo_stereo_frame_begin();
	check(!halo_stereo_film() && !halo_stereo_film_letterbox() && !halo_stereo_screen_framing() &&
		!halo_stereo_hud_split(), "HEAD, two windows, a cutscene: no 3D film, no screen framing, no split HUD");
	game_letterbox = 0;
	for (frame = 0; frame < 4 * PORTAL_FRAMES; frame++)
		halo_stereo_frame_begin();

	/* SCREEN mode: no 3D TV and none of its band framing (main.c) */
	setting_stereo = "screen";
	restart("snap", 30.0, 120.0, 0);
	check(halo_stereo_frame()->eye_count == 0 && !halo_stereo_screen_gameplay() && !halo_stereo_screen_framing(),
		"SCREEN, two windows: no eyes, no 3D TV, no screen framing");
	probe_windows = 1;
	halo_stereo_frame_begin();
	check(halo_stereo_frame()->eye_count == 2 && halo_stereo_screen_gameplay() && halo_stereo_screen_framing(),
		"SCREEN, one window: the 3D TV, framed");

	/* the side-by-side view: mono, then its eyes again with one window */
	setting_stereo = "side_by_side";
	probe_drawable[0] = 1920;
	probe_drawable[1] = 1080;
	probe_windows = 2;
	restart("snap", 30.0, 120.0, 0);
	check(halo_stereo_frame()->eye_count == 0 && !halo_stereo_hud_split(), "side by side, two windows: no eyes");
	probe_windows = 1;
	halo_stereo_frame_begin();
	check(halo_stereo_frame()->eye_count == 2 && halo_stereo_frame()->eye_width == 960,
		"side by side, one window: the eyes again");

	setting_stereo = "head";
	probe_drawable[0] = probe_drawable[1] = 0;
	restart("snap", 30.0, 120.0, 0);
	check(halo_stereo_frame()->eye_count == 2 && halo_stereo_head_drives_look(0),
		"HEAD, back to one window: the eyes, and the head drives the look");
}

/* where a point in the camera's frame (x right, y up, z ahead, world units)
appears to a viewer's eye, as the tangents of its direction from that eye in
the screen's frame: through the film on the screen (the eye's own film eye,
the screen's half extents, the viewer's eye against the screen), or in the
full view (the same eye at its real place in the head, midpoint at the
camera) */
static void seen_on_film(const struct halo_stereo_eye *film_eye, const float point[3], const float viewer[3],
	float half_width, float half_height, float seen[2])
{
	float tx = (point[0] - film_eye->offset[0]) / (point[2] + film_eye->offset[2]);
	float ty = (point[1] - film_eye->offset[1]) / (point[2] + film_eye->offset[2]);
	float sx = -half_width + 2.0f * half_width * (tx + film_eye->left) / (film_eye->left + film_eye->right);
	float sy = -half_height + 2.0f * half_height * (ty + film_eye->down) / (film_eye->up + film_eye->down);

	seen[0] = (sx - viewer[0]) / viewer[2];
	seen[1] = (sy - viewer[1]) / viewer[2];
}

/* the worst difference, over both eyes and points near and far, between
where the film puts the world and where the full view will (tangents) */
static float film_window_misfit(void)
{
	const float half_width = 2.309f / 3.048f, half_height = 0.75f * half_width, distance = 4.0f / 3.048f;
	const float points[5][3] = { { 0.1f, -0.05f, 0.3f }, { -0.2f, 0.1f, 1.0f }, { 0.5f, 0.3f, 3.0f },
		{ -2.0f, -1.0f, 10.0f }, { 30.0f, 10.0f, 100.0f } };
	float worst = 0.0f;
	int eye, k;

	for (eye = 0; eye < 2; eye++) {
		const float viewer[3] = { (eye == 0 ? -0.032f : 0.032f) / 3.048f, 0.0f, distance };

		for (k = 0; k < 5; k++) {
			float film[2], full[2];

			seen_on_film(&halo_stereo_frame()->eyes[eye], points[k], viewer, half_width, half_height, film);
			full[0] = (points[k][0] - viewer[0]) / points[k][2];
			full[1] = (points[k][1] - viewer[1]) / points[k][2];
			worst = fmaxf(worst, fmaxf(fabsf(film[0] - full[0]), fabsf(film[1] - full[1])));
		}
	}
	return worst;
}

/* the film eases into a window onto the world before the cutscene window
expands, so the expansion's first frame shows the world where the film
did: no step in magnification or disparity */
static void film_to_window(void)
{
	float first = -1.0f, last = 0.0f, yaw, pitch, last_pitch = 0.0f;
	int frames = 0;

	printf("the film easing into a window onto the world:\n");
	restart("snap", 30.0, 120.0, 0);
	game_first_person = 0;
	game_settled = 1;
	game_letterbox = 1;
	cutscene_frame();
	halo_stereo_screen_frusta(0.335f);
	first = film_window_misfit();
	oriented(0.0f, 10.0f * DEGREES, &yaw, &pitch);
	printf("  the cutscene's film: the world %.4f (tangent) from where the full view puts it, the camera's pitch %.2f deg\n",
		first, pitch);
	game_letterbox = 0;
	while (halo_stereo_film() && frames < 100) {
		last = film_window_misfit();
		oriented(0.0f, 10.0f * DEGREES, &yaw, &last_pitch);
		cutscene_frame();
		halo_stereo_screen_frusta(0.335f);
		frames++;
	}
	printf("  its last frame, %d frames on: %.2e from the full view, the camera's pitch %.4f deg\n", frames, last,
		last_pitch);
	check(first > 0.01f, "the cutscene's own film puts the world elsewhere (the step the ease removes)");
	check(last < 1e-4f, "the film's last frame shows the world where the full view will: no step in size or depth");
	check(fabsf(last_pitch) < 1e-3f, "and its camera is level, as the full view puts the room's level on the world's");
	check(immersive(), "then the full view");
}

/* with the head turned off the screen's axis, the full view's first frame
keeps the world through the screen where it was: the look takes the turn */
static void film_handover_yaw(void)
{
	float facing_yaw = 0.0f, facing_pitch = 0.0f, entry_yaw;
	struct loop_frame f;
	int frame;

	printf("the head turned off the screen at a cutscene's end:\n");
	restart("snap", 30.0, 120.0, 0);
	game_first_person = 0;
	game_settled = 1;
	game_letterbox = 1;
	probe_screen_yaw = 20.0f * DEGREES;
	loop(&facing_yaw, &facing_pitch, 20.0f, 0.0f, 0.0f);
	game_letterbox = 0;
	entry_yaw = facing_yaw;
	for (frame = 0; frame < 100 && halo_stereo_film(); frame++)
		loop(&facing_yaw, &facing_pitch, 20.0f, 0.0f, 0.0f);
	f = loop(&facing_yaw, &facing_pitch, 20.0f, 0.0f, 0.0f);
	printf("  the full view's first frames: the head's direction shows the world at %.4f deg (the facing %.4f)\n",
		f.view_yaw, facing_yaw / DEGREES);
	check(!halo_stereo_film() && degrees_apart(f.view_yaw, entry_yaw / DEGREES + 20.0f) < 0.01f,
		"the world the screen showed stays put: the head, 20 deg off it, sees the world 20 deg off the facing");
	f = loop(&facing_yaw, &facing_pitch, 20.0f, 0.0f, 0.0f);
	check(degrees_apart(facing_yaw / DEGREES, entry_yaw / DEGREES + 20.0f) < 0.01f &&
		degrees_apart(f.view_yaw, entry_yaw / DEGREES + 20.0f) < 0.01f,
		"the look takes the 20 degrees once, the view holding");
	probe_screen_yaw = 0.0f;
	set_pose(0.0f, 0.0f, 0.0f);
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();
}

/* the presenter's memory of an expansion (halo_stereo_window.h), across an
interruption it never sees */
static void expansion_memory(void)
{
	struct halo_stereo_expansion_memory memory;
	int fresh[7];

	printf("the presenter's memory of an expansion:\n");
	memset(&memory, 0, sizeof(memory));
	fresh[0] = halo_stereo_expansion_fresh(&memory, 1, 0.0f);
	fresh[1] = halo_stereo_expansion_fresh(&memory, 1, 0.3f);
	fresh[2] = halo_stereo_expansion_fresh(&memory, 1, 0.6f);
	/* a new cutscene, its film never reaching this presenter; the next
	expansion's first frame */
	fresh[3] = halo_stereo_expansion_fresh(&memory, 1, 0.0f);
	fresh[4] = halo_stereo_expansion_fresh(&memory, 1, 0.5f);
	/* interrupted again, and the next expansion's first frame missed (no
	space that frame) */
	fresh[5] = halo_stereo_expansion_fresh(&memory, 1, 1.0f / 30.0f);
	halo_stereo_expansion_fresh(&memory, 0, 0.0f);
	fresh[6] = halo_stereo_expansion_fresh(&memory, 1, 0.2f);
	check(fresh[0] && !fresh[1] && !fresh[2], "an expansion is fresh on its first frame only");
	check(fresh[3] && !fresh[4], "an expansion interrupted unseen: the next begins afresh, not at the old window");
	check(fresh[5], "a first frame missed: the progress going back begins afresh");
	check(fresh[6], "after a frame without one: afresh");
}

/* small rulings: a fade-out under a title takes its bars down with the
title, not at once; a vehicle's film ends a cutscene's latch */
static void cutscene_rulings(void)
{
	float bars;
	int frame;

	printf("the bars under a fade, and the latch after a vehicle's film:\n");
	bars = halo_stereo_title_bars_ease(1.0f, 0.7f, 0.5f, 1.0f / 30.0f);
	check(fabsf(bars - 0.7f) < 1e-6f, "a fade over a title fading out: the bars follow the title down");
	bars = halo_stereo_title_bars_ease(0.3f, 1.0f, 0.5f, 1.0f / 30.0f);
	check(fabsf(bars - 0.3f) < 1e-6f, "and never rise while the fade shows");
	restart("snap", 30.0, 120.0, 0);
	game_first_person = 0;
	game_letterbox = 1;
	cutscene_frame();
	game_letterbox = 0;
	vehicle_screen = 1;
	game_third_person = 1;
	for (frame = 0; frame < 40; frame++)
		cutscene_frame();
	game_third_person = 0;
	game_letterbox = 1;
	game_first_person = 1;
	/* (past the vehicle film's own hold) */
	for (frame = 0; frame <= FILM_HOLD_FRAMES; frame++)
		cutscene_frame();
	check(immersive(), "a first-person cutscene right after a vehicle's film is immersive: the latch ended");
	game_letterbox = 0;
	game_first_person = 0;
	vehicle_screen = 0;
	for (frame = 0; frame < 8 * FILM_HOLD_FRAMES; frame++)
		cutscene_frame();
}


/* the eye camera's yaw and pitch (degrees, left and up positive) after
halo_stereo_head_orient turns a level camera at camera_yaw */
static void immersive_orient(float camera_yaw, float *yaw, float *pitch)
{
	float forward[3] = { cosf(camera_yaw * DEGREES), sinf(camera_yaw * DEGREES), 0.0f }, up[3] = { 0.0f, 0.0f, 1.0f };

	halo_stereo_head_orient(forward, up);
	*yaw = atan2f(forward[1], forward[0]) / DEGREES;
	*pitch = asinf(fmaxf(-1.0f, fminf(1.0f, forward[2]))) / DEGREES;
}

/* Task 12k's spike (debug.cutscene_immersive): a cutscene's third-person
film frame renders immersive; the eyes are the camera turned by the head
from the director's frame, anchored in the room as the cutscene began; a
telephoto camera stays on the film */
static void immersive_cutscene(void)
{
	float yaw, pitch, forward[3], up[3], tangents[2], dim, look_yaw, look_pitch;
	int frame, passed;

	printf("the immersive cutscene (debug.cutscene_immersive):\n");
	restart("snap", 30.0, 120.0, 0);
	cutscene_immersive_setting = 1;
	cutscene_immersive_min_fov = 40.0f * DEGREES;
	cutscene_outside_dim = 0.6f;
	game_first_person = 0;
	game_field_of_view = 70.0f * DEGREES;
	/* the head 20 degrees left when the cutscene begins */
	for (frame = 1; frame <= 10; frame++) {
		set_pose(2.0f * frame * DEGREES, 0.0f, 0.0f);
		cutscene_frame();
	}
	game_letterbox = 1;
	cutscene_frame();
	check(immersive() && halo_stereo_cutscene_immersive() && halo_stereo_cutscene_immersive_letterbox() &&
		!halo_stereo_hud_split(), "a third-person cutscene camera at 70 degrees: immersive, the HUD layer whole");
	immersive_orient(0.0f, &yaw, &pitch);
	check(fabsf(yaw) < 0.01f && fabsf(pitch) < 0.01f, "the head where it was as the cutscene began: the camera's view");
	check(halo_stereo_cutscene_frame(forward, up, tangents, &dim) && fabsf(forward[2] + 1.0f) < 1e-4f &&
		fabsf(up[1] - 1.0f) < 1e-4f, "and the director's frame straight ahead of the eyes");
	check(fabsf(tangents[0] - 0.85f * tanf(35.0f * DEGREES)) < 1e-5f &&
		fabsf(tangents[1] - tangents[0] * 9.0f / 16.0f) < 1e-5f && fabsf(dim - 0.6f) < 1e-6f,
		"the frame: the camera's 4:3 frame's width, 16:9; the outside's dim");
	/* the head turns 30 degrees further left and 10 up */
	for (frame = 1; frame <= 10; frame++) {
		set_pose((20.0f + 3.0f * frame) * DEGREES, frame * DEGREES, 0.0f);
		cutscene_frame();
	}
	immersive_orient(0.0f, &yaw, &pitch);
	halo_stereo_cutscene_frame(forward, up, tangents, &dim);
	printf("  the head 30 deg left, 10 up from the anchor: the eyes at %.3f, %.3f deg; the frame's forward in them "
		"%.4f %.4f %.4f\n", yaw, pitch, forward[0], forward[1], forward[2]);
	check(fabsf(yaw - 30.0f) < 0.01f && fabsf(pitch - 10.0f) < 0.01f, "the head turns the eyes from the camera");
	/* (the camera's forward seen from eyes 30 left and 10 up: atan(sin 30 /
	(cos 10 cos 30)) = 30.38 degrees right, and atan(tan 10) below) */
	check(fabsf(atan2f(forward[0], -forward[2]) / DEGREES - 30.38f) < 0.01f &&
		fabsf(atan2f(-forward[1], -forward[2]) / DEGREES - 10.0f) < 0.01f,
		"the frame stays where it was in the room: right of and below the eyes' center");
	/* a cut: the next camera faces 90 degrees left of the last; the frame
	stays put in the room (the head's turn against it is unchanged) */
	immersive_orient(90.0f, &yaw, &pitch);
	{
		float after[3];

		halo_stereo_cutscene_frame(after, up, tangents, &dim);
		check(fabsf(yaw - 120.0f) < 0.01f && fabsf(after[0] - forward[0]) < 1e-4f && fabsf(after[2] - forward[2]) < 1e-4f,
			"a cut re-aims the new camera at the room-anchored frame");
	}
	/* a camera pitched 20 degrees up: the head's yaw turns about the
	world's up (the third-person path's reading), so the eyes keep the
	room's vertical and the frame pitches with the camera */
	{
		float pitched_forward[3] = { cosf(20.0f * DEGREES), 0.0f, sinf(20.0f * DEGREES) };
		float pitched_up[3] = { -sinf(20.0f * DEGREES), 0.0f, cosf(20.0f * DEGREES) };
		float right_z, frame[3];

		halo_stereo_head_orient(pitched_forward, pitched_up);
		/* the eyes' right (forward x up) stays level: no roll from the turn */
		right_z = pitched_forward[0] * pitched_up[1] - pitched_forward[1] * pitched_up[0];
		halo_stereo_cutscene_frame(frame, up, tangents, &dim);
		printf("  a camera 20 deg up, the head 30 left and 10 up: the eyes at %.3f, %.3f deg; their right's rise %.5f\n",
			atan2f(pitched_forward[1], pitched_forward[0]) / DEGREES,
			asinf(fminf(1.0f, pitched_forward[2])) / DEGREES, right_z);
		check(fabsf(atan2f(pitched_forward[1], pitched_forward[0]) / DEGREES - 30.0f) < 0.01f &&
			fabsf(asinf(fminf(1.0f, pitched_forward[2])) / DEGREES - 30.0f) < 0.01f && fabsf(right_z) < 1e-5f,
			"a pitched camera: the yaw about the world's up, the pitch added, the eyes level across");
	}
	check(!halo_stereo_head_look(0, 0.0f, &look_yaw, &look_pitch) || (look_yaw == 0.0f && look_pitch == 0.0f),
		"the head never turns the look in the immersive cutscene");
	/* a telephoto camera: the film, through the cut */
	game_field_of_view = 30.0f * DEGREES;
	cutscene_frame();
	check(on_film() && !halo_stereo_cutscene_immersive(), "a 30 degree camera, under the 40 degree threshold: the film");
	game_field_of_view = 70.0f * DEGREES;
	cutscene_frame();
	immersive_orient(0.0f, &yaw, &pitch);
	check(immersive() && halo_stereo_cutscene_immersive() && fabsf(yaw - 30.0f) < 0.01f,
		"70 degrees again: immersive, the frame still where the cutscene anchored it");
	{
		float progress, bars;

		check(!halo_stereo_expansion(&progress, &bars),
			"a film shot cutting to an immersive one doesn't start the cutscene window's expansion");
	}
	/* a two-tick gap in the letterbox with the camera at the eyes: still
	immersive */
	game_settled = 1;
	passed = 1;
	game_letterbox = 0;
	for (frame = 0; frame < 2; frame++) {
		cutscene_frame();
		passed &= halo_stereo_cutscene_immersive() && !halo_stereo_film();
	}
	game_letterbox = 1;
	cutscene_frame();
	check(passed && halo_stereo_cutscene_immersive(), "a two-tick gap in the letterbox, the camera at the eyes: immersive");
	/* the cutscene's end with the camera gliding back to the eyes: the film
	at once, through the cut, until it settles; then the window expands out
	to the full view */
	game_letterbox = 0;
	game_settled = 0;
	cutscene_frame();
	check(on_film() && !halo_stereo_cutscene_immersive(), "the end, the camera gliding: the film at once");
	passed = 1;
	for (frame = 0; frame < 30; frame++) {
		cutscene_frame();
		passed &= on_film();
	}
	check(passed, "the film holds while the camera glides");
	game_settled = 1;
	{
		float progress, bars;
		int expanded = 0;

		passed = 1;
		for (frame = 0; frame < 8 * FILM_HOLD_FRAMES && !expanded; frame++) {
			cutscene_frame();
			expanded = halo_stereo_expansion(&progress, &bars);
			passed &= !halo_stereo_cutscene_immersive();
		}
		check(expanded && passed, "settled: the film expands out to the full view, never immersive again");
	}
	for (frame = 0; frame < 8 * FILM_HOLD_FRAMES; frame++)
		cutscene_frame();
	check(immersive() && !halo_stereo_film() && !halo_stereo_cutscene_immersive(), "then the full view");
	/* the next cutscene ends with the camera already at the eyes: the hold
	immersive, then the film (the cut covers the handoff), then the expansion */
	game_letterbox = 1;
	for (frame = 0; frame < 5; frame++)
		cutscene_frame();
	check(halo_stereo_cutscene_immersive(), "the next cutscene: immersive");
	game_letterbox = 0;
	passed = 1;
	for (frame = 0; frame < FILM_HOLD_FRAMES; frame++) {
		cutscene_frame();
		passed &= halo_stereo_cutscene_immersive();
	}
	cutscene_frame();
	check(passed && on_film(), "its end, the camera at the eyes: immersive through the hold, then the film");
	for (frame = 0; frame < 8 * FILM_HOLD_FRAMES; frame++)
		cutscene_frame();
	check(immersive() && !halo_stereo_film() && !halo_stereo_cutscene_immersive(), "then the full view again");
	/* the setting off: the film, as before */
	cutscene_immersive_setting = 0;
	game_letterbox = 1;
	cutscene_frame();
	check(on_film() && !halo_stereo_cutscene_immersive(), "debug.cutscene_immersive off: the film");
	game_letterbox = 0;
	for (frame = 0; frame < 8 * FILM_HOLD_FRAMES; frame++)
		cutscene_frame();
}

/* display.seat_look_fade: the seat's 45 degree limit, the fade from 5
degrees past it to black at 45 (the defaults); the world keeps following the
head throughout (seat_yaw_limit checks that with the fade off, and the fade
changes nothing the eye cameras do) */
static void seat_look_fade(void)
{
	struct seat_run run;
	float head = 0.0f, dim, blur, last, worst_step = 0.0f, body0, expected;
	int frame, held = 1;

	printf("a seat's look fade (display.seat_look_fade, 5 to 45 deg past the limit):\n");
	seat_start(&run, "snap");
	seat_look_fade_setting = 1;
	seat_look_fade_blur_setting = 1;
	seat_look_fade_start = 5.0f * DEGREES;
	seat_look_fade_black = 45.0f * DEGREES;
	seat_hold(&run, head, 3);
	body0 = run.body;
	seat_turn_to(&run, &head, 48.0f, 0.0f);
	seat_hold(&run, head, 30);
	halo_stereo_seat_look_fade(&dim, &blur);
	printf("  3 deg past: dim %.3f, blur %.3f\n", dim, blur);
	check(dim == 0.0f && blur == 0.0f, "inside the fade's start the view is clear");
	seat_turn_to(&run, &head, 70.0f, 0.0f);
	seat_hold(&run, head, 30);
	halo_stereo_seat_look_fade(&dim, &blur);
	printf("  25 deg past: dim %.3f, blur %.3f, the body %.4f deg from its start\n", dim, blur,
		remainderf(run.body - body0, 360.0f));
	check(fabsf(dim - 0.5f) < 1e-3f && blur > 0.998f, "half way to black: half dimmed, fully blurred");
	check(fabsf(remainderf(run.body - body0, 360.0f)) < 0.01f, "and the world still holds still in the room");
	seat_turn_to(&run, &head, 100.0f, 0.0f);
	seat_hold(&run, head, 30);
	halo_stereo_seat_look_fade(&dim, &blur);
	printf("  55 deg past: dim %.3f, blur %.3f\n", dim, blur);
	check(dim == 1.0f, "past the black angle the view is black");
	/* paused: neither the look nor the clamp runs; the head turns further */
	for (frame = 0; frame < 30; frame++) {
		set_pose((100.0f + (float)frame) * DEGREES, 0.0f, 0.0f);
		halo_stereo_frame_begin();
		halo_stereo_seat_look_fade(&dim, &blur);
		held &= dim == 1.0f;
	}
	set_pose(100.0f * DEGREES, 0.0f, 0.0f);
	halo_stereo_frame_begin();
	check(held, "paused in the seat, the fade holds");
	seat_look_fade_blur_setting = 0;
	halo_stereo_seat_look_fade(&dim, &blur);
	check(dim == 1.0f && blur == 0.0f, "display.seat_look_fade_blur false: dimmed, never blurred");
	seat_look_fade_blur_setting = 1;
	/* back inside the limit at once: eased out, never a flash */
	last = 1.0f;
	for (frame = 0; frame < 30; frame++) {
		seat_step(&run, 0.0f, 0.0f);
		halo_stereo_seat_look_fade(&dim, &blur);
		worst_step = fmaxf(worst_step, fabsf(dim - last));
		last = dim;
	}
	printf("  the head back to 0 in a frame: the fade's largest change a frame %.3f, now %.3f\n", worst_step, dim);
	check(worst_step <= SEAT_LOOK_FADE_RATE / 30.0f + 1e-4f && dim == 0.0f,
		"a jump in the head's angle eases the fade (at most 1 in 0.25 s), down to clear");
	/* a smoothstep in between */
	expected = halo_stereo_seat_look_fade_ease(0.0f, 15.0f * DEGREES, 5.0f * DEGREES, 45.0f * DEGREES, 10.0f);
	check(fabsf(expected - 0.15625f) < 1e-4f, "10 deg into the 40 deg ramp: a smoothstep's 0.156");
	/* leaving the seat with the head past the limit: the fade goes */
	head = 0.0f;
	seat_turn_to(&run, &head, 80.0f, 0.0f);
	seat_hold(&run, head, 30);
	seated = 0;
	seat_hold(&run, head, 30);
	halo_stereo_seat_look_fade(&dim, &blur);
	printf("  out of the seat: dim %.3f\n", dim);
	check(dim == 0.0f, "out of the seat (no clamp) the view is clear");
	/* the setting off */
	seat_start(&run, "snap");
	seat_look_fade_setting = 0;
	head = 0.0f;
	seat_turn_to(&run, &head, 120.0f, 0.0f);
	seat_hold(&run, head, 30);
	halo_stereo_seat_look_fade(&dim, &blur);
	check(dim == 0.0f && blur == 0.0f, "display.seat_look_fade false: never faded");
	seated = 0;
	/* the presenter's veil (halo_stereo_veil.h): fully blurred, it's the
	immersive cutscene's dimmed outside; nothing blurred or dimmed, the
	picture; dim 1, black */
	{
		const float sharp[3] = { 0.8f, 0.4f, 0.1f }, blurred[3] = { 0.3f, 0.6f, 0.9f };
		float dimmed[3], worst = 0.0f, sharp_luma = halo_stereo_veil_luma(sharp[0], sharp[1], sharp[2]);
		float blurred_luma = halo_stereo_veil_luma(blurred[0], blurred[1], blurred[2]);
		int channel;

		halo_stereo_cutscene_dimmed(blurred, 0.6f, dimmed);
		for (channel = 0; channel < 3; channel++) {
			worst = fmaxf(worst, fabsf(halo_stereo_veil(sharp[channel], blurred[channel], sharp_luma, blurred_luma,
				1.0f, 0.6f) - dimmed[channel]));
			worst = fmaxf(worst, fabsf(halo_stereo_veil(sharp[channel], blurred[channel], sharp_luma, blurred_luma,
				0.0f, 0.0f) - sharp[channel]));
			worst = fmaxf(worst, fabsf(halo_stereo_veil(sharp[channel], blurred[channel], sharp_luma, blurred_luma,
				0.5f, 1.0f)));
		}
		check(worst < 1e-6f, "the veil: the cutscene's dimmed outside fully blurred, the picture unveiled, black at "
			"dim 1");
	}
	seat_look_fade_setting = 0;
}

int main(void)
{
	pole_crossing();
	pole();
	pan("level pan at 0 deg pitch", 0.0f, 0.0f);
	pan("level pan at -10 deg pitch", -10.0f * DEGREES, 0.0f);
	/* the views' forward (0.005, 0.009, -1) from the session-1 log */
	pan("level pan at the views' tilt", asinf(0.009f), 0.0f);
	pan("level pan at -10 deg, 5 deg roll", -10.0f * DEGREES, 5.0f * DEGREES);
	pan("level pan at 30 deg pitch", 30.0f * DEGREES, 0.0f);
	turning();
	vignette_easing();
	vignette_binocular();
	film_mapping_check();
	film_hold_reason();
	third_person();
	third_person_entries();
	paused();
	replay_look();
	interpolated_turns();
	glide();
	seat_yaw_limit();
	seat_leftover_kept();
	seat_look_fade();
	third_person_ends();
	first_person_cutscenes();
	released_cutscene();
	look_disabled();
	cutscene_end();
	cutscene_expansion();
	title_bars();
	expansion_window();
	film_to_window();
	film_handover_yaw();
	expansion_memory();
	cutscene_rulings();
	immersive_cutscene();
	split_screen();
	if (failures)
	{
		printf("stereo head probe: %d failed\n", failures);
		return 1;
	}
	printf("stereo head probe: PASS\n");
	return 0;
}
