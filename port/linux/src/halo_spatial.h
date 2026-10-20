/* Tracked-controller aiming's poses and buttons, as they cross from the host
(port/ios/host/host_spatial.m, the imports host_spatial_sample and
host_spatial_feedback in guest_host.h) to the guest
(port/linux/game/controller_aim_game.c). The structs cross as they are:
fixed-width fields only, and tools/ios_test.py pins their layouts
(port/ios/tests/gpu_layout_probe.c). */
#ifndef HALO_SPATIAL_H
#define HALO_SPATIAL_H

#include <stdint.h>

enum halo_spatial_state
{
	HALO_SPATIAL_NONE,          /* not connected */
	HALO_SPATIAL_UNTRACKED,
	HALO_SPATIAL_ORIENTATION,   /* orientation only: position is the last good one */
	HALO_SPATIAL_TRACKED,
	HALO_SPATIAL_LOW_ACCURACY   /* position and orientation, low accuracy */
};

enum halo_spatial_reason
{
	HALO_SPATIAL_READY,          /* controller aim can run */
	HALO_SPATIAL_OFF,            /* input.aim isn't "controller" */
	HALO_SPATIAL_NO_CONTROLLERS,
	HALO_SPATIAL_ONE_CONTROLLER,
	HALO_SPATIAL_NEEDS_27,
	HALO_SPATIAL_DENIED,
	HALO_SPATIAL_WAITING,        /* authorization or accessory loading pending */
	/* the aiming controller reads well above the head for a while: its
	position and the head's are not in one frame (host_spatial_pose.h's
	misplaced test), so the hand can't aim until they agree */
	HALO_SPATIAL_MISPLACED
};

/* host_spatial_sample's kind */
enum { HALO_SPATIAL_SAMPLE_GAME, HALO_SPATIAL_SAMPLE_DRAW };

/* a location on the controller in the room's level frame: the room's axes
turned into the game's (x the room's forward, -Z; y its left, -X; z up, +Y),
unit axes; position in meters from the head */
struct halo_spatial_location
{
	float forward[3];   /* the barrel (the aim location's forward axis, HOST_SPATIAL_AIM_FORWARD) */
	float left[3];
	float up[3];
	float position[3];
};

struct halo_spatial_hand
{
	int32_t state;      /* enum halo_spatial_state */
	int32_t held;       /* ar_accessory_anchor_is_held */
	int32_t chirality;  /* 0 unknown, 1 left, 2 right (ar_accessory_anchor_get_held_chirality) */
	struct halo_spatial_location aim, grip;
};

/* XINPUT_GAMEPAD's layout (port/include/xdk/xdk_pdb.h:1253), padded to 4 */
struct halo_spatial_gamepad
{
	uint16_t buttons;
	uint8_t analog[8];  /* A, B, X, Y, black, white, left trigger, right trigger */
	int16_t thumb[4];   /* left x, left y, right x, right y */
	uint16_t pad;
};

struct halo_spatial_sample
{
	int32_t reason;          /* enum halo_spatial_reason */
	int32_t aiming_hand;     /* 0 left, 1 right (input.aim_hand) */
	int32_t synthetic;       /* 1: debug.test_controller; 2: debug.controller_replay */
	/* the head's forward yaw in the room's level frame (radians, left
	positive) at this sample's latch: the game sample's, the last
	host_stereo_frame's; the drawing sample's, this frame's. 0 with no head */
	/* (the game sample's head is one latch old by design: the look holds that
	latch's head, and ruling 1's body yaw makes a fresh controller pose exact
	against it. Do not "fix" it to the device anchor at the sample's time) */
	float head_yaw;
	int32_t head_level;      /* 1 while that head's pitch is under 85 degrees (or there is no head) */
	struct halo_spatial_hand hands[2];   /* 0 left, 1 right */
	/* the game sample only: 1 with both Sense controllers' buttons in
	gamepad (host_spatial_gamepad.c's table), read at the aiming hand's
	anchor timestamp (live while that anchor is stale); xinput_sdl.c merges
	it into port 0 */
	int32_t gamepad_valid;
	struct halo_spatial_gamepad gamepad;
};

struct halo_spatial_feedback
{
	int32_t aiming;          /* 1 while a hand aims: the host hides the upper limbs */
	int32_t buzz_serial;     /* a new value asks for one short buzz on the aiming hand */
	/* the game's left and right motors, 0 to 1, as XInputSetState last had
	them for port 0 (zeros while the game is paused or in a menu); sent once
	a frame, and the host stops them when they stop coming */
	float rumble[2];
};

/* port/linux/game/controller_aim_game.c, for the port's callers. Off iOS
(no host) both samples are reason HALO_SPATIAL_OFF. */
/* once a frame from XInputGetState's port 0 (input_update): the game sample;
a second call in the same frame keeps the first's, so the buttons of two
instants never mix */
void halo_controller_sample_game(void);
/* once a frame from halo_stereo_frame_begin, after the eyes are latched: the drawing sample */
void halo_controller_sample_draw(void);
/* for xinput_sdl.c's XInputSetState, port 0 (the input thread's): the
game's two motors, 0 to 65535, for the next frame's feedback to the host */
void halo_controller_rumble(unsigned int left_motor, unsigned int right_motor);
const struct halo_spatial_sample *halo_controller_game_sample(void);
const struct halo_spatial_sample *halo_controller_draw_sample(void);

/* for player_control.c (declared there too): after the look's update, as
the action is built: 1, the action's aim (radians, the game's convention:
world angles) and its pad when a hand aims this frame (blend above 0, or a
press latched under one); 0 when the look does, and the caller writes pad 0.
Network controller sessions also return 1 for held/simulated aim without
tracking and independent seat stick aim; their action pad is always 0. */
int halo_controller_action_aim(short local_player_index, float look_yaw, float look_pitch,
	unsigned long control_flags, float *yaw, float *pitch, short *pad);
/* 1 while a hand aims for this local player (the magnetism block's gate) */
int halo_controller_hand_aiming(short local_player_index);
/* for players.c and units.c (declared there too): the pad of each player's
newest action, by absolute player index, set every tick (a static array
over the players, not game state); the hand flag; and the action's delta
(look yaw - aim yaw), for players.c's control-panel test */
void halo_player_hand_aim_set(long player_absolute_index, short pad);
int halo_player_hand_aim(long player_absolute_index);
int halo_player_hand_aim_delta(long player_absolute_index, float *delta_yaw);
/* for players.c: controller_facing_unpack */
int halo_action_facing_unpack(short pad, float *delta_yaw);
/* source/game/player_control.c, for the glue: the seat's yaw arc on a world
yaw (radians), the stick's clamp, shared */
float player_control_seat_yaw_clamped(short local_player_index, float yaw);
/* for aim_assist.c (declared there too): the eye ray while controller aim
owns this local player's direction. Local play takes the blended/latched
hand ray; network play takes the actual clipped/rate-limited unit aim,
including held aim without a current pose and ordinary seat stick aim. */
int halo_controller_aim_ray(short local_player_index, float origin[3], float direction[3]);
/* Network controller sessions keep HMD look local even while tracking is
unavailable. Ordinary network facing carries hand aim on foot and stick aim
in seats; drawing/targeting use the actual simulated unit aim. */
int halo_controller_network_head_camera(short local_player_index);
/* for render.c (declared there too), once a frame after the eye cameras
turn (halo_stereo_eye_yaw) and before the eye passes: the drawing sample's
barrel and sightline from the eyes' position (the camera's, world units,
raised by display.eye_height_offset as the eyes are: halo_stereo_eye_frame),
and what the eye sees along it. It writes nothing the tick reads */
void halo_controller_draw_update(short local_player_index, const float camera_position[3]);
/* the drawing state, for the reticle (Task 10), the gun (Task 11) and the
projectiles (Task 13); world units and unit vectors. Every field but active
is stale while active is 0 (the last active frame's, or zero): read them
only while it is 1 */
struct halo_controller_draw
{
	int active;                 /* a hand aims and the drawing sample has a level body yaw */
	float blend;                /* the action's blend (track times gate), for easing the gun and the reticle */
	float eye[3];               /* E: the eyes' position (the eye cameras' frame's origin) */
	float barrel_origin[3], barrel_forward[3], barrel_up[3];
	float sightline[3];         /* unit, from E; its yaw inside a seat's arc, as the action's */
	float aim_point[3];         /* P from the drawing sample */
	float hit_point[3];         /* the first hit from E along the sightline (the reticle's place) */
	float hit_distance;         /* units from E; CONTROLLER_SKY_UNITS (200) for the sky */
	/* the hit is nearer than P, with P beyond CONTROLLER_NEAR_UNITS: the
	barrel's ray passes what the eye's line doesn't. Inside that the near
	filter can temporarily aim away from a moving P without cover. */
	int past_cover;
	int dim;                    /* the track's dim, or the seat's yaw arc holding the sightline back */
	/* the reticle's distance from E along the sightline, units: the hit
	pulled 2% toward the eye, low-passed (50 ms toward nearer, 150 ms toward
	farther), restarting at the target when active turns on */
	float reticle_distance;
	/* a line from the muzzle to the reticle, laser_opacity (above 0, to 1)
	of its own opacity: input.laser "on", "close" while the reticle is
	within input.laser_distance (fading in over its last half meter), or
	past_cover; eased over a tenth of a second, so it never pops */
	int laser;
	float laser_opacity;
};
const struct halo_controller_draw *halo_controller_draw_state(void);
/* for xinput_sdl.c's chords: the reticle's easing (input.reticle_ease) on
and off, the solved arms' turn limit (input.arms_turn_limit) between 35 and
45 degrees, and their follow (input.arms_follow) between 0 and 0.5, for the session */
void halo_controller_reticle_ease_toggle(void);
void halo_controller_arms_turn_limit_cycle(void);
void halo_controller_arms_follow_cycle(void);
/* for xinput_sdl.c's chord: the first-person gun's smoothing
(input.weapon_smoothing) on and off for the session */
void halo_controller_weapon_smoothing_toggle(void);
/* the first-person arms' mode (input.first_person_arms) to the next (direction 1) or the previous one */
void halo_controller_arms_cycle(int direction);
/* for first_person_weapons.c and rasterizer_xbox.c (declared there too):
1 and the frame the first-person weapon takes this frame while a hand
aims: render.camera's position, the sightline's forward eased from the
camera's by the action's blend, and the barrel's up made square to it
(controller_weapon_frame: the camera's up eased toward the barrel's by the
same blend, so the gun leaves no roll behind at blend 0). 0 otherwise, and
on any frame halo_controller_draw_update didn't leave the drawing state
active (a mono frame), and the camera's frame stands */
int halo_controller_weapon_frame(short local_player_index, const float camera_forward[3], const float camera_up[3],
	float forward[3], float up[3]);
/* for render.c (declared there too), once a stereo frame before the
passes: the zoomed pass's camera this frame, which it builds zoomed or not:
its forward and up (world), its half tangents across and up (the view's,
halo_stereo_zoom_view, over the magnification) and the magnification (1
unzoomed). While zoomed the action keeps the hand's aim only while its
reticle shows in sight in the zoomed picture (inside what both eyes see of
it, within the eyes' shared view: halo_stereo_shared_view, latched here), by
the camera latched the frame before at the zoom's magnification then
(controller_zoom_project_sight, ruling 9) */
void halo_controller_zoom_view(const float forward[3], const float up[3], float tangent_across, float tangent_up,
	float magnification);
/* for d3d8_device.c, in a zoomed frame: 1 and where the drawing sightline
shows in this frame's zoomed picture (the eyes' frame: x right, y up, z
back; unit) while a hand aims; 2, with shown untouched, while a hand aims
past the picture's edge, where there is no reticle (the default: the hand
keeps the aim at any angle); 0 while no hand aims. With input.zoom_takeover
the action holds the hand's aim inside the picture, and the reticle is
clamped onto the picture's edge if this frame's camera has moved past it,
so 2 never comes */
int halo_controller_zoom_reticle(float shown[3]);
/* for render.c (declared there too), in each pass before the world's
window, the eyes' or the zoomed one: with debug.test_controller in the
side-by-side view, a small cross at the drawing state's hit point (the
game's debug lines) */
void halo_controller_debug_marker(void);
/* for xinput_sdl.c's live chord: input.aim_pitch_offset up or down a degree
(direction 1 tilts the barrel further down), within -10 to 45, for the
session; logged and shown */
void halo_controller_pitch_offset_step(int direction);
/* aim assist's strength (port/linux/game/aim_assist_strength.h), for
aim_assist.c (declared there too): what a player's shot takes this tick, by
the hand flag of its newest action, and what a local player's targeting query
takes, both the game's own (1) unless a hand aims in a single player game;
and a weapon's autoaim and deviation angles (radians) at a strength */
float halo_aim_assist_player_strength(long player_absolute_index);
float halo_aim_assist_local_strength(short local_player_index);
void halo_aim_assist_apply(float strength, float *autoaim_angle, float *deviation_angle);
/* for xinput_sdl.c's live chord: input.aim_assist up or down half a step
(direction 1 up), within 0 to 2, for the session; logged and shown. In a
multiplayer game it changes nothing and says so */
void halo_aim_assist_step(int direction);
/* for xinput_sdl.c's live chord: on-target steadying (input.aim_steady,
port/linux/game/controller_aim.h) to its next setting, off, default (1),
strong (2) and off again, for the session; logged and shown. In a
multiplayer game it changes nothing and says so */
void halo_aim_steady_cycle(void);
/* for xinput_sdl.c's live chord: the aim ray's origin
(input.aim_origin_raise, port/linux/game/controller_aim.h) a tenth toward
the eyes (direction 1) or back toward the hand (-1), within 0 to 1; logged,
shown and written to the config, so the next launch keeps it */
void halo_controller_origin_raise_step(int direction);
/* for xinput_sdl.c's live chord: stereo's lens flares counted by each eye
(display.stereo_flare_counting, d3d8_device.c) on and off for the session;
logged and shown */
void halo_controller_flare_counting_toggle(void);
/* for xinput_sdl.c's live chord: the laser (input.laser,
port/linux/game/controller_aim.h) to its next mode, off, close and on, for
the session; logged and shown */
void halo_controller_laser_cycle(void);
/* for input_replay.c: 1 once any action this run carried a hand flag; 1 when
the configuration can aim with a hand (decided at the first tick); and the
local player's unit (position, facing yaw and aim yaw in degrees), 0 with no
unit */
int halo_controller_run_aimed(void);
int halo_controller_aim_configured(void);
int halo_controller_unit_state(float position[3], float *facing_yaw, float *aim_yaw);

#endif
