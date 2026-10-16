/* Replay camera diagnostics. Angles are the action's Euler radians, before the
   game's vehicle/seat conversion; a missing action never supplies a pose. */
#ifndef HALO_INPUT_REPLAY_H
#define HALO_INPUT_REPLAY_H

struct input_replay_camera_sample
{
	long segment, tick, previous_tick;
	float facing[2], previous_facing[2];
	short zoom;
	unsigned char valid, previous_valid;
	double timeline;
};

unsigned char input_replay_camera_sample(struct input_replay_camera_sample *sample);

/* Apply a newly injected action once, before the camera reads player control. */
unsigned char input_replay_apply_control(short local_player_index, float facing[2], short *zoom);
unsigned char input_replay_controls_facing(short local_player_index);
unsigned char input_replay_action_camera_active(void);

#endif
