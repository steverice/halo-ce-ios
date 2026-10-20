/* The PlayStation VR2 Sense controllers' buttons onto the Xbox gamepad for
controller aim (host_spatial.m fills the game sample's gamepad with it). Plain
C, so a host-side test can check it (port/ios/tests/spatial_gamepad_probe.c).

The table (the spec's section 5), right-handed:
	R2 fire (right trigger)          L2 grenade (left trigger)
	R1 melee (B)                     L1 flashlight (white)
	Cross jump (A)                   Square switch weapon (Y)
	Circle action and reload (X)     Triangle switch grenades (black)
	right stick turn, click zoom     left stick move, click crouch
	(right thumb)                    (left thumb)
	Options start                    Create back
input.aim_hand = "left" mirrors it: every pair swaps sides. In menus Circle
is B (xinput_sdl.c's merge moves it, since only the guest knows a menu is up). */
#ifndef HOST_SPATIAL_GAMEPAD_H
#define HOST_SPATIAL_GAMEPAD_H

#include "halo_spatial.h"

/* one Sense controller's controls (the PS button is the system's) */
struct host_spatial_controls
{
	float trigger, grip;          /* 0 to 1 (R2/L2, R1/L1) */
	int face_low, face_high;      /* Cross/Square (low), Circle/Triangle (high) on each hand */
	int stick_click, menu;        /* stick press; Options (right) / Create (left) */
	float stick_x, stick_y;       /* -1 to 1, up positive */
};

/* the right and left controllers onto the Xbox gamepad, by the spec's
table; aiming_hand 0 (left) mirrors every pair */
void host_spatial_gamepad_map(const struct host_spatial_controls *left, const struct host_spatial_controls *right,
	int aiming_hand, struct halo_spatial_gamepad *gamepad);

/* the game's rumble on the two controllers. The game mixes everything that
shakes the pad (your own fire, damage, explosions, scripted shakes) into the
Xbox pad's two motors and doesn't say which is which, so the weapon's rumble
can't be told apart for the weapon's hand. The motors keep their sides and
their character instead: the left motor (the heavy, low-frequency one in the
pad's left grip) plays dull on the table's left controller, the right motor
(the light, high-frequency one in the right grip, the trigger's) plays sharp
on the aiming hand's; input.aim_hand = "left" mirrors them as it mirrors
the buttons. input.controller_rumble scales both (1 the game's own strength,
0 off, up to HOST_SPATIAL_RUMBLE_STRENGTH_MAXIMUM) */
#define HOST_SPATIAL_RUMBLE_LOW_SHARPNESS 0.15f
#define HOST_SPATIAL_RUMBLE_HIGH_SHARPNESS 0.75f
#define HOST_SPATIAL_RUMBLE_STRENGTH_MAXIMUM 2.0f
struct host_spatial_rumble
{
	/* by hand, 0 left and 1 right: Core Haptics' intensity (0 silent, to 1) and sharpness */
	float intensity[2];
	float sharpness[2];
};
/* the motors (0 to 1; anything else, a NaN too, is clamped) times strength
(clamped to 0 to the maximum; a NaN is 0) onto the hands */
void host_spatial_rumble_map(float left_motor, float right_motor, int aiming_hand, float strength,
	struct host_spatial_rumble *rumble);

/* when the game's rumble must stop on its own: the game sends the motors
once a frame (zeros while it is paused or in a menu), so levels older than
this are a stalled game (a load, a hang), and the controllers go quiet */
#define HOST_SPATIAL_RUMBLE_STALE_SECONDS 0.25
/* 1 while levels sent at sent (seconds) may still play at now */
int host_spatial_rumble_fresh(double sent, double now);

/* when the game sample reads both controllers' buttons, decided once per
sample for both: at the aiming hand's anchor timestamp (aligned) while that
anchor is fresh (0 to HOST_SPATIAL_ALIGNED_SECONDS old), else live (now).
After going live it returns to aligned only after
HOST_SPATIAL_FRESH_SAMPLES fresh samples in a row, and the read time never
steps back: an aligned read older than the last read is held at the last
read's time, so a press is never seen twice or lost to a backward step.
Zeroed, it starts live */
#define HOST_SPATIAL_ALIGNED_SECONDS 0.1
#define HOST_SPATIAL_FRESH_SAMPLES 3
struct host_spatial_button_clock
{
	int aligned;          /* 1 reading at the anchor's timestamp, 0 live */
	int fresh_samples;    /* fresh samples in a row while live */
	double last_read;     /* the last read's time (seconds, the anchors' clock) */
};
/* this sample's way and time (anchor_timestamp 0 with no anchor; now on
the anchors' clock): 1 and *read_time to read at, or 0 to read live
(*read_time is now, or the last read's time if that is later) */
int host_spatial_button_clock_choose(struct host_spatial_button_clock *clock, double anchor_timestamp, double now,
	double *read_time);
/* an aligned read gave no state: live from this sample on, the same rules
to come back */
void host_spatial_button_clock_failed(struct host_spatial_button_clock *clock, double now);

#endif
