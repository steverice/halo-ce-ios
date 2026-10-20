/* Controller aim's game glue (input.aim = "controller"): the hand's aim
into the game's action, its gates, its messages and its log lines.

The samples. The game sample comes from XInputGetState's port 0, once a
frame, so the pose and the buttons of one instant reach the game in the
frame they were sampled; a second poll of port 0 in the same frame keeps the
first's, so the buttons of two instants never mix. The drawing sample comes
from halo_stereo_frame_begin, after the eyes are latched. Off iOS (the
desktop build, no host) both samples are reason HALO_SPATIAL_OFF, without a
host call.

The action (halo_controller_action_aim, from player_control.c as the action
is built, once a frame). The look stays the head's (player->desired_angles
is never written); the action's desired_facing carries the hand's aim in
world angles, in local play blended toward the look by tracking loss and by the gate
(seats, the film, cutscenes, and while zoomed the hand's aim
leaving the zoomed picture), and its pad carries the
hand flag and the look's yaw less the aim's (controller_facing_pack), from
which players.c turns the facing and the looking vectors back to the head.
Network on-foot actions instead carry ordinary facing with a zero pad:
existing peers aim, turn and move relative to that direction. Network seats
keep independent stick aim. Tracking loss holds the last valid hand aim
for 2 seconds, then eases to the head and stick until the hand recovers;
before any hand has been tracked in the session (a gamepad, one Sense
controller, tracking denied, an older SDK), network play aims with the head
and stick as input.aim = "head" does, until one is. The network reticle and targeting ray use the unit's
actual aiming vector, including its ordinary clipping and rate limits.
The hand's aim is the sightline from the eye (controller_sightline) to where
the barrel's ray meets the world, so the shot, which leaves from the eye,
goes where the barrel points. A press keeps the aim it was pressed with
until a tick takes it (controller_latch_update). Nothing here draws a random
number; under debug.test_controller both seeds are checked across the aim
path.

What follows the aim. The targeting query (aim_assist.c) looks along the
action's aim from the eye while a hand aims (halo_controller_aim_ray), so
autoaim and the crosshair's target are what the gun points at. Once a frame
before the eye passes, halo_controller_draw_update computes the drawing
sample's barrel and sightline as the action's (hand_sightline), from the
eyes' position (the render camera's, raised by display.eye_height_offset)
and with the body yaw from the eye cameras, and what the eye sees along it:
the reticle's place and its low-passed distance, for the reticle, the gun
and the drawn projectiles. That path writes nothing the tick reads. One
thing crosses from the render to the action: the zoomed camera render.c
latches each stereo frame (halo_controller_zoom_view, the zoom_view
struct), which the next frame's action reads for the zoom's gate.
Like the hand's pose it is input to the action, not game state: the
action carries what it decided, so a recording replays without it.

The hand flag per player (halo_player_hand_aim_set and its readers): the
pad of each player's newest action, by absolute player index, set every
tick by players_update_before_game for unit_update (no aiming clip for a
hand-aimed unit) and players.c's own tests. It is rebuilt from the action
each tick, so it is not game state.

Aim assist (aim_assist_strength.h). A hand keeps the bullets' autoaim: the
shot leaves the eye along the action's aim, so player_aim_projectile bends
it onto a target inside the weapon's autoaim cone as it does a stick's.
Magnetism (the look slowed and dragged by a target) stays off for a hand:
it turns the look, which is the head's. input.aim_assist scales the autoaim
cone of a hand-aimed shot (halo_aim_assist_player_strength, each tick, by
the action's hand flag) and of the targeting query that turns the reticle
red (halo_aim_assist_local_strength). A network game and a multiplayer game
type take the game's own cone whatever the setting says (fixed_rules), so
every machine decides hits alike. The setting is this machine's and the
live chord can step it, so, like display.eye_height_offset, a recording
replays alike only at the strength it was made with.

On-target steadying (input.aim_steady, controller_aim.h). A stick is
steadied on a target by the game's friction and adhesion, which slow and
drag the look; neither can slow a real hand, so a hand's tremor reaches the
reticle whole, magnified by a zoom. While the hand's aim is on a target (the
game's own autoaim level from the last targeting query along the hand's ray:
above 0 inside the autoaim cone, 1 where the reticle is red), the barrel's
low-pass (controller_zoom_stabilize) takes a longer time constant, by the
setting times input.aim_assist times that level, for the action and for the
drawing alike. The aim is never turned toward the target, and the cone and
the red reticle's rule are untouched: a shot still bends only where the
reticle is red, and a deliberate move still ends where the hand points. Off
in a network game and a multiplayer game type (aim_assist_fixed_rules). The
live chord (Back and a click of the right stick) cycles it off, default and
strong for the session; like the assist strength it is this machine's, not
part of a recording. By default the setting follows the zoom: none
unzoomed (a reticle that follows the hand late only while it is on an enemy
feels disconnected),
input.aim_steady_zoom_2x at 2x and input.aim_steady_zoom_8x at 8x; the chord
cycles by zoom, off, default and strong. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cseries.h"
#include "camera/director.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/hud_messaging.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "render/render.h"
#include "render/render_debug.h"
#include "tag_files/tag_groups.h"
#include "units/unit_definitions.h"
#include "units/units.h"

#include "aim_assist_strength.h"
#include "controller_aim.h"
#include "first_person_arms.h"
#include "../src/halo_spatial.h"
#include "../src/halo_stereo.h"

/* port/linux/src/xbox_kernel.c: presented frames, and the frame clock (with
debug.fixed_timestep, presented frames at 1/30 s each) */
unsigned long platform_clock_frames(void);
int platform_fixed_timestep(void);
double halo_frame_trace_milliseconds(void);
void platform_log(const char *format, ...);
/* port/linux/src/port_config.c */
const char *config_string(const char *name);
double config_real(const char *name);
int config_boolean(const char *name);
/* port/linux/game/cinematic_screen.c */
int halo_look_disabled_first_person(void);
#ifdef HALO_IOS
/* port/ios/host/host_spatial.m, imported by the guest (guest_host.h) */
void host_spatial_sample(int kind, struct halo_spatial_sample *sample);
void host_spatial_feedback(const struct halo_spatial_feedback *feedback);
#endif

#define CONTROLLER_PI 3.14159265f
#define DEGREES(radians) ((radians) * (180.0f / CONTROLLER_PI))
/* input.aim_pitch_offset's range (degrees) */
#define PITCH_OFFSET_MINIMUM -10.0f
#define PITCH_OFFSET_MAXIMUM 45.0f
/* "low accuracy tracking" logs at most this often (seconds) */
#define LOW_ACCURACY_LOG_EVERY 5.0f

/* ---------- the samples */

static struct halo_spatial_sample game_sample = { .reason = HALO_SPATIAL_OFF, .aiming_hand = 1, .head_level = 1 };
static struct halo_spatial_sample draw_sample = { .reason = HALO_SPATIAL_OFF, .aiming_hand = 1, .head_level = 1 };
/* the frame the game sample was taken in, and whether one was */
static unsigned long game_sample_frame;
static int game_sampled;

static void sample(int kind, struct halo_spatial_sample *into)
{
#ifdef HALO_IOS
	host_spatial_sample(kind, into);
#else
	(void)kind;
	memset(into, 0, sizeof(*into));
	into->reason = HALO_SPATIAL_OFF;
	into->aiming_hand = 1;
	into->head_level = 1;
#endif
}

static void feedback(void);

/* the game's two motors as XInputSetState last had them, the left in the
high 16 bits: written on the game's input thread, read on the game thread */
static uint32_t rumble_motors;

void halo_controller_rumble(unsigned int left_motor, unsigned int right_motor)
{
	__atomic_store_n(&rumble_motors, (left_motor & 0xffff) << 16 | (right_motor & 0xffff), __ATOMIC_RELAXED);
}

/* for player_effects.c (declared there): input.blast_rumble, read once: 1
when a blast whose damage effect shakes the camera without a vibration of its
own should rumble the heavy motor (scale 0 to 1, for duration seconds), and
the log's line for it */
int halo_blast_rumble(float scale, float duration)
{
	static int on = -1;

	if (on < 0)
		on = config_boolean("input.blast_rumble");
	if (on)
		platform_log("rumble: a blast's camera shake as rumble: the heavy motor %.2f for %.2f s (input.blast_rumble) "
			"tick %ld", scale, duration, (long)game_time_get());
	return on;
}

void halo_controller_sample_game(void)
{
	unsigned long frame = platform_clock_frames();

	if (game_sampled && frame == game_sample_frame)
		return;
	game_sampled = 1;
	game_sample_frame = frame;
	sample(HALO_SPATIAL_SAMPLE_GAME, &game_sample);
	/* once a frame whatever the game is doing (a menu, a pause: no tick
	sends any), so the host has the motors' zeros as soon as the game does */
	feedback();
}

void halo_controller_sample_draw(void)
{
	sample(HALO_SPATIAL_SAMPLE_DRAW, &draw_sample);
}

const struct halo_spatial_sample *halo_controller_game_sample(void)
{
	return &game_sample;
}

const struct halo_spatial_sample *halo_controller_draw_sample(void)
{
	return &draw_sample;
}

/* ---------- the hand flag per player */

static short player_pads[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
/* any tick's action this run carried a hand flag (a replay's too: the pad is
set after the recording's action replaces the live one) */
static int run_aimed;

void halo_player_hand_aim_set(long player_absolute_index, short pad)
{
	if (player_absolute_index >= 0 && player_absolute_index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
	{
		player_pads[player_absolute_index] = pad;
		if (pad & 1)
			run_aimed = 1;
	}
}

int halo_controller_run_aimed(void)
{
	return run_aimed;
}

int halo_player_hand_aim_delta(long player_absolute_index, float *delta_yaw)
{
	*delta_yaw = 0.0f;
	if (player_absolute_index < 0 || player_absolute_index >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
		return 0;
	return controller_facing_unpack(player_pads[player_absolute_index], delta_yaw);
}

int halo_player_hand_aim(long player_absolute_index)
{
	float delta_yaw;

	return halo_player_hand_aim_delta(player_absolute_index, &delta_yaw);
}

int halo_action_facing_unpack(short pad, float *delta_yaw)
{
	return controller_facing_unpack(pad, delta_yaw);
}

/* ---------- the action's aim */

/* the hand's last tracked aim location (orientation-only aims with the live
orientation from that position), one for each sample */
struct hand_memory
{
	int known;
	struct halo_spatial_location location;
};

/* the barrel and the sightline from the eye, for one sample */
struct hand_sightline
{
	float origin[3], forward[3], up[3];  /* the barrel */
	float aim_point[3];                  /* P: where the barrel's ray meets the world */
	float direction[3];                  /* the sightline, unit, from the eye */
	float aim_distance;                  /* the eye's distance to P (units) */
	/* the eye's distance to where the barrel's ray meets the world, before
	input.reticle_ease moved P along the ray (the same without it) */
	float surface_distance;
	/* the range along the barrel's ray to that surface, and to P (the same
	without input.reticle_ease, and 0 both when a wall hides the hand) */
	float ray_range, eased_range;
};

/* local player 0's (the only one HEAD mode drives) */
static struct
{
	struct controller_body body;
	struct controller_near near;
	struct controller_range range;
	struct controller_zoom_filter zoom_filter;
	struct controller_track track;
	struct controller_latch latch;
	float gate;                     /* the gate ease's linear progress, 0 to 1 */
	/* the hand's last good aim (radians), and its last tracked aim location */
	int good_aim_known;
	float good_yaw, good_pitch;
	struct hand_memory good_location;
	long last_unit, last_time;      /* for controller_needs_reset */
	/* the session's first READY: from then on a sample that isn't READY is
	a lost hand (controller_track_update), not a hand that never aimed */
	int ever_ready;
	int hand_aiming;                /* this frame's action carries the hand's aim */
	/* a network game's share of hand aim (the gate's ease): it falls to 0 once the
	hand has been lost CONTROLLER_NETWORK_LOSS_SECONDS, and rises when it recovers */
	float network_hand;
	int network_head_aim;           /* and it is 0: the head and stick aim */
	/* a network action's facing is the hand's aim with a zero pad, so its
	throttle turns by the look's yaw less the aim's (halo_controller_network_throttle) */
	float network_move_delta;
	float ray_direction[3];         /* and that aim, unit (halo_controller_aim_ray) */
	/* under debug.test_controller: where the last hand aim's sightline from
	the eye meets the world (the shot's landing), for the draw log line */
	float shot_point[3];
	int dim;                        /* the track's dim this frame (the reticle's) */
	/* while zoomed, the hand's aim showed in the zoomed picture at the last
	test (controller_zoom_project's inside); 1 whenever unzoomed */
	int zoom_inside;
	short last_zoom_level;          /* the zoom level the last action saw, for the log */
	int buzz_serial;
	int last_state;                 /* the aiming hand's last state, for the low accuracy log */
	float since_low_accuracy_log;
	unsigned can_not_start_shown;   /* 1 << enum halo_spatial_reason */
	double clock;                   /* the frame clock's last reading (seconds), negative before one */
	int last_target;                /* the gate's last target, -1 none yet (logged as it changes) */
	int last_lost;                  /* the track's last lost, and its blend (logged as they change) */
	float last_track_blend;
	/* the previous action, logged once a second of game time with the unit
	the tick that took it left (debug.test_controller) */
	int previous_known;
	float previous_look_yaw, previous_look_pitch, previous_yaw, previous_pitch, previous_blend, previous_gate,
		previous_hit;
	short previous_pad;
	/* the previous frame's blend (track times gate) and the stick's aim it
	blended against, for the handover log */
	float previous_effective_blend, previous_stick_yaw, previous_stick_pitch, previous_hand_yaw, previous_hand_pitch;
	long logged_second;
} aim = { .last_unit = NONE, .clock = -1.0, .last_state = HALO_SPATIAL_NONE, .since_low_accuracy_log = 1000.0f,
	.logged_second = -1, .latch = { .tick = -1 }, .last_target = -1, .last_track_blend = 1.0f, .zoom_inside = 1, .network_hand = 1.0f,
	.last_zoom_level = NONE };

/* the drawing sample's own (halo_controller_draw_update): the body yaw from
the eye cameras, the near low-pass, the last tracked aim location, the frame
clock's last reading (negative before one) and the second last logged */
static struct
{
	struct controller_body body;
	struct controller_near near;
	struct controller_range range;
	struct controller_zoom_filter zoom_filter;
	struct hand_memory location;
	double clock;
	long logged_second;
	int reticle_known;          /* draw.reticle_distance holds a value to low-pass from */
	int seat_clamped;           /* this frame's sightline was turned back into the seat's yaw arc */
	float laser_opacity;        /* the laser's eased opacity (controller_laser_ease), 0 when no hand aims */
	/* the first-person gun's frame this frame: the sightline and the
	barrel's up, low-passed (controller_weapon_smooth) unless smoothing is off */
	struct controller_weapon_filter weapon_filter;
	float weapon_forward[3], weapon_up[3];
	float ray_range, eased_range;   /* this frame's hand_sightline's, for debug.aim_trace */
	/* the hand's running average offset from the eyes (the view's level
	frame: forward, left, up) and this frame's grip shift from it (world),
	for the "solved" arms */
	int hand_neutral_known;
	float hand_neutral[3], grip_shift[3];
	/* render.frame_index of the last frame draw_update left draw.active set:
	a frame that skips it (a mono frame while the eyes are away: a load, the
	space closed) must not turn the gun by the last stereo frame's state */
	long active_frame;
} drawing = { .clock = -1.0, .logged_second = -1, .active_frame = -1 };
static struct halo_controller_draw draw;

/* the zoomed pass's camera (halo_controller_zoom_view), latched by render.c
each stereo frame, zoomed or not: the action tests the hand's aim against
the last frame's, the zoomed reticle projects through this frame's */
static struct
{
	int known;
	long frame;                 /* render.frame_index it was latched in */
	float forward[3], up[3];    /* world */
	float tangents[2];          /* half tangents across and up, over the magnification */
	float magnification;        /* 1 unzoomed */
	/* where a reticle on the picture is in sight (halo_stereo_shared_view
	and HALO_STEREO_SHARED_VIEW_DEGREES, of the frame it was latched in) */
	struct controller_zoom_sight sight;
} zoom_view;

/* the settings, read once (settings) */
static int settings_read;
static int aim_controller;          /* input.aim = "controller" */
static int test_controller;         /* debug.test_controller or debug.controller_replay set */
static int aim_trace;               /* debug.aim_trace: a line every drawn frame while a hand aims */
static float pitch_offset_degrees;  /* input.aim_pitch_offset; the live chord steps it */
/* input.laser's mode (CONTROLLER_LASER_*); the live chord cycles it, and
its change shows from the next action */
static int laser_setting;
static int laser_message_pending;
static float laser_distance = CONTROLLER_LASER_DISTANCE_DEFAULT;  /* input.laser_distance, meters */
/* whether the line was drawn last frame, for the log; -1 before one */
static int laser_logged = -1;
/* input.aim_origin_raise, 0 (the hand) to 1 (the eyes): where the aim ray
starts (controller_origin_raise). The live chord steps it and writes it to
the config, and its step shows from the next action. Like the assist
strength it is this machine's: a recording replays alike only at the value
it was made with */
static float origin_raise;
static int origin_raise_message_pending;
/* input.weapon_smoothing (seconds; 0 none): the first-person gun's low-pass
(controller_weapon_smooth). The live chord switches it on and off, and its
change shows from the next action */
static float weapon_smoothing = CONTROLLER_WEAPON_SMOOTH_DEFAULT;
static int weapon_smoothing_on = 1;
static int weapon_smoothing_message_pending;
/* input.reticle_ease (seconds; 0 none): the aim point's range along the
barrel's ray, eased (controller_range_ease), for the action and the drawing
alike. The live chord switches it on and off */
static float reticle_ease = CONTROLLER_RANGE_EASE_DEFAULT;
static int reticle_ease_on = 1;
static int reticle_ease_message_pending;
/* input.weapon_turn_limit and input.weapon_roll_limit (degrees): the
"limited" arms' hold on the turned model (controller_weapon_limit) */
static float weapon_turn_limit = 25.0f, weapon_roll_limit = 15.0f;
/* input.first_person_arms ("turned", "limited", "solved": HALO_ARMS_*,
port/linux/game/first_person_arms.c): how the first-person model follows
the hand. The live chord (Back and the left stick) cycles it; arms_mode_live
holds the chord's choice over the setting once used. input.arms_follow is
the "solved" mode's position term: that share of the hand's displacement
from its running average (ARMS_NEUTRAL_SECONDS) moves the grip */
#define ARMS_NEUTRAL_SECONDS 10.0f
static int arms_mode = HALO_ARMS_SOLVED;
static int arms_mode_live = -1;
static int arms_mode_message_pending;
static float arms_follow = 0.5f;
/* the live chords' turn limit and follow steps, shown from the next action:
the turn limit goes between ARMS_TURN_LIMIT_NEAR and ARMS_TURN_LIMIT_FAR, the
follow between 0 and ARMS_FOLLOW_ON; arms_follow_live holds the chord's
choice over the setting once used */
#define ARMS_TURN_LIMIT_NEAR 35.0f
#define ARMS_TURN_LIMIT_FAR 45.0f
#define ARMS_FOLLOW_ON 0.5f
static float arms_follow_live = -1.0f;
static int arms_turn_limit_message_pending, arms_follow_message_pending;
/* input.zoom_takeover: 1, the zoom gate: zoomed, the head takes the aim
(and the reticle goes to the center) where the hand's reticle would leave
sight in the zoomed picture. 0, the default: the hand keeps the aim at any
angle, the reticle is wherever the zoomed picture shows the gun's line, and
past the picture's edge there is none; the shot follows the gun either way */
static int zoom_takeover;
/* display.eye_height_offset (world units), clamped as stereo.c clamps it:
the eyes, and so the real head the hand's pose is relative to, sit this far
above the game's eye. A setting, so the action path may read it */
static float eye_raise_setting;
#define EYE_RAISE_LIMIT 0.1f
/* a pitch offset step to show from the next action (the chord runs in XInputGetState) */
static int pitch_offset_message_pending;
/* input.aim_assist, 0 to AIM_ASSIST_STRENGTH_MAXIMUM; the live chord steps
it, and its step shows from the next action */
static float assist_setting = AIM_ASSIST_STRENGTH_DEFAULT;
static int assist_message_pending;
/* the strength the last hand-aimed shot or query was logged with, negative before one */
static float assist_logged = -1.0f;
/* on-target steadying's settings. By zoom, the default (steady_by_zoom):
input.aim_steady unzoomed (0, none), input.aim_steady_zoom_2x up to 2x and
input.aim_steady_zoom_8x at 8x (controller_steady_by_zoom). The live chord
leaves by zoom for one setting at every magnification, steady_setting (off,
the default, strong), then comes back; its change shows from the next action */
static int steady_by_zoom = 1;
static float steady_unzoomed, steady_zoom_2x = CONTROLLER_STEADY_DEFAULT, steady_zoom_8x = CONTROLLER_STEADY_MAXIMUM;
static float steady_setting;
static int steady_message_pending;
/* debug.aim_steady_level: from 0 up, how firmly on target the hand's aim
counts as, in place of the game's autoaim level (a test without an enemy);
negative, the game's */
static float steady_test_level = -1.0f;
/* whether the last steadying asked for was any, for the log; -1 before one */
static int steady_logged = -1;

static void settings(void)
{
	if (settings_read)
		return;
	settings_read = 1;
	aim_controller = !strcmp(config_string("input.aim"), "controller");
	/* a replayed pose stream (debug.controller_replay) is checked like a synthetic one */
	test_controller = config_string("debug.test_controller")[0] != 0 || config_string("debug.controller_replay")[0] != 0;
	aim_trace = config_boolean("debug.aim_trace");
	laser_setting = controller_laser_mode(config_string("input.laser"));
	laser_distance = (float)config_real("input.laser_distance");
	if (controller_laser_distance_clamp(laser_distance) != laser_distance)
	{
		platform_log("controller aim: input.laser_distance %g is outside %g to %g meters; using %g", laser_distance,
			CONTROLLER_LASER_DISTANCE_MINIMUM, CONTROLLER_LASER_DISTANCE_MAXIMUM,
			controller_laser_distance_clamp(laser_distance));
		laser_distance = controller_laser_distance_clamp(laser_distance);
	}
	weapon_smoothing = (float)config_real("input.weapon_smoothing");
	if (controller_weapon_smooth_clamp(weapon_smoothing) != weapon_smoothing)
	{
		platform_log("controller aim: input.weapon_smoothing %g is outside 0 to %g seconds; using %g", weapon_smoothing,
			CONTROLLER_WEAPON_SMOOTH_MAXIMUM, controller_weapon_smooth_clamp(weapon_smoothing));
		weapon_smoothing = controller_weapon_smooth_clamp(weapon_smoothing);
	}
	reticle_ease = (float)config_real("input.reticle_ease");
	if (controller_range_ease_clamp(reticle_ease) != reticle_ease)
	{
		platform_log("controller aim: input.reticle_ease %g is outside 0 to %g seconds; using %g", reticle_ease,
			CONTROLLER_RANGE_EASE_MAXIMUM, controller_range_ease_clamp(reticle_ease));
		reticle_ease = controller_range_ease_clamp(reticle_ease);
	}
	weapon_turn_limit = (float)config_real("input.weapon_turn_limit");
	weapon_roll_limit = (float)config_real("input.weapon_roll_limit");
	if (!(weapon_turn_limit >= 0.0f && weapon_turn_limit <= 90.0f))
		weapon_turn_limit = 25.0f;
	if (!(weapon_roll_limit >= 0.0f && weapon_roll_limit <= 90.0f))
		weapon_roll_limit = 15.0f;
	platform_log("controller aim: reticle ease %.2f s; limited arms: turn %.0f, roll %.0f degrees", reticle_ease,
		weapon_turn_limit, weapon_roll_limit);
	{
		const char *mode = config_string("input.first_person_arms");

		if (!strcmp(mode, "turned"))
			arms_mode = HALO_ARMS_TURNED;
		else if (!strcmp(mode, "limited"))
			arms_mode = HALO_ARMS_LIMITED;
		else
		{
			if (strcmp(mode, "solved"))
				platform_log("controller aim: input.first_person_arms \"%s\" isn't turned, limited or solved; using solved",
					mode);
			arms_mode = HALO_ARMS_SOLVED;
		}
		if (arms_mode_live >= 0)
			arms_mode = arms_mode_live;
		arms_follow = (float)config_real("input.arms_follow");
		if (!(arms_follow >= 0.0f && arms_follow <= 1.0f))
			arms_follow = 0.5f;
		if (arms_follow_live >= 0.0f)
			arms_follow = arms_follow_live;
	}
	zoom_takeover = config_boolean("input.zoom_takeover");
	if (zoom_takeover)
		platform_log("controller aim: input.zoom_takeover: zoomed, the head takes the aim where the hand's reticle "
			"leaves sight");
	origin_raise = (float)config_real("input.aim_origin_raise");
	if (controller_origin_raise_clamp(origin_raise) != origin_raise)
	{
		platform_log("controller aim: input.aim_origin_raise %g is outside 0 to 1; using %g", origin_raise,
			controller_origin_raise_clamp(origin_raise));
		origin_raise = controller_origin_raise_clamp(origin_raise);
	}
	if (origin_raise > 0.0f)
		platform_log("controller aim: aim origin raise %.2f (input.aim_origin_raise: 0 the hand, 1 the eyes)", origin_raise);
	if (laser_setting != CONTROLLER_LASER_OFF)
		platform_log("controller aim: laser %s (input.laser; close is within %.1f m)",
			controller_laser_mode_name(laser_setting), laser_distance);
	eye_raise_setting = (float)config_real("display.eye_height_offset");
	eye_raise_setting = eye_raise_setting != eye_raise_setting ? 0.0f :
		fmaxf(-EYE_RAISE_LIMIT, fminf(EYE_RAISE_LIMIT, eye_raise_setting));
	pitch_offset_degrees = (float)config_real("input.aim_pitch_offset");
	if (!(pitch_offset_degrees >= PITCH_OFFSET_MINIMUM && pitch_offset_degrees <= PITCH_OFFSET_MAXIMUM))
	{
		float clamped = pitch_offset_degrees > PITCH_OFFSET_MAXIMUM ? PITCH_OFFSET_MAXIMUM :
			pitch_offset_degrees < PITCH_OFFSET_MINIMUM ? PITCH_OFFSET_MINIMUM : 0.0f;

		platform_log("controller aim: input.aim_pitch_offset %g is outside %g to %g degrees; using %g",
			pitch_offset_degrees, PITCH_OFFSET_MINIMUM, PITCH_OFFSET_MAXIMUM, clamped);
		pitch_offset_degrees = clamped;
	}
	assist_setting = (float)config_real("input.aim_assist");
	if (aim_assist_strength_clamp(assist_setting) != assist_setting)
	{
		platform_log("controller aim: input.aim_assist %g is outside 0 to %g; using %g", assist_setting,
			AIM_ASSIST_STRENGTH_MAXIMUM, aim_assist_strength_clamp(assist_setting));
		assist_setting = aim_assist_strength_clamp(assist_setting);
	}
	{
		static const char *const names[3] = { "input.aim_steady", "input.aim_steady_zoom_2x",
			"input.aim_steady_zoom_8x" };
		float *const values[3] = { &steady_unzoomed, &steady_zoom_2x, &steady_zoom_8x };
		int index;

		for (index = 0; index < 3; index++)
		{
			*values[index] = (float)config_real(names[index]);
			if (controller_steady_clamp(*values[index]) != *values[index])
			{
				platform_log("controller aim: %s %g is outside 0 to %g; using %g", names[index], *values[index],
					CONTROLLER_STEADY_MAXIMUM, controller_steady_clamp(*values[index]));
				*values[index] = controller_steady_clamp(*values[index]);
			}
		}
		platform_log("controller aim: aim steadying by zoom: %.1f unzoomed, %.1f at 2x, %.1f at 8x", steady_unzoomed,
			steady_zoom_2x, steady_zoom_8x);
	}
	steady_test_level = (float)config_real("debug.aim_steady_level");
	if (steady_test_level >= 0.0f)
		platform_log("controller aim: debug.aim_steady_level %g: the hand's aim counts as that firmly on target",
			steady_test_level);
}

static int aim_assist_fixed_rules(void);
static float steady_strength(void);

/* the frame clock's time since its last reading, in seconds, at most a
tenth of a second (a load or a pause doesn't run a whole ease in one frame) */
static float frame_time_delta(double *clock)
{
	double now = platform_fixed_timestep() ? (double)platform_clock_frames() / 30.0 :
		halo_frame_trace_milliseconds() / 1000.0;
	double delta = *clock < 0.0 ? 0.0 : now - *clock;

	*clock = now;
	return (float)fmax(0.0, fmin(0.1, delta));
}

/* the HUD's message line for local player 0, and the log. The game's
wchar_t is 16-bit, so the ASCII text widens by hand */
static void show_message(const char *text)
{
	wchar_t message[96];
	size_t index;

	for (index = 0; text[index] && index < sizeof(message) / sizeof(message[0]) - 1; index++)
		message[index] = (wchar_t)(unsigned char)text[index];
	message[index] = 0;
	hud_print_message(0, message);
	platform_log("controller aim: message \"%s\"", text);
}

/* a yaw into the game's range, 0 to 2 pi */
static float yaw_in_range(float yaw)
{
	yaw = fmodf(yaw, 2.0f * CONTROLLER_PI);
	return yaw < 0.0f ? yaw + 2.0f * CONTROLLER_PI : yaw;
}

/* a yaw in degrees, -180 to 180, for the log */
static float yaw_degrees(float yaw)
{
	return DEGREES(remainderf(yaw, 2.0f * CONTROLLER_PI));
}

/* the look as the stick's action would aim it: player_aiming_vector_from_facing
tilts a first-person seat's aim with its vehicle (players.c), which the
hand's world aim skips. The hand blends against this, so a blend of 0 is
exactly the stick's aim and a handover eases the tilt in and out */
static void tilted_look(float look_yaw, float look_pitch, float *yaw, float *pitch)
{
	real_euler_angles2d look, tilted;
	real_vector3d vector;
	long player_index = local_player_get_player_index(0);

	*yaw = look_yaw;
	*pitch = look_pitch;
	if (player_index == NONE)
		return;
	look.yaw = look_yaw;
	look.pitch = look_pitch;
	player_aiming_vector_from_facing(player_index, &vector, &look);
	euler_angles2d_from_vector3d(&tilted, &vector);
	*yaw = tilted.yaw;
	*pitch = tilted.pitch;
}

/* the gate (spec section 4): 1 when the hand may aim for local player 0
(input.aim and the samples aside); *reason says why not */
static int gate_target(long unit_index, const char **reason)
{
	const struct halo_stereo_frame *frame = halo_stereo_frame();
	struct unit_datum *unit = unit_get(unit_index);
	director_perspective perspective;
	short connection = game_connection();

	*reason = "the view";
	if (frame->mode == HALO_STEREO_HEAD)
	{
		if (!halo_stereo_head_drives_look(0))
			return 0;
	}
	/* the Mac's test view */
	else if (frame->mode != HALO_STEREO_SIDE_BY_SIDE || !test_controller || frame->eye_count != 2 ||
		halo_stereo_film())
		return 0;
	/* a10's cryo pod */
	*reason = "the look disabled";
	if (halo_look_disabled_first_person())
		return 0;
	*reason = "dead";
	if (TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
		return 0;
	/* a seat whose view is first person aims with the hand, a third-person
	seat with the stick (the user's rule): the camera the seat wants, and the
	one on screen, which trails it after seating and is scripted in cutscene
	cameras */
	*reason = "the camera";
	if (director_desired_perspective(unit_index, &perspective) ||
		director_peek_perspective(0) != _director_perspective_first_person ||
		unit_get_aiming_unit_index(unit_index) != unit_index)
		return 0;
	*reason = "a network seat";
	if ((connection == _game_connection_network_client || connection == _game_connection_network_server) &&
		unit->object.parent_object_index != NONE)
		return 0;
	*reason = "";
	return 1;
}

/* port: a hand has been tracked this session (or is this frame). Until then
a network player has no hand aim to hold, so aims with the head and stick as
input.aim = "head" does. */
static int hand_tracked_this_session(void)
{
	return aim.ever_ready || halo_controller_game_sample()->reason == HALO_SPATIAL_READY;
}

/* This session predicate deliberately does not depend on the gate. Neither
a lost hand nor an old observer tick may expose head look once a hand has
aimed. */
static int network_controller_aim(void)
{
	const struct halo_stereo_frame *frame = halo_stereo_frame();
	short connection = game_connection();

	return aim_controller && hand_tracked_this_session() &&
		(connection == _game_connection_network_client || connection == _game_connection_network_server) &&
		(frame->mode == HALO_STEREO_HEAD ||
		 (frame->mode == HALO_STEREO_SIDE_BY_SIDE && test_controller && frame->eye_count == 2 && !halo_stereo_film()));
}

/* Head look stays local for the whole network session, but a gameplay ray
belongs to this rider only while that unit owns its ordinary aim. */
static int network_gameplay_aim_eligible(long unit_index)
{
	const struct halo_stereo_frame *frame = halo_stereo_frame();
	struct unit_datum *unit;
	director_perspective perspective;
	const char *reason;
	float ray[3];

	if (unit_index == NONE || !network_controller_aim())
		return 0;
	unit = unit_get(unit_index);
	if (unit->object.parent_object_index == NONE)
		return gate_target(unit_index, &reason);
	/* A network passenger aims with the stick, but the rider's own held
	weapon still needs a simulated-ray sightline. The zoom selector checks
	the actual seat, aiming unit and weapon ownership. */
	if (frame->mode == HALO_STEREO_HEAD && !halo_stereo_head_drives_look(0))
		return 0;
	if (halo_look_disabled_first_person() || TEST_FLAG(unit->object.damage_flags, _object_dead_bit) ||
		director_desired_perspective(unit_index, &perspective) ||
		director_peek_perspective(0) != _director_perspective_first_person ||
		unit_get_aiming_unit_index(unit_index) != unit_index)
		return 0;
	return halo_zoom_ordinary_aim_ray(0, ray);
}

/* the "can't start" messages, once a session each */
static void can_not_start_messages(const struct halo_spatial_sample *sample)
{
	const struct halo_stereo_frame *frame = halo_stereo_frame();
	const char *text;

	if (frame->mode != HALO_STEREO_HEAD || frame->eye_count != 2)
		return;
	switch (sample->reason)
	{
	case HALO_SPATIAL_ONE_CONTROLLER: text = "Controller aim needs both Sense controllers"; break;
	case HALO_SPATIAL_DENIED: text = "Controller aim needs accessory tracking permission"; break;
	case HALO_SPATIAL_NEEDS_27: text = "Controller aim needs visionOS 27"; break;
	case HALO_SPATIAL_MISPLACED:
		text = network_controller_aim() ? "Controller tracking is off: holding gameplay aim" :
			"Controller tracking is off: aiming with your head";
		break;
	default: return;
	}
	if (aim.can_not_start_shown & (1u << sample->reason))
		return;
	aim.can_not_start_shown |= 1u << sample->reason;
	show_message(text);
}

/* death, respawn, a revert, a map load */
static void reset(const char *reason)
{
	memset(&aim.latch, 0, sizeof(aim.latch));
	aim.latch.tick = -1;
	aim.near.known = 0;
	drawing.near.known = 0;
	aim.range.known = drawing.range.known = 0;
	aim.zoom_filter.known = drawing.zoom_filter.known = 0;
	drawing.reticle_known = 0;
	controller_track_reset(&aim.track);
	aim.gate = 0.0f;
	aim.network_hand = 1.0f;
	aim.network_head_aim = 0;
	aim.good_aim_known = 0;
	aim.previous_known = 0;
	aim.last_target = -1;
	aim.zoom_inside = 1;
	aim.last_lost = 0;
	aim.last_track_blend = 1.0f;
	platform_log("controller aim: reset (%s)", reason);
}

/* the first hit of a ray from start along vector (units), ignoring
ignore_object_index; 0 and no point when nothing is hit */
static int ray_hit(const float start[3], const float vector[3], long ignore_object_index, float point[3])
{
	struct collision_result collision;
	real_point3d origin;
	real_vector3d delta;

	origin.x = start[0];
	origin.y = start[1];
	origin.z = start[2];
	delta.i = vector[0];
	delta.j = vector[1];
	delta.k = vector[2];
	if (!collision_test_vector(_collision_test_for_projectiles_flags, &origin, &delta, ignore_object_index,
		&collision))
		return 0;
	point[0] = collision.point.x;
	point[1] = collision.point.y;
	point[2] = collision.point.z;
	return 1;
}

/* the hand's barrel and sightline, for the game sample (the action) and the
drawing sample (halo_controller_draw_update) alike: the barrel placed from
head (where the eyes are: the hand's pose is relative to the real head), the
test from head to its origin, the barrel's ray out to CONTROLLER_RAY_UNITS,
P, and the sightline from eye to P with its near low-pass. steady is the
on-target steadying the barrel's low-pass takes (steady_strength). The drawing path
passes the raised eye as both; the action passes the game's eye, which the
simulation fires from, and the head raised by display.eye_height_offset
above it, so the shot converges on the P the reticle shows. Both tests
ignore the unit's ultimate parent, the whole vehicle a seated unit rides, as
projectiles do (projectiles.c); on foot the unit itself. 0 while the hand
has no location yet */
static int hand_sightline(long unit_index, const struct halo_spatial_hand *hand, struct hand_memory *memory,
	struct controller_near *near, struct controller_range *range, struct controller_zoom_filter *zoom_filter,
	float body_yaw, const float head[3], const float eye[3], float time_delta, float steady,
	struct hand_sightline *out)
{
	struct halo_spatial_location location;
	long ignore_object_index = object_get_ultimate_parent(unit_index);
	float vector[3];
	int axis, eased = 0;

	if (hand->state == HALO_SPATIAL_TRACKED || hand->state == HALO_SPATIAL_LOW_ACCURACY)
	{
		memory->location = hand->aim;
		memory->known = 1;
	}
	if (!memory->known)
		return 0;
	location = memory->location;
	if (hand->state == HALO_SPATIAL_ORIENTATION)
	{
		/* the live orientation from the last good position */
		memcpy(location.forward, hand->aim.forward, sizeof(location.forward));
		memcpy(location.left, hand->aim.left, sizeof(location.left));
		memcpy(location.up, hand->aim.up, sizeof(location.up));
	}
	controller_zoom_stabilize(zoom_filter, &location, halo_zoom_magnification(0), steady, time_delta,
		aim.track.lost || !hand->held || hand->state == HALO_SPATIAL_NONE || hand->state == HALO_SPATIAL_UNTRACKED);
	controller_location_to_world(&location, body_yaw, head, pitch_offset_degrees * (CONTROLLER_PI / 180.0f),
		out->origin, out->forward, out->up);
	/* input.aim_origin_raise: the ray starts that share of the way from the
	hand to the head, with the hand's direction; everything below, and
	everything made from this sightline (the reticle, the shot's
	convergence, the targeting query, the zoom's reticle, the laser), takes
	the moved origin. At 0 nothing is touched */
	controller_origin_raise(out->origin, head, origin_raise);
	/* the barrel's origin hidden from the head (a wall between the head and
	the hand): the near point is the wall. (An origin at the head itself
	has nothing between) */
	for (axis = 0; axis < 3; axis++)
		vector[axis] = out->origin[axis] - head[axis];
	if (!(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2] > 1e-10f) ||
		!ray_hit(head, vector, ignore_object_index, out->aim_point))
	{
		for (axis = 0; axis < 3; axis++)
			vector[axis] = out->forward[axis] * CONTROLLER_RAY_UNITS;
		if (!ray_hit(out->origin, vector, ignore_object_index, out->aim_point))
		{
			for (axis = 0; axis < 3; axis++)
				out->aim_point[axis] = out->origin[axis] + vector[axis];
		}
		eased = 1;
	}
	for (axis = 0; axis < 3; axis++)
		vector[axis] = out->aim_point[axis] - eye[axis];
	out->surface_distance = sqrtf(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
	/* input.reticle_ease: P moves along the barrel's ray to the new surface
	over a twentieth of a second instead of at once, so the reticle and the
	shot glide across an outline (a wall between the head and the hand is
	not on the ray: nothing to ease, and the next ray starts afresh) */
	if (eased)
	{
		float span[3], raw, moved;

		for (axis = 0; axis < 3; axis++)
			span[axis] = out->aim_point[axis] - out->origin[axis];
		raw = sqrtf(span[0] * span[0] + span[1] * span[1] + span[2] * span[2]);
		moved = controller_range_ease(range, out->forward, location.position, raw, reticle_ease_on ? reticle_ease : 0.0f,
			time_delta);
		if (moved != raw)
			for (axis = 0; axis < 3; axis++)
				out->aim_point[axis] = out->origin[axis] + out->forward[axis] * moved;
		out->ray_range = raw;
		out->eased_range = moved;
	}
	else
	{
		range->known = 0;
		out->ray_range = out->eased_range = 0.0f;
	}
	for (axis = 0; axis < 3; axis++)
		vector[axis] = out->aim_point[axis] - eye[axis];
	out->aim_distance = sqrtf(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
	controller_sightline(near, eye, out->aim_point, out->forward, time_delta, out->direction);
	return 1;
}

/* the hand's aim this frame (radians), from the eye along the sightline to
where the barrel's ray meets the world; *hit is the eye's distance to that
point (units). 0 while the hand has no location yet */
static int hand_aim(long unit_index, const struct halo_spatial_hand *hand, float body_yaw, float time_delta,
	float *yaw, float *pitch, float *hit)
{
	struct hand_sightline sightline;
	real_point3d eye;
	float eye_point[3], head[3];

	unit_get_camera_position(unit_index, &eye);
	eye_point[0] = eye.x;
	eye_point[1] = eye.y;
	eye_point[2] = eye.z;
	/* the real head, which the hand's pose is relative to: the eyes, raised
	by display.eye_height_offset (along the world's up; the eyes' own up has
	the head's roll, which the tick doesn't know) */
	head[0] = eye_point[0];
	head[1] = eye_point[1];
	head[2] = eye_point[2] + eye_raise_setting;
	if (!hand_sightline(unit_index, hand, &aim.good_location, &aim.near, &aim.range, &aim.zoom_filter, body_yaw, head, eye_point,
		time_delta, steady_strength(), &sightline))
		return 0;
	*hit = sightline.aim_distance;
	controller_angles(sightline.direction, yaw, pitch);
	/* the shot's landing, for the draw log's check against the reticle */
	if (test_controller)
	{
		float vector[3];
		int axis;

		for (axis = 0; axis < 3; axis++)
			vector[axis] = sightline.direction[axis] * CONTROLLER_RAY_UNITS;
		if (!ray_hit(eye_point, vector, object_get_ultimate_parent(unit_index), aim.shot_point))
			for (axis = 0; axis < 3; axis++)
				aim.shot_point[axis] = eye_point[axis] + vector[axis];
	}
	return 1;
}

/* once a second of game time under debug.test_controller: the previous
action (the one the last tick took) with the unit that tick left */
static void tick_log(long unit_index, long time)
{
	struct unit_datum *unit;
	float delta_yaw;
	int hand;

	if (!test_controller || !aim.previous_known || time / TICKS_PER_SECOND == aim.logged_second)
		return;
	aim.logged_second = time / TICKS_PER_SECOND;
	unit = unit_get(unit_index);
	hand = controller_facing_unpack(aim.previous_pad, &delta_yaw);
	platform_log("controller aim: tick %ld look yaw %.2f pitch %.2f aim yaw %.2f pitch %.2f facing %.2f hand %d "
		"unit aim %.2f state %d blend %.3f gate %.3f throttle %.2f,%.2f hit %.2f",
		time, yaw_degrees(aim.previous_look_yaw), DEGREES(aim.previous_look_pitch), yaw_degrees(aim.previous_yaw),
		DEGREES(aim.previous_pitch), DEGREES(delta_yaw), hand,
		DEGREES(atan2f(unit->unit.aiming_vector.j, unit->unit.aiming_vector.i)), (int)unit->unit.animation.state,
		aim.previous_blend, aim.previous_gate, unit->unit.throttle.i, unit->unit.throttle.j, aim.previous_hit);
}

static void feedback(void)
{
#ifdef HALO_IOS
	struct halo_spatial_feedback out;
	uint32_t motors;

	memset(&out, 0, sizeof(out));
	out.aiming = aim.hand_aiming;
	out.buzz_serial = aim.buzz_serial;
	motors = __atomic_load_n(&rumble_motors, __ATOMIC_RELAXED);
	out.rumble[0] = (float)(motors >> 16) / 65535.0f;
	out.rumble[1] = (float)(motors & 0xffff) / 65535.0f;
	host_spatial_feedback(&out);
#endif
}

/* zoomed: 1 while the hand's aim (radians, world) shows in the
zoomed picture where its reticle is in sight (zoom_view.sight: the part both
eyes see, within the shared view), by the zoomed camera render.c latched
last frame at the zoom's magnification now (the frame a zoom starts, the
last frame's camera was unzoomed); the edge at 100% of that while inside,
re-entry within 90%. So the head takes the aim as the hand's reticle would
leave sight, not at the picture's edge, which the eyes' shared view stops
well short of. 1 before render.c latched a camera */
static int zoom_inside(float yaw, float pitch, long time)
{
	float direction[3], shown[3], tangents[2], magnification;
	int inside;

	if (!zoom_view.known)
		return 1;
	direction[0] = cosf(pitch) * cosf(yaw);
	direction[1] = cosf(pitch) * sinf(yaw);
	direction[2] = sinf(pitch);
	magnification = fmaxf(halo_zoom_magnification(0), 1.0f);
	tangents[0] = zoom_view.tangents[0] * zoom_view.magnification / magnification;
	tangents[1] = zoom_view.tangents[1] * zoom_view.magnification / magnification;
	inside = controller_zoom_project_sight(direction, zoom_view.forward, zoom_view.up, tangents, magnification,
		&zoom_view.sight, aim.zoom_inside, shown);
	if (inside != aim.zoom_inside)
	{
		/* where the hand points off the zoomed camera, and where the
		picture shows that (the camera's own frame at 1x, then at the
		zoom's), for the edge's checks */
		const float all[2] = { 1e9f, 1e9f };
		float hand[3] = { 0.0f, 0.0f, -1.0f }, at[3] = { 0.0f, 0.0f, -1.0f };
		int ahead = controller_zoom_project_clamped(direction, zoom_view.forward, zoom_view.up, all, 1.0f, hand) &&
			controller_zoom_project_clamped(direction, zoom_view.forward, zoom_view.up, all, magnification, at);

		platform_log("controller aim: zoom: the hand's aim %s the zoomed view tick %ld frame %lu (%.2fx): the hand "
			"%.1f degrees right, %.1f up of the view's center%s, shown %.1f right, %.1f up, %.1f off center; in sight "
			"to %.1f left, %.1f right, %.1f up, %.1f down, %.1f off center",
			inside ? "back inside" : "outside", time, platform_clock_frames(), magnification,
			DEGREES(atan2f(hand[0], -hand[2])), DEGREES(atan2f(hand[1], -hand[2])), ahead ? "" : " (behind it)",
			DEGREES(atan2f(at[0], -at[2])), DEGREES(atan2f(at[1], -at[2])),
			DEGREES(atan2f(sqrtf(at[0] * at[0] + at[1] * at[1]), -at[2])),
			DEGREES(atanf(zoom_view.sight.left)), DEGREES(atanf(zoom_view.sight.right)),
			DEGREES(atanf(zoom_view.sight.up)), DEGREES(atanf(zoom_view.sight.down)),
			DEGREES(atanf(zoom_view.sight.off_center)));
	}
	aim.zoom_inside = inside;
	return inside;
}

/* the aim path proper (halo_controller_action_aim): 1 when the action
carries the hand's aim */
static int action_aim(long unit_index, float look_yaw, float look_pitch, unsigned long control_flags,
	float *yaw, float *pitch, short *pad)
{
	const struct halo_spatial_sample *sample = halo_controller_game_sample();
	const struct halo_spatial_hand *hand = &sample->hands[sample->aiming_hand ? 1 : 0];
	struct controller_track_out track;
	long time = game_time_get();
	float time_delta = frame_time_delta(&aim.clock);
	float body_yaw, gate, blend, hand_yaw = look_yaw, hand_pitch = look_pitch, hit = 0.0f;
	float stick_yaw = look_yaw, stick_pitch = look_pitch;
	const char *not_why;
	int target, snap, state, held, latched, hand_known = 0;
	int needs_reset = controller_needs_reset(aim.last_unit, unit_index, aim.last_time, time);
	const char *reset_reason = unit_index != aim.last_unit ? "unit" : "time";

	aim.last_unit = unit_index;
	aim.last_time = time;
	if (!aim_controller)
		return 0;
	can_not_start_messages(sample);
	if (sample->reason == HALO_SPATIAL_READY)
		aim.ever_ready = 1;
	/* (nothing to reset, or to say, before a hand has aimed) */
	if (!aim.ever_ready)
		return 0;
	if (needs_reset)
		reset(reset_reason);
	if (pitch_offset_message_pending)
	{
		char text[64];

		pitch_offset_message_pending = 0;
		snprintf(text, sizeof(text), "Aim pitch offset %d degrees", (int)lroundf(pitch_offset_degrees));
		show_message(text);
	}
	if (assist_message_pending)
	{
		char text[64];

		assist_message_pending = 0;
		if (aim_assist_fixed_rules())
			snprintf(text, sizeof(text), "Aim assist is fixed in multiplayer");
		else
			snprintf(text, sizeof(text), "Aim assist %.1fx", assist_setting);
		show_message(text);
	}
	if (steady_message_pending)
	{
		char text[64];

		steady_message_pending = 0;
		if (aim_assist_fixed_rules())
			snprintf(text, sizeof(text), "Aim steadying is off in multiplayer");
		else if (steady_by_zoom)
			snprintf(text, sizeof(text), "Aim steadying by zoom (%.1f, %.1f at 2x, %.1f at 8x)", steady_unzoomed,
				steady_zoom_2x, steady_zoom_8x);
		else if (steady_setting <= 0.0f)
			snprintf(text, sizeof(text), "Aim steadying off");
		else
			snprintf(text, sizeof(text), "Aim steadying %.1fx%s", steady_setting,
				steady_setting >= CONTROLLER_STEADY_MAXIMUM ? " (strong)" : "");
		show_message(text);
	}
	if (reticle_ease_message_pending)
	{
		char text[64];

		reticle_ease_message_pending = 0;
		if (reticle_ease_on && reticle_ease > 0.0f)
			snprintf(text, sizeof(text), "Reticle easing on (%.2f s)", reticle_ease);
		else
			snprintf(text, sizeof(text), "Reticle easing off");
		show_message(text);
	}
	if (weapon_smoothing_message_pending)
	{
		char text[64];

		weapon_smoothing_message_pending = 0;
		if (weapon_smoothing_on && weapon_smoothing > 0.0f)
			snprintf(text, sizeof(text), "Gun smoothing on (%.2f s)", weapon_smoothing);
		else
			snprintf(text, sizeof(text), "Gun smoothing off");
		show_message(text);
	}
	if (arms_turn_limit_message_pending)
	{
		char text[64];

		arms_turn_limit_message_pending = 0;
		snprintf(text, sizeof(text), "Arms turn limit %.0f degrees", halo_first_person_arms_turn_limit());
		show_message(text);
	}
	if (arms_follow_message_pending)
	{
		char text[64];

		arms_follow_message_pending = 0;
		snprintf(text, sizeof(text), "Arms follow %.1f%s", arms_follow, arms_follow <= 0.0f ? " (the gun stays put)" : "");
		show_message(text);
	}
	if (arms_mode_message_pending)
	{
		arms_mode_message_pending = 0;
		show_message(arms_mode == HALO_ARMS_TURNED ? "Arms: turned whole (before)" :
			arms_mode == HALO_ARMS_LIMITED ? "Arms: turned, limited" : "Arms: solved at the grip");
	}
	if (origin_raise_message_pending)
	{
		char text[64];

		origin_raise_message_pending = 0;
		snprintf(text, sizeof(text), "Aim origin %.1f%s", origin_raise,
			origin_raise <= 0.0f ? " (the hand)" : origin_raise >= 1.0f ? " (the eye)" : " toward the eye");
		show_message(text);
	}
	if (laser_message_pending)
	{
		char text[64];

		laser_message_pending = 0;
		if (laser_setting == CONTROLLER_LASER_CLOSE)
			snprintf(text, sizeof(text), "Laser at close range (%.1f m)", laser_distance);
		else
			snprintf(text, sizeof(text), "Laser %s", laser_setting == CONTROLLER_LASER_ON ? "always on" : "off");
		show_message(text);
	}
	target = gate_target(unit_index, &not_why);

	/* a sample that isn't READY (a controller disconnected, the permission
	withdrawn) is a lost hand: the head takes over as from tracking loss */
	state = sample->reason == HALO_SPATIAL_READY ? hand->state : HALO_SPATIAL_NONE;
	held = sample->reason == HALO_SPATIAL_READY && hand->held;
	controller_track_update(&aim.track, state, held, time_delta, &track);
	/* (unzoomed, the low-pass keeps a history only while it steadies the
	aim on a target, and drops it itself otherwise) */
	if (track.use_held)
		aim.zoom_filter.known = aim.range.known = 0;
	aim.since_low_accuracy_log = fminf(aim.since_low_accuracy_log + time_delta, 1000.0f);
	if (state == HALO_SPATIAL_LOW_ACCURACY && aim.last_state != HALO_SPATIAL_LOW_ACCURACY &&
		aim.since_low_accuracy_log >= LOW_ACCURACY_LOG_EVERY)
	{
		aim.since_low_accuracy_log = 0.0f;
		platform_log("controller aim: low accuracy tracking");
	}
	aim.last_state = state;
	/* loss and recovery, for the Mac's loss checks */
	if (aim.track.lost != aim.last_lost)
	{
		aim.last_lost = aim.track.lost;
		platform_log("controller aim: hand %s tick %ld frame %lu", aim.track.lost ? "lost" : "recovered", time,
			platform_clock_frames());
	}
	if (track.blend <= 0.0f && aim.last_track_blend > 0.0f)
		platform_log(network_controller_aim() ?
			"controller aim: gameplay aim held %.3f s after the loss, tick %ld frame %lu" :
			"controller aim: head aims %.3f s after the loss, tick %ld frame %lu", aim.track.lost_for, time,
			platform_clock_frames());
	aim.last_track_blend = track.blend;
	aim.dim = track.dim;

	/* the hand's aim, wherever it may hold the aim after the gate's ease
	(the gate open, or still easing out) */
	body_yaw = controller_body_yaw(&aim.body, look_yaw, sample->head_yaw, sample->head_level);
	if (aim.body.known && track.blend > 0.0f && (target || aim.gate > 0.0f))
	{
		if (track.use_held || !hand_aim(unit_index, hand, body_yaw, time_delta, &hand_yaw, &hand_pitch, &hit))
		{
			/* the hand's last good aim, or the look before it had one */
			hand_yaw = aim.good_aim_known ? aim.good_yaw : look_yaw;
			hand_pitch = aim.good_aim_known ? aim.good_pitch : look_pitch;
		}
		else
		{
			aim.good_aim_known = 1;
			aim.good_yaw = hand_yaw;
			aim.good_pitch = hand_pitch;
		}
		/* the seat's arc on the hand's world yaw, as on the stick's */
		hand_yaw = player_control_seat_yaw_clamped(0, hand_yaw);
		hand_known = 1;
	}

	/* a change to the look eases over 200 ms; back to the hand snaps.
	Zoomed, the hand keeps the aim wherever it points. With
	input.zoom_takeover it keeps it only while its reticle is in sight in
	the zoomed picture (zoom_inside); leaving that hands the aim to the look
	at once, and coming back inside 90% of it hands it back at once.
	Zooming itself changes nothing */
	if (test_controller && player_control_get_zoom_level(0) != aim.last_zoom_level)
	{
		aim.last_zoom_level = player_control_get_zoom_level(0);
		platform_log("controller aim: zoom level %d tick %ld frame %lu (%.2fx)", aim.last_zoom_level, time,
			platform_clock_frames(), halo_zoom_magnification(0));
	}
	snap = target == 1;
	if (!zoom_takeover || network_controller_aim())
		aim.zoom_inside = 1;
	else if (target && player_control_get_zoom_level(0) != NONE)
	{
		if (hand_known && !zoom_inside(hand_yaw, hand_pitch, time))
		{
			target = 0;
			snap = 1;
			not_why = "outside the zoomed view";
		}
	}
	else if (player_control_get_zoom_level(0) == NONE)
		aim.zoom_inside = 1;
	gate = controller_gate_ease(&aim.gate, target, time_delta, snap);
	if (target != aim.last_target)
	{
		aim.last_target = target;
		if (target)
			platform_log("controller aim: gate 1 tick %ld frame %lu", time, platform_clock_frames());
		else
			platform_log("controller aim: gate 0 tick %ld frame %lu (%s)", time, platform_clock_frames(), not_why);
	}
	/* what the player is told, while the hand has the aim (the gate's own
	changes say nothing) */
	if (aim.gate > 0.0f)
	{
		if (track.message == 1)
			show_message(network_controller_aim() ? "Holding gameplay aim" : "Aiming with your head");
		else if (track.message == 2)
			show_message("Aiming with the controller");
		if (track.buzz)
			aim.buzz_serial++;
	}

	blend = aim.body.known ? track.blend * gate : 0.0f;
	if (network_controller_aim())
	{
		/* Head look stays local. Loss and gate easing hold the last valid
		hand direction instead of mixing the headset into ordinary facing,
		until the hand has been lost CONTROLLER_NETWORK_LOSS_SECONDS: then the
		head and stick aim, eased over, and the hand eases back in on recovery. */
		float hand_share = controller_gate_ease(&aim.network_hand,
			!(aim.track.lost && aim.track.lost_for >= CONTROLLER_NETWORK_LOSS_SECONDS), time_delta, 0);

		aim.network_head_aim = aim.network_hand <= 0.0f;
		blend = aim.good_aim_known ? hand_share : 0.0f;
		if (blend > 0.0f)
		{
			*yaw = yaw_in_range(controller_yaw_blend(look_yaw, aim.good_yaw, blend));
			*pitch = look_pitch + blend * (aim.good_pitch - look_pitch);
		}
	}
	else if (blend > 0.0f)
	{
		tilted_look(look_yaw, look_pitch, &stick_yaw, &stick_pitch);
		*yaw = yaw_in_range(controller_yaw_blend(stick_yaw, hand_yaw, blend));
		*pitch = stick_pitch + blend * (hand_pitch - stick_pitch);
	}
	/* the facing and the look turn back to the stick's aim's yaw (players.c):
	on foot the look's own, and continuous with the stick's at a blend of 0 */
	*pad = controller_facing_pack(blend > 0.0f, stick_yaw - *yaw);
	/* the last hand-blended frame before the stick takes over must already
	be the stick's aim (a vehicle's tilt eases out, never steps) */
	if (blend <= 0.0f && aim.previous_effective_blend > 0.0f && aim.previous_known)
		platform_log("controller aim: handover tick %ld last blend %.3f aim yaw %.2f pitch %.2f stick yaw %.2f "
			"pitch %.2f hand yaw %.2f pitch %.2f look yaw %.2f pitch %.2f", time, aim.previous_effective_blend,
			yaw_degrees(aim.previous_yaw), DEGREES(aim.previous_pitch), yaw_degrees(aim.previous_stick_yaw),
			DEGREES(aim.previous_stick_pitch), yaw_degrees(aim.previous_hand_yaw), DEGREES(aim.previous_hand_pitch),
			yaw_degrees(aim.previous_look_yaw), DEGREES(aim.previous_look_pitch));

	/* a press keeps the aim it was pressed with until a tick takes it; the
	game runs as update_client_queue_push tests it. Each latch logs under
	debug.test_controller (a held trigger latches again each tick) */
	latched = aim.latch.latched && aim.latch.tick == time;
	if (controller_latch_update(&aim.latch, time, !game_time_get_paused() && !game_time_held(), control_flags,
		FLAG(_unit_control_weapon_primary_trigger_bit) | FLAG(_unit_control_weapon_secondary_trigger_bit),
		yaw, pitch, pad) && !latched && test_controller)
	{
		platform_log("controller aim: latch tick %ld frame %lu aim yaw %.2f pitch %.2f hand %d", time,
			platform_clock_frames(), yaw_degrees(*yaw), DEGREES(*pitch), *pad & 1);
	}

	tick_log(unit_index, time);
	aim.previous_known = 1;
	aim.previous_look_yaw = look_yaw;
	aim.previous_look_pitch = look_pitch;
	aim.previous_yaw = *yaw;
	aim.previous_pitch = *pitch;
	aim.previous_pad = *pad;
	aim.previous_blend = track.blend;
	aim.previous_gate = gate;
	aim.previous_hit = hit;
	aim.previous_effective_blend = blend;
	if (blend <= 0.0f)
		tilted_look(look_yaw, look_pitch, &stick_yaw, &stick_pitch);
	aim.previous_stick_yaw = stick_yaw;
	aim.previous_stick_pitch = stick_pitch;
	aim.previous_hand_yaw = hand_yaw;
	aim.previous_hand_pitch = hand_pitch;
	/* what the gun points at, for the targeting query (the action's aim:
	blended, inside the seat's arc and as latched, where the shot goes) */
	aim.ray_direction[0] = cosf(*pitch) * cosf(*yaw);
	aim.ray_direction[1] = cosf(*pitch) * sinf(*yaw);
	aim.ray_direction[2] = sinf(*pitch);
	return (*pad & 1) != 0;
}

int halo_controller_action_aim(short local_player_index, float look_yaw, float look_pitch,
	unsigned long control_flags, float *yaw, float *pitch, short *pad)
{
	long unit_index;
	unsigned long seed = 0, local_seed = 0;
	int hand;
	short connection;
	int network;
	float seat_yaw = 0.0f, seat_pitch = 0.0f;
	extern void player_control_network_seat_facing(short local_player_index, float *yaw, float *pitch);

	*yaw = look_yaw;
	*pitch = look_pitch;
	*pad = 0;
	aim.network_move_delta = 0.0f;
	if (local_player_index != 0)
		return 0;
	settings();
	network = network_controller_aim();
	if (network)
		player_control_network_seat_facing(local_player_index, &seat_yaw, &seat_pitch);
	unit_index = player_control_get_unit_index(0);
	/* the death tick: player_died has cleared the unit */
	if (unit_index == NONE)
	{
		aim.last_unit = NONE;
		aim.hand_aiming = 0;
		if (network)
			*yaw = *pitch = 0.0f;
		feedback();
		return network;
	}
	connection = game_connection();
	if (connection == _game_connection_network_client || connection == _game_connection_network_server)
	{
		const char *reason;

		/* A world-space hand angle cannot ease into a seat-relative action,
		or survive a camera/death transition. Tracking loss holds aim on foot. */
		if (!gate_target(unit_index, &reason))
		{
			if (aim.hand_aiming)
				reset("network view");
			aim.hand_aiming = 0;
			if (network)
			{
				real_euler_angles2d facing;

				if (unit_get(unit_index)->object.parent_object_index != NONE)
				{
					*yaw = seat_yaw;
					*pitch = seat_pitch;
				}
				else
				{
					euler_angles2d_from_vector3d(&facing, &unit_get(unit_index)->unit.aiming_vector);
					*yaw = facing.yaw;
					*pitch = facing.pitch;
				}
			}
			feedback();
			return network;
		}
	}
	/* (the seeds are read with get_random_seed, which skips
	get_global_random_seed_address's assertion that the global seed isn't
	locked: in a network game it is, and the assertion halted the tick) */
	if (test_controller)
	{
		seed = get_random_seed();
		local_seed = *get_global_local_random_seed_address();
	}
	hand = action_aim(unit_index, look_yaw, look_pitch, control_flags, yaw, pitch, pad);
	if (test_controller &&
		(seed != get_random_seed() || local_seed != *get_global_local_random_seed_address()))
		platform_log("controller aim: random seed changed");
	if (!hand)
	{
		if (network && !aim.network_head_aim)
		{
			real_euler_angles2d facing;

			/* No tracked aim yet: preserve simulated aim, never publish head look. */
			euler_angles2d_from_vector3d(&facing, &unit_get(unit_index)->unit.aiming_vector);
			*yaw = facing.yaw;
			*pitch = facing.pitch;
		}
		else
		{
			*yaw = look_yaw;
			*pitch = look_pitch;
		}
		*pad = 0;
	}
	aim.hand_aiming = hand;
	/* The compressed relay has only ordinary facing. Keep presentation's
	hand state, but never ask a peer to interpret local hand/body metadata. */
	if (connection == _game_connection_network_client || connection == _game_connection_network_server)
	{
		float delta_yaw;

		/* port: the throttle is in the head's frame, as the facing is in a local game
		(players.c turns the facing back to the look); with the hand's aim as the
		facing, the throttle takes the same turn, so walking follows the head, not the gun */
		if (hand && halo_action_facing_unpack(*pad, &delta_yaw))
			aim.network_move_delta = delta_yaw;
		*pad = 0;
	}
	feedback();
	return hand || network;
}

/* for player_control.c as the action is built, after halo_controller_action_aim:
a network action's throttle turned to the facing it will be taken with */
void halo_controller_network_throttle(short local_player_index, float *throttle_i, float *throttle_j)
{
	float cosine, sine, i;

	if (local_player_index != 0 || aim.network_move_delta == 0.0f)
		return;
	cosine = cosf(aim.network_move_delta);
	sine = sinf(aim.network_move_delta);
	i = *throttle_i;
	*throttle_i = i * cosine - *throttle_j * sine;
	*throttle_j = i * sine + *throttle_j * cosine;
}

/* Keep this camera source for the whole eligible network session, including
tracking loss and the old hand-facing ticks left in observer history. The
render camera still owns cinematic, replay, perspective and seat exclusions. */
int halo_controller_network_head_camera(short local_player_index)
{
	settings();
	return local_player_index == 0 && network_controller_aim();
}

int halo_controller_hand_aiming(short local_player_index)
{
	return local_player_index == 0 && aim.hand_aiming;
}

void halo_controller_pitch_offset_step(int direction)
{
	float stepped;

	settings();
	stepped = fminf(fmaxf(pitch_offset_degrees + (float)direction, PITCH_OFFSET_MINIMUM), PITCH_OFFSET_MAXIMUM);
	if (stepped == pitch_offset_degrees)
		return;
	pitch_offset_degrees = stepped;
	platform_log("controller aim: pitch offset %d", (int)lroundf(pitch_offset_degrees));
	pitch_offset_message_pending = 1;
}

/* ---------- aim assist */

/* the game's own autoaim for everyone: a network game (its hits are each
shooter's own to report, network_damage.c) and a multiplayer game type
(split screen too) */
static int aim_assist_fixed_rules(void)
{
	return game_connection() != _game_connection_local || game_engine_running();
}

/* each change of the strength in use, once */
static float assist_strength(int hand_aims)
{
	int fixed = aim_assist_fixed_rules();
	float strength = aim_assist_strength_effective(assist_setting, hand_aims, fixed);

	if (hand_aims && strength != assist_logged)
	{
		assist_logged = strength;
		platform_log("controller aim: aim assist strength %.2f (input.aim_assist %.2f%s) tick %ld", strength,
			assist_setting, fixed ? ", fixed: multiplayer" : "", (long)game_time_get());
	}
	return strength;
}

float halo_aim_assist_player_strength(long player_absolute_index)
{
	settings();
	return assist_strength(halo_player_hand_aim(player_absolute_index));
}

float halo_aim_assist_local_strength(short local_player_index)
{
	settings();
	return assist_strength(halo_controller_hand_aiming(local_player_index));
}

void halo_aim_assist_apply(float strength, float *autoaim_angle, float *deviation_angle)
{
	aim_assist_strength_apply(strength, autoaim_angle, deviation_angle);
}

void halo_aim_assist_step(int direction)
{
	float stepped;

	settings();
	assist_message_pending = 1;
	if (aim_assist_fixed_rules())
		return;
	stepped = aim_assist_strength_step(assist_setting, direction);
	if (stepped == assist_setting)
		return;
	assist_setting = stepped;
	platform_log("controller aim: aim assist %.1f", assist_setting);
}

/* for player_control.c (declared there): 1 while the game's magnetism
(friction on the look's input and adhesion to a moving target) must stay off
because the head drives the look: whatever aims, the hand, the head with the
hand lost, or an ordinary gamepad in HEAD mode. input.head_magnetism turns
the game's own back on */
int halo_head_look_no_magnetism(short gamepad_index, float level)
{
	static int allowed = -1;
	static long logged_second = -1;
	int off;

	if (allowed < 0)
	{
		allowed = config_boolean("input.head_magnetism");
		if (allowed)
			platform_log("controller aim: input.head_magnetism: the game's magnetism may turn the head-tracked view");
	}
	settings();
	off = !allowed && halo_stereo_frame()->mode == HALO_STEREO_HEAD && halo_stereo_head_drives_look(gamepad_index);
	/* under debug.test_controller or debug.aim_trace, once a second of game
	time while a target has a hold on the look: what the game's magnetism
	would have used, and whether it was left out */
	if ((test_controller || aim_trace) && level > 0.0f && game_time_get() / TICKS_PER_SECOND != logged_second)
	{
		logged_second = game_time_get() / TICKS_PER_SECOND;
		platform_log("controller aim: head look: a target's magnetism %.2f %s tick %ld", level,
			off ? "left out" : "applied by the game", (long)game_time_get());
	}
	return off;
}

/* ---------- on-target steadying */

/* the steadying the barrel's low-pass takes this frame, for the action and
the drawing alike: by how firmly the last targeting query found the hand's
aim on a target (player_control.c runs it along the last action's aim,
halo_controller_aim_ray). Each start and end of it logs under
debug.test_controller */
static float steady_strength(void)
{
	float level = steady_test_level >= 0.0f ? steady_test_level :
		aim.hand_aiming ? player_control_get_autoaim_level(0) : 0.0f;
	float setting = steady_by_zoom ? controller_steady_by_zoom(steady_unzoomed, steady_zoom_2x, steady_zoom_8x,
		halo_zoom_magnification(0)) : steady_setting;
	float strength = controller_steady_strength(setting, assist_setting, level, aim.hand_aiming,
		aim_assist_fixed_rules());

	if (test_controller && (strength > 0.0f) != steady_logged)
	{
		steady_logged = strength > 0.0f;
		platform_log("controller aim: steadying %.2f (on target %.2f, setting %.1f%s, input.aim_assist %.1f, "
			"%.2fx) tick %ld frame %lu", strength, level, setting, steady_by_zoom ? " by zoom" : "", assist_setting,
			halo_zoom_magnification(0),
			(long)game_time_get(), platform_clock_frames());
	}
	return strength;
}

void halo_controller_origin_raise_step(int direction)
{
	char value[16];

	settings();
	origin_raise_message_pending = 1;
	origin_raise = controller_origin_raise_step(origin_raise, direction);
	/* kept for the next launch (the file's value is what settings() reads) */
	snprintf(value, sizeof(value), "%.1f", origin_raise);
	config_write("input.aim_origin_raise", value);
	platform_log("controller aim: aim origin raise %.2f tick %ld frame %lu", origin_raise, (long)game_time_get(),
		platform_clock_frames());
}

void halo_controller_reticle_ease_toggle(void)
{
	settings();
	reticle_ease_on = !reticle_ease_on;
	reticle_ease_message_pending = 1;
	platform_log("controller aim: reticle easing %s (input.reticle_ease %.2f s) tick %ld frame %lu",
		reticle_ease_on && reticle_ease > 0.0f ? "on" : "off", reticle_ease, (long)game_time_get(),
		platform_clock_frames());
}

void halo_controller_weapon_smoothing_toggle(void)
{
	settings();
	weapon_smoothing_on = !weapon_smoothing_on;
	weapon_smoothing_message_pending = 1;
	platform_log("controller aim: gun smoothing %s (input.weapon_smoothing %.2f s) tick %ld frame %lu",
		weapon_smoothing_on && weapon_smoothing > 0.0f ? "on" : "off", weapon_smoothing, (long)game_time_get(),
		platform_clock_frames());
}

static const char *arms_mode_name(int mode)
{
	return mode == HALO_ARMS_TURNED ? "turned" : mode == HALO_ARMS_LIMITED ? "limited" : "solved";
}

void halo_controller_arms_cycle(int direction)
{
	settings();
	arms_mode_live = (arms_mode + (direction < 0 ? NUMBER_OF_HALO_ARMS_MODES - 1 : 1)) % NUMBER_OF_HALO_ARMS_MODES;
	arms_mode = arms_mode_live;
	arms_mode_message_pending = 1;
	platform_log("controller aim: first-person arms %s (input.first_person_arms) tick %ld frame %lu",
		arms_mode_name(arms_mode), (long)game_time_get(), platform_clock_frames());
}

void halo_controller_arms_turn_limit_cycle(void)
{
	float next;

	settings();
	next = halo_first_person_arms_turn_limit() < 0.5f * (ARMS_TURN_LIMIT_NEAR + ARMS_TURN_LIMIT_FAR) ?
		ARMS_TURN_LIMIT_FAR : ARMS_TURN_LIMIT_NEAR;
	halo_first_person_arms_turn_limit_set(next);
	arms_turn_limit_message_pending = 1;
	platform_log("controller aim: arms turn limit %.0f (input.arms_turn_limit) tick %ld frame %lu", next,
		(long)game_time_get(), platform_clock_frames());
}

void halo_controller_arms_follow_cycle(void)
{
	settings();
	arms_follow_live = arms_follow > 0.5f * ARMS_FOLLOW_ON ? 0.0f : ARMS_FOLLOW_ON;
	arms_follow = arms_follow_live;
	arms_follow_message_pending = 1;
	platform_log("controller aim: arms follow %.1f (input.arms_follow) tick %ld frame %lu", arms_follow,
		(long)game_time_get(), platform_clock_frames());
}

int halo_controller_arms_mode(void)
{
	return arms_mode;
}

void halo_controller_laser_cycle(void)
{
	settings();
	laser_setting = controller_laser_cycle(laser_setting);
	laser_message_pending = 1;
	platform_log("controller aim: laser %s tick %ld frame %lu", controller_laser_mode_name(laser_setting),
		(long)game_time_get(), platform_clock_frames());
}

void halo_aim_steady_cycle(void)
{
	settings();
	steady_message_pending = 1;
	if (aim_assist_fixed_rules())
		return;
	/* by zoom, off, the default, strong, by zoom again */
	if (steady_by_zoom)
	{
		steady_by_zoom = 0;
		steady_setting = 0.0f;
	}
	else if (steady_setting >= CONTROLLER_STEADY_MAXIMUM)
		steady_by_zoom = 1;
	else
		steady_setting = controller_steady_cycle(steady_setting);
	if (steady_by_zoom)
		platform_log("controller aim: aim steadying by zoom tick %ld frame %lu", (long)game_time_get(),
			platform_clock_frames());
	else
		platform_log("controller aim: aim steadying %.1f tick %ld frame %lu", steady_setting, (long)game_time_get(),
			platform_clock_frames());
}

/* ---------- the targeting query and the drawing sightline */

int halo_controller_aim_ray(short local_player_index, float origin[3], float direction[3])
{
	long unit_index;
	real_point3d eye;

	if (local_player_index != 0 || (!aim.hand_aiming && !network_controller_aim()))
		return 0;
	unit_index = player_control_get_unit_index(0);
	if (unit_index == NONE)
		return 0;
	if (network_controller_aim() && !network_gameplay_aim_eligible(unit_index))
		return 0;
	/* the targeting query runs as the action is built, before this frame's
	aim (player_control.c): the eye now, along the last action's aim, as
	the camera's forward it replaces is the last frame's */
	unit_get_camera_position(unit_index, &eye);
	origin[0] = eye.x;
	origin[1] = eye.y;
	origin[2] = eye.z;
	if (game_connection() == _game_connection_network_client || game_connection() == _game_connection_network_server)
	{
		/* Ordinary peers consume the unit's clipped/rate-limited aim. */
		direction[0] = unit_get(unit_index)->unit.aiming_vector.i;
		direction[1] = unit_get(unit_index)->unit.aiming_vector.j;
		direction[2] = unit_get(unit_index)->unit.aiming_vector.k;
	}
	else
		memcpy(direction, aim.ray_direction, sizeof(aim.ray_direction));
	return 1;
}

/* the drawing sample's sightline (draw_update's work under the seed check):
0 when no hand aims for local player 0 */
static int draw_sightline(const float camera_position[3], float time_delta)
{
	const struct halo_spatial_sample *sample = halo_controller_draw_sample();
	struct halo_spatial_hand hand = sample->hands[sample->aiming_hand ? 1 : 0];
	struct hand_sightline sightline;
	long unit_index = player_control_get_unit_index(0);
	float body_yaw, vector[3], hit[3], yaw, pitch, clamped_yaw;
	int axis;
	int network = game_connection() == _game_connection_network_client ||
		game_connection() == _game_connection_network_server;
	int network_seat;

	/* the room-to-world yaw from the eye cameras this frame, held at the
	pole as the action's is */
	body_yaw = controller_body_yaw(&drawing.body, halo_stereo_eye_yaw(), sample->head_yaw, sample->head_level);
	if (unit_index == NONE ||
		(network && !network_gameplay_aim_eligible(unit_index)) ||
		(!aim.hand_aiming && !network_controller_aim()) || (!network && !drawing.body.known))
		return 0;
	network_seat = network && unit_get(unit_index)->object.parent_object_index != NONE;
	/* a sample that isn't READY is a lost hand, as the action's: the last
	tracked location */
	if (sample->reason != HALO_SPATIAL_READY)
		hand.state = HALO_SPATIAL_NONE;
	/* Seats use independent stick aim. Do not reuse a cached hand pose for
	the weapon frame while the rider holds an ordinary weapon. */
	if (network_seat || !hand_sightline(unit_index, &hand, &drawing.location, &drawing.near, &drawing.range, &drawing.zoom_filter, body_yaw,
		camera_position, camera_position, time_delta, steady_strength(), &sightline))
	{
		if (!network)
			return 0;
		/* Keep an honest reticle with no current pose. The gun has a neutral
		frame until tracking supplies a location; gameplay still uses unit aim. */
		memset(&sightline, 0, sizeof(sightline));
		memcpy(sightline.origin, camera_position, sizeof(sightline.origin));
		sightline.up[2] = 1.0f;
	}
	/* The hand requests a direction; ordinary unit simulation determines
	the firing direction. Show that direction even during a rapid wrist turn. */
	if (network)
	{
		real_vector3d *actual = &unit_get(unit_index)->unit.aiming_vector;

		sightline.direction[0] = actual->i;
		sightline.direction[1] = actual->j;
		sightline.direction[2] = actual->k;
	}
	/* the seat's arc on the sightline's world yaw, as the action clamps the
	hand's (action_aim, before its blend): the gun, the marker and the
	reticle show where the shot goes, dimmed while the arc holds the aim
	back. On foot, and inside the arc, the yaw is unchanged */
	controller_angles(sightline.direction, &yaw, &pitch);
	clamped_yaw = network ? yaw : player_control_seat_yaw_clamped(0, yaw);
	drawing.seat_clamped = fabsf(remainderf(clamped_yaw - yaw, 2.0f * CONTROLLER_PI)) > 1e-4f;
	if (drawing.seat_clamped)
	{
		/* (at the sightline's pitch, clamped as the action's) */
		sightline.direction[0] = cosf(pitch) * cosf(clamped_yaw);
		sightline.direction[1] = cosf(pitch) * sinf(clamped_yaw);
		sightline.direction[2] = sinf(pitch);
	}
	memcpy(draw.eye, camera_position, sizeof(draw.eye));
	memcpy(draw.barrel_origin, sightline.origin, sizeof(draw.barrel_origin));
	memcpy(draw.barrel_forward, sightline.forward, sizeof(draw.barrel_forward));
	memcpy(draw.barrel_up, sightline.up, sizeof(draw.barrel_up));
	memcpy(draw.sightline, sightline.direction, sizeof(draw.sightline));
	memcpy(draw.aim_point, sightline.aim_point, sizeof(draw.aim_point));
	drawing.ray_range = sightline.ray_range;
	drawing.eased_range = sightline.eased_range;
	/* what the eye sees along the sightline: the reticle's place */
	for (axis = 0; axis < 3; axis++)
		vector[axis] = sightline.direction[axis] * CONTROLLER_RAY_UNITS;
	if (ray_hit(camera_position, vector, object_get_ultimate_parent(unit_index), hit))
	{
		memcpy(draw.hit_point, hit, sizeof(draw.hit_point));
		for (axis = 0; axis < 3; axis++)
			vector[axis] = hit[axis] - camera_position[axis];
		draw.hit_distance = sqrtf(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
	}
	else
	{
		draw.hit_distance = CONTROLLER_SKY_UNITS;
		for (axis = 0; axis < 3; axis++)
			draw.hit_point[axis] = camera_position[axis] + sightline.direction[axis] * CONTROLLER_SKY_UNITS;
	}
	/* The near filter can temporarily aim away from P while it follows a
	moving hand, and a sightline the seat's arc turned away from P never
	passes through it. Beyond the filter, a nearer eye hit is cover. */
	draw.past_cover = !network && !drawing.seat_clamped && sightline.aim_distance > CONTROLLER_NEAR_UNITS &&
		draw.hit_distance < sightline.aim_distance - CONTROLLER_PAST_COVER_UNITS &&
		/* (and nearer than the surface the ray really meets: while input.reticle_ease
		carries P from a far surface to a near one, P is past the near one for a moment) */
		draw.hit_distance < sightline.surface_distance - CONTROLLER_PAST_COVER_UNITS;
	return 1;
}

void halo_controller_draw_update(short local_player_index, const float camera_position[3])
{
	unsigned long seed = 0, local_seed = 0;
	float time_delta, eye[3], eye_forward[3], eye_up[3], raise, target;
	long time;
	int axis;

	if (local_player_index != 0)
		return;
	settings();
	draw.active = 0;
	if (!aim_controller)
		return;
	time_delta = frame_time_delta(&drawing.clock);
	if (test_controller)
	{
		seed = get_random_seed();
		local_seed = *get_global_local_random_seed_address();
	}
	/* the eyes' position: the camera's, raised as display.eye_height_offset
	raises the eyes, so the hand sits where it is relative to the view and
	the eyes' frame (the reticle's, the laser's) starts at E */
	halo_stereo_eye_frame(eye_forward, eye_up);
	raise = halo_stereo_eye_raise();
	for (axis = 0; axis < 3; axis++)
		eye[axis] = camera_position[axis] + eye_up[axis] * raise;
	draw.active = draw_sightline(eye, time_delta);
	if (test_controller &&
		(seed != get_random_seed() || local_seed != *get_global_local_random_seed_address()))
		platform_log("controller aim: random seed changed (drawing)");
	if (!draw.active)
	{
		/* the near low-pass and the reticle's distance start afresh when a
		hand aims again */
		drawing.near.known = 0;
		drawing.zoom_filter.known = 0;
		drawing.reticle_known = 0;
		drawing.laser_opacity = 0.0f;
		drawing.weapon_filter.known = 0;
		drawing.range.known = 0;
		return;
	}
	/* the gun's frame: the sightline and the barrel's up, low-passed in the
	room (the drawing's body yaw) unless smoothing is off; nothing else
	reads it, so the reticle and the shot stay on the live sightline */
	memcpy(drawing.weapon_forward, draw.sightline, sizeof(draw.sightline));
	memcpy(drawing.weapon_up, draw.barrel_up, sizeof(draw.barrel_up));
	controller_weapon_smooth(&drawing.weapon_filter, drawing.body.yaw, weapon_smoothing_on ? weapon_smoothing : 0.0f,
		time_delta, drawing.weapon_forward, drawing.weapon_up);
	/* the hand's offset from the eyes in the view's level frame (forward,
	left, up), and its running average, the neutral the "solved" arms' grip
	moves away from (first_person_arms.c) */
	{
		float level_forward[3], level_left[3], offset[3], components[3], length;

		for (axis = 0; axis < 3; axis++)
			offset[axis] = draw.barrel_origin[axis] - eye[axis];
		level_forward[0] = eye_forward[0];
		level_forward[1] = eye_forward[1];
		level_forward[2] = 0.0f;
		length = sqrtf(level_forward[0] * level_forward[0] + level_forward[1] * level_forward[1]);
		if (length > 1e-4f)
		{
			level_forward[0] /= length;
			level_forward[1] /= length;
		}
		else
		{
			level_forward[0] = 1.0f;
			level_forward[1] = 0.0f;
		}
		level_left[0] = -level_forward[1];
		level_left[1] = level_forward[0];
		level_left[2] = 0.0f;
		components[0] = offset[0] * level_forward[0] + offset[1] * level_forward[1];
		components[1] = offset[0] * level_left[0] + offset[1] * level_left[1];
		components[2] = offset[2];
		if (!drawing.hand_neutral_known || !(time_delta > 0.0f) || time_delta > 0.25f)
		{
			memcpy(drawing.hand_neutral, components, sizeof(components));
			drawing.hand_neutral_known = 1;
		}
		else
		{
			float gain = 1.0f - expf(-time_delta / ARMS_NEUTRAL_SECONDS);

			for (axis = 0; axis < 3; axis++)
				drawing.hand_neutral[axis] += (components[axis] - drawing.hand_neutral[axis]) * gain;
		}
		for (axis = 0; axis < 3; axis++)
			drawing.grip_shift[axis] = arms_follow * ((components[0] - drawing.hand_neutral[0]) * level_forward[axis] +
				(components[1] - drawing.hand_neutral[1]) * level_left[axis]);
		drawing.grip_shift[2] += arms_follow * (components[2] - drawing.hand_neutral[2]);
	}
	drawing.active_frame = render.frame_index;
	draw.blend = network_controller_aim() ? 1.0f : aim.previous_effective_blend;
	draw.dim = aim.dim || drawing.seat_clamped || (network_controller_aim() && !aim.hand_aiming);
	/* the reticle's distance: the hit pulled 2% toward the eye, low-passed
	faster toward nearer than farther so a sweep across a railing against
	the sky doesn't jump the eyes' vergence (the direction is never
	smoothed) */
	target = draw.hit_distance * (1.0f - CONTROLLER_RETICLE_PULL);
	/* (the sky's 200 units too, 196 after the pull: past the presenter's
	far-plane clamp and vergence's reach either way, so it is left alone) */
	if (!drawing.reticle_known)
	{
		draw.reticle_distance = target;
		drawing.reticle_known = 1;
	}
	else
	{
		float seconds = target < draw.reticle_distance ? CONTROLLER_RETICLE_NEARER_SECONDS :
			CONTROLLER_RETICLE_FARTHER_SECONDS;

		draw.reticle_distance += (target - draw.reticle_distance) * (1.0f - expf(-time_delta / seconds));
	}
	/* the laser: by the reticle's own low-passed distance, so a sweep off a
	near rock onto the far shore fades the line out as the reticle recedes */
	draw.laser_opacity = controller_laser_ease(drawing.laser_opacity, controller_laser_opacity(laser_setting,
		laser_distance, draw.reticle_distance * 3.048f, draw.past_cover), time_delta);
	drawing.laser_opacity = draw.laser_opacity;
	draw.laser = draw.laser_opacity >= CONTROLLER_LASER_OPACITY_MINIMUM;
	if (test_controller && draw.laser != laser_logged)
	{
		laser_logged = draw.laser;
		platform_log("controller aim: laser line %s (%s, reticle %.2f m, past cover %d) tick %ld frame %lu",
			draw.laser ? "shown" : "hidden", controller_laser_mode_name(laser_setting), draw.reticle_distance * 3.048f,
			draw.past_cover, (long)game_time_get(), platform_clock_frames());
	}
	/* debug.aim_trace: every drawn frame, the live sightline and the gun's
	frame (degrees), the reticle's distance, and what the action last did
	(its blend, the gate's target), for measuring the gun's smoothing and a
	zoomed hand against the gate */
	if (aim_trace)
	{
		float sight_yaw, sight_pitch, trace_yaw, trace_pitch;

		controller_angles(draw.sightline, &sight_yaw, &sight_pitch);
		controller_angles(drawing.weapon_forward, &trace_yaw, &trace_pitch);
		platform_log("controller aim: trace frame %lu tick %ld sight %.3f %.3f gun %.3f %.3f body %.2f reticle %.2f "
			"blend %.2f gate %d zoom %.1f range %.3f eased %.3f barrel %.2f %.2f %.2f origin %.3f %.3f %.3f",
			platform_clock_frames(), (long)game_time_get(), yaw_degrees(sight_yaw),
			DEGREES(sight_pitch), yaw_degrees(trace_yaw), DEGREES(trace_pitch), yaw_degrees(drawing.body.yaw),
			draw.reticle_distance, draw.blend, aim.last_target, halo_zoom_magnification(0), drawing.ray_range,
			drawing.eased_range, draw.barrel_forward[0], draw.barrel_forward[1], draw.barrel_forward[2],
			draw.barrel_origin[0] - draw.eye[0], draw.barrel_origin[1] - draw.eye[1], draw.barrel_origin[2] - draw.eye[2]);
	}
	/* once a second of game time: the sightline's yaw, which holds with the
	hand while the head turns */
	time = game_time_get();
	if (test_controller && time / TICKS_PER_SECOND != drawing.logged_second)
	{
		float yaw, pitch;

		drawing.logged_second = time / TICKS_PER_SECOND;
		controller_angles(draw.sightline, &yaw, &pitch);
		float gun_yaw, gun_pitch;

		controller_angles(drawing.weapon_forward, &gun_yaw, &gun_pitch);
		platform_log("controller aim: draw yaw %.2f pitch %.2f tick %ld eye yaw %.2f body %.2f hit %.2f past cover %d "
			"reticle %.2f dim %d seat clamped %d laser %d opacity %.2f hit point %.3f %.3f %.3f shot %.3f %.3f %.3f "
			"gun yaw %.2f pitch %.2f", yaw_degrees(yaw),
			DEGREES(pitch), time, yaw_degrees(halo_stereo_eye_yaw()), yaw_degrees(drawing.body.yaw), draw.hit_distance,
			draw.past_cover, draw.reticle_distance, draw.dim, drawing.seat_clamped, draw.laser, draw.laser_opacity,
			draw.hit_point[0],
			draw.hit_point[1], draw.hit_point[2], aim.shot_point[0], aim.shot_point[1], aim.shot_point[2],
			yaw_degrees(gun_yaw), DEGREES(gun_pitch));
	}
}

const struct halo_controller_draw *halo_controller_draw_state(void)
{
	return &draw;
}

int halo_controller_weapon_frame(short local_player_index, const float camera_forward[3], const float camera_up[3],
	float forward[3], float up[3])
{
	if (local_player_index != 0 || !draw.active || drawing.active_frame != render.frame_index)
		return 0;
	/* "solved": the model is mounted at the view and re-posed
	(first_person_arms.c): the camera's frame stands for every caller */
	if (arms_mode == HALO_ARMS_SOLVED)
		return 0;
	if (!controller_weapon_frame(camera_forward, camera_up, drawing.weapon_forward, drawing.weapon_up, draw.blend,
		forward, up))
		return 0;
	if (arms_mode == HALO_ARMS_LIMITED)
		controller_weapon_limit(camera_forward, camera_up, weapon_turn_limit * (CONTROLLER_PI / 180.0f),
			weapon_roll_limit * (CONTROLLER_PI / 180.0f), forward, up);
	return 1;
}

int halo_controller_gun_frame(short local_player_index, float forward[3], float up[3], float *blend, float shift[3])
{
	if (local_player_index != 0 || !draw.active || drawing.active_frame != render.frame_index)
		return 0;
	memcpy(forward, drawing.weapon_forward, sizeof(drawing.weapon_forward));
	memcpy(up, drawing.weapon_up, sizeof(drawing.weapon_up));
	*blend = draw.blend;
	memcpy(shift, drawing.grip_shift, sizeof(drawing.grip_shift));
	return 1;
}

void halo_controller_zoom_view(const float forward[3], const float up[3], float tangent_across, float tangent_up,
	float magnification)
{
	zoom_view.known = 1;
	zoom_view.frame = render.frame_index;
	memcpy(zoom_view.forward, forward, sizeof(zoom_view.forward));
	memcpy(zoom_view.up, up, sizeof(zoom_view.up));
	zoom_view.tangents[0] = tangent_across;
	zoom_view.tangents[1] = tangent_up;
	zoom_view.magnification = magnification >= 1.0f ? magnification : 1.0f;
	/* (render.c calls this in a stereo frame, so the frame has its eyes) */
	{
		float shared[4];

		halo_stereo_shared_view(shared);
		zoom_view.sight.left = shared[0];
		zoom_view.sight.right = shared[1];
		zoom_view.sight.up = shared[2];
		zoom_view.sight.down = shared[3];
		zoom_view.sight.off_center = tanf(HALO_STEREO_SHARED_VIEW_DEGREES * (CONTROLLER_PI / 180.0f));
	}
}

int halo_controller_zoom_reticle(float shown[3])
{
	if (!draw.active || drawing.active_frame != render.frame_index || !zoom_view.known ||
		zoom_view.frame != render.frame_index)
		return 0;
	if (network_controller_aim() && !network_gameplay_aim_eligible(player_control_get_unit_index(0)))
		return 0;
	/* the hand keeps the aim wherever it points: the reticle is where the
	picture shows the gun's line, and past the picture's edge there is none */
	if (!zoom_takeover || network_controller_aim())
		return controller_zoom_project(draw.sightline, zoom_view.forward, zoom_view.up, zoom_view.tangents,
			zoom_view.magnification, 1, shown) ? 1 : 2;
	/* (the action, which tested last frame's camera, holds the hand's aim
	inside the picture whenever draw.active is set: past this frame's edge
	during a fast head turn the reticle sits on the edge rather than
	flickering to the center) */
	return controller_zoom_project_clamped(draw.sightline, zoom_view.forward, zoom_view.up, zoom_view.tangents,
		zoom_view.magnification, shown);
}

void halo_controller_debug_marker(void)
{
	const union real_argb_color *color;
	real_point3d center, ends[4];
	float right[3], up[3], length, arm;
	int end;

	if (!test_controller || !draw.active || halo_stereo_frame()->mode != HALO_STEREO_SIDE_BY_SIDE)
		return;
	/* a cross square to the sightline, its arms 0.02 units at 1 unit from
	the eye and growing with the distance, so it keeps its size on screen
	(in the zoomed pass too: over the magnification); drawn 2% toward the
	eye, as the reticle is, so the surface it sits on doesn't cut it.
	Yellow when the barrel's ray passes cover the eye's line doesn't. In
	the zoomed pass it lies on the sightline from the zoomed camera (the
	eyes' frame's origin, with display.eye_height_offset at 0), so it shows
	where controller_zoom_project puts the zoomed reticle */
	right[0] = draw.sightline[1];
	right[1] = -draw.sightline[0];
	right[2] = 0.0f;
	length = sqrtf(right[0] * right[0] + right[1] * right[1]);
	if (length < 1e-4f)
	{
		/* straight up or down: any horizontal right will do */
		right[0] = 0.0f;
		right[1] = -1.0f;
		length = 1.0f;
	}
	right[0] /= length;
	right[1] /= length;
	up[0] = right[1] * draw.sightline[2] - right[2] * draw.sightline[1];
	up[1] = right[2] * draw.sightline[0] - right[0] * draw.sightline[2];
	up[2] = right[0] * draw.sightline[1] - right[1] * draw.sightline[0];
	arm = 0.02f * draw.hit_distance * 0.98f;
	if (halo_stereo_zoom() && zoom_view.known && zoom_view.frame == render.frame_index)
		arm /= zoom_view.magnification;
	center.x = draw.eye[0] + (draw.hit_point[0] - draw.eye[0]) * 0.98f;
	center.y = draw.eye[1] + (draw.hit_point[1] - draw.eye[1]) * 0.98f;
	center.z = draw.eye[2] + (draw.hit_point[2] - draw.eye[2]) * 0.98f;
	for (end = 0; end < 4; end++)
	{
		const float *along = end < 2 ? right : up;
		float sign = (end & 1) ? -arm : arm;

		ends[end] = center;
		ends[end].x += along[0] * sign;
		ends[end].y += along[1] * sign;
		ends[end].z += along[2] * sign;
	}
	color = draw.past_cover ? global_real_argb_yellow : global_real_argb_magenta;
	render_debug_line(TRUE, &ends[0], &ends[1], color);
	render_debug_line(TRUE, &ends[2], &ends[3], color);
}

/* ---------- for input_replay.c */

/* for input_replay.c's header, decided at a run's first tick: 1 when this
run's configuration can aim with a hand (input.aim = "controller" and a
synthetic controller, or a host that has the Sense controllers connected or
loading). The caller adds the stereo mode */
int halo_controller_aim_configured(void)
{
	settings();
	if (!aim_controller)
		return 0;
	if (test_controller)
		return 1;
	halo_controller_sample_game();
	return game_sample.reason == HALO_SPATIAL_READY || game_sample.reason == HALO_SPATIAL_WAITING;
}

/* the local player's unit as input_replay.c's end-of-run line reports it:
its position (game units), its facing's yaw and its aim's yaw (degrees); 0
with no unit */
int halo_controller_unit_state(float position[3], float *facing_yaw, float *aim_yaw)
{
	long unit_index = player_control_get_unit_index(0);
	real_point3d origin;
	real_vector3d facing, aiming;

	if (unit_index == NONE)
		return 0;
	object_get_origin(unit_index, &origin);
	unit_get_facing_vector(unit_index, &facing);
	unit_get_aiming_vector(unit_index, &aiming);
	position[0] = origin.x;
	position[1] = origin.y;
	position[2] = origin.z;
	*facing_yaw = DEGREES(atan2f(facing.j, facing.i));
	*aim_yaw = DEGREES(atan2f(aiming.j, aiming.i));
	return 1;
}
