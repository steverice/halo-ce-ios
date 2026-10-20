/* Tracked-controller poses (host_spatial_pose.h). */
#include "host_spatial_pose.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* at or past this pitch the head isn't level: 85 degrees, as
host_stereo_head.c's HEAD_YAW_PITCH_LIMIT */
#define HEAD_LEVEL_PITCH_LIMIT (85.0f * (float)M_PI / 180.0f)

/* the synthetic hand (debug.test_controller), in the room's level frame
(the game's axes, meters from the head): the aim location 0.35 m ahead,
0.25 m right and 0.30 m down, the grip 0.05 m behind it along the barrel */
static const float SYNTHETIC_AIM_POSITION[3] = { 0.35f, -0.25f, -0.30f };
#define SYNTHETIC_GRIP_BEHIND 0.05f
/* "sweep": +-30 degrees of yaw, a sine over 4 s */
#define SYNTHETIC_SWEEP_DEGREES 30.0f
#define SYNTHETIC_SWEEP_SECONDS 4.0
/* "loss:<seconds>": the schedule repeats every 10 s, untracked then
orientation-only for that long each, so at most 5 s */
#define SYNTHETIC_LOSS_PERIOD 10.0
#define SYNTHETIC_LOSS_MAXIMUM 5.0f

#define RADIANS(degrees) ((degrees) * (float)M_PI / 180.0f)

void host_spatial_room_to_game(const float room[3], float game[3])
{
	float x = room[0], y = room[1], z = room[2];

	game[0] = -z;
	game[1] = -x;
	game[2] = y;
}

float host_spatial_head_yaw(const float back[3], int *level)
{
	float forward[3] = { -back[0], -back[1], -back[2] };
	float pitch = asinf(fmaxf(-1.0f, fminf(1.0f, forward[1])));

	/* yaw about the room's up, left positive (host_stereo_head.c's) */
	*level = fabsf(pitch) < HEAD_LEVEL_PITCH_LIMIT;
	return atan2f(-forward[0], -forward[2]);
}

static void normalize(float v[3])
{
	float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (length > 0.0f)
	{
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}
}

static void cross(const float a[3], const float b[3], float out[3])
{
	float x = a[1] * b[2] - a[2] * b[1];
	float y = a[2] * b[0] - a[0] * b[2];
	float z = a[0] * b[1] - a[1] * b[0];

	out[0] = x;
	out[1] = y;
	out[2] = z;
}

/* a transform's axis (HOST_SPATIAL_AXIS_*) in the room */
static void transform_axis(const float transform[16], int axis, float out[3])
{
	const float *column = transform + 4 * (axis / 2);
	float sign = axis % 2 ? -1.0f : 1.0f;

	out[0] = sign * column[0];
	out[1] = sign * column[1];
	out[2] = sign * column[2];
}

/* the location's left and up from its forward and an up near it: unit and
square to each other (the game's axes are right-handed: forward x left = up) */
static void location_axes(struct halo_spatial_location *location, const float up[3])
{
	normalize(location->forward);
	cross(up, location->forward, location->left);
	normalize(location->left);
	cross(location->forward, location->left, location->up);
}

void host_spatial_location(const float room_from_location[16], const float head_position[3],
	int forward_axis, int up_axis, struct halo_spatial_location *location)
{
	float room[3], up[3];

	transform_axis(room_from_location, forward_axis, room);
	host_spatial_room_to_game(room, location->forward);
	transform_axis(room_from_location, up_axis, room);
	host_spatial_room_to_game(room, up);
	location_axes(location, up);
	room[0] = room_from_location[12] - head_position[0];
	room[1] = room_from_location[13] - head_position[1];
	room[2] = room_from_location[14] - head_position[2];
	host_spatial_room_to_game(room, location->position);
}

int host_spatial_misplaced_update(struct host_spatial_misplaced *state, const struct halo_spatial_hand *hand,
	int has_head, double now)
{
	int placed = hand->state == HALO_SPATIAL_TRACKED || hand->state == HALO_SPATIAL_LOW_ACCURACY;
	int high = placed && hand->held && has_head && hand->aim.position[2] > HOST_SPATIAL_MISPLACED_UP_METERS;
	/* what moves the state on: high while not flagged; while flagged, a
	position that can be judged and is under the height (a lost hand or a
	missing head says nothing either way) */
	int toward = state->flagged ? placed && has_head && !high : high;

	state->up = placed ? hand->aim.position[2] : 0.0f;
	if (!toward)
	{
		state->timing = 0;
		return 0;
	}
	if (!state->timing)
	{
		state->timing = 1;
		state->since = now;
	}
	if (now - state->since < (state->flagged ? HOST_SPATIAL_MISPLACED_CLEAR_SECONDS : HOST_SPATIAL_MISPLACED_SECONDS))
		return 0;
	state->flagged = !state->flagged;
	state->timing = 0;
	return state->flagged ? 1 : -1;
}

static const char *const AXIS_NAMES[] = { "+x", "-x", "+y", "-y", "+z", "-z" };

const char *host_spatial_axis_name(int axis)
{
	return axis >= 0 && axis < 6 ? AXIS_NAMES[axis] : "?";
}

/* "+x" to "-z" at text (two characters, either case): the axis, or -1 */
static int axis_parse(const char *text)
{
	int line;

	if ((text[0] != '+' && text[0] != '-') || !text[1])
		return -1;
	switch (text[1])
	{
	case 'x': case 'X': line = 0; break;
	case 'y': case 'Y': line = 1; break;
	case 'z': case 'Z': line = 2; break;
	default: return -1;
	}
	return 2 * line + (text[0] == '-');
}

int host_spatial_axes_parse(const char *value, int *forward_axis, int *up_axis)
{
	int forward, up;

	if (!value || strlen(value) != 5 || value[2] != ',')
		return 0;
	forward = axis_parse(value);
	up = axis_parse(value + 3);
	/* (an axis's line is axis / 2: forward and up on one line can't be square) */
	if (forward < 0 || up < 0 || forward / 2 == up / 2)
		return 0;
	*forward_axis = forward;
	*up_axis = up;
	return 1;
}

/* the whole of value is prefix then one number (b NULL) or two, "a,b":
1 when it is */
static int parse_numbers(const char *value, const char *prefix, float *a, float *b)
{
	size_t length = strlen(prefix);
	int end = -1;

	if (strncmp(value, prefix, length))
		return 0;
	value += length;
	if (b)
		return sscanf(value, "%f,%f%n", a, b, &end) == 2 && end >= 0 && value[end] == '\0';
	return sscanf(value, "%f%n", a, &end) == 1 && end >= 0 && value[end] == '\0';
}

int host_spatial_synthetic_parse(const char *value, struct host_spatial_synthetic *synthetic)
{
	float yaw, pitch, seconds;

	memset(synthetic, 0, sizeof(*synthetic));
	if (!value)
		return 0;
	if (parse_numbers(value, "fixed:", &yaw, &pitch))
	{
		/* (a pitch of 90 degrees has no level roll) */
		if (!isfinite(yaw) || !isfinite(pitch) || fabsf(yaw) > 180.0f || fabsf(pitch) >= 90.0f)
			return 0;
		synthetic->kind = HOST_SPATIAL_SYNTHETIC_FIXED;
		synthetic->yaw = yaw;
		synthetic->pitch = pitch;
		return 1;
	}
	if (!strcmp(value, "sweep"))
	{
		synthetic->kind = HOST_SPATIAL_SYNTHETIC_SWEEP;
		return 1;
	}
	if (parse_numbers(value, "loss:", &seconds, NULL))
	{
		if (!(seconds > 0.0f && seconds <= SYNTHETIC_LOSS_MAXIMUM))
			return 0;
		synthetic->kind = HOST_SPATIAL_SYNTHETIC_LOSS;
		synthetic->loss_seconds = seconds;
		return 1;
	}
	return 0;
}

void host_spatial_synthetic_hand(const struct host_spatial_synthetic *synthetic, double seconds,
	struct halo_spatial_hand *hand)
{
	/* degrees from the room's forward, yaw right positive */
	float yaw_right = 0.0f, pitch = 0.0f, yaw;
	const float room_up[3] = { 0.0f, 0.0f, 1.0f };
	int axis;

	memset(hand, 0, sizeof(*hand));
	hand->held = 1;
	hand->chirality = 2;
	hand->state = HALO_SPATIAL_TRACKED;
	switch (synthetic->kind)
	{
	case HOST_SPATIAL_SYNTHETIC_FIXED:
		yaw_right = synthetic->yaw;
		pitch = synthetic->pitch;
		break;
	case HOST_SPATIAL_SYNTHETIC_SWEEP:
		yaw_right = SYNTHETIC_SWEEP_DEGREES * (float)sin(2.0 * M_PI * fmod(seconds, SYNTHETIC_SWEEP_SECONDS) /
			SYNTHETIC_SWEEP_SECONDS);
		break;
	case HOST_SPATIAL_SYNTHETIC_LOSS:
	{
		double phase = fmod(seconds, SYNTHETIC_LOSS_PERIOD);

		if (phase < synthetic->loss_seconds)
			hand->state = HALO_SPATIAL_UNTRACKED;
		else if (phase < 2.0 * synthetic->loss_seconds)
			hand->state = HALO_SPATIAL_ORIENTATION;
		break;
	}
	}
	/* the game's yaw is left positive */
	yaw = -RADIANS(yaw_right);
	pitch = RADIANS(pitch);
	hand->aim.forward[0] = cosf(pitch) * cosf(yaw);
	hand->aim.forward[1] = cosf(pitch) * sinf(yaw);
	hand->aim.forward[2] = sinf(pitch);
	/* level-rolled: its left is level */
	location_axes(&hand->aim, room_up);
	for (axis = 0; axis < 3; axis++)
	{
		hand->aim.position[axis] = SYNTHETIC_AIM_POSITION[axis];
		hand->grip.forward[axis] = hand->aim.forward[axis];
		hand->grip.left[axis] = hand->aim.left[axis];
		hand->grip.up[axis] = hand->aim.up[axis];
		hand->grip.position[axis] = hand->aim.position[axis] - SYNTHETIC_GRIP_BEHIND * hand->aim.forward[axis];
	}
}

void host_spatial_hold_poses(struct halo_spatial_sample *sample, const struct halo_spatial_sample *reference)
{
	int hand;

	sample->head_yaw = reference->head_yaw;
	sample->head_level = reference->head_level;
	for (hand = 0; hand < 2; hand++)
	{
		struct halo_spatial_hand *live = &sample->hands[hand];
		const struct halo_spatial_hand *held = &reference->hands[hand];

		if (live->state < HALO_SPATIAL_ORIENTATION)
			continue;
		if (held->state < HALO_SPATIAL_ORIENTATION)
		{
			live->state = HALO_SPATIAL_UNTRACKED;
			continue;
		}
		live->aim = held->aim;
		live->grip = held->grip;
		/* Orientation-only references must not turn into full positions. */
		if (held->state == HALO_SPATIAL_ORIENTATION)
			live->state = HALO_SPATIAL_ORIENTATION;
	}
}
