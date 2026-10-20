/* Tracked-controller poses (port/ios/host/host_spatial_pose.c, included):
ARKit's room in the game's axes, the head's yaw and whether it's level, a
controller location in the room's level frame, and the synthetic controller
debug.test_controller makes, and debug.controller_aim_axes's axes. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../host/host_spatial_pose.c"

static int failures;

#define DEGREES(radians) ((radians) * 180.0 / M_PI)
#define TEST_RADIANS(degrees) ((degrees) * M_PI / 180.0)

static void check(int condition, const char *what)
{
	if (!condition)
	{
		printf("FAILED: %s\n", what);
		failures++;
	}
}

static int near(double a, double b, double tolerance)
{
	return fabs(a - b) <= tolerance;
}

static int near3(const float a[3], double x, double y, double z)
{
	return near(a[0], x, 1e-5) && near(a[1], y, 1e-5) && near(a[2], z, 1e-5);
}

static void check3(const float a[3], double x, double y, double z, const char *what)
{
	if (!near3(a, x, y, z))
	{
		printf("FAILED: %s: (%.6f, %.6f, %.6f), expected (%.6f, %.6f, %.6f)\n", what, a[0], a[1], a[2], x, y, z);
		failures++;
	}
}

/* the device's back axis for a head yawed (left positive), pitched (up
positive) and rolled, as ARKit gives its transform: yaw about the room's up,
then pitch about the head's right, then roll about its back */
static void head_back(double yaw, double pitch, double roll, float back[3])
{
	/* the roll turns the back axis about itself: it stays (0, 0, 1) */
	double local[3] = { 0.0, 0.0, 1.0 };
	double pitched[3], rolled[3];

	rolled[0] = local[0] * cos(roll) - local[1] * sin(roll);
	rolled[1] = local[0] * sin(roll) + local[1] * cos(roll);
	rolled[2] = local[2];
	/* pitch up about x: forward (-z) tips toward +y, so back tips toward -y */
	pitched[0] = rolled[0];
	pitched[1] = rolled[1] * cos(pitch) - rolled[2] * sin(pitch);
	pitched[2] = rolled[1] * sin(pitch) + rolled[2] * cos(pitch);
	/* yaw left about y */
	back[0] = (float)(pitched[0] * cos(yaw) + pitched[2] * sin(yaw));
	back[1] = (float)pitched[1];
	back[2] = (float)(-pitched[0] * sin(yaw) + pitched[2] * cos(yaw));
}

/* a forward axis's yaw (left positive) and pitch (up positive), degrees, in the game's axes */
static double yaw_of(const float forward[3])
{
	return DEGREES(atan2(forward[1], forward[0]));
}

static double pitch_of(const float forward[3])
{
	return DEGREES(asin(forward[2]));
}

static void axis_change(void)
{
	const float room_forward[3] = { 0.0f, 0.0f, -1.0f }, room_right[3] = { 1.0f, 0.0f, 0.0f },
		room_up[3] = { 0.0f, 1.0f, 0.0f };
	float game[3];

	host_spatial_room_to_game(room_forward, game);
	check3(game, 1.0, 0.0, 0.0, "the room's forward is the game's +x");
	host_spatial_room_to_game(room_right, game);
	check3(game, 0.0, -1.0, 0.0, "the room's right is the game's -y");
	host_spatial_room_to_game(room_up, game);
	check3(game, 0.0, 0.0, 1.0, "the room's up is the game's +z");
}

static void head_yaw(void)
{
	float back[3];
	int level = -1;
	float yaw;

	back[0] = (float)sin(TEST_RADIANS(30.0));
	back[1] = 0.0f;
	back[2] = (float)cos(TEST_RADIANS(30.0));
	yaw = host_spatial_head_yaw(back, &level);
	check(near(DEGREES(yaw), 30.0, 0.01), "a head turned 30 degrees left has yaw +30");
	check(level == 1, "a level head is level");

	head_back(TEST_RADIANS(10.0), TEST_RADIANS(84.0), TEST_RADIANS(20.0), back);
	level = -1;
	host_spatial_head_yaw(back, &level);
	check(level == 1, "a head pitched 84 degrees with a 20 degree roll is level");
	head_back(TEST_RADIANS(10.0), TEST_RADIANS(86.0), TEST_RADIANS(20.0), back);
	level = -1;
	host_spatial_head_yaw(back, &level);
	check(level == 0, "a head pitched 86 degrees with a 20 degree roll isn't level");
	head_back(TEST_RADIANS(10.0), TEST_RADIANS(-89.0), TEST_RADIANS(20.0), back);
	level = -1;
	host_spatial_head_yaw(back, &level);
	check(level == 0, "a head pitched 89 degrees down with a 20 degree roll isn't level");
	head_back(TEST_RADIANS(10.0), TEST_RADIANS(89.0), TEST_RADIANS(20.0), back);
	level = -1;
	host_spatial_head_yaw(back, &level);
	check(level == 0, "a head pitched 89 degrees up with a 20 degree roll isn't level");
}

static void location(void)
{
	/* identity rotation, 0.3048 m to the room's right of the head */
	float transform[16] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.3048f, 0.0f, 0.0f, 1.0f,
	};
	const float head[3] = { 0.0f, 0.0f, 0.0f };
	const float head_moved[3] = { 0.1f, 1.5f, -0.2f };
	struct halo_spatial_location result;

	host_spatial_location(transform, head, HOST_SPATIAL_AXIS_MINUS_Z, HOST_SPATIAL_AXIS_PLUS_Y, &result);
	check3(result.forward, 1.0, 0.0, 0.0, "identity: forward");
	check3(result.left, 0.0, 1.0, 0.0, "identity: left");
	check3(result.up, 0.0, 0.0, 1.0, "identity: up");
	check3(result.position, 0.0, -0.3048, 0.0, "identity: position");

	/* the position is from the head */
	transform[12] = 0.1f + 0.3048f;
	transform[13] = 1.5f;
	transform[14] = -0.2f;
	host_spatial_location(transform, head_moved, HOST_SPATIAL_AXIS_MINUS_Z, HOST_SPATIAL_AXIS_PLUS_Y, &result);
	check3(result.position, 0.0, -0.3048, 0.0, "the position is from the head");

	/* turned 90 degrees about the room's +y (left): x goes to -z, z to +x */
	transform[0] = 0.0f; transform[1] = 0.0f; transform[2] = -1.0f;
	transform[8] = 1.0f; transform[9] = 0.0f; transform[10] = 0.0f;
	host_spatial_location(transform, head_moved, HOST_SPATIAL_AXIS_MINUS_Z, HOST_SPATIAL_AXIS_PLUS_Y, &result);
	check3(result.forward, 0.0, 1.0, 0.0, "turned 90 degrees left: forward is the game's left");
	check3(result.left, -1.0, 0.0, 0.0, "turned 90 degrees left: left is the game's back");
	check3(result.up, 0.0, 0.0, 1.0, "turned 90 degrees left: up");

	/* other axes name the barrel: +x forward, +z up (the identity's x is the room's right) */
	transform[0] = 1.0f; transform[2] = 0.0f;
	transform[8] = 0.0f; transform[10] = 1.0f;
	host_spatial_location(transform, head, HOST_SPATIAL_AXIS_PLUS_X, HOST_SPATIAL_AXIS_PLUS_Z, &result);
	check3(result.forward, 0.0, -1.0, 0.0, "+x as the barrel: the room's right");
	check3(result.up, -1.0, 0.0, 0.0, "+z as its up: the room's back");
	check3(result.left, 0.0, 0.0, 1.0, "and its left is up");
}

static void synthetic(void)
{
	struct host_spatial_synthetic parsed;
	struct halo_spatial_hand hand;
	double seconds, most_left = -1000.0, most_right = 1000.0;

	check(host_spatial_synthetic_parse("fixed:30,0", &parsed) == 1, "fixed:30,0 parses");
	check(parsed.kind == HOST_SPATIAL_SYNTHETIC_FIXED, "fixed:30,0 is fixed");
	host_spatial_synthetic_hand(&parsed, 3.0, &hand);
	check(near(yaw_of(hand.aim.forward), -30.0, 0.01), "fixed:30,0 aims 30 degrees right of the room's forward");
	check(near(pitch_of(hand.aim.forward), 0.0, 0.01), "fixed:30,0 aims level");
	check(hand.held == 1, "the synthetic hand is held");
	check(hand.state == HALO_SPATIAL_TRACKED, "the synthetic hand is tracked");
	check(hand.chirality == 2, "the synthetic hand is the right");
	check(near(hand.aim.left[2], 0.0, 1e-5), "the aim is level-rolled");
	check(hand.aim.up[2] > 0.0f, "the aim's up is up");
	check3(hand.aim.position, 0.35, -0.25, -0.30, "the aim at 0.25 m right, 0.30 m down, 0.35 m ahead");
	check3(hand.grip.position, 0.35 - 0.05 * hand.aim.forward[0], -0.25 - 0.05 * hand.aim.forward[1],
		-0.30 - 0.05 * hand.aim.forward[2], "the grip 0.05 m behind the aim along the barrel");
	check3(hand.grip.forward, hand.aim.forward[0], hand.aim.forward[1], hand.aim.forward[2],
		"the grip's barrel is the aim's");

	check(host_spatial_synthetic_parse("fixed:0,20", &parsed) == 1, "fixed:0,20 parses");
	host_spatial_synthetic_hand(&parsed, 0.0, &hand);
	check(near(pitch_of(hand.aim.forward), 20.0, 0.01), "fixed:0,20 pitches up 20 degrees");
	check(near(yaw_of(hand.aim.forward), 0.0, 0.01), "fixed:0,20 aims at the room's forward");
	check(near(hand.aim.left[2], 0.0, 1e-5), "pitched, the aim is still level-rolled");
	check(hand.aim.up[2] > 0.9f, "pitched, the aim's up is still up");

	check(host_spatial_synthetic_parse("sweep", &parsed) == 1, "sweep parses");
	for (seconds = 0.0; seconds < 4.0; seconds += 0.01)
	{
		double yaw;

		host_spatial_synthetic_hand(&parsed, seconds, &hand);
		yaw = yaw_of(hand.aim.forward);
		most_left = fmax(most_left, yaw);
		most_right = fmin(most_right, yaw);
		check(fabs(yaw) <= 30.0 + 1e-3, "sweep stays inside 30 degrees");
	}
	check(near(most_left, 30.0, 0.05) && near(most_right, -30.0, 0.05), "sweep reaches +-30 degrees in 4 s");

	check(host_spatial_synthetic_parse("loss:1", &parsed) == 1, "loss:1 parses");
	{
		static const struct { double seconds; int state; } schedule[] = {
			{ 0.0, HALO_SPATIAL_UNTRACKED }, { 0.5, HALO_SPATIAL_UNTRACKED }, { 0.99, HALO_SPATIAL_UNTRACKED },
			{ 1.01, HALO_SPATIAL_ORIENTATION }, { 1.5, HALO_SPATIAL_ORIENTATION }, { 1.99, HALO_SPATIAL_ORIENTATION },
			{ 2.01, HALO_SPATIAL_TRACKED }, { 9.99, HALO_SPATIAL_TRACKED }, { 10.5, HALO_SPATIAL_UNTRACKED },
			{ 11.5, HALO_SPATIAL_ORIENTATION }, { 12.5, HALO_SPATIAL_TRACKED },
		};
		size_t index;

		for (index = 0; index < sizeof(schedule) / sizeof(schedule[0]); index++)
		{
			char what[96];

			host_spatial_synthetic_hand(&parsed, schedule[index].seconds, &hand);
			snprintf(what, sizeof(what), "loss:1 at %.2f s is state %d", schedule[index].seconds,
				schedule[index].state);
			check(hand.state == schedule[index].state, what);
			check(hand.held == 1, "loss: the hand stays held");
		}
		check(near(yaw_of(hand.aim.forward), 0.0, 0.01) && near(pitch_of(hand.aim.forward), 0.0, 0.01),
			"loss aims at fixed:0,0");
	}

	memset(&parsed, 0x55, sizeof(parsed));
	check(host_spatial_synthetic_parse("fixed:abc", &parsed) == 0, "fixed:abc doesn't parse");
	check(host_spatial_synthetic_parse("fixed:30", &parsed) == 0, "fixed:30 doesn't parse");
	check(host_spatial_synthetic_parse("fixed:30,0x", &parsed) == 0, "fixed:30,0x doesn't parse");
	check(host_spatial_synthetic_parse("loss:0", &parsed) == 0, "loss:0 doesn't parse");
	check(host_spatial_synthetic_parse("loss:6", &parsed) == 0, "loss:6 doesn't parse (two spells fill 10 s)");
	check(host_spatial_synthetic_parse("sweeping", &parsed) == 0, "sweeping doesn't parse");
	check(host_spatial_synthetic_parse("", &parsed) == 0, "the empty string doesn't parse");
}

/* debug.controller_aim_axes: "<forward>,<up>", each one of +x -x +y -y +z -z,
on different lines */
static void aim_axes(void)
{
	static const struct { const char *value; int forward, up; } good[] = {
		{ "-z,+y", HOST_SPATIAL_AXIS_MINUS_Z, HOST_SPATIAL_AXIS_PLUS_Y },
		{ "+x,-z", HOST_SPATIAL_AXIS_PLUS_X, HOST_SPATIAL_AXIS_MINUS_Z },
		{ "-y,+x", HOST_SPATIAL_AXIS_MINUS_Y, HOST_SPATIAL_AXIS_PLUS_X },
		{ "+Z,-Y", HOST_SPATIAL_AXIS_PLUS_Z, HOST_SPATIAL_AXIS_MINUS_Y },
	};
	static const char *const bad[] = { "", "-z", "-z,", "-z,+z", "-z,-z", "+y,-y", "z,+y", "-z,+y,", "-z, +y",
		" -z,+y", "-w,+y", "-z;+y", "--z,+y" };
	size_t index;
	int forward, up;

	for (index = 0; index < sizeof(good) / sizeof(good[0]); index++)
	{
		char what[96];

		forward = up = -1;
		snprintf(what, sizeof(what), "\"%s\" parses", good[index].value);
		check(host_spatial_axes_parse(good[index].value, &forward, &up) == 1, what);
		snprintf(what, sizeof(what), "\"%s\" names its forward and up", good[index].value);
		check(forward == good[index].forward && up == good[index].up, what);
	}
	for (index = 0; index < sizeof(bad) / sizeof(bad[0]); index++)
	{
		char what[96];

		forward = up = 77;
		snprintf(what, sizeof(what), "\"%s\" doesn't parse", bad[index]);
		check(host_spatial_axes_parse(bad[index], &forward, &up) == 0, what);
		snprintf(what, sizeof(what), "\"%s\" leaves the axes alone", bad[index]);
		check(forward == 77 && up == 77, what);
	}
	check(host_spatial_axes_parse(NULL, &forward, &up) == 0, "no value doesn't parse");
	check(!strcmp(host_spatial_axis_name(HOST_SPATIAL_AXIS_MINUS_Z), "-z") &&
		!strcmp(host_spatial_axis_name(HOST_SPATIAL_AXIS_PLUS_X), "+x") &&
		!strcmp(host_spatial_axis_name(HOST_SPATIAL_AXIS_PLUS_Y), "+y"), "the axes' names");
}

/* Between Crown recenter callbacks and the next head latch, the game
must use a pose in the latched head's origin, with current buttons/loss.
The drawing sample needs the same hold when its device anchor is missing. */
static void recenter_sample(void)
{
	struct halo_spatial_sample reference = { 0 }, sample = { 0 };
	reference.head_yaw = 0.4f;
	reference.head_level = 1;
	reference.hands[1].state = HALO_SPATIAL_TRACKED;
	reference.hands[1].aim.forward[0] = 1;
	reference.hands[1].aim.position[1] = -0.25f;
	sample.hands[1].state = HALO_SPATIAL_TRACKED;
	sample.hands[1].aim.forward[1] = 1;
	sample.gamepad_valid = 1;
	sample.gamepad.buttons = 0x1234;
	host_spatial_hold_poses(&sample, &reference);
	check3(sample.hands[1].aim.forward, 1, 0, 0, "recenter sample: pose uses latched origin");
	check3(sample.hands[1].aim.position, 0, -0.25, 0, "recenter sample: position uses latched origin");
	check(sample.head_yaw == 0.4f && sample.head_level, "recenter sample: head shares pose origin");
	check(sample.gamepad_valid && sample.gamepad.buttons == 0x1234, "recenter sample: live buttons survive");
	sample.hands[1].state = HALO_SPATIAL_UNTRACKED;
	host_spatial_hold_poses(&sample, &reference);
	check(sample.hands[1].state == HALO_SPATIAL_UNTRACKED, "recenter sample: actual pose loss survives");
	reference.hands[1].state = HALO_SPATIAL_NONE;
	sample.hands[1].state = HALO_SPATIAL_TRACKED;
	host_spatial_hold_poses(&sample, &reference);
	check(sample.hands[1].state == HALO_SPATIAL_UNTRACKED, "recenter sample: no reference cannot invent a pose");
}

/* a controller reading above the head (host_spatial_misplaced_update) */
static void misplaced(void)
{
	struct host_spatial_misplaced state;
	struct halo_spatial_hand hand;
	double now = 10.0;
	int frame, result = 0;

	memset(&state, 0, sizeof(state));
	memset(&hand, 0, sizeof(hand));
	hand.state = HALO_SPATIAL_TRACKED;
	hand.held = 1;
	/* in the lap, then overhead for a second: never flagged */
	hand.aim.position[2] = -0.5f;
	for (frame = 0; frame < 90; frame++, now += 1.0 / 90.0)
		result |= host_spatial_misplaced_update(&state, &hand, 1, now);
	hand.aim.position[2] = 0.6f;
	for (frame = 0; frame < 90; frame++, now += 1.0 / 90.0)
		result |= host_spatial_misplaced_update(&state, &hand, 1, now);
	hand.aim.position[2] = -0.5f;
	result |= host_spatial_misplaced_update(&state, &hand, 1, now);
	check(result == 0 && !state.flagged, "misplaced: a second overhead is a raised arm");
	/* 0.63 m above the head, as the headset session had it: flagged after 1.5 s, once */
	hand.aim.position[2] = 0.63f;
	for (frame = 0, result = 0; frame < 134; frame++, now += 1.0 / 90.0)
		result += host_spatial_misplaced_update(&state, &hand, 1, now);
	check(result == 0 && !state.flagged, "misplaced: not before 1.5 s");
	for (frame = 0; frame < 90; frame++, now += 1.0 / 90.0)
		result += host_spatial_misplaced_update(&state, &hand, 1, now);
	check(result == 1 && state.flagged, "misplaced: flagged once after 1.5 s above the head");
	/* no head, not held, or untracked: no judgment, and the flag stands */
	result = host_spatial_misplaced_update(&state, &hand, 0, now + 5.0);
	hand.state = HALO_SPATIAL_UNTRACKED;
	result |= host_spatial_misplaced_update(&state, &hand, 1, now + 10.0);
	hand.state = HALO_SPATIAL_TRACKED;
	check(result == 0 && state.flagged, "misplaced: a lost hand or a missing head doesn't clear it");
	/* under the height for half a second: cleared once */
	hand.aim.position[2] = -0.5f;
	now += 20.0;
	for (frame = 0, result = 0; frame < 60; frame++, now += 1.0 / 90.0)
		result += host_spatial_misplaced_update(&state, &hand, 1, now);
	check(result == -1 && !state.flagged, "misplaced: cleared after half a second back under the head");
	/* without a head (a load: the position is from the room's origin) nothing flags */
	hand.aim.position[2] = 0.9f;
	for (frame = 0, result = 0; frame < 900; frame++, now += 1.0 / 90.0)
		result |= host_spatial_misplaced_update(&state, &hand, 0, now);
	check(result == 0 && !state.flagged, "misplaced: no head, no judgment");
}

int main(void)
{
	misplaced();
	recenter_sample();
	axis_change();
	head_yaw();
	location();
	synthetic();
	aim_axes();
	if (failures)
	{
		printf("spatial pose probe: %d FAILED\n", failures);
		return 1;
	}
	printf("spatial pose probe: OK\n");
	return 0;
}
