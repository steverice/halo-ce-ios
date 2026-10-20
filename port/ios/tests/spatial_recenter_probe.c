/* A system origin change keeps the head's view still and rotates the hand's
room vectors into the same continuous frame. */
#include <assert.h>
#include <math.h>
#include <string.h>

#include "../host/host_stereo_head.c"

int main(void)
{
	struct host_stereo_head head = {0};
	struct halo_stereo_frame frame = {0};
	const float up[3] = {0.0f, 1.0f, 0.0f};
	const float right_zero[3] = {1.0f, 0.0f, 0.0f};
	const float back_zero[3] = {0.0f, 0.0f, 1.0f};
	const float right_shifted[3] = {0.0f, 0.0f, -1.0f};
	const float back_shifted[3] = {1.0f, 0.0f, 0.0f};
	float hand_forward[3] = {0.0f, 1.0f, 0.0f};

	host_stereo_head_turn(&head, right_zero, up, back_zero, &frame);
	host_stereo_head_recenter(&head);
	frame.head_yaw = 0.0f;
	host_stereo_head_turn(&head, right_shifted, up, back_shifted, &frame);
	assert(fabsf(frame.head_yaw) < 0.0001f);
	assert(fabsf(head.room_yaw_offset + (float)M_PI / 2.0f) < 0.0001f);
	host_stereo_head_room_vector(&head, hand_forward);
	assert(fabsf(hand_forward[0] - 1.0f) < 0.0001f);
	assert(fabsf(hand_forward[1]) < 0.0001f);
	return 0;
}
