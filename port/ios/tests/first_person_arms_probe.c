/* The first-person arms solve (port/linux/game/first_person_arms.c,
included, not linked) on a synthetic first-person skeleton in the game's
shape (the root at the view, upper arm, forearm, wrist and fingers on each
side, the gun and a part under the right wrist): with the gun's frame equal
to the view's nothing moves; a turn within the limits points the gun along
it exactly, keeps the right hand's offset to the gun, every bone's length,
the shoulders, the fingers' and the part's relation to their parent, and
every matrix orthonormal; a turn past a limit is held at the eased limit;
a roll past its limit likewise, and the forearm takes a share of the
wrist's twist; a hand behind the head is held at the limit; blend 0 leaves
the pose; a left hand away from the gun stays where the animation put it;
a grip shifted past the arm's reach pulls the wrist and the gun back onto
the reach; a graph without the gun node comes back unchanged. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef float real;
typedef struct { real i, j, k; } real_vector3d;
typedef struct { real x, y, z; } real_point3d;
struct real_matrix4x3
{
	real scale;
	real_vector3d forward, left, up;
	real_point3d position;
};
typedef struct real_matrix4x3 real_matrix4x3;

#define FIRST_PERSON_ARMS_PROBE
#include "../../linux/game/first_person_arms.c"

static int failures;

#define CHECK(condition, ...) do { if (!(condition)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------- the skeleton */

struct probe_node
{
	char name[32];
	short next_sibling_node_index, first_child_node_index, parent_node_index;
	short pad[13];   /* 0x40 bytes, as the graph's */
};

enum { ROOT, L_UPPER, R_UPPER, L_FOREARM, R_FOREARM, L_WRIST, R_WRIST, GUN, MAGAZINE, R_INDEX, L_INDEX, NODE_COUNT };

static struct probe_node nodes[NODE_COUNT];
static real_matrix4x3 base[NODE_COUNT], pose[NODE_COUNT];

static void node(int index, const char *name, int parent)
{
	memset(&nodes[index], 0, sizeof(nodes[index]));
	strncpy(nodes[index].name, name, 31);
	nodes[index].parent_node_index = (short)parent;
	nodes[index].next_sibling_node_index = nodes[index].first_child_node_index = -1;
}

static void place(int index, const float position[3], const float forward_in[3], const float up_in[3])
{
	struct arms_rotation frame;
	real_matrix4x3 *m = &base[index];
	int i;

	if (!frame_from_vectors(forward_in, up_in, &frame))
	{
		printf("FAIL: node %d's frame is degenerate\n", index);
		exit(1);
	}
	m->scale = 1.0f;
	for (i = 0; i < 3; i++)
	{
		(&m->forward.i)[i] = frame.r[i][0];
		(&m->left.i)[i] = frame.r[i][1];
		(&m->up.i)[i] = frame.r[i][2];
	}
	m->position.x = position[0];
	m->position.y = position[1];
	m->position.z = position[2];
}

static void along(const float from[3], const float direction_in[3], float length, float out[3])
{
	float direction[3];

	memcpy(direction, direction_in, sizeof(direction));
	normalize3(direction);
	out[0] = from[0] + direction[0] * length;
	out[1] = from[1] + direction[1] * length;
	out[2] = from[2] + direction[2] * length;
}

#define UPPER_LENGTH 0.103f
#define FOREARM_LENGTH 0.109f

static void build_skeleton(void)
{
	static const float z_up[3] = { 0.0f, 0.0f, 1.0f }, x_forward[3] = { 1.0f, 0.0f, 0.0f };
	float root[3] = { -0.09f, -0.03f, -0.21f };
	float r_shoulder[3] = { -0.12f, -0.2f, -0.19f }, l_shoulder[3] = { 0.0f, 0.2f, -0.23f };
	float r_elbow[3], r_wrist[3], l_elbow[3], l_wrist[3], gun[3], magazine[3], r_index[3], l_index[3];
	float r_upper_direction[3] = { 0.5f, 0.1f, -0.85f }, r_forearm_direction[3] = { 0.9f, 0.35f, 0.25f };
	float l_upper_direction[3] = { 0.6f, -0.2f, -0.75f }, l_forearm_direction[3] = { 0.9f, -0.35f, 0.25f };
	float direction[3];
	int i;

	node(ROOT, "frame bone24", -1);
	node(L_UPPER, "frame l upperarm", ROOT);
	node(R_UPPER, "frame r upperarm", ROOT);
	node(L_FOREARM, "frame l forearm", L_UPPER);
	node(R_FOREARM, "frame r forearm", R_UPPER);
	node(L_WRIST, "frame l wriste", L_FOREARM);
	node(R_WRIST, "frame r wriste", R_FOREARM);
	node(GUN, "frame gun", R_WRIST);
	node(MAGAZINE, "frame magazine", GUN);
	node(R_INDEX, "frame r index low", R_WRIST);
	node(L_INDEX, "frame l index low", L_WRIST);
	along(r_shoulder, r_upper_direction, UPPER_LENGTH, r_elbow);
	along(r_elbow, r_forearm_direction, FOREARM_LENGTH, r_wrist);
	along(l_shoulder, l_upper_direction, UPPER_LENGTH, l_elbow);
	along(l_elbow, l_forearm_direction, FOREARM_LENGTH, l_wrist);
	gun[0] = r_wrist[0] + 0.1f;
	gun[1] = r_wrist[1] + 0.02f;
	gun[2] = r_wrist[2] - 0.01f;
	magazine[0] = gun[0] - 0.054f;
	magazine[1] = gun[1];
	magazine[2] = gun[2] - 0.009f;
	for (i = 0; i < 3; i++)
	{
		r_index[i] = r_wrist[i] + (i == 0 ? 0.035f : i == 2 ? 0.015f : -0.004f);
		l_index[i] = l_wrist[i] + (i == 0 ? 0.035f : i == 2 ? 0.015f : 0.004f);
	}
	place(ROOT, root, x_forward, z_up);
	for (i = 0; i < 3; i++)
		direction[i] = r_elbow[i] - r_shoulder[i];
	place(R_UPPER, r_shoulder, direction, z_up);
	for (i = 0; i < 3; i++)
		direction[i] = r_wrist[i] - r_elbow[i];
	place(R_FOREARM, r_elbow, direction, z_up);
	place(R_WRIST, r_wrist, x_forward, z_up);
	place(GUN, gun, x_forward, z_up);
	place(MAGAZINE, magazine, x_forward, z_up);
	place(R_INDEX, r_index, x_forward, z_up);
	for (i = 0; i < 3; i++)
		direction[i] = l_elbow[i] - l_shoulder[i];
	place(L_UPPER, l_shoulder, direction, z_up);
	for (i = 0; i < 3; i++)
		direction[i] = l_wrist[i] - l_elbow[i];
	place(L_FOREARM, l_elbow, direction, z_up);
	place(L_WRIST, l_wrist, x_forward, z_up);
	place(L_INDEX, l_index, x_forward, z_up);
}

/* ---------- measures */

static float distance(const real_matrix4x3 *a, const real_matrix4x3 *b)
{
	float d[3] = { a->position.x - b->position.x, a->position.y - b->position.y, a->position.z - b->position.z };

	return length3(d);
}

static int orthonormal(const real_matrix4x3 *m)
{
	const float *f = &m->forward.i, *l = &m->left.i, *u = &m->up.i;
	float cross[3];

	if (fabsf(length3(f) - 1.0f) > 1e-3f || fabsf(length3(l) - 1.0f) > 1e-3f || fabsf(length3(u) - 1.0f) > 1e-3f)
		return 0;
	if (fabsf(dot3(f, l)) > 1e-3f || fabsf(dot3(f, u)) > 1e-3f || fabsf(dot3(l, u)) > 1e-3f)
		return 0;
	cross3(u, f, cross);
	return fabsf(cross[0] - l[0]) < 1e-3f && fabsf(cross[1] - l[1]) < 1e-3f && fabsf(cross[2] - l[2]) < 1e-3f;
}

/* b's position and axes in a's frame */
static void relative(const real_matrix4x3 *a, const real_matrix4x3 *b, float out[12])
{
	const float *rows[3] = { &b->forward.i, &b->left.i, &b->up.i };
	const float *axes[3] = { &a->forward.i, &a->left.i, &a->up.i };
	float offset[3] = { b->position.x - a->position.x, b->position.y - a->position.y, b->position.z - a->position.z };
	int i, j;

	for (i = 0; i < 3; i++)
		out[i] = dot3(offset, axes[i]);
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
			out[3 + i * 3 + j] = dot3(rows[i], axes[j]);
}

static float relative_change(const real_matrix4x3 *base_a, const real_matrix4x3 *base_b, const real_matrix4x3 *a,
	const real_matrix4x3 *b)
{
	float before[12], after[12], worst = 0.0f;
	int i;

	relative(base_a, base_b, before);
	relative(a, b, after);
	for (i = 0; i < 12; i++)
		if (fabsf(after[i] - before[i]) > worst)
			worst = fabsf(after[i] - before[i]);
	return worst;
}

static int same(const real_matrix4x3 *a, const real_matrix4x3 *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

static void yaw_pitch_roll_frame(float yaw_degrees, float pitch_degrees, float roll_degrees, float forward[3],
	float up[3])
{
	float yaw = ARMS_RADIANS(yaw_degrees), pitch = ARMS_RADIANS(pitch_degrees), roll = ARMS_RADIANS(roll_degrees);
	float level[3], side[3];
	int i;

	forward[0] = cosf(pitch) * cosf(yaw);
	forward[1] = cosf(pitch) * sinf(yaw);
	forward[2] = sinf(pitch);
	level[0] = -sinf(pitch) * cosf(yaw);
	level[1] = -sinf(pitch) * sinf(yaw);
	level[2] = cosf(pitch);
	cross3(forward, level, side);
	for (i = 0; i < 3; i++)
		up[i] = cosf(roll) * level[i] + sinf(roll) * side[i];
}

static struct arms_limits limits = { ARMS_TURN_LIMIT_DEFAULT, ARMS_UP_LIMIT_DEFAULT, ARMS_DOWN_LIMIT_DEFAULT,
	ARMS_ROLL_LIMIT_DEFAULT };

static int solve(float yaw, float pitch, float roll, float blend, const float shift[3], int left_on_grip,
	struct arms_report *report)
{
	struct arms_input input;
	int i;

	memcpy(pose, base, sizeof(pose));
	memset(&input, 0, sizeof(input));
	input.view_forward[0] = 1.0f;
	input.view_up[2] = 1.0f;
	yaw_pitch_roll_frame(yaw, pitch, roll, input.gun_forward, input.gun_up);
	input.blend = blend;
	if (shift)
		memcpy(input.grip_shift, shift, sizeof(input.grip_shift));
	input.left_grip_valid = 1;
	for (i = 0; i < 3; i++)
		input.left_grip[i] = (&base[L_WRIST].position.x)[i] + (left_on_grip ? 0.0f : (i == 2 ? -0.3f : 0.0f));
	return first_person_arms_solve(pose, NODE_COUNT, nodes, sizeof(nodes[0]), &input, &limits, report);
}

static void check_invariants(const char *label)
{
	int i;

	for (i = 0; i < NODE_COUNT; i++)
		CHECK(orthonormal(&pose[i]), "%s: node %d (%s) is not orthonormal", label, i, nodes[i].name);
	CHECK(same(&pose[ROOT], &base[ROOT]), "%s: the root moved", label);
	CHECK(distance(&pose[R_UPPER], &base[R_UPPER]) < 1e-5f, "%s: the right shoulder moved", label);
	CHECK(distance(&pose[L_UPPER], &base[L_UPPER]) < 1e-5f, "%s: the left shoulder moved", label);
	CHECK(fabsf(distance(&pose[R_UPPER], &pose[R_FOREARM]) - UPPER_LENGTH) < 1e-4f, "%s: the right upper arm's length changed to %.4f", label, distance(&pose[R_UPPER], &pose[R_FOREARM]));
	CHECK(fabsf(distance(&pose[R_FOREARM], &pose[R_WRIST]) - FOREARM_LENGTH) < 1e-4f, "%s: the right forearm's length changed to %.4f", label, distance(&pose[R_FOREARM], &pose[R_WRIST]));
	CHECK(fabsf(distance(&pose[L_UPPER], &pose[L_FOREARM]) - UPPER_LENGTH) < 1e-4f, "%s: the left upper arm's length changed", label);
	CHECK(fabsf(distance(&pose[L_FOREARM], &pose[L_WRIST]) - FOREARM_LENGTH) < 1e-4f, "%s: the left forearm's length changed", label);
	CHECK(relative_change(&base[GUN], &base[R_WRIST], &pose[GUN], &pose[R_WRIST]) < 1e-4f, "%s: the right hand left the grip", label);
	CHECK(relative_change(&base[GUN], &base[MAGAZINE], &pose[GUN], &pose[MAGAZINE]) < 1e-4f, "%s: the magazine left the gun", label);
	CHECK(relative_change(&base[R_WRIST], &base[R_INDEX], &pose[R_WRIST], &pose[R_INDEX]) < 1e-4f, "%s: the right finger left the wrist", label);
	CHECK(relative_change(&base[L_WRIST], &base[L_INDEX], &pose[L_WRIST], &pose[L_INDEX]) < 1e-4f, "%s: the left finger left the wrist", label);
	/* the forearm's axis points at the wrist */
	{
		float bone[3] = { pose[R_WRIST].position.x - pose[R_FOREARM].position.x,
			pose[R_WRIST].position.y - pose[R_FOREARM].position.y, pose[R_WRIST].position.z - pose[R_FOREARM].position.z };

		normalize3(bone);
		CHECK(dot3(bone, &pose[R_FOREARM].forward.i) > 0.999f, "%s: the right forearm's forward is off its bone (%.3f)", label, dot3(bone, &pose[R_FOREARM].forward.i));
	}
}

int main(void)
{
	struct arms_report report;
	float none[3] = { 0.0f, 0.0f, 0.0f };
	int i;

	build_skeleton();
	for (i = 0; i < NODE_COUNT; i++)
		CHECK(orthonormal(&base[i]), "the base node %d is not orthonormal", i);

	/* the gun along the view: nothing moves */
	CHECK(solve(0.0f, 0.0f, 0.0f, 1.0f, none, 1, &report) == 1, "the straight solve was refused");
	check_invariants("straight");
	for (i = 0; i < NODE_COUNT; i++)
		CHECK(distance(&pose[i], &base[i]) < 1e-4f && relative_change(&base[ROOT], &base[i], &pose[ROOT], &pose[i]) < 1e-3f,
			"straight: node %d (%s) moved", i, nodes[i].name);

	/* a turn within the limits: the gun points along it */
	{
		float forward[3], up[3];

		CHECK(solve(30.0f, -15.0f, 10.0f, 1.0f, none, 1, &report) == 1, "the turned solve was refused");
		check_invariants("turned");
		yaw_pitch_roll_frame(30.0f, -15.0f, 10.0f, forward, up);
		CHECK(dot3(forward, report.gun_forward) > 0.9999f, "turned: the gun's forward is off (%.4f)", dot3(forward, report.gun_forward));
		CHECK(dot3(up, report.gun_up) > 0.999f, "turned: the gun's up is off (%.4f)", dot3(up, report.gun_up));
		CHECK(distance(&pose[GUN], &base[GUN]) < 1e-5f, "turned: the gun's position moved %.4f", distance(&pose[GUN], &base[GUN]));
		CHECK(report.right_pulled == 0 && report.left_pulled == 0, "turned: an arm was pulled (%d %d)", report.right_pulled, report.left_pulled);
		CHECK(fabsf(report.left_share - 1.0f) < 1e-6f, "turned: the left hand's share is %.2f", report.left_share);
		CHECK(relative_change(&base[GUN], &base[L_WRIST], &pose[GUN], &pose[L_WRIST]) < 1e-4f, "turned: the left hand left the gun");
		CHECK(fabsf(report.limited_turn_degrees - 30.0f) < 0.01f, "turned: the limited turn is %.2f", report.limited_turn_degrees);
	}

	/* past the turn limit: held at the eased limit */
	{
		CHECK(solve(80.0f, 0.0f, 0.0f, 1.0f, none, 1, &report) == 1, "the far turn was refused");
		check_invariants("far turn");
		CHECK(fabsf(report.turn_degrees - 80.0f) < 0.01f, "far turn: measured %.2f", report.turn_degrees);
		CHECK(report.limited_turn_degrees > ARMS_TURN_LIMIT_DEFAULT * ARMS_LIMIT_KNEE && report.limited_turn_degrees <= ARMS_TURN_LIMIT_DEFAULT,
			"far turn: the limited turn is %.2f", report.limited_turn_degrees);
		CHECK(fabsf(ARMS_DEGREES(atan2f(report.gun_forward[1], report.gun_forward[0])) - report.limited_turn_degrees) < 0.05f,
			"far turn: the gun's yaw %.2f is not the limited turn %.2f", ARMS_DEGREES(atan2f(report.gun_forward[1], report.gun_forward[0])), report.limited_turn_degrees);
		CHECK(solve(-80.0f, 0.0f, 0.0f, 1.0f, none, 1, &report) == 1, "the far left turn was refused");
		CHECK(report.limited_turn_degrees < -ARMS_TURN_LIMIT_DEFAULT * ARMS_LIMIT_KNEE && report.limited_turn_degrees >= -ARMS_TURN_LIMIT_DEFAULT,
			"far left turn: the limited turn is %.2f", report.limited_turn_degrees);
	}

	/* past the elevation limits */
	{
		CHECK(solve(0.0f, 75.0f, 0.0f, 1.0f, none, 1, &report) == 1, "the high solve was refused");
		check_invariants("high");
		CHECK(report.limited_elevation_degrees <= ARMS_UP_LIMIT_DEFAULT && report.limited_elevation_degrees > ARMS_UP_LIMIT_DEFAULT * ARMS_LIMIT_KNEE,
			"high: the limited elevation is %.2f", report.limited_elevation_degrees);
		CHECK(solve(0.0f, -85.0f, 0.0f, 1.0f, none, 1, &report) == 1, "the low solve was refused");
		check_invariants("low");
		CHECK(report.limited_elevation_degrees >= -ARMS_DOWN_LIMIT_DEFAULT && report.limited_elevation_degrees < -ARMS_DOWN_LIMIT_DEFAULT * ARMS_LIMIT_KNEE,
			"low: the limited elevation is %.2f", report.limited_elevation_degrees);
	}

	/* past the roll limit, and the forearm's share of the twist */
	{
		float wrist_before[12], wrist_after[12];

		/* Isolate twist sharing from a support-hand reach correction, which
		also changes the forearm's axis. Coupled contact is checked below. */
		CHECK(solve(0.0f, 0.0f, 120.0f, 1.0f, none, 0, &report) == 1, "the rolled solve was refused");
		check_invariants("rolled");
		CHECK(fabsf(report.roll_degrees - 120.0f) < 0.05f, "rolled: measured %.2f", report.roll_degrees);
		CHECK(report.limited_roll_degrees <= ARMS_ROLL_LIMIT_DEFAULT && report.limited_roll_degrees > ARMS_ROLL_LIMIT_DEFAULT * ARMS_LIMIT_KNEE,
			"rolled: the limited roll is %.2f", report.limited_roll_degrees);
		CHECK(fabsf(report.limited_turn_degrees) < 0.05f && fabsf(report.limited_elevation_degrees) < 0.05f,
			"rolled: the roll leaked into the turn (%.2f) or elevation (%.2f)", report.limited_turn_degrees, report.limited_elevation_degrees);
		/* the wrist's twist relative to the forearm fell by the forearm's share */
		relative(&base[R_FOREARM], &base[R_WRIST], wrist_before);
		relative(&pose[R_FOREARM], &pose[R_WRIST], wrist_after);
		{
			/* the wrist's up in the forearm's frame: its angle about the forearm's forward */
			float before = atan2f(wrist_before[3 + 6 + 1], wrist_before[3 + 6 + 2]);
			float after = atan2f(wrist_after[3 + 6 + 1], wrist_after[3 + 6 + 2]);
			float twist = ARMS_DEGREES(after - before);

			while (twist > 180.0f)
				twist -= 360.0f;
			while (twist < -180.0f)
				twist += 360.0f;
			CHECK(fabsf(fabsf(twist) - report.limited_roll_degrees * (1.0f - ARMS_TWIST_SHARE)) < 1.5f,
				"rolled: the wrist's twist against the forearm is %.2f, not %.2f", twist, report.limited_roll_degrees * (1.0f - ARMS_TWIST_SHARE));
		}
	}

	/* a hand behind the head: the limit on the side it left by */
	{
		CHECK(solve(150.0f, 0.0f, 0.0f, 1.0f, none, 1, &report) == 1, "the behind solve was refused");
		check_invariants("behind");
		CHECK(fabsf(report.limited_turn_degrees - ARMS_TURN_LIMIT_DEFAULT) < 0.5f, "behind: the limited turn is %.2f", report.limited_turn_degrees);
		CHECK(solve(-150.0f, 0.0f, 0.0f, 1.0f, none, 1, &report) == 1, "the behind-left solve was refused");
		CHECK(fabsf(report.limited_turn_degrees + ARMS_TURN_LIMIT_DEFAULT) < 0.5f, "behind left: the limited turn is %.2f", report.limited_turn_degrees);
	}

	/* blend 0: the pose stands; blend 0.5: half the turn */
	{
		CHECK(solve(40.0f, 0.0f, 0.0f, 0.0f, none, 1, &report) == 1, "the blend-0 solve was refused");
		for (i = 0; i < NODE_COUNT; i++)
			CHECK(distance(&pose[i], &base[i]) < 1e-4f, "blend 0: node %d (%s) moved", i, nodes[i].name);
		CHECK(solve(40.0f, 0.0f, 0.0f, 0.5f, none, 1, &report) == 1, "the blend-0.5 solve was refused");
		check_invariants("half blend");
		CHECK(fabsf(ARMS_DEGREES(atan2f(report.gun_forward[1], report.gun_forward[0])) - report.limited_turn_degrees * 0.5f) < 0.1f,
			"half blend: the gun's yaw is %.2f, not half of %.2f", ARMS_DEGREES(atan2f(report.gun_forward[1], report.gun_forward[0])), report.limited_turn_degrees);
	}

	/* the left hand away from the gun: it keeps the animation's pose */
	{
		CHECK(solve(30.0f, 0.0f, 0.0f, 1.0f, none, 0, &report) == 1, "the left-off solve was refused");
		check_invariants("left off");
		CHECK(report.left_share == 0.0f, "left off: the share is %.2f", report.left_share);
		CHECK(distance(&pose[L_WRIST], &base[L_WRIST]) < 1e-5f, "left off: the left wrist moved");
		CHECK(distance(&pose[L_FOREARM], &base[L_FOREARM]) < 1e-4f, "left off: the left elbow moved");
	}

	/* the grip shifted past the arm's reach: the wrist and the gun pulled back onto it */
	{
		float shift[3] = { 1.0f, 0.0f, 0.0f };
		float reach;

		CHECK(solve(0.0f, 0.0f, 0.0f, 1.0f, shift, 1, &report) == 1, "the far shift was refused");
		check_invariants("far shift");
		CHECK(report.right_pulled == 1, "far shift: the right arm was not pulled (%d)", report.right_pulled);
		reach = distance(&pose[R_UPPER], &pose[R_WRIST]);
		CHECK(reach <= (UPPER_LENGTH + FOREARM_LENGTH) * ARMS_REACH_SHARE + 1e-4f,
			"far shift: the wrist is out of reach (%.4f from the shoulder)", reach);
		CHECK(distance(&pose[GUN], &base[GUN]) < 0.3f, "far shift: the gun went %.3f", distance(&pose[GUN], &base[GUN]));
		CHECK(relative_change(&base[GUN], &base[L_WRIST], &pose[GUN], &pose[L_WRIST]) < 1e-4f,
			"far shift: reach correction separated the support hand from the gun (%.6f)",
			relative_change(&base[GUN], &base[L_WRIST], &pose[GUN], &pose[L_WRIST]));
		/* a small shift is taken whole */
		shift[0] = 0.02f;
		CHECK(solve(0.0f, 0.0f, 0.0f, 1.0f, shift, 1, &report) == 1, "the small shift was refused");
		check_invariants("small shift");
		CHECK(report.right_pulled == 0, "small shift: pulled");
		CHECK(fabsf(distance(&pose[GUN], &base[GUN]) - 0.02f) < 1e-4f, "small shift: the gun moved %.4f", distance(&pose[GUN], &base[GUN]));
		/* A released hand does not inherit the gun's reach correction. */
		shift[0] = 1.0f;
		CHECK(solve(0.0f, 0.0f, 0.0f, 1.0f, shift, 0, &report) == 1,
			"the released far-shift solve was refused");
		check_invariants("released far shift");
		CHECK(distance(&pose[L_WRIST], &base[L_WRIST]) < 1e-5f,
			"released far shift: the released wrist followed the gun");
		CHECK(solve(0.0f, 0.0f, 120.0f, 1.0f, none, 1, &report) == 1,
			"the supported roll solve was refused");
		check_invariants("supported roll");
		CHECK(report.left_pulled == 1, "supported roll: did not exercise support-arm reach");
		CHECK(relative_change(&base[GUN], &base[L_WRIST], &pose[GUN], &pose[L_WRIST]) < 1e-4f,
			"supported roll: reach correction separated the support hand from the gun");
	}

	/* a graph without the gun: unchanged */
	{
		char kept[32];

		memcpy(kept, nodes[GUN].name, sizeof(kept));
		strcpy(nodes[GUN].name, "frame body");
		CHECK(solve(30.0f, 0.0f, 0.0f, 1.0f, none, 1, &report) == 0, "a graph without the gun was solved");
		for (i = 0; i < NODE_COUNT; i++)
			CHECK(same(&pose[i], &base[i]), "no gun: node %d changed", i);
		memcpy(nodes[GUN].name, kept, sizeof(kept));
	}

	/* a sweep through and past every limit: never refused, never a NaN */
	{
		float yaw, pitch, roll;

		for (yaw = -180.0f; yaw <= 180.0f; yaw += 15.0f)
			for (pitch = -89.0f; pitch <= 89.0f; pitch += 22.25f)
				for (roll = -180.0f; roll <= 180.0f; roll += 30.0f)
				{
					char label[64];

					if (solve(yaw, pitch, roll, 1.0f, none, 1, &report) != 1)
					{
						CHECK(0, "sweep %.0f %.0f %.0f refused", yaw, pitch, roll);
						continue;
					}
					snprintf(label, sizeof(label), "sweep %.0f %.0f %.0f", yaw, pitch, roll);
					check_invariants(label);
					for (i = 0; i < NODE_COUNT; i++)
						CHECK(pose[i].position.x == pose[i].position.x && pose[i].forward.i == pose[i].forward.i,
							"%s: node %d is NaN", label, i);
					if (failures > 20)
					{
						printf("stopping after %d failures\n", failures);
						return 1;
					}
				}
	}

	if (failures)
	{
		printf("%d failures\n", failures);
		return 1;
	}
	printf("first-person arms probe: all checks pass\n");
	return 0;
}
