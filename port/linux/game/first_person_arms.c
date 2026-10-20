/*
FIRST_PERSON_ARMS.C

The first-person gun and arms while a hand aims in HEAD mode
(input.first_person_arms = "solved"; the design in agent-specs
2026-10-13-halo-ce-ios-first-person-arms-design.md).

The game's first-person model was animated for a gun mounted on the camera:
its root node ("frame bone24") is placed at the camera each frame and the
arms come up from below the view. Turning that root onto the hand's
sightline (the "turned" mode, controller_aim_game.c's
halo_controller_weapon_frame) turns the arms about the eyes too, so a hand
pointed to the side brings the forearms in from the side of the view. This
file keeps the root at the view and re-poses the model, once a frame, on the
node matrices the animation built (render-only: nothing the tick reads):

- the gun node ("frame gun", a child of the right wrist in every
  first-person graph) turns about its own position, the grip, by the
  rotation that takes the view's frame to the hand's limited frame, on top
  of the animation's own gun motion, so recoil, a reload's tilt and a
  melee's swing still show, turned to where the hand points. Every node
  under the right wrist (the wrist itself, the fingers, the gun and its
  parts) takes the same rigid move, so the hand stays wrapped around the
  grip as animated;
- the right arm is solved again from its shoulder (the upper arm node,
  which stays where the animation put it) to the moved wrist: a two-bone
  solve with the animation's elbow as the pole, so the elbow stays where
  the animator put it and only bends to follow the wrist. Reach corrections
  move the gun and its supporting hand together before solving either arm;
  past the limits the gun stops short of the hand while the reticle goes on;
- the forearm takes ARMS_TWIST_SHARE of the wrist's twist about the
  forearm's axis, so a rolled gun spreads its twist over the forearm and
  the wrist's blend band instead of folding at the wrist;
- the left wrist and its fingers take the same move weighted by how near
  the left hand is to its place on the gun (the "left hand cyborg" marker,
  or the gun node): fully on the foregrip, not at all while a reload, a
  grenade throw or the flashlight takes the hand away; then the left arm
  is solved the same way.

The limits are angles of the gun relative to the view, in degrees: the
turn across the view, the elevation up and down, and the roll about the
gun's forward, each exact to ARMS_LIMIT_KNEE of its limit and easing to
the limit beyond (aim-round-4's knee), so the gun never flips upside down.
A hand behind the head is held at the limit on the side it left by.

Everything is a function of this frame's inputs; the sightline it follows
is the one input.weapon_smoothing already low-passed in the room's frame.
*/

#include <math.h>
#include <string.h>

#ifndef FIRST_PERSON_ARMS_PROBE
#include "cseries.h"
#include "math/real_math.h"
#include "render/render.h"
#include "first_person_arms.h"
#else
#include "first_person_arms.h"
#endif

#define ARMS_PI 3.14159265358979f
#define ARMS_DEGREES(radians) ((radians) * (180.0f / ARMS_PI))
#define ARMS_RADIANS(degrees) ((degrees) * (ARMS_PI / 180.0f))

/* ---------- vectors (world, float[3]) */

static float dot3(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static float length3(const float a[3])
{
	return sqrtf(dot3(a, a));
}

static int normalize3(float a[3])
{
	float length = length3(a);

	if (!(length > 1e-6f))
		return 0;
	a[0] /= length;
	a[1] /= length;
	a[2] /= length;
	return 1;
}

/* v less its part along the unit vector axis, made unit; 0 if nothing is left */
static int square_to(const float v[3], const float axis[3], float out[3])
{
	float along = dot3(v, axis);
	int i;

	for (i = 0; i < 3; i++)
		out[i] = v[i] - along * axis[i];
	return normalize3(out);
}

/* ---------- rotations: a 3x3 acting on column vectors, r[row][column] */

struct arms_rotation
{
	float r[3][3];
};

static void rotation_identity(struct arms_rotation *rotation)
{
	memset(rotation, 0, sizeof(*rotation));
	rotation->r[0][0] = rotation->r[1][1] = rotation->r[2][2] = 1.0f;
}

static void rotate3(const struct arms_rotation *rotation, const float v[3], float out[3])
{
	float x = v[0], y = v[1], z = v[2];
	int i;

	for (i = 0; i < 3; i++)
		out[i] = rotation->r[i][0] * x + rotation->r[i][1] * y + rotation->r[i][2] * z;
}

/* Rodrigues: the rotation by angle about the unit axis */
static void rotation_from_axis_angle(const float axis[3], float angle, struct arms_rotation *rotation)
{
	float c = cosf(angle), s = sinf(angle), t = 1.0f - c;
	float x = axis[0], y = axis[1], z = axis[2];

	rotation->r[0][0] = t * x * x + c;
	rotation->r[0][1] = t * x * y - s * z;
	rotation->r[0][2] = t * x * z + s * y;
	rotation->r[1][0] = t * x * y + s * z;
	rotation->r[1][1] = t * y * y + c;
	rotation->r[1][2] = t * y * z - s * x;
	rotation->r[2][0] = t * x * z - s * y;
	rotation->r[2][1] = t * y * z + s * x;
	rotation->r[2][2] = t * z * z + c;
}

/* the rotation's axis (unit) and angle (0 to pi); the axis is arbitrary at 0 */
static void rotation_to_axis_angle(const struct arms_rotation *rotation, float axis[3], float *angle)
{
	const float (*r)[3] = rotation->r;
	float trace = r[0][0] + r[1][1] + r[2][2];
	float c = (trace - 1.0f) * 0.5f;

	if (c > 1.0f)
		c = 1.0f;
	if (c < -1.0f)
		c = -1.0f;
	*angle = acosf(c);
	axis[0] = r[2][1] - r[1][2];
	axis[1] = r[0][2] - r[2][0];
	axis[2] = r[1][0] - r[0][1];
	if (!normalize3(axis))
	{
		/* 0 (any axis) or pi (the axis is the eigenvector: the largest diagonal) */
		int k = 0, i;

		if (r[1][1] > r[k][k])
			k = 1;
		if (r[2][2] > r[k][k])
			k = 2;
		for (i = 0; i < 3; i++)
			axis[i] = r[i][k] + (i == k ? 1.0f : 0.0f);
		if (!normalize3(axis))
		{
			axis[0] = 1.0f;
			axis[1] = axis[2] = 0.0f;
		}
	}
}

/* the least rotation taking the unit vector a onto the unit vector b */
static void rotation_between(const float a[3], const float b[3], struct arms_rotation *rotation)
{
	float axis[3], s, c;

	cross3(a, b, axis);
	s = length3(axis);
	c = dot3(a, b);
	if (s < 1e-6f)
	{
		if (c > 0.0f)
		{
			rotation_identity(rotation);
			return;
		}
		/* opposite: half a turn about anything square to a */
		{
			float pick[3] = { 1.0f, 0.0f, 0.0f }, perpendicular[3];

			if (fabsf(a[0]) > 0.9f)
			{
				pick[0] = 0.0f;
				pick[1] = 1.0f;
			}
			if (!square_to(pick, a, perpendicular))
				perpendicular[2] = 1.0f;
			rotation_from_axis_angle(perpendicular, ARMS_PI, rotation);
		}
		return;
	}
	axis[0] /= s;
	axis[1] /= s;
	axis[2] /= s;
	rotation_from_axis_angle(axis, atan2f(s, c), rotation);
}

/* an orthonormal frame from a forward and an up (made square to it): the
columns are forward, left (up x forward) and up, as the engine's matrices
hold them; 0 if degenerate */
static int frame_from_vectors(const float forward_in[3], const float up_in[3], struct arms_rotation *frame)
{
	float forward[3], up[3], left[3];
	int i;

	memcpy(forward, forward_in, sizeof(forward));
	if (!normalize3(forward) || !square_to(up_in, forward, up))
		return 0;
	cross3(up, forward, left);
	for (i = 0; i < 3; i++)
	{
		frame->r[i][0] = forward[i];
		frame->r[i][1] = left[i];
		frame->r[i][2] = up[i];
	}
	return 1;
}

/* a * b (b first) */
static void rotation_multiply(const struct arms_rotation *a, const struct arms_rotation *b, struct arms_rotation *out)
{
	struct arms_rotation product;
	int i, j;

	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			product.r[i][j] = a->r[i][0] * b->r[0][j] + a->r[i][1] * b->r[1][j] + a->r[i][2] * b->r[2][j];
	*out = product;
}

static void rotation_transpose(const struct arms_rotation *a, struct arms_rotation *out)
{
	struct arms_rotation t;
	int i, j;

	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			t.r[i][j] = a->r[j][i];
	*out = t;
}

/* ---------- node matrices (real_matrix4x3: rows forward, left, up, position) */

static void matrix_rows(real_matrix4x3 *matrix, float *rows[3])
{
	rows[0] = &matrix->forward.i;
	rows[1] = &matrix->left.i;
	rows[2] = &matrix->up.i;
}

/* the matrix moved rigidly: its axes turned by rotation, its position
turned about pivot and moved by shift */
static void matrix_move(real_matrix4x3 *matrix, const struct arms_rotation *rotation, const float pivot[3],
	const float shift[3])
{
	float *rows[3], turned[3], offset[3];
	int i;

	matrix_rows(matrix, rows);
	for (i = 0; i < 3; i++)
	{
		rotate3(rotation, rows[i], turned);
		memcpy(rows[i], turned, sizeof(turned));
	}
	offset[0] = matrix->position.x - pivot[0];
	offset[1] = matrix->position.y - pivot[1];
	offset[2] = matrix->position.z - pivot[2];
	rotate3(rotation, offset, turned);
	matrix->position.x = pivot[0] + turned[0] + shift[0];
	matrix->position.y = pivot[1] + turned[1] + shift[1];
	matrix->position.z = pivot[2] + turned[2] + shift[2];
}

static void matrix_position(const real_matrix4x3 *matrix, float out[3])
{
	out[0] = matrix->position.x;
	out[1] = matrix->position.y;
	out[2] = matrix->position.z;
}

static void matrix_set_position(real_matrix4x3 *matrix, const float position[3])
{
	matrix->position.x = position[0];
	matrix->position.y = position[1];
	matrix->position.z = position[2];
}

/* ---------- the nodes */

/* the animation graph's node block, as first_person_weapons.c and
model_animations.c read it (0x40 bytes: the name, then the links) */
struct arms_node
{
	char name[32];
	short next_sibling_node_index;
	short first_child_node_index;
	short parent_node_index;
};

static const struct arms_node *node_at(const void *nodes, long stride, short index)
{
	return (const struct arms_node *)((const char *)nodes + (long)index * stride);
}

static int name_is(const char *name, const char *wanted)
{
	int i;

	for (i = 0; i < 32 && name[i] && wanted[i]; i++)
	{
		char a = name[i], b = wanted[i];

		if (a >= 'A' && a <= 'Z')
			a = (char)(a - 'A' + 'a');
		if (a != b)
			return 0;
	}
	return (i == 32 || !name[i]) && !wanted[i];
}

static short node_named(const void *nodes, long stride, short node_count, const char *wanted)
{
	short index;

	for (index = 0; index < node_count; index++)
		if (name_is(node_at(nodes, stride, index)->name, wanted))
			return index;
	return -1;
}

/* whether node is ancestor or under it (parent links, bounded) */
static int node_under(const void *nodes, long stride, short node_count, short node, short ancestor)
{
	int steps;

	for (steps = 0; steps < node_count && node >= 0 && node < node_count; steps++)
	{
		if (node == ancestor)
			return 1;
		node = node_at(nodes, stride, node)->parent_node_index;
	}
	return 0;
}

/* ---------- the limits */

/* an angle held within limit: itself to ARMS_LIMIT_KNEE of the limit,
easing to the limit beyond with a continuous slope */
static float limited_angle(float angle, float limit)
{
	float knee = limit * ARMS_LIMIT_KNEE, size = fabsf(angle), room = limit - knee;

	if (!(limit > 0.0f))
		return angle;
	if (size <= knee || !(room > 0.0f))
		return angle < -limit ? -limit : angle > limit ? limit : angle;
	size = knee + room * (1.0f - expf(-(size - knee) / room));
	return angle < 0.0f ? -size : size;
}

/* the gun's frame (forward, up; world) held within the limits relative to
the view's frame; 0 if a frame is degenerate */
static int limit_gun_frame(const struct arms_rotation *view, const float forward_in[3], const float up_in[3],
	const struct arms_limits *limits, struct arms_rotation *gun, struct arms_report *report)
{
	float forward[3], up[3], local[3], turn, elevation, roll, level[3], side[3], f[3], u[3];
	float view_forward[3], view_left[3], view_up[3];
	struct arms_rotation view_inverse;
	int i;

	memcpy(forward, forward_in, sizeof(forward));
	if (!normalize3(forward) || !square_to(up_in, forward, up))
		return 0;
	for (i = 0; i < 3; i++)
	{
		view_forward[i] = view->r[i][0];
		view_left[i] = view->r[i][1];
		view_up[i] = view->r[i][2];
	}
	rotation_transpose(view, &view_inverse);
	rotate3(&view_inverse, forward, local);
	/* (x forward, y left, z up in the view) */
	turn = atan2f(local[1], local[0]);
	elevation = atan2f(local[2], sqrtf(local[0] * local[0] + local[1] * local[1]));
	/* a hand behind the head: held at the limit on the side it left by */
	if (local[0] < 0.0f)
		turn = turn < 0.0f ? -ARMS_PI * 0.5f : ARMS_PI * 0.5f;
	report->turn_degrees = ARMS_DEGREES(turn);
	report->elevation_degrees = ARMS_DEGREES(elevation);
	turn = limited_angle(turn, ARMS_RADIANS(limits->turn_degrees));
	if (elevation > 0.0f)
		elevation = limited_angle(elevation, ARMS_RADIANS(limits->up_degrees));
	else
		elevation = limited_angle(elevation, ARMS_RADIANS(limits->down_degrees));
	for (i = 0; i < 3; i++)
		f[i] = cosf(elevation) * (cosf(turn) * view_forward[i] + sinf(turn) * view_left[i]) +
			sinf(elevation) * view_up[i];
	/* the roll: the up carried onto the limited forward, measured from the
	view's up made square to it, about the forward */
	if (!square_to(up, f, u))
		memcpy(u, view_up, sizeof(u));
	if (!square_to(view_up, f, level))
	{
		/* straight up or down: the view's forward made square stands for level */
		if (!square_to(view_forward, f, level))
			return 0;
	}
	cross3(f, level, side);
	roll = atan2f(dot3(u, side), dot3(u, level));
	report->roll_degrees = ARMS_DEGREES(roll);
	roll = limited_angle(roll, ARMS_RADIANS(limits->roll_degrees));
	for (i = 0; i < 3; i++)
		u[i] = cosf(roll) * level[i] + sinf(roll) * side[i];
	report->limited_turn_degrees = ARMS_DEGREES(turn);
	report->limited_elevation_degrees = ARMS_DEGREES(elevation);
	report->limited_roll_degrees = ARMS_DEGREES(roll);
	return frame_from_vectors(f, u, gun);
}

/* ---------- the arm */

/* a two-bone solve from the shoulder (upper arm node, fixed) through the
elbow (forearm node) to the wrist's new position, on the base pose: the
upper arm's and forearm's matrices are turned by the least rotations that
take the base bones onto the new ones, so their roll is the animation's,
then the forearm takes ARMS_TWIST_SHARE of the wrist's twist about the
forearm's axis relative to the base pose. wrist_target is the wrist's new
position in and, out of reach, pulled back onto the arm's reach out;
returns 1 when it was pulled, 0 when reached, -1 when the arm is
degenerate (nothing changed) */
static int solve_arm(real_matrix4x3 *upper, real_matrix4x3 *forearm, const real_matrix4x3 *base_upper,
	const real_matrix4x3 *base_forearm, const real_matrix4x3 *base_wrist, const real_matrix4x3 *wrist,
	float wrist_target[3], const float fallback_pole[3])
{
	float shoulder[3], elbow[3], base_wrist_position[3], a, c, v[3], d, unit[3], pole[3], x, h, new_elbow[3];
	float bone[3], new_bone[3], axis[3];
	struct arms_rotation turn;
	int pulled = 0, i;

	matrix_position(base_upper, shoulder);
	matrix_position(base_forearm, elbow);
	matrix_position(base_wrist, base_wrist_position);
	for (i = 0; i < 3; i++)
	{
		bone[i] = elbow[i] - shoulder[i];
		new_bone[i] = base_wrist_position[i] - elbow[i];
	}
	a = length3(bone);
	c = length3(new_bone);
	if (!(a > 1e-4f) || !(c > 1e-4f))
		return -1;
	for (i = 0; i < 3; i++)
		v[i] = wrist_target[i] - shoulder[i];
	d = length3(v);
	if (!(d > 1e-5f))
		return -1;
	for (i = 0; i < 3; i++)
		unit[i] = v[i] / d;
	if (d > (a + c) * ARMS_REACH_SHARE)
	{
		d = (a + c) * ARMS_REACH_SHARE;
		pulled = 1;
	}
	if (d < fabsf(a - c) * 1.001f + 1e-4f)
	{
		d = fabsf(a - c) * 1.001f + 1e-4f;
		pulled = 1;
	}
	for (i = 0; i < 3; i++)
		wrist_target[i] = shoulder[i] + unit[i] * d;
	/* the pole: the animation's elbow, square to the new shoulder-to-wrist line */
	if (!square_to(bone, unit, pole) && !square_to(fallback_pole, unit, pole))
		return -1;
	x = (a * a - c * c + d * d) / (2.0f * d);
	h = a * a - x * x;
	h = h > 0.0f ? sqrtf(h) : 0.0f;
	for (i = 0; i < 3; i++)
		new_elbow[i] = shoulder[i] + unit[i] * x + pole[i] * h;
	/* the upper arm: the base bone onto the new one, about the shoulder */
	for (i = 0; i < 3; i++)
	{
		bone[i] /= a;
		new_bone[i] = (new_elbow[i] - shoulder[i]) / a;
	}
	if (!normalize3(new_bone))
		return -1;
	rotation_between(bone, new_bone, &turn);
	*upper = *base_upper;
	{
		float none[3] = { 0.0f, 0.0f, 0.0f };

		matrix_move(upper, &turn, shoulder, none);
	}
	/* the forearm: its base bone onto the new one, placed at the new elbow */
	for (i = 0; i < 3; i++)
	{
		bone[i] = (base_wrist_position[i] - elbow[i]) / c;
		new_bone[i] = wrist_target[i] - new_elbow[i];
	}
	if (!normalize3(new_bone))
		return -1;
	rotation_between(bone, new_bone, &turn);
	*forearm = *base_forearm;
	{
		float none[3] = { 0.0f, 0.0f, 0.0f };

		matrix_move(forearm, &turn, elbow, none);
	}
	matrix_set_position(forearm, new_elbow);
	/* the twist: the wrist's up about the forearm's axis, against the
	forearm's up, now against the base pose; the forearm takes a share of the
	change */
	memcpy(axis, new_bone, sizeof(axis));
	{
		float base_axis[3], forearm_up[3], wrist_up[3], before, after, delta;
		float *rows[3];
		float side[3];

		memcpy(base_axis, bone, sizeof(base_axis));
		matrix_rows((real_matrix4x3 *)base_forearm, rows);
		if (!square_to(rows[2], base_axis, forearm_up))
			return pulled;
		matrix_rows((real_matrix4x3 *)base_wrist, rows);
		if (!square_to(rows[2], base_axis, wrist_up))
			return pulled;
		cross3(base_axis, forearm_up, side);
		before = atan2f(dot3(wrist_up, side), dot3(wrist_up, forearm_up));
		matrix_rows(forearm, rows);
		if (!square_to(rows[2], axis, forearm_up))
			return pulled;
		matrix_rows((real_matrix4x3 *)wrist, rows);
		if (!square_to(rows[2], axis, wrist_up))
			return pulled;
		cross3(axis, forearm_up, side);
		after = atan2f(dot3(wrist_up, side), dot3(wrist_up, forearm_up));
		delta = after - before;
		while (delta > ARMS_PI)
			delta -= 2.0f * ARMS_PI;
		while (delta < -ARMS_PI)
			delta += 2.0f * ARMS_PI;
		if (fabsf(delta) > 1e-4f)
		{
			float none[3] = { 0.0f, 0.0f, 0.0f };

			rotation_from_axis_angle(axis, delta * ARMS_TWIST_SHARE, &turn);
			matrix_move(forearm, &turn, new_elbow, none);
		}
	}
	return pulled;
}

/* ---------- the solve */

/* Project a wrist onto its arm's reach without changing the pose. Reach
corrections are shared by the gun and its supporting hand before either
arm is solved, so one arm cannot pull its hand away from the other. */
static int wrist_reach_correction(const real_matrix4x3 *upper, const real_matrix4x3 *forearm,
	const real_matrix4x3 *wrist, const float target[3], float correction[3])
{
	float shoulder[3], elbow[3], hand[3], a[3], b[3], delta[3], distance, minimum, maximum, reached;
	int i;

	matrix_position(upper, shoulder);
	matrix_position(forearm, elbow);
	matrix_position(wrist, hand);
	for (i = 0; i < 3; i++)
	{
		a[i] = elbow[i] - shoulder[i];
		b[i] = hand[i] - elbow[i];
		delta[i] = target[i] - shoulder[i];
		correction[i] = 0.0f;
	}
	distance = length3(delta);
	if (!(distance > 1e-5f) || !(length3(a) > 1e-4f) || !(length3(b) > 1e-4f))
		return 0;
	minimum = fabsf(length3(a) - length3(b)) * 1.001f + 1e-4f;
	maximum = (length3(a) + length3(b)) * ARMS_REACH_SHARE;
	reached = distance < minimum ? minimum : distance > maximum ? maximum : distance;
	for (i = 0; i < 3; i++)
		correction[i] = delta[i] * ((reached - distance) / distance);
	return fabsf(reached - distance) > 1e-6f;
}

/* the hand's share of the gun's move: 1 on the grip, 0 away from it */
static float left_hand_share(const real_matrix4x3 *left_wrist, const float grip[3])
{
	float position[3], distance;

	matrix_position(left_wrist, position);
	position[0] -= grip[0];
	position[1] -= grip[1];
	position[2] -= grip[2];
	distance = length3(position);
	if (distance <= ARMS_LEFT_ON_GRIP)
		return 1.0f;
	if (distance >= ARMS_LEFT_OFF_GRIP)
		return 0.0f;
	{
		float t = (distance - ARMS_LEFT_ON_GRIP) / (ARMS_LEFT_OFF_GRIP - ARMS_LEFT_ON_GRIP);

		return 1.0f - t * t * (3.0f - 2.0f * t);
	}
}

int first_person_arms_solve(real_matrix4x3 *matrices, short node_count, const void *nodes, long node_stride,
	const struct arms_input *input, const struct arms_limits *limits, struct arms_report *report)
{
	struct arms_rotation view, gun, turn, full;
	float axis[3], angle, pivot[3], shift[3], left_grip[3], none[3] = { 0.0f, 0.0f, 0.0f };
	short root, right_upper, right_forearm, right_wrist, gun_node, left_upper, left_forearm, left_wrist, index;
	real_matrix4x3 base_right_upper, base_right_forearm, base_right_wrist, base_left_upper, base_left_forearm;
	real_matrix4x3 base_left_wrist, base_gun;
	float right_target[3], left_target[3], fallback_pole[3];
	float left_share;
	int i;

	memset(report, 0, sizeof(*report));
	if (!matrices || node_count <= 0 || !nodes || node_stride < (long)sizeof(struct arms_node))
		return 0;
	root = node_named(nodes, node_stride, node_count, "frame bone24");
	right_upper = node_named(nodes, node_stride, node_count, "frame r upperarm");
	right_forearm = node_named(nodes, node_stride, node_count, "frame r forearm");
	right_wrist = node_named(nodes, node_stride, node_count, "frame r wriste");
	gun_node = node_named(nodes, node_stride, node_count, "frame gun");
	left_upper = node_named(nodes, node_stride, node_count, "frame l upperarm");
	left_forearm = node_named(nodes, node_stride, node_count, "frame l forearm");
	left_wrist = node_named(nodes, node_stride, node_count, "frame l wriste");
	if (root < 0 || right_upper < 0 || right_forearm < 0 || right_wrist < 0 || gun_node < 0 || left_upper < 0 ||
		left_forearm < 0 || left_wrist < 0)
		return 0;
	/* (the gun under the right wrist, the chain's links as the solve assumes) */
	if (!node_under(nodes, node_stride, node_count, gun_node, right_wrist) ||
		node_at(nodes, node_stride, right_wrist)->parent_node_index != right_forearm ||
		node_at(nodes, node_stride, right_forearm)->parent_node_index != right_upper ||
		node_at(nodes, node_stride, left_wrist)->parent_node_index != left_forearm ||
		node_at(nodes, node_stride, left_forearm)->parent_node_index != left_upper)
		return 0;
	if (!frame_from_vectors(input->view_forward, input->view_up, &view))
		return 0;
	if (!limit_gun_frame(&view, input->gun_forward, input->gun_up, limits, &gun, report))
		return 0;
	/* the turn from the view's frame to the gun's, eased in by the blend
	along its own axis */
	rotation_transpose(&view, &turn);
	rotation_multiply(&gun, &turn, &full);
	rotation_to_axis_angle(&full, axis, &angle);
	{
		float blend = input->blend < 0.0f ? 0.0f : input->blend > 1.0f ? 1.0f : input->blend;

		rotation_from_axis_angle(axis, angle * blend, &full);
		for (i = 0; i < 3; i++)
			shift[i] = input->grip_shift[i] * blend;
		report->blend = blend;
	}
	base_right_upper = matrices[right_upper];
	base_right_forearm = matrices[right_forearm];
	base_right_wrist = matrices[right_wrist];
	base_left_upper = matrices[left_upper];
	base_left_forearm = matrices[left_forearm];
	base_left_wrist = matrices[left_wrist];
	base_gun = matrices[gun_node];
	matrix_position(&base_gun, pivot);
	/* the left hand's place on the gun, from the base pose */
	if (input->left_grip_valid)
		memcpy(left_grip, input->left_grip, sizeof(left_grip));
	else
		memcpy(left_grip, pivot, sizeof(left_grip));
	left_share = left_hand_share(&base_left_wrist, left_grip);
	report->left_share = left_share;
	/* the elbows' fallback pole: the view's down, so an arm that was
	straight bends downward */
	for (i = 0; i < 3; i++)
		fallback_pole[i] = -view.r[i][2];
	/* the right wrist and everything under it (the fingers, the gun and its
	parts): the gun's move about the grip */
	for (index = 0; index < node_count; index++)
		if (node_under(nodes, node_stride, node_count, index, right_wrist))
			matrix_move(&matrices[index], &full, pivot, shift);
	/* the left wrist and its fingers: the same move, by the hand's share */
	if (left_share > 0.0f)
	{
		struct arms_rotation part;
		float part_shift[3];

		rotation_from_axis_angle(axis, angle * report->blend * left_share, &part);
		for (i = 0; i < 3; i++)
			part_shift[i] = shift[i] * left_share;
		for (index = 0; index < node_count; index++)
			if (node_under(nodes, node_stride, node_count, index, left_wrist))
				matrix_move(&matrices[index], &part, pivot, part_shift);
	}
	/* Find a shared translation within both arms' reach. A released hand
	(left_share == 0) keeps its animation. During release/regrip it takes
	the same fraction of this correction as of the original gun motion.
	Alternating the reach projections handles either arm reaching its limit
	first, while preserving the gun's orientation and both grip transforms. */
	{
		float correction[3], common[3] = { 0.0f, 0.0f, 0.0f };
		int iteration;
		struct arms_rotation identity;

		matrix_position(&matrices[right_wrist], right_target);
		matrix_position(&matrices[left_wrist], left_target);
		for (iteration = 0; iteration < 64; iteration++)
		{
			int changed = 0;

			if (wrist_reach_correction(&base_right_upper, &base_right_forearm, &base_right_wrist,
				right_target, correction))
			{
				report->right_pulled = changed = 1;
				for (i = 0; i < 3; i++)
				{
					common[i] += correction[i];
					right_target[i] += correction[i];
					left_target[i] += correction[i] * left_share;
				}
			}
			if (left_share > 0.001f && wrist_reach_correction(&base_left_upper, &base_left_forearm,
				&base_left_wrist, left_target, correction))
			{
				report->left_pulled = changed = 1;
				for (i = 0; i < 3; i++)
				{
					/* Fade the support hand's influence as it releases. Do
					not divide by a near-zero contact weight and move the
					gun a long way to satisfy a detached hand. */
					common[i] += correction[i] * left_share;
					right_target[i] += correction[i] * left_share;
					left_target[i] += correction[i] * left_share * left_share;
				}
			}
			if (!changed)
				break;
		}
		rotation_identity(&identity);
		for (index = 0; index < node_count; index++)
			if (node_under(nodes, node_stride, node_count, index, right_wrist))
				matrix_move(&matrices[index], &identity, none, common);
		for (i = 0; i < 3; i++)
			common[i] *= left_share;
		for (index = 0; index < node_count; index++)
			if (node_under(nodes, node_stride, node_count, index, left_wrist))
				matrix_move(&matrices[index], &identity, none, common);
	}
	/* Solve the arms only after both hand targets share the reach correction.
	The final individual clamp remains a fallback for incompatible reaches. */
	matrix_position(&matrices[right_wrist], right_target);
	report->right_pulled |= solve_arm(&matrices[right_upper], &matrices[right_forearm], &base_right_upper,
		&base_right_forearm, &base_right_wrist, &matrices[right_wrist], right_target, fallback_pole);
	if (report->right_pulled > 0)
	{
		float moved[3], was[3];
		struct arms_rotation identity;

		rotation_identity(&identity);
		matrix_position(&matrices[right_wrist], was);
		for (i = 0; i < 3; i++)
			moved[i] = right_target[i] - was[i];
		for (index = 0; index < node_count; index++)
			if (node_under(nodes, node_stride, node_count, index, right_wrist))
				matrix_move(&matrices[index], &identity, none, moved);
	}
	matrix_position(&matrices[left_wrist], left_target);
	report->left_pulled |= solve_arm(&matrices[left_upper], &matrices[left_forearm], &base_left_upper,
		&base_left_forearm, &base_left_wrist, &matrices[left_wrist], left_target, fallback_pole);
	if (report->left_pulled > 0)
	{
		float moved[3], was[3];
		struct arms_rotation identity;

		rotation_identity(&identity);
		matrix_position(&matrices[left_wrist], was);
		for (i = 0; i < 3; i++)
			moved[i] = left_target[i] - was[i];
		for (index = 0; index < node_count; index++)
			if (node_under(nodes, node_stride, node_count, index, left_wrist))
				matrix_move(&matrices[index], &identity, none, moved);
	}
	{
		float *rows[3];

		matrix_rows(&matrices[gun_node], rows);
		memcpy(report->gun_forward, rows[0], sizeof(report->gun_forward));
		memcpy(report->gun_up, rows[2], sizeof(report->gun_up));
		matrix_position(&matrices[gun_node], report->gun_position);
		matrix_position(&matrices[right_forearm], report->right_elbow);
		matrix_position(&matrices[left_forearm], report->left_elbow);
	}
	(void)root;
	return 1;
}

#ifndef FIRST_PERSON_ARMS_PROBE

/* ---------- the engine's call */

int config_boolean(const char *name);
double config_real(const char *name);
void platform_log(const char *format, ...);
unsigned long platform_clock_frames(void);

/* the turn limit the live chord chose (halo_first_person_arms_turn_limit_set),
over input.arms_turn_limit once set; negative until then */
static float turn_limit_live = -1.0f;

void halo_first_person_arms_turn_limit_set(float degrees)
{
	turn_limit_live = degrees;
}

static struct arms_limits arms_limits_setting(void)
{
	struct arms_limits limits;

	limits.turn_degrees = turn_limit_live >= 0.0f ? turn_limit_live : (float)config_real("input.arms_turn_limit");
	limits.up_degrees = (float)config_real("input.arms_up_limit");
	limits.down_degrees = (float)config_real("input.arms_down_limit");
	limits.roll_degrees = (float)config_real("input.arms_roll_limit");
	if (!(limits.turn_degrees >= 0.0f && limits.turn_degrees <= 90.0f))
		limits.turn_degrees = ARMS_TURN_LIMIT_DEFAULT;
	if (!(limits.up_degrees >= 0.0f && limits.up_degrees <= 89.0f))
		limits.up_degrees = ARMS_UP_LIMIT_DEFAULT;
	if (!(limits.down_degrees >= 0.0f && limits.down_degrees <= 89.0f))
		limits.down_degrees = ARMS_DOWN_LIMIT_DEFAULT;
	if (!(limits.roll_degrees >= 0.0f && limits.roll_degrees <= 180.0f))
		limits.roll_degrees = ARMS_ROLL_LIMIT_DEFAULT;
	return limits;
}

float halo_first_person_arms_turn_limit(void)
{
	return arms_limits_setting().turn_degrees;
}

int halo_first_person_arms_apply(short local_player_index, real_matrix4x3 *matrices, short node_count,
	const void *graph_nodes, long graph_node_count, const float left_grip[3], int left_grip_valid)
{
	struct arms_input input;
	struct arms_limits limits;
	struct arms_report report;
	static long logged_second = -1;
	static int log_on = -1;
	int result;

	if (halo_controller_arms_mode() != HALO_ARMS_SOLVED)
		return 0;
	if (!halo_controller_gun_frame(local_player_index, input.gun_forward, input.gun_up, &input.blend,
		input.grip_shift))
		return 0;
	input.view_forward[0] = render.camera.forward.i;
	input.view_forward[1] = render.camera.forward.j;
	input.view_forward[2] = render.camera.forward.k;
	input.view_up[0] = render.camera.up.i;
	input.view_up[1] = render.camera.up.j;
	input.view_up[2] = render.camera.up.k;
	input.left_grip_valid = left_grip_valid;
	if (left_grip_valid)
		memcpy(input.left_grip, left_grip, sizeof(input.left_grip));
	else
		memset(input.left_grip, 0, sizeof(input.left_grip));
	limits = arms_limits_setting();
	if (graph_node_count < node_count)
		node_count = (short)graph_node_count;
	result = first_person_arms_solve(matrices, node_count, graph_nodes, 0x40, &input, &limits, &report);
	if (log_on < 0)
		log_on = config_boolean("debug.aim_trace");
	if (log_on)
	{
		long second = (long)(platform_clock_frames() / 20);

		if (second != logged_second)
		{
			logged_second = second;
			platform_log("first-person arms: %s turn %.1f elev %.1f roll %.1f -> %.1f %.1f %.1f blend %.2f left %.2f "
				"pulled %d %d gun %.3f %.3f %.3f", result ? "solved" : "not solved", report.turn_degrees,
				report.elevation_degrees, report.roll_degrees, report.limited_turn_degrees,
				report.limited_elevation_degrees, report.limited_roll_degrees, report.blend, report.left_share,
				report.right_pulled, report.left_pulled, report.gun_position[0], report.gun_position[1],
				report.gun_position[2]);
		}
	}
	return result;
}

#endif
