/* The head's angles for head-tracked stereo (host_stereo.m), from the
device's pose in the room. Plain C, so a host-side test can check it
(port/ios/tests/stereo_head_probe.c). */
#ifndef HOST_STEREO_HEAD_H
#define HOST_STEREO_HEAD_H

#include "halo_stereo.h"

struct host_stereo_head
{
	int known;                 /* the head's pose has been read once */
	int room_known;            /* a reference survives canceled and mono frames */
	int recenter;              /* rebase at the next tracked pose */
	float room_yaw_offset;     /* ARKit origin yaw into the continuous room frame */
	float yaw, pitch, roll;    /* its last angles, radians */
	float yaw_from_right;      /* its last yaw read from its right (host_stereo_head.c) */
};

/* fills the frame's head_yaw, head_pitch and head_roll (halo_stereo.h) from
the device's axes in the room (ARKit's: x right, y up, z back) */
void host_stereo_head_turn(struct host_stereo_head *head, const float right[3], const float up[3],
	const float back[3], struct halo_stereo_frame *frame);
/* a frame without the head's pose: it holds its last pitch and roll, with no turn */
void host_stereo_head_hold(const struct host_stereo_head *head, struct halo_stereo_frame *frame);
void host_stereo_head_recenter(struct host_stereo_head *head);
void host_stereo_head_room_vector(const struct host_stereo_head *head, float vector[3]);

#endif
