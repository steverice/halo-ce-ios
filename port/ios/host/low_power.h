/* Low Power Mode's frame rate (host_gpu_dispatch.c, host_low_power.m), in plain C so that
   port/ios/tests/low_power_probe.c drives it with made-up notifications and displays.

In Low Power Mode the system lowers the CPU's and GPU's performance, and an iPhone's or iPad's ProMotion display
loses its faster refresh rates (60 Hz at most). While it is on, gameplay and cutscenes aim for half the frame rate
they aim for otherwise: 120 becomes 60 and 60 becomes 30, never under the game's 30 ticks a second and never over
what the display does now, so a cutscene's 30 stays 30. Menus keep the display's pace. The pacer, each frame's
deadline (display.frame_deadline) and dynamic resolution's budget read the halved rate through
host_frame_rate_target, so a ProMotion display held at 60 is not taken for missed 120 Hz frames. The rate is
latched between frames and goes back when Low Power Mode turns off. debug.low_power_frame_rate: "true" (default)
follows Low Power Mode, "false" ignores it, "always" acts as if it were on. Not on a Vision Pro, whose frames the
Compositor times. */
#ifndef HALO_LOW_POWER_H
#define HALO_LOW_POWER_H

#include <string.h>

#define LOW_POWER_FLOOR 30.0
/* a ProMotion display's highest rate in Low Power Mode */
#define LOW_POWER_DISPLAY_LIMIT 60.0

enum low_power_override
{
	LOW_POWER_FOLLOW, /* "true": follow Low Power Mode */
	LOW_POWER_IGNORE, /* "false" */
	LOW_POWER_ALWAYS  /* "always": as if it were on */
};

/* debug.low_power_frame_rate */
static inline enum low_power_override low_power_override_setting(const char *value)
{
	if (!strcmp(value, "false"))
		return LOW_POWER_IGNORE;
	if (!strcmp(value, "always"))
		return LOW_POWER_ALWAYS;
	return LOW_POWER_FOLLOW;
}

/* the display's highest rate while Low Power Mode is on (display: its highest otherwise, 0 unknown; limited: 1 where
   Low Power Mode turns faster rates off, an iPhone or iPad). 0 when unknown */
static inline double low_power_display_rate(double display, int limited)
{
	if (limited && (display <= 0.0 || display > LOW_POWER_DISPLAY_LIMIT))
		return LOW_POWER_DISPLAY_LIMIT;
	return display > 0.0 ? display : 0.0;
}

/* Low Power Mode's cap in frames a second, 0 for none of its own. configured: the rate frames aim for without it
   (display.frame_rate's cap together with a scene class's rate; 0 for the display's); display: the display's
   highest rate (0 unknown); limited: as for low_power_display_rate; menu: the frame is the main menu's */
static inline double low_power_cap(double configured, double display, int limited, int menu)
{
	double now = low_power_display_rate(display, limited), rate;

	if (menu)
		/* (the display's pace: a cap only where Low Power Mode lowered the display's highest rate) */
		return now > 0.0 && display > now ? now : 0.0;
	/* (half of what frames aim for otherwise, not of what Low Power Mode leaves the display: a ProMotion display's
	120 is 60, as is a cap over a Mac's display) */
	rate = configured > 0.0 ? configured : display > 0.0 ? display : 60.0;
	if (display > 0.0 && rate > display)
		rate = display;
	rate = (double)(long)(rate / 2.0 + 0.5);
	if (rate < LOW_POWER_FLOOR)
		rate = LOW_POWER_FLOOR;
	if (now > 0.0 && rate > now)
		rate = now;
	return rate;
}

/* what frames aim for: the configured rate, or Low Power Mode's cap where it is lower (0 for the display's rate) */
static inline double low_power_target(double configured, double cap)
{
	return cap > 0.0 && (configured <= 0.0 || cap < configured) ? cap : configured;
}

/* the state frames see, changed only between frames (low_power_latch) */
struct low_power
{
	enum low_power_override override;
	int on;     /* Low Power Mode in force for frames */
	double cap; /* low_power_cap's, 0 off or none */
};

enum
{
	LOW_POWER_UNCHANGED,
	LOW_POWER_CAP_CHANGED, /* still on, another cap (a scene class or the display changed) */
	LOW_POWER_TURNED_ON,
	LOW_POWER_TURNED_OFF
};

/* between frames: enabled is the system's Low Power Mode as last notified; the rest as for low_power_cap. Returns
   what changed */
static inline int low_power_latch(struct low_power *state, int enabled, double configured, double display, int limited,
	int menu)
{
	int on = state->override == LOW_POWER_ALWAYS || (state->override == LOW_POWER_FOLLOW && enabled);
	double cap = on ? low_power_cap(configured, display, limited, menu) : 0.0;
	int change = on && !state->on ? LOW_POWER_TURNED_ON : !on && state->on ? LOW_POWER_TURNED_OFF :
		cap != state->cap ? LOW_POWER_CAP_CHANGED : LOW_POWER_UNCHANGED;

	state->on = on;
	state->cap = cap;
	return change;
}

#endif
