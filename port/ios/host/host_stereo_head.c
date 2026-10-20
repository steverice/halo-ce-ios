/* The head's angles for head-tracked stereo (host_stereo_head.h). */
#include "host_stereo_head.h"
#include <math.h>

/* past this pitch the head's yaw comes from its right: 85 degrees */
#define HEAD_YAW_PITCH_LIMIT (85.0f * (float)M_PI / 180.0f)

/* angle wrapped into -pi..pi */
static float wrapped(float angle)
{
	while (angle > (float)M_PI)
		angle -= 2.0f * (float)M_PI;
	while (angle < -(float)M_PI)
		angle += 2.0f * (float)M_PI;
	return angle;
}

void host_stereo_head_recenter(struct host_stereo_head *head)
{
	head->recenter = 1;
}

void host_stereo_head_room_vector(const struct host_stereo_head *head, float vector[3])
{
	float x = vector[0], y = vector[1];
	float c = cosf(head->room_yaw_offset), s = sinf(head->room_yaw_offset);
	vector[0] = c * x - s * y;
	vector[1] = s * x + c * y;
}

void host_stereo_head_turn(struct host_stereo_head *head, const float right[3], const float up[3],
	const float back[3], struct halo_stereo_frame *frame)
{
	float forward[3] = { -back[0], -back[1], -back[2] };
	/* yaw about the room's up, left positive, as the game's yaw; pitch up
	positive; roll left ear down positive */
	float yaw = atan2f(-forward[0], -forward[2]);
	float pitch = asinf(fmaxf(-1.0f, fminf(1.0f, forward[1])));
	float roll = atan2f(right[1], up[1]);

	/* near straight up or down forward's horizontal part vanishes and its yaw
	can swing by pi in a frame; the head's right stays level there, and a turn
	about the room's up turns it by the same angle at any pitch or roll. Its
	own yaw is off from forward's by about the roll, so a frame's turn takes
	both ends from one source: right's whenever this frame or the last is
	past the limit, else forward's. Crossing the limit with the head tilted
	then doesn't step the view by the tilt */
	float yaw_from_right = atan2f(-right[2], right[0]);
	yaw = wrapped(yaw + head->room_yaw_offset);
	yaw_from_right = wrapped(yaw_from_right + head->room_yaw_offset);
	if (head->recenter)
	{
		if (head->room_known)
		{
			float delta = fabsf(pitch) >= HEAD_YAW_PITCH_LIMIT || fabsf(head->pitch) >= HEAD_YAW_PITCH_LIMIT ?
				wrapped(head->yaw_from_right - yaw_from_right) : wrapped(head->yaw - yaw);
			head->room_yaw_offset = wrapped(head->room_yaw_offset + delta);
			yaw = wrapped(yaw + delta);
			yaw_from_right = wrapped(yaw_from_right + delta);
		}
		head->recenter = 0;
		frame->head_yaw = 0.0f;
	}

	/* the yaw is a turn since the last frame (the body's yaw is the game's,
	which the stick turns too); the pitch and roll are the head's own, so the
	camera's up stays the room's: a pitch kept as turns drifts from the
	head's (the game starts level, levels itself, clamps), and the yaw about
	the room's up then rolls the world as the head pans */
	if (head->known)
	{
		if (fabsf(pitch) >= HEAD_YAW_PITCH_LIMIT || fabsf(head->pitch) >= HEAD_YAW_PITCH_LIMIT)
			frame->head_yaw = wrapped(yaw_from_right - head->yaw_from_right);
		else
			frame->head_yaw = wrapped(yaw - head->yaw);
	}
	frame->head_pitch = pitch;
	frame->head_roll = roll;
	head->yaw = yaw;
	head->yaw_from_right = yaw_from_right;
	head->pitch = pitch;
	head->roll = roll;
	head->known = 1;
	head->room_known = 1;
}

void host_stereo_head_hold(const struct host_stereo_head *head, struct halo_stereo_frame *frame)
{
	frame->head_pitch = head->known ? head->pitch : 0.0f;
	frame->head_roll = head->known ? head->roll : 0.0f;
}
