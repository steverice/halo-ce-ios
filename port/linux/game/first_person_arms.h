/*
FIRST_PERSON_ARMS.H

The first-person gun and arms re-posed for hand aiming (first_person_arms.c).
*/

#ifndef __FIRST_PERSON_ARMS_H
#define __FIRST_PERSON_ARMS_H

/* (the probe, port/ios/tests/first_person_arms_probe.c, defines
real_matrix4x3 in the engine's shape before including first_person_arms.c) */

/* input.first_person_arms */
enum
{
	HALO_ARMS_TURNED = 0,       /* the whole model turned onto the sightline at the eyes (before) */
	HALO_ARMS_LIMITED,          /* the same, the turn and roll held near the view's center (aim-round-4) */
	HALO_ARMS_SOLVED,           /* the gun turned at the grip, the arms solved (this) */
	NUMBER_OF_HALO_ARMS_MODES
};

/* the limits' knee: exact to this share of a limit, easing to it beyond */
#define ARMS_LIMIT_KNEE 0.7f
/* the gun's angles relative to the view (degrees): the defaults */
#define ARMS_TURN_LIMIT_DEFAULT 45.0f
#define ARMS_UP_LIMIT_DEFAULT 40.0f
#define ARMS_DOWN_LIMIT_DEFAULT 50.0f
#define ARMS_ROLL_LIMIT_DEFAULT 60.0f
/* the forearm's share of the wrist's twist about the forearm's axis */
#define ARMS_TWIST_SHARE 0.5f
/* how much of an arm's full length a wrist may be from the shoulder */
#define ARMS_REACH_SHARE 0.995f
/* the left hand on the gun: fully within ARMS_LEFT_ON_GRIP of its place
(world units; 0.033 is 10 cm), not at all beyond ARMS_LEFT_OFF_GRIP (25 cm) */
#define ARMS_LEFT_ON_GRIP 0.033f
#define ARMS_LEFT_OFF_GRIP 0.082f

struct arms_input
{
	float view_forward[3], view_up[3];   /* the mounting: the view's frame (world, unit) */
	float gun_forward[3], gun_up[3];     /* where the gun should point (world; the smoothed sightline and barrel up) */
	float blend;                         /* the action's blend: 0 the view's frame, 1 the gun's */
	float grip_shift[3];                 /* the grip's move (world units): the position term */
	float left_grip[3];                  /* the left palm's place on the gun in the base pose (world) */
	int left_grip_valid;
};

struct arms_limits
{
	float turn_degrees, up_degrees, down_degrees, roll_degrees;
};

struct arms_report
{
	float turn_degrees, elevation_degrees, roll_degrees;                            /* the hand's, before the limits */
	float limited_turn_degrees, limited_elevation_degrees, limited_roll_degrees;    /* after */
	float blend, left_share;
	int right_pulled, left_pulled;       /* 1 the wrist was out of reach and pulled back; -1 the arm was degenerate */
	float gun_forward[3], gun_up[3], gun_position[3], right_elbow[3], left_elbow[3];
};

/* The solve on the animation's node matrices (world; the root at the view),
in place. nodes is the animation graph's node block (name first, the parent
link at 36: node_stride 0x40 in the engine). Returns 1 when the pose was
changed, 0 when the graph isn't a first-person weapon's (the eight named
nodes and their links), a frame is degenerate, or the inputs are missing;
then the matrices are left as they were */
/* for controller_aim_game.c's live chord: the solved arms' turn limit in
use (degrees), and a value to use over input.arms_turn_limit for the session */
float halo_first_person_arms_turn_limit(void);
void halo_first_person_arms_turn_limit_set(float degrees);
int first_person_arms_solve(real_matrix4x3 *matrices, short node_count, const void *nodes, long node_stride,
	const struct arms_input *input, const struct arms_limits *limits, struct arms_report *report);

#ifndef FIRST_PERSON_ARMS_PROBE
/* first_person_weapons.c: after the pose is built and interpolated for the
frame, in "solved" mode while a hand aims: the solve on the local player's
first-person node matrices (graph_nodes: the graph's node block, 0x40
bytes each; left_grip: the "left hand cyborg" marker's world position from
the base pose, when the weapon has one). 1 when the pose was changed */
int halo_first_person_arms_apply(short local_player_index, real_matrix4x3 *matrices, short node_count,
	const void *graph_nodes, long graph_node_count, const float left_grip[3], int left_grip_valid);

/* controller_aim_game.c: the mode in force (input.first_person_arms, or the
live chord's), its live cycle (Back and the left stick), and the gun's frame
for the solve: the smoothed sightline and barrel up (world), the action's
blend and the grip's shift; 0 when no hand aims this frame */
int halo_controller_arms_mode(void);
void halo_controller_arms_cycle(int direction);
int halo_controller_gun_frame(short local_player_index, float forward[3], float up[3], float *blend,
	float shift[3]);
#endif

#endif
