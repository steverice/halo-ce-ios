/* Controller aim's math (input.aim = "controller"): the hand from the room
into the game's world, the sightline from the eye, the facing offset packed
into player_action.pad, tracking loss and recovery, the gate's ease, the
pending flags with the press latch, and when the aim state resets. Pure
math with no game headers, so a host-side test can check it
(port/ios/tests/controller_aim_probe.c); controller_aim_game.c wires it
into the game.

Angles are radians in the game's convention: yaw about +Z, left positive;
pitch up positive. Times are seconds. */
#ifndef CONTROLLER_AIM_H
#define CONTROLLER_AIM_H

#include <math.h>
#include <stdint.h>

#include "../src/halo_spatial.h"

#define CONTROLLER_METERS_PER_UNIT 3.048f
#define CONTROLLER_NEAR_UNITS 0.5f        /* P's low-pass inside this distance */
#define CONTROLLER_NEAR_SECONDS 0.030f    /* P's low-pass inside CONTROLLER_NEAR_UNITS */
#define CONTROLLER_RAY_UNITS 1024.0f
#define CONTROLLER_PITCH_LIMIT (85.5f * 3.14159265f / 180.0f)
#define CONTROLLER_SKY_UNITS 200.0f        /* the reticle's distance when the eye's line meets nothing */
#define CONTROLLER_PAST_COVER_UNITS 0.05f  /* the eye's hit this much nearer than P: the barrel passes cover */
#define CONTROLLER_RETICLE_PULL 0.02f      /* the reticle's share of the way back toward the eye from the hit */
#define CONTROLLER_RETICLE_NEARER_SECONDS 0.050f   /* its distance's low-pass toward nearer */
#define CONTROLLER_RETICLE_FARTHER_SECONDS 0.150f  /* and toward farther */

/* the room-to-world yaw: look_yaw - head_yaw from a level head, else the
last; known 0 until the first level head (then the caller aims with the head) */
struct controller_body { float yaw; int known; };
float controller_body_yaw(struct controller_body *body, float look_yaw, float head_yaw, int head_level);

/* a location into the world: turned by body_yaw about +Z, its position
scaled by 1 / CONTROLLER_METERS_PER_UNIT and added to eye. pitch_offset
(radians, positive down) turns forward and up about the location's left axis */
void controller_location_to_world(const struct halo_spatial_location *location, float body_yaw,
	const float eye[3], float pitch_offset, float origin[3], float forward[3], float up[3]);

/* from eye to the aim point P at every distance, with P low-passed over
CONTROLLER_NEAR_SECONDS while within CONTROLLER_NEAR_UNITS of the eye (not
beyond). direction is unit length; a point at the eye uses barrel_forward */
/* point: the low-passed P less the eye */
struct controller_near { float point[3]; int known; };
void controller_sightline(struct controller_near *near, const float eye[3], const float aim_point[3],
	const float barrel_forward[3], float time_delta, float direction[3]);

/* yaw and pitch (radians, the game's convention) of a direction, pitch
clamped to CONTROLLER_PITCH_LIMIT */
void controller_angles(const float direction[3], float *yaw, float *pitch);

/* player_action.pad: bit 0 the hand flag, bits 1-15 delta (look yaw - aim
yaw, wrapped to -pi..pi) in steps of pi/16384, arithmetic shift. A hand flag
of 0 always packs to 0, the original game */
short controller_facing_pack(int hand, float delta_yaw);
int controller_facing_unpack(short pad, float *delta_yaw);

/* a yaw blended the short way around +-pi: from + blend * wrap(to - from) */
float controller_yaw_blend(float from, float to, float blend);

/* tracking loss and recovery (spec section 1, "Tracking states") */
struct controller_track
{
	float blend;            /* 1 the hand, 0 the head */
	int lost;               /* from a loss until its recovery starts easing */
	float lost_for;         /* seconds since the loss */
	float tracked_for;      /* seconds tracked and held since the loss */
	float orientation_for;  /* seconds orientation-only */
	float since_recovery;   /* seconds since the last recovery began; large at start */
	float since_loss;       /* seconds since the last loss; large at start */
	float since_message;    /* seconds since the last message or buzz; large at start */
	int dwell_step;         /* 0: 100 ms, 1: 500 ms, 2: 1 s */
};
struct controller_track_out
{
	float blend;            /* 1 the hand, 0 the head */
	int use_held;           /* 1: the hand's side is the last good aim (hold and ease out) */
	int dim;                /* the reticle dims */
	int message;            /* 0 none, 1 "Aiming with your head", 2 "Aiming with the controller" */
	int buzz;               /* 1: one short buzz now */
};
void controller_track_reset(struct controller_track *track);
/* state is enum halo_spatial_state, held the hand's is_held */
void controller_track_update(struct controller_track *track, int state, int held, float time_delta,
	struct controller_track_out *out);

/* the gate's ease (seats, the film, cutscenes, a network game): toward
target (0 or 1) over 0.2 s, smoothstepped; snap 1 sets it at once. *value
holds the ease's linear progress (start it at 0 or 1); the return is the
smoothstepped blend, 1 the hand */
float controller_gate_ease(float *value, int target, float time_delta, int snap);

/* the pending flags and the press latch, mirroring update_client_queue_push
(player_queues_new.c:613-634): pending clears when tick changes and on every
frame the game isn't running (paused or held: no latch then). A frame that
sets a fire bit (fire_mask) not yet pending latches yaw, pitch and pad; until
the tick changes the action takes the latched ones. Returns 1 while latched */
struct controller_latch { unsigned long pending; long tick; int latched; float yaw, pitch; short pad; };
int controller_latch_update(struct controller_latch *latch, long tick, int game_running, unsigned long flags_now,
	unsigned long fire_mask, float *yaw, float *pitch, short *pad);

/* 1 when the aim state must reset: the unit changed (death, respawn), or
game time went backward or to 0 (a revert, a map load) */
int controller_needs_reset(long last_unit, long unit, long last_time, long time);

/* the camera-mounted first-person gun's frame while a hand aims (milestone
1): forward turned from the camera's toward the sightline by blend (0 to 1,
the action's), along the arc between them; up the camera's blended toward
the barrel's by the same share, made square to forward, so at blend 0 the
frame is the camera's and at 1 it is the sightline's with the barrel's roll.
All unit vectors. Returns 0, leaving forward and up alone, when the frame is
undefined: the sightline straight behind the camera's forward mid-blend, or
no up square to forward */
int controller_weapon_frame(const float camera_forward[3], const float camera_up[3], const float sightline[3],
	const float barrel_up[3], float blend, float forward[3], float up[3]);

/* The aim point's range, eased (input.reticle_ease). The reticle marks
where the barrel's ray meets the world, seen from the eyes, and the ray
starts below them: when it moves from a far surface onto a near one (an
enemy's outline against the ground behind it) the point comes nearer along
the ray, and its direction from the eyes steps along the ray's projection,
about 3 degrees for a hand 0.54 m below the eyes and an enemy at 8 m. The
range along the ray is low-passed over seconds (its inverse is, in which the
step seen from the eyes is linear), so the point glides along the ray between
the two surfaces. Only while the ray itself turns slowly: a fast move of the
hand that also changes the range would be carried past where it ends and
back (the eased point lies farther along the new ray than its surface), so
the easing is whole under CONTROLLER_RANGE_EASE_SLOW_DEGREES a second of the
ray's turn and gone at CONTROLLER_RANGE_EASE_FAST_DEGREES, where the step is
lost in the move anyway. The action and the drawing each ease their own, the
same way, so the reticle still shows where the shot goes. seconds 0 or less,
an unusable time step, the first call after known is cleared, or a hand that
is suddenly somewhere else (its position, meters from the head, more than
CONTROLLER_RANGE_EASE_JUMP_METERS from the last: tracking found again, or the
head's place changed under it): the range itself. So is a range under
CONTROLLER_NEAR_UNITS, and the one after it: in inverse range a hit a hand's
width from the muzzle outweighs everything, and one frame of it (something
passing the gun) held the eased point near for a third of a second and swung
the reticle 19 degrees in a replayed recording. That close the sightline has
its own low-pass. forward is the ray's direction (unit) */
#define CONTROLLER_RANGE_EASE_DEFAULT 0.05f
#define CONTROLLER_RANGE_EASE_MAXIMUM 0.3f
#define CONTROLLER_RANGE_EASE_SLOW_DEGREES 30.0f
#define CONTROLLER_RANGE_EASE_FAST_DEGREES 120.0f
#define CONTROLLER_RANGE_EASE_JUMP_METERS 0.25f
struct controller_range { float inverse; float forward[3], position[3]; int known; };
/* input.reticle_ease into 0 to CONTROLLER_RANGE_EASE_MAXIMUM; the default for a value that isn't a number */
float controller_range_ease_clamp(float seconds);
float controller_range_ease(struct controller_range *state, const float forward[3], const float position[3],
	float range, float seconds, float time_delta);

/* The first-person gun's limits (input.weapon_limits): the model is mounted
at the camera and turned whole onto the sightline, so a gun pointed far to
one side, or rolled, brings the arms in through the side of the view. This
keeps the model's frame near the view's center: its turn across the view
follows the hand up to CONTROLLER_WEAPON_LIMIT_KNEE of turn_limit and eases
to turn_limit beyond; past turn_limit the model sinks, a degree and a half
for each degree the hand goes on, up to CONTROLLER_WEAPON_LIMIT_SINK_DEGREES,
so it leaves through the bottom of the view, where the arms come from; its
elevation is held within CONTROLLER_WEAPON_LIMIT_ELEVATION_DEGREES the same
way, and its roll about its own forward within roll_limit of the view's up.
Only the drawn model: the reticle and the shot keep the hand's line. Angles
in radians; a limit of 0 or less leaves that part alone. forward and up
(unit, world) in and out; left alone when the camera's frame is degenerate
or the gun points behind it */
#define CONTROLLER_WEAPON_LIMIT_KNEE 0.7f
#define CONTROLLER_WEAPON_LIMIT_SINK_DEGREES 60.0f
#define CONTROLLER_WEAPON_LIMIT_ELEVATION_DEGREES 45.0f
void controller_weapon_limit(const float camera_forward[3], const float camera_up[3], float turn_limit,
	float roll_limit, float forward[3], float up[3]);

/* The first-person gun's own low-pass (input.weapon_smoothing): the gun and
the arms are drawn 0.3 m from the eyes and turned whole onto the sightline,
so a hand's tremor and every step of the aim point (the barrel's ray moving
from a far surface onto a near one) show on them as a jerk. The model takes a
low-passed copy of the sightline and the barrel's up instead; the reticle,
the ray and the shot keep the live ones. Filtered in the room's frame (the
world turned back by body_yaw, radians), so a head turn doesn't lag it and a
snap turn carries it along whole. A difference under
CONTROLLER_WEAPON_SMOOTH_FREE_DEGREES takes the whole time constant; a larger
one (a sweep) catches up faster, down to CONTROLLER_WEAPON_SMOOTH_FAST_SECONDS
at CONTROLLER_WEAPON_SMOOTH_FULL_DEGREES. seconds 0 or less, an unusable time
step, or the first call after known is cleared: the live frame, untouched */
#define CONTROLLER_WEAPON_SMOOTH_DEFAULT 0.08f
#define CONTROLLER_WEAPON_SMOOTH_MAXIMUM 0.5f
#define CONTROLLER_WEAPON_SMOOTH_FREE_DEGREES 3.0f
#define CONTROLLER_WEAPON_SMOOTH_FULL_DEGREES 12.0f
#define CONTROLLER_WEAPON_SMOOTH_FAST_SECONDS 0.01f
struct controller_weapon_filter { float forward[3], up[3]; int known; };
/* input.weapon_smoothing into 0 to CONTROLLER_WEAPON_SMOOTH_MAXIMUM; the
default for a value that isn't a number */
float controller_weapon_smooth_clamp(float seconds);
/* sightline and barrel_up (unit, world) in, the gun's (unit, world) out */
void controller_weapon_smooth(struct controller_weapon_filter *filter, float body_yaw, float seconds,
	float time_delta, float sightline[3], float barrel_up[3]);

/* where a world direction shows in the zoomed picture. The zoomed camera
looks along zoom_forward (up zoom_up) with half tangents tangents[0] across,
tangents[1] up, the unzoomed view's tangents over the magnification. Returns
1 and the direction (eyes' frame: x right, y up, z back) at which the
full-view picture shows it, or 0 when it is outside the picture: with inside
1, outside past 100% of a tangent; with inside 0, inside again only within
90% (ruling 9). shown is unit, and left alone when outside */
#define CONTROLLER_ZOOM_REENTRY 0.9f
int controller_zoom_project(const float direction[3], const float zoom_forward[3], const float zoom_up[3],
	const float tangents[2], float magnification, int inside, float shown[3]);
/* where the zoomed reticle is in sight: the part of the view both eyes see
(halo_stereo_shared_view: the tangents left, right, up and down of the view's
center on the HUD's plane) and how far off that center the eyes' shared view
reaches (the tangent of HALO_STEREO_SHARED_VIEW_DEGREES, where the HUD pins
the unzoomed reticle). The picture is locked to the head and reaches past
both, so a reticle the hand has carried outside them is drawn where an eye,
or both, can't see it */
struct controller_zoom_sight
{
	float left, right, up, down;
	float off_center;
};
/* controller_zoom_project, with the edge where the reticle leaves sight
rather than where the picture ends: 1 only while the direction is inside the
picture and shows inside sight (what it shows at, the picture's tangents
times the magnification, within each of sight's tangents and within
off_center of the center). The same hysteresis on every edge: with inside 1,
outside past 100%; with inside 0, inside again only within
CONTROLLER_ZOOM_REENTRY of each. So the head takes the aim, and the reticle
goes to the center, as the hand-aimed reticle would go out of sight, at any
magnification and for any eyes */
int controller_zoom_project_sight(const float direction[3], const float zoom_forward[3], const float zoom_up[3],
	const float tangents[2], float magnification, const struct controller_zoom_sight *sight, int inside,
	float shown[3]);
/* the same for the reticle while the action already holds the hand's aim
inside the picture: a direction past the picture's edge (this frame's
camera has moved on from the one the action tested) shows clamped onto
that edge, each tangent within its own; 0 only when it is behind or square
to the camera, or the camera is degenerate */
int controller_zoom_project_clamped(const float direction[3], const float zoom_forward[3], const float zoom_up[3],
	const float tangents[2], float magnification, float shown[3]);

/* Stabilize the room-space barrel orientation before collision queries, so
the sightline and ballistics use the same stabilized ray: while zoomed, and
at any magnification while the aim is on a target (steady, below). Position
stays live. Small angular motion has a low-pass whose time constant grows
with the magnification and with steady; larger intentional motion catches up
promptly, so a deliberate move still arrives where the hand points. The
action and rendering own separate states. Unzoomed with no steadying, on a
new zoom level, or during tracking loss, copy exactly and discard old
history. Recovery begins from the current pose without lag.

steady is the on-target steadying asked for now (controller_steady_strength,
0 for none). The filter eases its own share of it (CONTROLLER_STEADY_RISE_SECONDS
up, CONTROLLER_STEADY_FALL_SECONDS down), so a target that dies or leaves the
aim hands the reticle back to the live hand over a tenth of a second instead
of in one frame. Fully on target at strength 1 it adds
CONTROLLER_STEADY_SECONDS + CONTROLLER_STEADY_SECONDS_PER_MAGNIFICATION *
magnification to the time constant (0.10 s unzoomed, 0.12 s at 2x, 0.24 s at
8x, beside the zoom's own 0.05 s and 0.11 s), never past
CONTROLLER_STEADY_SECONDS_LIMIT. Nothing here turns the aim toward a target
or holds it there: only how long a small move takes to arrive changes. */
#define CONTROLLER_STEADY_DEFAULT 1.0f
#define CONTROLLER_STEADY_MAXIMUM 2.0f     /* the setting's; the live chord's "strong" */
#define CONTROLLER_STEADY_SECONDS 0.08f
#define CONTROLLER_STEADY_SECONDS_PER_MAGNIFICATION 0.02f
#define CONTROLLER_STEADY_SECONDS_LIMIT 0.6f
#define CONTROLLER_STEADY_RISE_SECONDS 0.05f
#define CONTROLLER_STEADY_FALL_SECONDS 0.10f
struct controller_zoom_filter
{
	struct halo_spatial_location location;
	float magnification;
	float steady;           /* the eased share of the steadying asked for */
	int known;
};
void controller_zoom_stabilize(struct controller_zoom_filter *filter,
	struct halo_spatial_location *location, float magnification, float steady, float time_delta,
	int tracking_lost);

/* input.aim_steady into 0 to CONTROLLER_STEADY_MAXIMUM; the default for a
value that isn't a number */
float controller_steady_clamp(float setting);
/* the steadying a hand's aim takes now: the setting times the aim assist
strength (input.aim_assist, 0 to 2: no assist, no steadying) times how firmly
the aim is on a target (the game's autoaim level, 0 off it, 1 where the
reticle is red). 0 unless a hand aims in a game whose rules this machine
owns: a network game and a multiplayer game type (fixed_rules) play without
it, as does a stick, a mouse or the head */
float controller_steady_strength(float setting, float assist, float on_target, int hand_aims, int fixed_rules);
/* the live chord's next setting: off (0), the default (1), strong (2), off
again; a setting between two of them goes to the next above it */
float controller_steady_cycle(float setting);
/* steadying by zoom, the default: the setting for the magnification now.
unzoomed (input.aim_steady) at 1x and under; at_2x (input.aim_steady_zoom_2x)
from there to 2x; from 2x to 8x a line from at_2x to at_8x
(input.aim_steady_zoom_8x); at_8x beyond. Each is clamped as the setting is */
float controller_steady_by_zoom(float unzoomed, float at_2x, float at_8x, float magnification);

/* The laser (input.laser): the line from the gun's muzzle to the reticle
while a hand aims. Off, close (only while the reticle's surface is near: the
reticle of a hand held below the eyes slides down a surface as the player
walks up to it, and the line shows that as the gun's beam meeting it), or on
(always). */
enum
{
	CONTROLLER_LASER_OFF,
	CONTROLLER_LASER_CLOSE,
	CONTROLLER_LASER_ON,
	CONTROLLER_LASER_MODE_COUNT
};
#define CONTROLLER_LASER_DISTANCE_DEFAULT 3.0f  /* input.laser_distance, meters */
#define CONTROLLER_LASER_DISTANCE_MINIMUM 1.0f
#define CONTROLLER_LASER_DISTANCE_MAXIMUM 20.0f
/* close mode's line fades in over the last this many meters inside the distance */
#define CONTROLLER_LASER_FADE_METERS 0.5f
/* input.laser's text into a mode: "close"; "on", "true" and "always"; anything else off */
int controller_laser_mode(const char *text);
const char *controller_laser_mode_name(int mode);
/* the live chord's next mode: off, close, on, off again */
int controller_laser_cycle(int mode);
/* input.laser_distance into its range; the default for a value that isn't a number */
float controller_laser_distance_clamp(float meters);
/* how opaque the line is, 0 (not drawn) to 1: on, 1; close, 1 while the
reticle is nearer than distance less CONTROLLER_LASER_FADE_METERS, easing to
0 at distance (meters, from the eyes) and beyond; off, 0. past_cover (the
gun's line passes what the eye's line doesn't) shows it whole in any mode */
float controller_laser_opacity(int mode, float distance, float reticle_meters, int past_cover);
/* the opacity drawn this frame: the last frame's eased toward target over
CONTROLLER_LASER_EASE_SECONDS, so the line never pops: a toggle fades, and
a frame or two of past_cover at a rock's edge shows a trace of it, not a
flash. A frame without a usable time step takes the target */
#define CONTROLLER_LASER_EASE_SECONDS 0.1f
/* under this the line isn't drawn (and an ease toward 0 ends) */
#define CONTROLLER_LASER_OPACITY_MINIMUM 0.01f
float controller_laser_ease(float opacity, float target, float seconds);

/* The aim ray's origin (input.aim_origin_raise): 0 is the hand, where the
barrel is, and 1 is the eyes; between, that share of the way from the hand
to the eyes along the line that joins them. The ray keeps the hand's
direction. A hand held below the eyes points at a spot that slides down a
near surface as the player walks up to it; a raised origin slides less, and
at 1 not at all, while the spot is then above where the controller itself
points by the raised height. The live chord steps it by
CONTROLLER_ORIGIN_RAISE_STEP. */
#define CONTROLLER_ORIGIN_RAISE_STEP 0.1f
/* input.aim_origin_raise into 0 to 1; 0 for a value that isn't a number */
float controller_origin_raise_clamp(float raise);
/* the next setting up (direction 1) or down (-1), on whole tenths, within 0 to 1 */
float controller_origin_raise_step(float raise, int direction);
/* origin moved raise of the way to head; untouched (not a bit of it) at 0 */
void controller_origin_raise(float origin[3], const float head[3], float raise);

#endif
