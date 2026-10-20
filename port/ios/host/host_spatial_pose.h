/* Tracked-controller poses for controller aim (host_spatial.m): ARKit's room
into the game's axes, the head's yaw, a controller location in the room's
level frame, and the synthetic controller (debug.test_controller). Plain C,
so a host-side test can check it (port/ios/tests/spatial_pose_probe.c). */
#ifndef HOST_SPATIAL_POSE_H
#define HOST_SPATIAL_POSE_H

#include "halo_spatial.h"

/* ARKit's room vector (x right, y up, z back) in the game's axes (x forward,
y left, z up): (x, y, z) becomes (-z, -x, y) */
void host_spatial_room_to_game(const float room[3], float game[3]);

/* the head's forward yaw in the room's level frame (radians, left positive)
from the device anchor's back axis (ARKit), and *level: 1 under 85 degrees of
pitch */
float host_spatial_head_yaw(const float back[3], int *level);

/* a location from its 4x4 room transform (ARKit, column-major: x, y, z axes
then position) and the head's position (meters, room): forward_axis and
up_axis name the location's axes (HOST_SPATIAL_AXIS_*) that are the barrel
and its up */
enum { HOST_SPATIAL_AXIS_PLUS_X, HOST_SPATIAL_AXIS_MINUS_X, HOST_SPATIAL_AXIS_PLUS_Y,
	HOST_SPATIAL_AXIS_MINUS_Y, HOST_SPATIAL_AXIS_PLUS_Z, HOST_SPATIAL_AXIS_MINUS_Z };
void host_spatial_location(const float room_from_location[16], const float head_position[3],
	int forward_axis, int up_axis, struct halo_spatial_location *location);
/* During a system origin transition, keep the last drawing pose paired with
its head reference. Current controls, connection/held flags and actual pose
loss remain current; a missing reference cannot supply a tracked pose. */
void host_spatial_hold_poses(struct halo_spatial_sample *sample, const struct halo_spatial_sample *reference);
/* A controller placed wrongly against the head. The hand's position is the
accessory anchor's less the device anchor's, and twice in one headset
session (2026-10-12) the two came in different frames for a whole opening of
the space: the controller read 1.1 m higher than it was, 0.6 m above the head
with the hand in the lap, so the aim ray started above the eyes. The test: the
aiming hand tracked in position and held, with a head to measure from, more
than HOST_SPATIAL_MISPLACED_UP_METERS above it for
HOST_SPATIAL_MISPLACED_SECONDS without a break (an arm stretched overhead
reaches about that far, and doesn't stay). Once flagged it clears after
HOST_SPATIAL_MISPLACED_CLEAR_SECONDS under the height. Returns 1 as it flags,
-1 as it clears, 0 otherwise; state->flagged holds it between */
#define HOST_SPATIAL_MISPLACED_UP_METERS 0.45f
#define HOST_SPATIAL_MISPLACED_SECONDS 1.5
#define HOST_SPATIAL_MISPLACED_CLEAR_SECONDS 0.5
struct host_spatial_misplaced
{
	int flagged;
	int timing;        /* 1 while since counts toward the next change */
	double since;
	float up;          /* the height read at the last update, meters above the head */
};
int host_spatial_misplaced_update(struct host_spatial_misplaced *state, const struct halo_spatial_hand *hand,
	int has_head, double now);
/* an axis's name, "+x" to "-z" */
const char *host_spatial_axis_name(int axis);
/* debug.controller_aim_axes: "<forward>,<up>", each one of +x, -x, +y, -y,
+z and -z (either case), on different lines (forward and up square): 1, and
the two axes; 0 for anything else, leaving them alone */
int host_spatial_axes_parse(const char *value, int *forward_axis, int *up_axis);

/* debug.test_controller: "fixed:<yaw>,<pitch>" (degrees from the room's
forward, positive yaw right, positive pitch up), "sweep" (+-30 degrees of yaw
over 4 s), "loss:<seconds>" (fixed:0,0, untracked for that long every 10 s,
then orientation-only as long). Returns 0 for anything else, which the
caller logs (host_spatial.m) */
enum { HOST_SPATIAL_SYNTHETIC_FIXED = 1, HOST_SPATIAL_SYNTHETIC_SWEEP, HOST_SPATIAL_SYNTHETIC_LOSS };
struct host_spatial_synthetic { int kind; float yaw, pitch, loss_seconds; };
int host_spatial_synthetic_parse(const char *value, struct host_spatial_synthetic *synthetic);
/* the right hand at seconds since start: held, tracked (or the loss
schedule), the aim location at 0.25 m right, 0.30 m down, 0.35 m ahead of the
head, the grip 0.05 m behind it along the barrel, both level-rolled */
void host_spatial_synthetic_hand(const struct host_spatial_synthetic *synthetic, double seconds,
	struct halo_spatial_hand *hand);

#endif
