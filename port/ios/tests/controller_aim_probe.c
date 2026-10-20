/* Controller aim's math (port/linux/game/controller_aim.c, included): the
body's yaw from a level head and held past 85 degrees of pitch, end to end
with host_stereo_head.c's look and host_spatial_pose.c's head yaw (both
compiled beside this file); a location into the world; the sightline from
the eye with its near low-pass; the angles' pitch clamp; the
facing offset packed into player_action.pad; the yaw blend the short way;
tracking loss and recovery with the growing dwell; the gate's ease; the
pending flags and the press latch; the resets; the first-person
gun's frame; and where a direction shows in the zoomed picture. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "halo_stereo.h"
#include "host_stereo_head.h"
#include "host_spatial_pose.h"

#include "../../linux/game/controller_aim.c"

static int failures;

#define DEGREES(radians) ((radians) * 180.0 / M_PI)
#define RADIANS(degrees) ((degrees) * M_PI / 180.0)

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

/* v made square to unit (both unit length going in), in place */
static void squared(float v[3], const float unit[3])
{
	float along = v[0] * unit[0] + v[1] * unit[1] + v[2] * unit[2], length;
	int axis;

	for (axis = 0; axis < 3; axis++)
		v[axis] -= along * unit[axis];
	length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	for (axis = 0; axis < 3; axis++)
		v[axis] /= length;
}

static void check3(const float a[3], double x, double y, double z, double tolerance, const char *what)
{
	if (!near(a[0], x, tolerance) || !near(a[1], y, tolerance) || !near(a[2], z, tolerance))
	{
		printf("FAILED: %s: (%.6f, %.6f, %.6f), expected (%.6f, %.6f, %.6f)\n", what, a[0], a[1], a[2], x, y, z);
		failures++;
	}
}

/* an angle in degrees wrapped into -180..180 */
static double wrapped_degrees(double degrees)
{
	return remainder(degrees, 360.0);
}

/* the angle between two directions, degrees */
static double angle_between(const float a[3], const float b[3])
{
	double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	double la = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
	double lb = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
	double cosine = dot / (la * lb);

	return DEGREES(acos(cosine > 1.0 ? 1.0 : cosine < -1.0 ? -1.0 : cosine));
}

static void body_yaw(void)
{
	struct controller_body body = { 0 };
	float yaw;

	yaw = controller_body_yaw(&body, (float)RADIANS(40.0), (float)RADIANS(10.0), 1);
	check(near(DEGREES(yaw), 30.0, 1e-4) && body.known, "body: look 40, head 10, level: 30");
	/* the head turned 30 degrees past the pole: the body holds */
	yaw = controller_body_yaw(&body, (float)RADIANS(70.0), (float)RADIANS(86.0), 0);
	check(near(DEGREES(yaw), 30.0, 1e-4), "body: past the pole, held at 30");
	yaw = controller_body_yaw(&body, (float)RADIANS(75.0), (float)RADIANS(50.0), 1);
	check(near(DEGREES(yaw), 25.0, 1e-4), "body: level again, look 75, head 50: 25");
	/* across +-pi it wraps */
	yaw = controller_body_yaw(&body, (float)RADIANS(170.0), (float)RADIANS(-170.0), 1);
	check(near(fabs(wrapped_degrees(DEGREES(yaw) + 20.0)), 0.0, 1e-3), "body: look 170, head -170: -20");
	memset(&body, 0, sizeof(body));
	controller_body_yaw(&body, (float)RADIANS(40.0), (float)RADIANS(10.0), 0);
	check(!body.known, "body: a fresh state with no level head isn't known");
}

/* the device's axes in the room (ARKit's: x right, y up, z back) for a head
yawed (left positive, about the room's up), then pitched (up positive, about
its right), then rolled (left ear down positive, about its back) */
static void head_axes(double yaw, double pitch, double roll, float right[3], float up[3], float back[3])
{
	const double local[3][3] = { { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 } };
	float *out[3] = { right, up, back };
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		const double *v = local[axis];
		double rolled[3], pitched[3];

		rolled[0] = v[0] * cos(roll) - v[1] * sin(roll);
		rolled[1] = v[0] * sin(roll) + v[1] * cos(roll);
		rolled[2] = v[2];
		pitched[0] = rolled[0];
		pitched[1] = rolled[1] * cos(pitch) - rolled[2] * sin(pitch);
		pitched[2] = rolled[1] * sin(pitch) + rolled[2] * cos(pitch);
		out[axis][0] = (float)(pitched[0] * cos(yaw) + pitched[2] * sin(yaw));
		out[axis][1] = (float)pitched[1];
		out[axis][2] = (float)(-pitched[0] * sin(yaw) + pitched[2] * cos(yaw));
	}
}

/* A Crown recenter rotates ARKit's origin 70 degrees without moving the
head or controller. Rebase the head, then check the same world barrel,
including when the body yaw is held near the pole. */
static void recenter(double pitch, int canceled)
{
	struct host_stereo_head head = { 0 };
	struct halo_stereo_frame frame = { 0 };
	struct controller_body body = { 0 };
	struct halo_spatial_location location = { 0 };
	float right[3], up[3], back[3], eye[3] = { 1, 2, 3 };
	float origin[3], forward[3], barrel_up[3];
	float look = RADIANS(40), yaw, sampled_yaw;
	int level;

	head_axes(RADIANS(20), RADIANS(pitch), 0, right, up, back);
	host_stereo_head_turn(&head, right, up, back, &frame);
	/* Establish the body while level before testing a pole recenter. */
	controller_body_yaw(&body, look, RADIANS(20), 1);
	/* Initial world yaw is body 20 + hand 10 = 30 degrees. */
	host_stereo_head_recenter(&head);
	if (canceled)
		head.known = 0;
	/* A missing pose must keep the pending rebase for recovery. */
	host_stereo_head_hold(&head, &frame);
	check(head.recenter, "recenter: pose loss retains pending reference");
	head_axes(RADIANS(-50), RADIANS(pitch), 0, right, up, back);
	frame.head_yaw = 0;
	host_stereo_head_turn(&head, right, up, back, &frame);
	look += frame.head_yaw;
	sampled_yaw = host_spatial_head_yaw(back, &level) + head.room_yaw_offset;
	yaw = controller_body_yaw(&body, look, sampled_yaw, level);
	location.forward[0] = cosf(RADIANS(-60));
	location.forward[1] = sinf(RADIANS(-60));
	location.up[2] = 1;
	/* Translation of the origin cancels before this head-relative position. */
	location.position[0] = 0.3f * cosf(RADIANS(-70));
	location.position[1] = 0.3f * sinf(RADIANS(-70));
	host_stereo_head_room_vector(&head, location.position);
	host_stereo_head_room_vector(&head, location.forward);
	controller_location_to_world(&location, yaw, eye, 0, origin, forward, barrel_up);
	check(near(DEGREES(look), 40, 1e-4), "recenter: view yaw stays put");
	check3(forward, cos(RADIANS(30)), sin(RADIANS(30)), 0, 1e-5, "recenter: world barrel stays put");
	check3(origin, 1 + 0.3 * cos(RADIANS(20)) / CONTROLLER_METERS_PER_UNIT,
		2 + 0.3 * sin(RADIANS(20)) / CONTROLLER_METERS_PER_UNIT, 3, 1e-5,
		"recenter: barrel position stays put");
	/* Back level, turning the head 5 degrees must turn the view but leave
	the held controller alone. A stick turn still rotates its world aim. */
	head_axes(RADIANS(-45), 0, 0, right, up, back);
	memset(&frame, 0, sizeof(frame));
	host_stereo_head_turn(&head, right, up, back, &frame);
	look += frame.head_yaw;
	check(near(DEGREES(look), 45, 1e-4), "recenter: subsequent head turn stays responsive");
	sampled_yaw = host_spatial_head_yaw(back, &level) + head.room_yaw_offset;
	yaw = controller_body_yaw(&body, look, sampled_yaw, level);
	controller_location_to_world(&location, yaw, eye, 0, origin, forward, barrel_up);
	check3(forward, cos(RADIANS(30)), sin(RADIANS(30)), 0, 1e-5, "recenter: head turn leaves barrel alone");
	yaw = controller_body_yaw(&body, look + RADIANS(30), sampled_yaw, level);
	controller_location_to_world(&location, yaw, eye, 0, origin, forward, barrel_up);
	check3(forward, cos(RADIANS(60)), sin(RADIANS(60)), 0, 1e-5, "recenter: intentional stick turn rotates barrel");
}

static double clamped(double value)
{
	return value < 0.0 ? 0.0 : value > 1.0 ? 1.0 : value;
}

/* Review Focus 4, end to end: the head pitches from 0 to 89 degrees and back
over 60 frames (pitch_sign -1: down at the feet), turning 10 degrees level
first and then 30 degrees about the room's up past 85 degrees, rolled
roll_in on the way in and roll_out on the way out. The look sums
host_stereo_head_turn's deltas as stereo.c does; the gun is held level at
room yaw 0, so the aim's world yaw is the body's */
static void pole(double roll_in, double roll_out, double pitch_sign, const char *name)
{
	struct host_stereo_head head = { 0 };
	struct controller_body body = { 0 };
	float look = 1.0f;
	double previous_aim = 0.0, largest_other_step = 0.0, leaving_step = 0.0, drift = 0.0;
	double last_level_offset = 0.0, start_aim = 0.0, end_aim = 0.0;
	int frame, was_level = 1, crossings = 0, identity_ok = 1, steps_ok = 1;
	char what[160];

	for (frame = 0; frame <= 60; frame++)
	{
		double pitch = pitch_sign * 89.0 * (1.0 - fabs(frame - 30.0) / 30.0);
		double yaw = 10.0 * clamped(frame / 20.0) + 30.0 * clamped((frame - 28.0) / 4.0);
		double roll = frame <= 30 ? roll_in : roll_out;
		float right[3], up[3], back[3];
		struct halo_stereo_frame stereo_frame;
		float head_yaw, aim;
		double step, offset;
		int level;

		head_axes(RADIANS(yaw), RADIANS(pitch), RADIANS(roll), right, up, back);
		memset(&stereo_frame, 0, sizeof(stereo_frame));
		host_stereo_head_turn(&head, right, up, back, &stereo_frame);
		look = remainderf(look + stereo_frame.head_yaw, 2.0f * (float)M_PI);
		head_yaw = host_spatial_head_yaw(back, &level);
		aim = controller_body_yaw(&body, look, head_yaw, level) + 0.0f;
		/* the look's drift against the room: its sum less forward's yaw */
		offset = DEGREES(look - head_yaw);
		if (frame == 0)
			start_aim = DEGREES(aim);
		else
		{
			step = wrapped_degrees(DEGREES(aim) - previous_aim);
			if (level && !was_level)
			{
				crossings++;
				leaving_step = step;
				drift = wrapped_degrees(offset - last_level_offset);
			}
			else if (fabs(step) > largest_other_step)
				largest_other_step = fabs(step);
			if (fabs(step) > 0.5)
				steps_ok = 0;
		}
		if (level)
		{
			last_level_offset = offset;
			/* the aim less the look plus the head's yaw: the gun's room yaw */
			if (fabs(wrapped_degrees(DEGREES(aim - look + head_yaw)) - 0.0) > 0.01)
				identity_ok = 0;
		}
		previous_aim = DEGREES(aim);
		end_aim = DEGREES(aim);
		was_level = level;
	}
	snprintf(what, sizeof(what), "pole (%s): the head crossed 85 degrees and came back once", name);
	check(crossings == 1, what);
	snprintf(what, sizeof(what), "pole (%s): once level, the aim less the look plus the head's yaw is the gun's", name);
	check(identity_ok, what);
	if (roll_in == roll_out)
	{
		snprintf(what, sizeof(what), "pole (%s): the aim never steps more than 0.5 degrees", name);
		check(steps_ok, what);
		snprintf(what, sizeof(what), "pole (%s): the aim stays put in the room (%.4f to %.4f)", name, start_aim,
			end_aim);
		check(fabs(wrapped_degrees(end_aim - start_aim)) <= 0.01, what);
	}
	else
	{
		snprintf(what, sizeof(what), "pole (%s): the aim's step on leaving (%.4f) is the look's drift (%.4f)", name,
			leaving_step, drift);
		check(near(leaving_step, drift, 0.01) && fabs(drift) > 1.0, what);
		snprintf(what, sizeof(what), "pole (%s): and nothing more (largest other step %.4f)", name,
			largest_other_step);
		check(largest_other_step <= 0.01, what);
	}
}

static void world(void)
{
	const struct halo_spatial_location location = {
		.forward = { 1.0f, 0.0f, 0.0f }, .left = { 0.0f, 1.0f, 0.0f }, .up = { 0.0f, 0.0f, 1.0f },
		.position = { 0.3048f, 0.0f, 0.0f } };
	struct halo_spatial_location pitched = location;
	const float eye[3] = { 10.0f, 20.0f, 30.0f };
	float origin[3], forward[3], up[3];
	double s = sin(RADIANS(20.0)), c = cos(RADIANS(20.0));

	controller_location_to_world(&location, (float)RADIANS(90.0), eye, 0.0f, origin, forward, up);
	check3(forward, 0.0, 1.0, 0.0, 1e-5, "world: forward turned 90 degrees");
	check3(up, 0.0, 0.0, 1.0, 1e-5, "world: up stays up");
	check3(origin, 10.0, 20.1, 30.0, 1e-5, "world: origin from the eye, scaled to units");
	controller_location_to_world(&location, (float)RADIANS(90.0), eye, (float)RADIANS(20.0), origin, forward, up);
	check3(forward, 0.0, c, -s, 1e-5, "world: pitch offset 20 degrees: forward below level");
	check3(up, 0.0, s, c, 1e-5, "world: pitch offset 20 degrees: up tips forward");
	check3(origin, 10.0, 20.1, 30.0, 1e-5, "world: the pitch offset leaves the origin");
	/* a barrel 30 degrees up, offset 20 degrees down: 10 degrees up, about its own left */
	pitched.forward[0] = (float)cos(RADIANS(30.0));
	pitched.forward[2] = (float)sin(RADIANS(30.0));
	pitched.up[0] = (float)-sin(RADIANS(30.0));
	pitched.up[2] = (float)cos(RADIANS(30.0));
	controller_location_to_world(&pitched, 0.0f, eye, (float)RADIANS(20.0), origin, forward, up);
	check3(forward, cos(RADIANS(10.0)), 0.0, sin(RADIANS(10.0)), 1e-5, "world: 30 up less 20: 10 up");
}

static void sightline(void)
{
	const float eye[3] = { 0.0f, 0.0f, 0.0f };
	const float along_x[3] = { 1.0f, 0.0f, 0.0f }, along_y[3] = { 0.0f, 1.0f, 0.0f };
	float off_30[3] = { (float)cos(RADIANS(30.0)), (float)sin(RADIANS(30.0)), 0.0f };
	struct controller_near near_state;
	float point[3], direction[3];
	int step;
	char what[160];

	memset(&near_state, 0, sizeof(near_state));
	point[0] = 100.0f, point[1] = 0.1f, point[2] = 0.0f;
	controller_sightline(&near_state, eye, point, along_x, 0.001f, direction);
	check(angle_between(direction, point) <= 0.01, "sightline: far, the direction is the eye's to P");
	check(near(sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]),
		1.0, 1e-6), "sightline: unit length");

	memset(&near_state, 0, sizeof(near_state));
	point[0] = 0.1f, point[1] = 0.0f, point[2] = 0.0f;
	controller_sightline(&near_state, eye, point, along_y, 0.001f, direction);
	check3(direction, 1.0, 0.0, 0.0, 1e-6, "sightline: even a close hit converges from the eye");

	memset(&near_state, 0, sizeof(near_state));
	point[0] = 0.325f, point[1] = 0.0f, point[2] = 0.0f;
	controller_sightline(&near_state, eye, point, off_30, 0.001f, direction);
	snprintf(what, sizeof(what), "sightline: at one meter, converges on the hit (%.3f degrees)",
		DEGREES(atan2(direction[1], direction[0])));
	check(near(DEGREES(atan2(direction[1], direction[0])), 0.0, 1e-4), what);

	/* A level barrel held 30 cm below the eye selects the same height on
	a wall as it approaches. Both the game eye and a raised render eye
	must converge on that point, including throughout the old near blend. */
	{
		int sample, frame;
		for (sample = 0; sample < 2; sample++)
		{
			float raised_eye[3] = { 0.0f, 0.0f, sample ? 0.08f : 0.0f };
			memset(&near_state, 0, sizeof(near_state));
			for (frame = 0; frame <= 90; frame++)
			{
				float meters = 2.0f - (float)frame / 60.0f;
				float landing[3], travel;
				int axis;
				point[0] = meters / CONTROLLER_METERS_PER_UNIT;
				point[1] = 0.06f;
				point[2] = -0.3f / CONTROLLER_METERS_PER_UNIT;
				/* Settle the near filter at each position to isolate geometry. */
				for (step = 0; step < 30; step++)
					controller_sightline(&near_state, raised_eye, point, along_x, 1.0f / 90.0f, direction);
				travel = (point[0] - raised_eye[0]) / direction[0];
				for (axis = 0; axis < 3; axis++)
					landing[axis] = raised_eye[axis] + travel * direction[axis];
				snprintf(what, sizeof(what), "sightline: eye %d wall %.2f m selects the barrel hit", sample, meters);
				check3(landing, point[0], point[1], point[2], 1e-5, what);
			}
		}
	}
	memset(&near_state, 0, sizeof(near_state));
	controller_sightline(&near_state, eye, eye, along_y, 0.001f, direction);
	check3(direction, 0.0, 1.0, 0.0, 1e-6, "sightline: a hit at the eye falls back to the barrel");

	/* P inside 0.5 units jumps 0.3 units: 63% of the way in 30 ms, 95% in 90 ms */
	memset(&near_state, 0, sizeof(near_state));
	point[0] = 0.1f, point[1] = 0.0f, point[2] = 0.0f;
	controller_sightline(&near_state, eye, point, along_x, 0.001f, direction);
	point[0] = 0.4f;
	for (step = 1; step <= 90; step++)
	{
		controller_sightline(&near_state, eye, point, along_x, 0.001f, direction);
		if (step == 30)
		{
			snprintf(what, sizeof(what), "sightline: low-pass, 63%% in 30 ms (%.1f%%)",
				100.0 * (near_state.point[0] - 0.1) / 0.3);
			check(near((near_state.point[0] - 0.1) / 0.3, 0.63, 0.02), what);
		}
	}
	snprintf(what, sizeof(what), "sightline: low-pass, 95%% in 90 ms (%.1f%%)", 100.0 * (near_state.point[0] - 0.1) / 0.3);
	check(near((near_state.point[0] - 0.1) / 0.3, 0.95, 0.02), what);
	/* beyond 0.5 units P follows at once */
	point[0] = 2.0f, point[1] = 0.5f;
	controller_sightline(&near_state, eye, point, along_x, 0.001f, direction);
	check(near_state.point[0] == point[0] && near_state.point[1] == point[1] && near_state.point[2] == point[2],
		"sightline: beyond 0.5 units P follows at once");
	check(angle_between(direction, point) <= 0.01, "sightline: and the direction is the eye's to it");

	/* the eye moving at 12 units/s with P 0.3 units ahead of it, 0.1 to the
	side: the low-pass is on P less the eye, so the direction doesn't lag */
	{
		const float relative[3] = { 0.3f, 0.1f, 0.0f };
		float moving_eye[3], still[3], worst = 0.0f;

		/* the same sightline from a still eye */
		memset(&near_state, 0, sizeof(near_state));
		controller_sightline(&near_state, eye, relative, along_x, 0.001f, still);
		memset(&near_state, 0, sizeof(near_state));
		for (step = 0; step < 300; step++)
		{
			int axis;

			moving_eye[0] = 100.0f + 0.012f * (float)step;
			moving_eye[1] = -50.0f + 0.006f * (float)step;
			moving_eye[2] = 7.0f;
			for (axis = 0; axis < 3; axis++)
				point[axis] = moving_eye[axis] + relative[axis];
			controller_sightline(&near_state, moving_eye, point, along_x, 0.001f, direction);
			if (step > 0 && angle_between(direction, still) > worst)
				worst = (float)angle_between(direction, still);
		}
		snprintf(what, sizeof(what), "sightline: a moving eye doesn't make P lag (%.4f degrees)", worst);
		check(worst <= 0.1f, what);
	}
}

static void angles(void)
{
	float up_87[3] = { (float)cos(RADIANS(87.0)), 0.0f, (float)sin(RADIANS(87.0)) };
	float down_87[3] = { (float)cos(RADIANS(87.0)), 0.0f, (float)-sin(RADIANS(87.0)) };
	float some[3] = { (float)(cos(RADIANS(10.0)) * cos(RADIANS(30.0))), (float)(cos(RADIANS(10.0)) * sin(RADIANS(30.0))),
		(float)sin(RADIANS(10.0)) };
	float yaw, pitch;

	controller_angles(up_87, &yaw, &pitch);
	check(near(DEGREES(pitch), 85.5, 1e-4) && near(DEGREES(yaw), 0.0, 1e-4), "angles: 87 up clamps to 85.5");
	controller_angles(down_87, &yaw, &pitch);
	check(near(DEGREES(pitch), -85.5, 1e-4), "angles: 87 down clamps to -85.5");
	controller_angles(some, &yaw, &pitch);
	check(near(DEGREES(yaw), 30.0, 1e-4) && near(DEGREES(pitch), 10.0, 1e-4), "angles: yaw 30 (left), pitch 10");
}

static void facing_pack(void)
{
	const double round_trips[] = { 0.0, 90.0, -90.0, 179.99, -180.0 };
	float delta = 1.0f;
	unsigned index;
	char what[160];

	check(controller_facing_pack(0, 0.0f) == 0 && controller_facing_pack(0, 1.0f) == 0 &&
		controller_facing_pack(0, -3.0f) == 0, "facing: hand flag 0 always packs to 0");
	check(controller_facing_unpack(0, &delta) == 0 && delta == 0.0f, "facing: 0 unpacks to hand 0, delta 0");
	for (index = 0; index < sizeof(round_trips) / sizeof(round_trips[0]); index++)
	{
		short pad = controller_facing_pack(1, (float)RADIANS(round_trips[index]));
		int hand = controller_facing_unpack(pad, &delta);

		snprintf(what, sizeof(what), "facing: %.2f degrees round trips with the hand (%.4f)", round_trips[index],
			DEGREES(delta));
		check(hand == 1 && fabs(wrapped_degrees(DEGREES(delta) - round_trips[index])) <= 0.011, what);
	}
	check(controller_facing_pack(1, (float)RADIANS(190.0)) == controller_facing_pack(1, (float)RADIANS(-170.0)),
		"facing: +190 packs as -170");
	controller_facing_unpack(controller_facing_pack(1, (float)RADIANS(190.0)), &delta);
	check(near(DEGREES(delta), -170.0, 0.011), "facing: +190 unpacks to -170");
	check(controller_facing_pack(1, 0.0f) == 1, "facing: hand 1, delta 0 packs to 1");
}

static void yaw_blend(void)
{
	float yaw = controller_yaw_blend((float)RADIANS(170.0), (float)RADIANS(-170.0), 0.5f);

	check(near(fabs(DEGREES(yaw)), 180.0, 1e-3), "yaw blend: +170 to -170 halfway is +-180, not 0");
	yaw = controller_yaw_blend((float)RADIANS(10.0), (float)RADIANS(50.0), 0.25f);
	check(near(DEGREES(yaw), 20.0, 1e-4), "yaw blend: 10 to 50 a quarter is 20");
}

/* tracking: one frame each millisecond */
#define TRACK_STEP 0.001f

struct track_run
{
	int messages[3], buzzes;
};

static void track_frame(struct controller_track *track, int state, int held, struct controller_track_out *out,
	struct track_run *run)
{
	controller_track_update(track, state, held, TRACK_STEP, out);
	if (out->message > 0 && out->message < 3)
		run->messages[out->message]++;
	run->buzzes += out->buzz;
}

/* a loss from t = 0 by the state and held given, then tracked and held from 1.0 s */
static void loss(int state, int held, const char *name)
{
	struct controller_track track;
	struct controller_track_out out;
	struct track_run run = { { 0 }, 0 };
	int t, ease_start = -1, full = -1, message_frame = -1, recovered = -1;
	char what[160];

	controller_track_reset(&track);
	for (t = 0; t < 1000; t++)
	{
		track_frame(&track, state, held, &out, &run);
		if (out.message == 1)
			message_frame = t;
		if (t == 149)
		{
			snprintf(what, sizeof(what), "loss (%s): at 149 ms held: blend 1, the last aim, dimmed", name);
			check(out.blend >= 0.9999f && out.use_held && out.dim, what);
		}
		if (t == 250)
		{
			snprintf(what, sizeof(what), "loss (%s): at 250 ms halfway (%.3f)", name, out.blend);
			check(near(out.blend, 0.5, 0.02), what);
		}
		if (t == 350)
		{
			snprintf(what, sizeof(what), "loss (%s): at 350 ms the head (%.6f)", name, out.blend);
			check(out.blend <= 1e-6f && out.dim, what);
		}
	}
	snprintf(what, sizeof(what), "loss (%s): one message and one buzz (%d, %d at %d ms)", name, run.messages[1],
		run.buzzes, message_frame);
	check(run.messages[1] == 1 && run.buzzes == 1 && message_frame >= 345 && message_frame <= 351, what);
	snprintf(what, sizeof(what), "loss (%s): and still the head at 1 s", name);
	check(out.blend == 0.0f && out.use_held, what);
	for (; t < 2000; t++)
	{
		track_frame(&track, HALO_SPATIAL_TRACKED, 1, &out, &run);
		if (ease_start < 0 && out.blend > 0.0f)
			ease_start = t;
		if (recovered < 0 && !out.use_held)
			recovered = t;
		if (full < 0 && out.blend >= 0.999f)
			full = t;
	}
	snprintf(what, sizeof(what), "loss (%s): recovery eases from 1.1 s (%d ms), whole by 1.3 s (%d ms)", name,
		ease_start, full);
	check(ease_start >= 1099 && ease_start <= 1102 && recovered >= 1098 && recovered <= 1101 && full >= 1290 &&
		full <= 1301, what);
	snprintf(what, sizeof(what), "loss (%s): no recovery message within 5 s of the last", name);
	check(run.messages[1] == 1 && run.messages[2] == 0 && run.buzzes == 1, what);
	check(out.blend == 1.0f && !out.dim && !out.use_held, "loss: recovered, the hand undimmed");
}

static void tracking(void)
{
	struct controller_track track;
	struct controller_track_out out;
	struct track_run run = { { 0 }, 0 };
	int t, ok = 1;

	controller_track_reset(&track);
	for (t = 0; t < 100; t++)
	{
		track_frame(&track, HALO_SPATIAL_TRACKED, 1, &out, &run);
		ok &= out.blend == 1.0f && !out.dim && !out.use_held && !out.message && !out.buzz;
	}
	check(ok, "tracking: tracked and held: the hand, undimmed, quiet");
	loss(HALO_SPATIAL_UNTRACKED, 1, "untracked");
	loss(HALO_SPATIAL_TRACKED, 0, "not held");
	loss(HALO_SPATIAL_NONE, 1, "disconnected");
	loss(HALO_SPATIAL_ORIENTATION, 0, "orientation-only, not held");

	/* orientation-only and held: the hand aims on; the dim from 2.0 s */
	controller_track_reset(&track);
	ok = 1;
	for (t = 0; t < 3000; t++)
	{
		track_frame(&track, HALO_SPATIAL_ORIENTATION, 1, &out, &run);
		ok &= out.blend == 1.0f && !out.use_held && !out.message && !out.buzz;
		if (t < 1995)
			ok &= !out.dim;
		if (t >= 2000)
			ok &= out.dim;
	}
	check(ok, "tracking: orientation-only: the hand aims on, dimmed from 2.0 s");
	/* low accuracy is tracked */
	controller_track_reset(&track);
	ok = 1;
	for (t = 0; t < 500; t++)
	{
		track_frame(&track, HALO_SPATIAL_LOW_ACCURACY, 1, &out, &run);
		ok &= out.blend == 1.0f && !out.dim && !out.use_held;
	}
	check(ok, "tracking: low accuracy aims as tracked");
}

/* untracked for lost_ms, then tracked until the recovery starts: the
milliseconds of tracking it needed */
static int dwell(struct controller_track *track, int lost_ms, struct track_run *run)
{
	struct controller_track_out out;
	int t;

	for (t = 0; t < lost_ms; t++)
		track_frame(track, HALO_SPATIAL_UNTRACKED, 1, &out, run);
	for (t = 1; t <= 5000; t++)
	{
		track_frame(track, HALO_SPATIAL_TRACKED, 1, &out, run);
		if (!out.use_held)
			return t;
	}
	return -1;
}

static void tracked_for(struct controller_track *track, int ms, struct track_run *run)
{
	struct controller_track_out out;
	int t;

	for (t = 0; t < ms; t++)
		track_frame(track, HALO_SPATIAL_TRACKED, 1, &out, run);
}

static void growing_dwell(void)
{
	struct controller_track track;
	struct controller_track_out out;
	struct track_run run = { { 0 }, 0 };
	int first, second, third, later, t;
	char what[160];

	controller_track_reset(&track);
	first = dwell(&track, 500, &run);
	tracked_for(&track, 500, &run);
	second = dwell(&track, 500, &run);
	tracked_for(&track, 500, &run);
	third = dwell(&track, 500, &run);
	/* 10 s without a loss */
	tracked_for(&track, 10000, &run);
	later = dwell(&track, 500, &run);
	snprintf(what, sizeof(what), "dwell: 100, 500, 1000, then 100 ms after 10 s (%d, %d, %d, %d)", first, second,
		third, later);
	check(near(first, 100, 2) && near(second, 500, 2) && near(third, 1000, 2) && near(later, 100, 2), what);

	/* flapping: 50 ms untracked every 400 ms for 10 s */
	controller_track_reset(&track);
	memset(&run, 0, sizeof(run));
	for (t = 0; t < 10000; t++)
		track_frame(&track, t % 400 < 50 ? HALO_SPATIAL_UNTRACKED : HALO_SPATIAL_TRACKED, 1, &out, &run);
	snprintf(what, sizeof(what), "dwell: flapping, at most two messages and two buzzes (%d, %d)",
		run.messages[1] + run.messages[2], run.buzzes);
	check(run.messages[1] + run.messages[2] <= 2 && run.buzzes <= 2, what);
}

static void gate_ease(void)
{
	float value = 0.0f, blend = 0.0f;
	int t, ok = 1;
	char what[160];

	for (t = 1; t <= 300; t++)
	{
		float last = blend;

		blend = controller_gate_ease(&value, 1, 0.001f, 0);
		ok &= blend >= last;
		if (t == 100)
		{
			snprintf(what, sizeof(what), "gate: halfway at 0.1 s (%.3f)", blend);
			check(near(blend, 0.5, 0.02), what);
		}
		if (t == 200)
			check(blend >= 0.999f, "gate: whole at 0.2 s");
	}
	check(ok && blend == 1.0f, "gate: rises to 1 and stays");
	blend = controller_gate_ease(&value, 0, 0.1f, 0);
	check(near(blend, 0.5, 0.02), "gate: and back toward 0 the same way");
	value = 0.0f;
	blend = controller_gate_ease(&value, 1, 0.001f, 1);
	check(blend == 1.0f, "gate: snap sets it at once");
}

#define FIRE 0x1ul
#define GRENADE 0x8ul

static void latch(void)
{
	struct controller_latch latch_state;
	float yaw, pitch;
	short pad;
	int latched;

	memset(&latch_state, 0, sizeof(latch_state));
	/* fire set on frame 1 of tick 5 latches */
	yaw = 0.1f, pitch = 0.2f, pad = 7;
	latched = controller_latch_update(&latch_state, 5, 1, FIRE, FIRE, &yaw, &pitch, &pad);
	check(latched && yaw == 0.1f && pitch == 0.2f && pad == 7, "latch: fire on frame 1 latches, its aim kept");
	yaw = 0.5f, pitch = 0.6f, pad = 9;
	latched = controller_latch_update(&latch_state, 5, 1, FIRE, FIRE, &yaw, &pitch, &pad);
	check(latched && yaw == 0.1f && pitch == 0.2f && pad == 7, "latch: frame 2 takes frame 1's aim");
	yaw = 0.7f, pitch = 0.6f, pad = 9;
	latched = controller_latch_update(&latch_state, 5, 1, 0, FIRE, &yaw, &pitch, &pad);
	check(latched && yaw == 0.1f && pad == 7, "latch: frame 3, released, still frame 1's");
	yaw = 0.8f;
	latched = controller_latch_update(&latch_state, 5, 1, FIRE, FIRE, &yaw, &pitch, &pad);
	check(latched && yaw == 0.1f, "latch: fire already pending doesn't relatch");
	yaw = 0.9f, pitch = 0.6f, pad = 9;
	latched = controller_latch_update(&latch_state, 6, 1, 0, FIRE, &yaw, &pitch, &pad);
	check(!latched && yaw == 0.9f && pitch == 0.6f && pad == 9, "latch: tick 6 clears it");

	/* a grenade alone doesn't latch */
	yaw = 1.0f;
	latched = controller_latch_update(&latch_state, 7, 1, GRENADE, FIRE, &yaw, &pitch, &pad);
	yaw = 1.1f;
	latched |= controller_latch_update(&latch_state, 7, 1, GRENADE, FIRE, &yaw, &pitch, &pad);
	check(!latched && yaw == 1.1f, "latch: a grenade bit alone doesn't latch");

	/* fire pressed while the game isn't running is dropped */
	yaw = 1.2f;
	latched = controller_latch_update(&latch_state, 8, 0, FIRE, FIRE, &yaw, &pitch, &pad);
	latched |= controller_latch_update(&latch_state, 8, 0, FIRE, FIRE, &yaw, &pitch, &pad);
	check(!latched && yaw == 1.2f && latch_state.pending == 0, "latch: fire while paused is dropped");
	/* the first running frame after, fire still held, latches */
	yaw = 1.3f;
	latched = controller_latch_update(&latch_state, 8, 1, FIRE, FIRE, &yaw, &pitch, &pad);
	yaw = 1.4f;
	latched &= controller_latch_update(&latch_state, 8, 1, FIRE, FIRE, &yaw, &pitch, &pad);
	check(latched && yaw == 1.3f, "latch: the first running frame after the pause latches");

	/* a latch made before a pause doesn't outlive it */
	yaw = 2.0f;
	controller_latch_update(&latch_state, 9, 1, FIRE, FIRE, &yaw, &pitch, &pad);
	yaw = 2.1f;
	latched = controller_latch_update(&latch_state, 9, 0, 0, FIRE, &yaw, &pitch, &pad);
	yaw = 2.2f;
	latched |= controller_latch_update(&latch_state, 9, 1, 0, FIRE, &yaw, &pitch, &pad);
	check(!latched && yaw == 2.2f, "latch: a pause clears the latch");
}

static void resets(void)
{
	check(controller_needs_reset(1, 2, 500, 501), "reset: a new unit");
	check(controller_needs_reset(1, 1, 500, 300), "reset: time went backward");
	check(controller_needs_reset(1, 1, 500, 0), "reset: time went to 0");
	check(!controller_needs_reset(1, 1, 500, 501), "reset: time went on");
}

static double dot3(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* the gun's frame: the camera's at blend 0, the sightline's with the
barrel's roll at 1, eased along the arc between, always unit and square */
/* the first-person gun's limits (controller_weapon_limit) */
static void weapon_limit(void)
{
	const float camera_forward[3] = { 1.0f, 0.0f, 0.0f }, camera_up[3] = { 0.0f, 0.0f, 1.0f };
	const float turn = (float)RADIANS(25.0), roll = (float)RADIANS(15.0);
	float forward[3], up[3];
	int degrees;

	/* 10 degrees left (+Y is left: -10 across) and level: inside the knee, untouched */
	forward[0] = (float)cos(RADIANS(10.0)); forward[1] = (float)sin(RADIANS(10.0)); forward[2] = 0.0f;
	up[0] = up[1] = 0.0f; up[2] = 1.0f;
	controller_weapon_limit(camera_forward, camera_up, turn, roll, forward, up);
	check3(forward, cos(RADIANS(10.0)), sin(RADIANS(10.0)), 0.0, 1e-5, "gun limits: 10 degrees to the side is untouched");
	check3(up, 0.0, 0.0, 1.0, 1e-5, "gun limits: a level gun keeps its up");
	/* the turn never passes the limit, and grows all the way; past it the model sinks */
	{
		double last = 0.0;

		for (degrees = 5; degrees <= 85; degrees += 5)
		{
			double across;

			forward[0] = (float)cos(RADIANS(degrees)); forward[1] = (float)-sin(RADIANS(degrees)); forward[2] = 0.0f;
			up[0] = up[1] = 0.0f; up[2] = 1.0f;
			controller_weapon_limit(camera_forward, camera_up, turn, roll, forward, up);
			across = atan2(-forward[1], forward[0]);
			check(across <= turn + 1e-5 && across > last, "gun limits: the turn grows with the hand and stays inside the limit");
			check(degrees <= 25 ? fabs(forward[2]) < 1e-5 : forward[2] < 0.0f, "gun limits: past the turn limit the model sinks, not before");
			check(near(dot3(forward, forward), 1.0, 1e-5) && near(dot3(up, forward), 0.0, 1e-5) && near(dot3(up, up), 1.0, 1e-5),
				"gun limits: unit and square");
			last = across;
		}
	}
	/* 60 degrees to the side sinks 52.5 degrees (35 past the limit, a degree and a half each) plus nothing more */
	forward[0] = (float)cos(RADIANS(60.0)); forward[1] = (float)-sin(RADIANS(60.0)); forward[2] = 0.0f;
	up[0] = up[1] = 0.0f; up[2] = 1.0f;
	controller_weapon_limit(camera_forward, camera_up, turn, roll, forward, up);
	check(near(asin(forward[2]), -RADIANS(52.5), 1e-4), "gun limits: 60 degrees to the side sinks 52.5");
	/* a gun rolled 80 degrees shows at most 15 */
	forward[0] = 1.0f; forward[1] = forward[2] = 0.0f;
	up[0] = 0.0f; up[1] = (float)sin(RADIANS(80.0)); up[2] = (float)cos(RADIANS(80.0));
	controller_weapon_limit(camera_forward, camera_up, turn, roll, forward, up);
	check(fabs(atan2(up[1], up[2])) <= roll + 1e-5 && fabs(atan2(up[1], up[2])) > RADIANS(13.0), "gun limits: the roll is held near its limit");
	/* a roll of 5 degrees is untouched; limits of 0 leave everything alone */
	up[0] = 0.0f; up[1] = (float)sin(RADIANS(5.0)); up[2] = (float)cos(RADIANS(5.0));
	controller_weapon_limit(camera_forward, camera_up, turn, roll, forward, up);
	check(near(atan2(up[1], up[2]), RADIANS(5.0), 1e-5), "gun limits: a small roll is untouched");
	forward[0] = (float)cos(RADIANS(60.0)); forward[1] = (float)-sin(RADIANS(60.0)); forward[2] = 0.0f;
	up[0] = 0.0f; up[1] = (float)sin(RADIANS(80.0)); up[2] = (float)cos(RADIANS(80.0));
	{
		float before_forward[3], before_up[3];

		squared(up, forward);
		memcpy(before_forward, forward, sizeof(forward));
		memcpy(before_up, up, sizeof(up));
		controller_weapon_limit(camera_forward, camera_up, 0.0f, 0.0f, forward, up);
		check3(forward, before_forward[0], before_forward[1], before_forward[2], 1e-6, "gun limits: no limits, the same forward");
		check3(up, before_up[0], before_up[1], before_up[2], 1e-5, "gun limits: no limits, the same up");
	}
}

/* the aim point's range, eased (controller_range_ease) */
static void range_ease(void)
{
	struct controller_range state = { .known = 0 };
	const float ahead[3] = { 1.0f, 0.0f, 0.0f };
	float range = 0.0f, turning[3], place[3] = { 0.3f, -0.2f, -0.5f };
	int frame;

	check(controller_range_ease(&state, ahead, place, 13.0f, 0.0f, 1.0f / 90.0f) == 13.0f && !state.known, "range ease: 0 seconds is the range itself");
	check(controller_range_ease(&state, ahead, place, 13.0f, 0.05f, 1.0f / 90.0f) == 13.0f && state.known, "range ease: the first range is taken whole");
	/* from 13 units to 2.6 (a far slope to an enemy at 8 m) with the ray
	still: the inverse range moves 1 - exp(-1) of the way in one time constant */
	for (frame = 0; frame < 9; frame++)
		range = controller_range_ease(&state, ahead, place, 2.6f, 0.05f, 0.05f / 9.0f);
	check(near(1.0 / range, 1.0 / 13.0 + (1.0 / 2.6 - 1.0 / 13.0) * (1.0 - exp(-1.0)), 1e-4),
		"range ease: 63% of the way, in inverse range, after one time constant");
	for (frame = 0; frame < 45; frame++)
		range = controller_range_ease(&state, ahead, place, 2.6f, 0.05f, 1.0f / 90.0f);
	check(near(range, 2.6, 1e-3), "range ease: it arrives");
	/* a slow sweep (20 degrees a second) eases as a still ray does */
	turning[0] = (float)cos(RADIANS(20.0 / 90.0)); turning[1] = (float)sin(RADIANS(20.0 / 90.0)); turning[2] = 0.0f;
	range = controller_range_ease(&state, turning, place, 13.0f, 0.05f, 1.0f / 90.0f);
	check(near(1.0 / range, 1.0 / 2.6 + (1.0 / 13.0 - 1.0 / 2.6) * (1.0 - exp(-1.0 / 90.0 / 0.05)), 1e-4), "range ease: a slow sweep eases whole");
	/* a fast move (200 degrees a second) takes the new range at once */
	turning[0] = (float)cos(RADIANS(20.0 / 90.0 + 200.0 / 90.0)); turning[1] = (float)sin(RADIANS(20.0 / 90.0 + 200.0 / 90.0));
	check(controller_range_ease(&state, turning, place, 40.0f, 0.05f, 1.0f / 90.0f) == 40.0f, "range ease: a fast move takes the range at once");
	check(controller_range_ease(&state, turning, place, 100.0f, 0.05f, 1.0f) == 100.0f, "range ease: a second-long frame starts afresh");
	/* one frame of a hit 0.04 units from the muzzle is taken whole, and so is the range after it */
	(void)controller_range_ease(&state, turning, place, 100.0f, 0.05f, 1.0f / 90.0f);
	check(controller_range_ease(&state, turning, place, 0.04f, 0.05f, 1.0f / 90.0f) == 0.04f && !state.known,
		"range ease: a hit at the muzzle is taken whole");
	check(controller_range_ease(&state, turning, place, 0.9f, 0.05f, 1.0f / 90.0f) == 0.9f, "range ease: and nothing eases out of it");
	/* a hand suddenly 1.1 m higher (the head's place changed under it) takes the new range at once */
	(void)controller_range_ease(&state, turning, place, 100.0f, 0.05f, 1.0f / 90.0f);
	place[2] += 1.1f;
	check(controller_range_ease(&state, turning, place, 2.0f, 0.05f, 1.0f / 90.0f) == 2.0f, "range ease: a hand that jumped starts afresh");
	check(controller_range_ease_clamp(-1.0f) == 0.0f && controller_range_ease_clamp(1.0f) == 0.3f, "range ease: the setting's range");
}

/* the first-person gun's low-pass (controller_weapon_smooth) */
static void weapon_smooth(void)
{
	struct controller_weapon_filter filter = { .known = 0 };
	float forward[3] = { 1.0f, 0.0f, 0.0f }, up[3] = { 0.0f, 0.0f, 1.0f }, live[3], before;
	double step = RADIANS(1.0), body = RADIANS(30.0);
	int frame;

	/* off: the live frame bit for bit, and no history kept */
	live[0] = (float)cos(step); live[1] = (float)sin(step); live[2] = 0.0f;
	memcpy(forward, live, sizeof(live));
	controller_weapon_smooth(&filter, 0.0f, 0.0f, 1.0f / 90.0f, forward, up);
	check(!memcmp(forward, live, sizeof(live)) && !filter.known, "gun smoothing: 0 seconds leaves the live frame");
	/* the first frame is the live frame; a 1 degree step then arrives over the time constant */
	forward[0] = 1.0f; forward[1] = 0.0f; forward[2] = 0.0f;
	controller_weapon_smooth(&filter, 0.0f, 0.08f, 1.0f / 90.0f, forward, up);
	check3(forward, 1.0, 0.0, 0.0, 1e-6, "gun smoothing: the first frame is the live frame");
	for (frame = 0; frame < 7; frame++)
	{
		memcpy(forward, live, sizeof(live));
		up[0] = up[1] = 0.0f; up[2] = 1.0f;
		controller_weapon_smooth(&filter, 0.0f, 0.08f, 1.0f / 90.0f, forward, up);
	}
	/* 7 frames at 90 Hz are 0.078 s: 1 - exp(-0.078 / 0.08) = 0.62 of the step */
	check(near(atan2(forward[1], forward[0]), step * (1.0 - exp(-7.0 / 90.0 / 0.08)), RADIANS(0.01)),
		"gun smoothing: a 1 degree step is 62% there after one time constant");
	check(near(dot3(forward, forward), 1.0, 1e-5) && near(dot3(up, forward), 0.0, 1e-5), "gun smoothing: unit and square");
	/* a snap turn (the body yaw steps 30 degrees, the hand still in the room) carries the gun whole */
	before = atan2f(forward[1], forward[0]);
	live[0] = (float)cos(step + body); live[1] = (float)sin(step + body);
	memcpy(forward, live, sizeof(live));
	up[0] = up[1] = 0.0f; up[2] = 1.0f;
	controller_weapon_smooth(&filter, (float)body, 0.08f, 1.0f / 90.0f, forward, up);
	check(atan2(forward[1], forward[0]) - before > body && atan2(forward[1], forward[0]) < step + body,
		"gun smoothing: a snap turn doesn't lag the gun");
	/* a 40 degree sweep catches up at the fast rate: most of the way in one frame */
	live[0] = (float)cos(step + body + RADIANS(40.0)); live[1] = (float)sin(step + body + RADIANS(40.0));
	memcpy(forward, live, sizeof(live));
	controller_weapon_smooth(&filter, (float)body, 0.08f, 1.0f / 90.0f, forward, up);
	check(atan2(forward[1], forward[0]) > step + body + RADIANS(20.0), "gun smoothing: a large move catches up fast");
	check(controller_weapon_smooth_clamp(-1.0f) == 0.0f && controller_weapon_smooth_clamp(2.0f) == 0.5f &&
		controller_weapon_smooth_clamp(0.08f) == 0.08f, "gun smoothing: the setting's range");
}

static void weapon_frame(void)
{
	const float camera_forward[3] = { 1.0f, 0.0f, 0.0f }, camera_up[3] = { 0.0f, 0.0f, 1.0f };
	/* the sightline 60 degrees left and 10 up; the barrel rolled 20 degrees
	right about it and not quite square to it (the near blend) */
	float sightline[3], barrel_up[3], forward[3], up[3], behind[3] = { -1.0f, 0.0f, 0.0f };
	double yaw = RADIANS(60.0), pitch = RADIANS(10.0), roll = RADIANS(20.0);
	float level_left[3], pitched_up[3];
	char what[160];
	int step, axis;

	sightline[0] = (float)(cos(pitch) * cos(yaw));
	sightline[1] = (float)(cos(pitch) * sin(yaw));
	sightline[2] = (float)sin(pitch);
	level_left[0] = (float)-sin(yaw);
	level_left[1] = (float)cos(yaw);
	level_left[2] = 0.0f;
	pitched_up[0] = (float)(-sin(pitch) * cos(yaw));
	pitched_up[1] = (float)(-sin(pitch) * sin(yaw));
	pitched_up[2] = (float)cos(pitch);
	/* rolled right: up tips toward -left; then 3 degrees of tilt along the sightline */
	for (axis = 0; axis < 3; axis++)
		barrel_up[axis] = (float)(cos(roll) * pitched_up[axis] - sin(roll) * level_left[axis] +
			sin(RADIANS(3.0)) * sightline[axis]);

	check(controller_weapon_frame(camera_forward, camera_up, sightline, barrel_up, 0.0f, forward, up),
		"weapon frame: blend 0 defined");
	check3(forward, 1.0, 0.0, 0.0, 1e-6, "weapon frame: blend 0: the camera's forward");
	check3(up, 0.0, 0.0, 1.0, 1e-6, "weapon frame: blend 0: the camera's up");

	check(controller_weapon_frame(camera_forward, camera_up, sightline, barrel_up, 1.0f, forward, up),
		"weapon frame: blend 1 defined");
	check3(forward, sightline[0], sightline[1], sightline[2], 1e-6, "weapon frame: blend 1: the sightline");
	check(near(dot3(up, forward), 0.0, 1e-6), "weapon frame: blend 1: up square to forward");
	check(near(dot3(up, up), 1.0, 1e-5), "weapon frame: blend 1: up unit");
	/* the barrel's roll kept: 20 degrees right of the sightline's own up */
	check(near(angle_between(up, pitched_up), 20.0, 0.01), "weapon frame: blend 1: the barrel's roll");
	check(dot3(up, level_left) < 0.0, "weapon frame: blend 1: rolled right, not left");

	/* mid-blend: forward on the arc, the angle shared by the blend; unit and square */
	for (step = 1; step < 10; step++)
	{
		float blend = (float)step / 10.0f;

		check(controller_weapon_frame(camera_forward, camera_up, sightline, barrel_up, blend, forward, up),
			"weapon frame: mid-blend defined");
		snprintf(what, sizeof(what), "weapon frame: blend %.1f: on the arc at its share", blend);
		check(near(angle_between(forward, camera_forward), blend * angle_between(sightline, camera_forward), 0.01) &&
			near(angle_between(forward, sightline), (1.0 - blend) * angle_between(sightline, camera_forward), 0.01),
			what);
		snprintf(what, sizeof(what), "weapon frame: blend %.1f: unit and square", blend);
		check(near(dot3(forward, forward), 1.0, 1e-5) && near(dot3(up, up), 1.0, 1e-5) &&
			near(dot3(up, forward), 0.0, 1e-5), what);
	}

	/* a blend outside 0 to 1 is clamped */
	check(controller_weapon_frame(camera_forward, camera_up, sightline, barrel_up, 1.5f, forward, up),
		"weapon frame: blend 1.5 defined");
	check3(forward, sightline[0], sightline[1], sightline[2], 1e-6, "weapon frame: blend 1.5 is 1");
	check(controller_weapon_frame(camera_forward, camera_up, sightline, barrel_up, -0.5f, forward, up),
		"weapon frame: blend -0.5 defined");
	check3(forward, 1.0, 0.0, 0.0, 1e-6, "weapon frame: blend -0.5 is 0");

	/* straight behind mid-blend: no arc; the outputs are left alone */
	forward[0] = 7.0f;
	check(!controller_weapon_frame(camera_forward, camera_up, behind, camera_up, 0.5f, forward, up),
		"weapon frame: straight behind mid-blend undefined");
	check(forward[0] == 7.0f, "weapon frame: undefined leaves forward alone");
	/* but at blend 1 it is the sightline */
	check(controller_weapon_frame(camera_forward, camera_up, behind, camera_up, 1.0f, forward, up),
		"weapon frame: straight behind at blend 1 defined");
	check3(forward, -1.0, 0.0, 0.0, 1e-6, "weapon frame: straight behind at blend 1: the sightline");

	/* the sightline straight up with the barrel's up along it: the camera's
	up squared stands in; with that along it too, undefined */
	{
		const float straight_up[3] = { 0.0f, 0.0f, 1.0f }, tipped_up[3] = { -1.0f, 0.0f, 0.0f };

		check(controller_weapon_frame(camera_forward, tipped_up, straight_up, straight_up, 1.0f, forward, up),
			"weapon frame: barrel up along forward: the camera's up stands in");
		check3(up, -1.0, 0.0, 0.0, 1e-6, "weapon frame: barrel up along forward: the camera's up");
		check(!controller_weapon_frame(straight_up, straight_up, straight_up, straight_up, 1.0f, forward, up),
			"weapon frame: no up square to forward: undefined");
	}
}

/* a camera's frame turned to yaw and pitch (radians, the game's convention:
+X forward, +Y left, +Z up): forward, up and right */
static void zoom_camera(double yaw, double pitch, float forward[3], float up[3], float right[3])
{
	forward[0] = (float)(cos(pitch) * cos(yaw));
	forward[1] = (float)(cos(pitch) * sin(yaw));
	forward[2] = (float)sin(pitch);
	up[0] = (float)(-sin(pitch) * cos(yaw));
	up[1] = (float)(-sin(pitch) * sin(yaw));
	up[2] = (float)cos(pitch);
	right[0] = (float)sin(yaw);
	right[1] = (float)-cos(yaw);
	right[2] = 0.0f;
}

/* the direction at tangents across (right) and up of the camera's forward */
static void zoom_direction(const float forward[3], const float up[3], const float right[3], double across,
	double upward, float direction[3])
{
	int axis;

	for (axis = 0; axis < 3; axis++)
		direction[axis] = (float)(forward[axis] + across * right[axis] + upward * up[axis]);
}

/* the head takes the aim where the zoomed reticle leaves sight
(controller_zoom_project_sight), with the headset's logged geometry: the
zoomed picture's half tangents 1.749 by 1.205 before the magnification; each
eye's frustum 1.732 out, 1.000 in, 1.000 up, 1.192 down. The eyes sit 34 mm
either side of the view's center and 26 mm under it (what the picture's
tangents say, on the plane 2 m ahead), so both eyes see 0.983 either way
across, 0.987 up and 1.205 down, and the eyes' shared view reaches 40
degrees off center (tangent 0.839), which is the nearest of them all round */
static void zoom_sight(void)
{
	const float picture[2] = { 1.749f, 1.205f };
	const struct controller_zoom_sight headset = { 0.983f, 0.983f, 0.987f, 1.205f,
		(float)tan(RADIANS(HALO_STEREO_SHARED_VIEW_DEGREES)) };
	const float magnifications[2] = { 2.0f, 8.0f };
	/* each way: right, left, up, down, and the four diagonals */
	const double ways[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { M_SQRT1_2, M_SQRT1_2 },
		{ -M_SQRT1_2, M_SQRT1_2 }, { M_SQRT1_2, -M_SQRT1_2 }, { -M_SQRT1_2, -M_SQRT1_2 } };
	const char *names[8] = { "right", "left", "up", "down", "up right", "up left", "down right", "down left" };
	float forward[3], up[3], right[3], direction[3], shown[3];
	char what[200];
	int level, way;

	zoom_camera(RADIANS(30.0), RADIANS(10.0), forward, up, right);
	check(near(HALO_STEREO_SHARED_VIEW_DEGREES, 40.0, 1e-6), "zoom sight: the eyes' shared view is 40 degrees");
	for (level = 0; level < 2; level++)
	{
		float magnification = magnifications[level];
		float tangents[2] = { picture[0] / magnification, picture[1] / magnification };
		/* the hand's angle off the view's center where its reticle shows
		40 degrees off: 22.8 degrees at 2x, 6.0 at 8x; back within 90% of
		the tangent: 20.7 and 5.4 */
		double edge = atan(headset.off_center / magnification);
		double reentry = atan(CONTROLLER_ZOOM_REENTRY * headset.off_center / magnification);

		snprintf(what, sizeof(what), "zoom sight: at %.0fx the edge is %.2f degrees of hand angle, re-entry %.2f",
			magnification, DEGREES(edge), DEGREES(reentry));
		check(near(DEGREES(edge), level == 0 ? 22.76 : 5.99, 0.01) &&
			near(DEGREES(reentry), level == 0 ? 20.69 : 5.39, 0.01), what);
		for (way = 0; way < 8; way++)
		{
			double angles[4] = { edge - RADIANS(0.05), edge + RADIANS(0.05), reentry + RADIANS(0.05),
				reentry - RADIANS(0.05) };
			int results[4], index;

			for (index = 0; index < 4; index++)
			{
				zoom_direction(forward, up, right, ways[way][0] * tan(angles[index]), ways[way][1] * tan(angles[index]),
					direction);
				results[index] = controller_zoom_project_sight(direction, forward, up, tangents, magnification, &headset,
					index < 2, shown);
			}
			snprintf(what, sizeof(what), "zoom sight: %.0fx, %s: the hand's aim to %.2f degrees, the head's past it",
				magnification, names[way], DEGREES(edge));
			check(results[0] && !results[1], what);
			snprintf(what, sizeof(what), "zoom sight: %.0fx, %s: the hand's aim again only inside %.2f degrees",
				magnification, names[way], DEGREES(reentry));
			check(!results[2] && results[3], what);
			/* the picture's own edge is farther out every way, so the old
			test kept the hand's aim there with its reticle out of sight */
			zoom_direction(forward, up, right, ways[way][0] * tan(angles[1]), ways[way][1] * tan(angles[1]), direction);
			snprintf(what, sizeof(what), "zoom sight: %.0fx, %s: that is still inside the picture", magnification,
				names[way]);
			check(controller_zoom_project(direction, forward, up, tangents, magnification, 1, shown), what);
		}
		/* inside, it shows where controller_zoom_project shows it */
		{
			float plain[3];

			zoom_direction(forward, up, right, 0.3 / magnification, -0.2 / magnification, direction);
			check(controller_zoom_project_sight(direction, forward, up, tangents, magnification, &headset, 1, shown) &&
				controller_zoom_project(direction, forward, up, tangents, magnification, 1, plain) &&
				near(shown[0], plain[0], 1e-6) && near(shown[1], plain[1], 1e-6) && near(shown[2], plain[2], 1e-6) &&
				near(shown[0] / -shown[2], 0.3, 1e-4) && near(shown[1] / -shown[2], -0.2, 1e-4),
				"zoom sight: inside, the picture's projection");
		}
	}
	/* any eyes: a frustum narrower than the shared view sets its own edge,
	each side by its own tangent (here 0.3 left, 0.6 right, 0.5 up, 0.7
	down, shown; a third of each at 3x), and the others stay */
	{
		const struct controller_zoom_sight narrow = { 0.3f, 0.6f, 0.5f, 0.7f, headset.off_center };
		const float tangents[2] = { picture[0] / 3.0f, picture[1] / 3.0f };
		const double edges[4][2] = { { 0.6, 0.0 }, { -0.3, 0.0 }, { 0.0, 0.5 }, { 0.0, -0.7 } };

		for (way = 0; way < 4; way++)
		{
			zoom_direction(forward, up, right, edges[way][0] * 0.999 / 3.0, edges[way][1] * 0.999 / 3.0, direction);
			level = controller_zoom_project_sight(direction, forward, up, tangents, 3.0f, &narrow, 1, shown);
			zoom_direction(forward, up, right, edges[way][0] * 1.001 / 3.0, edges[way][1] * 1.001 / 3.0, direction);
			snprintf(what, sizeof(what), "zoom sight: narrow eyes, %s: the edge is the eyes' own tangent", names[way]);
			check(level && !controller_zoom_project_sight(direction, forward, up, tangents, 3.0f, &narrow, 1, shown),
				what);
			zoom_direction(forward, up, right, edges[way][0] * 0.901 / 3.0, edges[way][1] * 0.901 / 3.0, direction);
			level = controller_zoom_project_sight(direction, forward, up, tangents, 3.0f, &narrow, 0, shown);
			zoom_direction(forward, up, right, edges[way][0] * 0.899 / 3.0, edges[way][1] * 0.899 / 3.0, direction);
			snprintf(what, sizeof(what), "zoom sight: narrow eyes, %s: re-entry within 90%% of it", names[way]);
			check(!level && controller_zoom_project_sight(direction, forward, up, tangents, 3.0f, &narrow, 0, shown),
				what);
		}
	}
	/* a picture smaller than what is in sight keeps its own edge, and its
	90% re-entry */
	{
		const float small[2] = { 0.09f, 0.07f };

		zoom_direction(forward, up, right, 0.0899, 0.0, direction);
		check(controller_zoom_project_sight(direction, forward, up, small, 2.0f, &headset, 1, shown),
			"zoom sight: a small picture: inside at 99.9% across");
		zoom_direction(forward, up, right, 0.0901, 0.0, direction);
		check(!controller_zoom_project_sight(direction, forward, up, small, 2.0f, &headset, 1, shown),
			"zoom sight: a small picture: outside at 100.1% across");
		zoom_direction(forward, up, right, 0.0, 0.0635, direction);
		check(!controller_zoom_project_sight(direction, forward, up, small, 2.0f, &headset, 0, shown),
			"zoom sight: a small picture: re-entry: still outside at 90.7% up");
	}
	/* a sweep across the edge and back crosses once each way: the hand out
	to 30 degrees right and back in tenths of a degree at 2x, the state
	carried as the action carries it */
	{
		const float tangents[2] = { picture[0] / 2.0f, picture[1] / 2.0f };
		int inside = 1, out = 0, back = 0, step;
		double out_at = 0.0, back_at = 0.0;

		for (step = 0; step <= 600; step++)
		{
			double angle = step <= 300 ? step * 0.1 : (600 - step) * 0.1;
			int now;

			zoom_direction(forward, up, right, tan(RADIANS(angle)), 0.0, direction);
			now = controller_zoom_project_sight(direction, forward, up, tangents, 2.0f, &headset, inside, shown);
			if (now != inside)
			{
				if (now)
				{
					back++;
					back_at = angle;
				}
				else
				{
					out++;
					out_at = angle;
				}
			}
			inside = now;
		}
		snprintf(what, sizeof(what), "zoom sight: a 30 degree sweep at 2x: out once (at %.1f degrees), back once (at %.1f)",
			out_at, back_at);
		check(out == 1 && back == 1 && near(out_at, 22.8, 0.051) && near(back_at, 20.6, 0.051), what);
	}
	/* behind the camera is outside, whatever is in sight */
	zoom_direction(forward, up, right, 0.0, 0.0, direction);
	direction[0] = -direction[0];
	direction[1] = -direction[1];
	direction[2] = -direction[2];
	shown[0] = 7.0f;
	check(!controller_zoom_project_sight(direction, forward, up, picture, 1.0f, &headset, 1, shown) && shown[0] == 7.0f,
		"zoom sight: behind is outside, and leaves shown alone");
}

static void zoom_project(void)
{
	/* the zoomed camera yawed 30 degrees left and pitched 10 up, so nothing
	leans on the world's axes */
	float forward[3], up[3], right[3], direction[3], shown[3] = { 7.0f, 7.0f, 7.0f };
	const float wide[2] = { 0.5f, 0.4f }, tangents[2] = { 0.09f, 0.07f };
	double across;

	zoom_camera(RADIANS(30.0), RADIANS(10.0), forward, up, right);

	/* 2 degrees right at 2x shows at atan(2 tan 2 degrees) right, level, in
	the eyes' frame (x right, y up, z back) */
	zoom_direction(forward, up, right, tan(RADIANS(2.0)), 0.0, direction);
	check(controller_zoom_project(direction, forward, up, wide, 2.0f, 1, shown), "zoom: 2 degrees right inside");
	check(near(DEGREES(atan2(shown[0], -shown[2])), DEGREES(atan(2.0 * tan(RADIANS(2.0)))), 0.01) &&
		near(DEGREES(atan(2.0 * tan(RADIANS(2.0)))), 3.995, 0.001), "zoom: 2 degrees right at 2x shows 3.995 right");
	check(near(shown[1], 0.0, 1e-6) && shown[2] < 0.0f, "zoom: 2 degrees right shows level and ahead");
	check(near(shown[0] * shown[0] + shown[1] * shown[1] + shown[2] * shown[2], 1.0, 1e-5), "zoom: shown is unit");
	/* up and left: each tangent times the magnification, the signs kept */
	zoom_direction(forward, up, right, -0.05, 0.03, direction);
	check(controller_zoom_project(direction, forward, up, wide, 3.0f, 1, shown), "zoom: up-left inside");
	check(near(shown[0] / -shown[2], -0.15, 1e-5) && near(shown[1] / -shown[2], 0.09, 1e-5),
		"zoom: up-left shows at three times its tangents");
	/* the direction's length doesn't matter */
	direction[0] *= 40.0f;
	direction[1] *= 40.0f;
	direction[2] *= 40.0f;
	check(controller_zoom_project(direction, forward, up, wide, 3.0f, 1, shown) &&
		near(shown[0] / -shown[2], -0.15, 1e-5), "zoom: a long direction shows the same");

	/* the zoomed tangents (0.09, 0.07), already over the magnification: 5
	degrees right (tan 0.0875) inside, 6 degrees (tan 0.105) outside */
	zoom_direction(forward, up, right, tan(RADIANS(5.0)), 0.0, direction);
	check(controller_zoom_project(direction, forward, up, tangents, 2.0f, 1, shown), "zoom: 5 degrees right inside");
	zoom_direction(forward, up, right, tan(RADIANS(6.0)), 0.0, direction);
	shown[0] = 7.0f;
	check(!controller_zoom_project(direction, forward, up, tangents, 2.0f, 1, shown), "zoom: 6 degrees right outside");
	check(shown[0] == 7.0f, "zoom: outside leaves shown alone");
	/* inside: the edge at 100% of each tangent, either side */
	for (across = -1.0; across <= 1.0; across += 2.0)
	{
		zoom_direction(forward, up, right, across * 0.0899, 0.0, direction);
		check(controller_zoom_project(direction, forward, up, tangents, 2.0f, 1, shown), "zoom: inside at 99.9% across");
		zoom_direction(forward, up, right, across * 0.0901, 0.0, direction);
		check(!controller_zoom_project(direction, forward, up, tangents, 2.0f, 1, shown), "zoom: outside at 100.1% across");
		zoom_direction(forward, up, right, 0.0, across * 0.0699, direction);
		check(controller_zoom_project(direction, forward, up, tangents, 2.0f, 1, shown), "zoom: inside at 99.9% up");
		zoom_direction(forward, up, right, 0.0, across * 0.0701, direction);
		check(!controller_zoom_project(direction, forward, up, tangents, 2.0f, 1, shown), "zoom: outside at 100.1% up");
		/* outside: inside again only within 90% (ruling 9) */
		zoom_direction(forward, up, right, across * 0.0815, 0.0, direction);
		check(!controller_zoom_project(direction, forward, up, tangents, 2.0f, 0, shown),
			"zoom: re-entry: still outside at 90.6% across");
		zoom_direction(forward, up, right, across * 0.0805, 0.0, direction);
		check(controller_zoom_project(direction, forward, up, tangents, 2.0f, 0, shown),
			"zoom: re-entry: inside at 89.4% across");
		zoom_direction(forward, up, right, 0.0, across * 0.0635, direction);
		check(!controller_zoom_project(direction, forward, up, tangents, 2.0f, 0, shown),
			"zoom: re-entry: still outside at 90.7% up");
		zoom_direction(forward, up, right, 0.0, across * 0.0625, direction);
		check(controller_zoom_project(direction, forward, up, tangents, 2.0f, 0, shown),
			"zoom: re-entry: inside at 89.3% up");
	}
	/* a corner inside both tangents, and one past the 90% box on one axis only */
	zoom_direction(forward, up, right, 0.085, 0.065, direction);
	check(controller_zoom_project(direction, forward, up, tangents, 2.0f, 1, shown), "zoom: the corner inside");
	zoom_direction(forward, up, right, 0.05, 0.065, direction);
	check(!controller_zoom_project(direction, forward, up, tangents, 2.0f, 0, shown),
		"zoom: re-entry needs both axes within 90%");

	/* behind the camera, even straight behind, is outside; so is square to it */
	zoom_direction(forward, up, right, 0.0, 0.0, direction);
	direction[0] = -direction[0];
	direction[1] = -direction[1];
	direction[2] = -direction[2];
	check(!controller_zoom_project(direction, forward, up, wide, 2.0f, 1, shown), "zoom: straight behind outside");
	zoom_direction(forward, up, right, 0.01, 0.0, direction);
	direction[0] = -direction[0];
	direction[1] = -direction[1];
	direction[2] = -direction[2];
	check(!controller_zoom_project(direction, forward, up, wide, 2.0f, 1, shown), "zoom: behind, near the axis, outside");
	check(!controller_zoom_project(right, forward, up, wide, 2.0f, 1, shown), "zoom: square to forward outside");
	/* clamped (the reticle while the action holds the hand's aim): inside
	it is the projection; past the edge it sits on the edge, each tangent
	within its own; behind is still nothing */
	zoom_direction(forward, up, right, 0.05, -0.03, direction);
	check(controller_zoom_project_clamped(direction, forward, up, tangents, 2.0f, shown) &&
		near(shown[0] / -shown[2], 0.10, 1e-5) && near(shown[1] / -shown[2], -0.06, 1e-5),
		"zoom clamped: inside, the projection");
	zoom_direction(forward, up, right, 0.12, -0.03, direction);
	check(controller_zoom_project_clamped(direction, forward, up, tangents, 2.0f, shown) &&
		near(shown[0] / -shown[2], 0.18, 1e-5) && near(shown[1] / -shown[2], -0.06, 1e-5),
		"zoom clamped: past the right edge, on it");
	zoom_direction(forward, up, right, -0.5, 0.5, direction);
	check(controller_zoom_project_clamped(direction, forward, up, tangents, 2.0f, shown) &&
		near(shown[0] / -shown[2], -0.18, 1e-5) && near(shown[1] / -shown[2], 0.14, 1e-5),
		"zoom clamped: past a corner, on the corner");
	for (across = 0; across < 3; across++)
		direction[(int)across] = -forward[(int)across];
	check(!controller_zoom_project_clamped(direction, forward, up, tangents, 2.0f, shown),
		"zoom clamped: behind, nothing");
	/* and nothing at all is inside nothing */
	{
		const float zero[3] = { 0.0f, 0.0f, 0.0f };

		check(!controller_zoom_project(zero, forward, up, wide, 2.0f, 1, shown), "zoom: no direction outside");
		check(!controller_zoom_project(forward, forward, forward, wide, 2.0f, 1, shown), "zoom: no up outside");
	}
}

int main(void)
{
	body_yaw();
	recenter(0, 0);
	recenter(87, 0);
	recenter(-87, 1);
	pole(20.0, 20.0, 1.0, "roll 20, up");
	pole(20.0, 20.0, -1.0, "roll 20, down");
	pole(0.0, 20.0, 1.0, "roll 0 in, 20 out, up");
	pole(0.0, 20.0, -1.0, "roll 0 in, 20 out, down");
	world();
	sightline();
	angles();
	facing_pack();
	yaw_blend();
	tracking();
	growing_dwell();
	gate_ease();
	latch();
	resets();
	weapon_frame();
	weapon_smooth();
	range_ease();
	weapon_limit();
	zoom_project();
	zoom_sight();
	if (failures)
	{
		printf("controller aim probe: %d FAILED\n", failures);
		return 1;
	}
	printf("controller aim probe: OK\n");
	return 0;
}
