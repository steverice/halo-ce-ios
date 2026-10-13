/* The frame pacer's planner: what the pacer decides, with no clock, no
threads and no graphics, so a probe can drive it with made-up times
(port/ios/tests/frame_pacer_probe.c). host_frame_pacer.c is the pacer
itself: it feeds these the display's refreshes, the settings and the times
frames were shown, and does the waiting.

Three separate things, after innerhat-dev/OpenCE's native Metal host
(test-v0.3.0-net28-mac13, port/macos/host/host_metal.mm: present_acquire and
present_schedule, present_drawable, host_metal_pace_vblank; CC0):

Frames in flight. A frame is in flight from the moment its Present asks for
somewhere to show it until the display shows it. A renderer left to itself
queues finished frames behind the one on screen (a CAMetalLayer has three
drawables, so two deep): each queued frame is a refresh more between the
game drawing something and the player seeing it, and when the queue runs dry
the display repeats a frame. With a limit, a frame waits for one of that many
slots, given back as frames are shown. A limit of 1 (innerhat's) starts
nothing toward the display until the frame before has been shown: the least
delay, at the cost of frames whenever a frame's CPU and GPU work together
take longer than a refresh, since the CPU may not prepare a frame while the
GPU finishes the last. 2 allows exactly that overlap and no more.

A minimum time on screen. A frame is presented to stay up until at least
half a refresh short of its share of the display's time, so one that
finishes early can't replace the frame before it too soon: its share is one
refresh, or with a cap under the display's rate, the cap's (two refreshes
for 60 on a 120 Hz display). Only for frames that wait for the display:
drawing faster than the display, it would itself be a cap.

The game's 60 Hz vertical blank following the display. A timer of its own
drifts against the refresh, and the frames the blank paces (interpolation
off) then bunch, two in one refresh and none in the next. A blank is counted
on each refresh that comes a sixtieth of a second or more after the last one
counted: every refresh at 60 Hz, every other at 120. */
#pragma once

#include <string.h>

/* ---------- frames in flight */

enum { FRAME_PACER_IN_FLIGHT_MAXIMUM = 3 };

/* display.frames_in_flight: "1" to "3"; "system" is 0, no limit of the
pacer's (the renderer's own queue); anything else, fallback */
static inline int frame_pacer_in_flight_setting(const char *value, int fallback)
{
	if (!strcmp(value, "system"))
		return 0;
	if (value[0] >= '1' && value[0] <= '0' + FRAME_PACER_IN_FLIGHT_MAXIMUM && !value[1])
		return value[0] - '0';
	return fallback;
}

struct frame_pacer_flight
{
	int limit; /* 0: none */
	int count; /* frames holding a slot */
};

/* whether a frame may start toward the display now: 1 and it holds a slot,
or 0 and it must wait for frame_pacer_flight_give. Without a limit, 1 and no
slot is held (see frame_pacer_flight_holds). */
static inline int frame_pacer_flight_take(struct frame_pacer_flight *flight)
{
	if (!flight->limit)
		return 1;
	if (flight->count >= flight->limit)
		return 0;
	flight->count++;
	return 1;
}

/* whether a frame that took (frame_pacer_flight_take returned 1) holds a
slot it must give back */
static inline int frame_pacer_flight_holds(const struct frame_pacer_flight *flight)
{
	return flight->limit != 0;
}

/* a frame holding a slot was shown, or had nothing to show */
static inline void frame_pacer_flight_give(struct frame_pacer_flight *flight)
{
	if (flight->count > 0)
		flight->count--;
}

/* ---------- a minimum time on screen */

/* how many refreshes a frame's share of the display is: one, or under a
cap (frames a second; 0 none) below the display's rate, the cap's */
static inline double frame_pacer_share(double period, double cap)
{
	double refreshes = 1.0;

	if (cap > 0.0 && period > 0.0)
	{
		refreshes = (double)(long)(1.0 / (cap * period) + 0.5);
		if (refreshes < 1.0)
			refreshes = 1.0;
	}
	return refreshes;
}

/* how long a frame must stay up after the one before it, in seconds; 0: no
minimum (spacing is off, the frame doesn't wait for the display, or the
refresh period, in seconds, isn't a display's) */
static inline double frame_pacer_minimum_duration(int spacing, int waits_for_display, double period, double cap)
{
	if (!spacing || !waits_for_display || period < 1.0 / 240.0 || period > 1.0 / 20.0)
		return 0.0;
	return (frame_pacer_share(period, cap) - 0.5) * period;
}

/* ---------- the 60 Hz vertical blank */

struct frame_pacer_blank
{
	double next;         /* the earliest refresh that counts as the next blank; 0: none seen yet */
	unsigned long ticks; /* blanks counted */
};

#define FRAME_PACER_BLANK_PERIOD (1.0 / 60.0)

/* a refresh of the display at this time (seconds, any steady clock):
whether it counts as a blank */
static inline int frame_pacer_blank_refresh(struct frame_pacer_blank *blank, double refresh)
{
	if (blank->next == 0.0)
		blank->next = refresh;
	/* (a millisecond's grace: a 60 Hz display's refreshes come a hair early
	or late) */
	if (refresh + 0.001 < blank->next)
		return 0;
	blank->ticks++;
	blank->next += FRAME_PACER_BLANK_PERIOD;
	/* the display stopped for a while, or is slower than 60 Hz: start again
	from this refresh instead of counting the missed ones */
	if (refresh > blank->next)
		blank->next = refresh + FRAME_PACER_BLANK_PERIOD;
	return 1;
}

/* ---------- when frames were shown */

struct frame_pacer_shown
{
	double last;         /* when the frame before was shown; 0: none yet */
	unsigned long frames, intervals, late;
	double longest;      /* seconds */
};

/* a frame was shown at this time (seconds; 0 or less: it never was, and
nothing is counted). Returns the time since the frame before it, or 0 for the
first. An interval over one and a half times the frame's share of the
display (refreshes) is late: a frame was repeated or dropped. A quarter of a
second or more is a load or the background, not a frame, and starts again. */
static inline double frame_pacer_shown_add(struct frame_pacer_shown *shown, double at, double period, double refreshes)
{
	double interval = 0.0;

	if (at <= 0.0)
		return 0.0;
	shown->frames++;
	if (shown->last > 0.0 && at > shown->last)
	{
		interval = at - shown->last;
		if (interval < 0.25)
		{
			shown->intervals++;
			if (period > 0.0 && interval > 1.5 * refreshes * period)
				shown->late++;
			if (interval > shown->longest)
				shown->longest = interval;
		}
	}
	shown->last = at;
	return interval;
}
