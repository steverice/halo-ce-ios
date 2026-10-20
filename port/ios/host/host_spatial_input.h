/* The Sense controllers' buttons and haptics for controller aim (visionOS;
host_spatial.m's tracker calls these on the game thread):
host_spatial_input.m reads a controller's input state into
host_spatial_controls (host_spatial_gamepad.h maps them onto the Xbox
gamepad), and plays the tracking-loss buzz and the game's rumble through one
CHHapticEngine per controller. */
#ifndef HOST_SPATIAL_INPUT_H
#define HOST_SPATIAL_INPUT_H

#include <TargetConditionals.h>
#if TARGET_OS_VISION
#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>
#include "host_spatial_gamepad.h"

/* one controller's controls from an input state (an anchor-aligned one, or a
live capture): the trigger, the grip, the two face buttons, the stick and its
click, and the menu button. The PS button is the system's. A control the
controller doesn't have reads as released */
void host_spatial_controls_read(id<GCDevicePhysicalInputState> state, struct host_spatial_controls *controls);

/* logs once, as a controller loads, the input elements GameController lists
for it, so the names read above can be confirmed from the log */
void host_spatial_controls_log_names(GCController *controller, const char *hand);

/* the controller's haptic engine (GCHapticsLocalityDefault), started; nil
(logged) without haptics */
CHHapticEngine *host_spatial_haptics_start(GCController *controller, const char *hand);

/* the tracking-loss buzz on that engine: one short pulse (intensity 0.8,
sharpness 0.5, 80 ms) */
void host_spatial_haptics_buzz(CHHapticEngine *engine, const char *hand);

/* The game's rumble: one continuous haptic per hand (0 left, 1 right) on
that hand's engine, its intensity following the game's motors
(host_spatial_gamepad.h's host_spatial_rumble). These take a lock of their
own, so the game thread, the main queue (the app's lifecycle) and the
engines' handlers may all call them. */
/* a hand's engine as its controller loads, or nil as it disconnects (its
rumble stops) */
void host_spatial_rumble_attach(int hand, CHHapticEngine *engine);
/* the levels the game sent this frame: a hand at intensity 0, or without an
engine, is stopped. Levels that aren't followed within
HOST_SPATIAL_RUMBLE_STALE_SECONDS stop by themselves */
void host_spatial_rumble_play(const struct host_spatial_rumble *rumble);
/* 1: stop both hands and ignore levels (the app in the background, or
quitting) until 0 */
void host_spatial_rumble_quiet(int quiet);
#endif

#endif
