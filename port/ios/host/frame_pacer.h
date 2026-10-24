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

/* When every slot is taken, a frame waits for one. On a Mac the window
server can hold the frame on screen for a tenth of a second at a time (a
trace of a network game showed one about every 5 seconds, its frames held
96 ms with the game's CPU and GPU idle), and waiting the whole hold stops
the game loop with it: input, the game's ticks and the network's messages.
There a frame waits about its share of the display (one refresh, or two
under a 60 Hz cap on a 120 Hz display) and then gives up its present, so
the loop keeps going and the window keeps the frame it has. Elsewhere, and
once frames have been giving up for limit seconds (a presented notice that
never comes must not keep every frame off the screen), a frame waits up to
limit and then goes ahead without a slot, as before.

drops: whether frames may give up (the Mac); period: the refresh period in
seconds (0 unknown); share: the frame's share of the display in refreshes
(frame_pacer_share); backlog: how long frames have been finding every slot
taken, in seconds (0: this is the first); limit: the longest wait. Returns
the most this frame may wait, in seconds. */
static inline double frame_pacer_slot_wait(int drops, double period, double share, double backlog, double limit)
{
	double turn;

	if (!drops || backlog >= limit)
		return limit;
	turn = (period >= 1.0 / 240.0 && period <= 1.0 / 20.0 ? period : 1.0 / 60.0) * (share >= 1.0 ? share : 1.0);
	return turn < limit - backlog ? turn : limit - backlog;
}

/* a frame's wait for a slot ran out (backlog now counts its wait too):
1 to give up its present, 0 to go ahead without a slot */
static inline int frame_pacer_slot_give_up(int drops, double backlog, double limit)
{
	return drops && backlog < limit;
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

/* the same where the panel refreshes faster than the display link that gives period (an iPhone, iPad or Apple TV
held under its screen's rate by display.frame_rate: the link runs at the cap, a ProMotion panel at up to 120 Hz):
half a panel refresh short of the frame's share, not half a link period, or a frame held for one 60 Hz period
shows after 8.3 ms on a 120 Hz panel. panel is the panel's refresh period, 0 for unknown */
static inline double frame_pacer_minimum_duration_panel(int spacing, int waits_for_display, double period, double cap,
	double panel)
{
	double duration = frame_pacer_minimum_duration(spacing, waits_for_display, period, cap);

	if (duration > 0.0 && panel > 0.0 && panel < period)
		duration += 0.5 * (period - panel);
	return duration;
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

/* ---------- a late start

The delay a player feels is from the moment the game reads the controller to
the moment the picture made from it is on screen. Left alone, the game starts
a frame's work as soon as the last one is handed over, finishes early, and the
finished picture then waits for the display: the wait is at the wrong end.
Starting late moves it to the front: the frame's work begins just early
enough to be done for the next refresh, so the controller is read as late as
it can be. (Nvidia's Reflex and the Vision Pro's Compositor do the same.)

Just early enough is a guess, and a wrong one costs a whole refresh, which is
worse than the wait it saves. So the guess is cautious:

- a frame's cost is the worst of the last FRAME_PACER_LATE_WINDOW frames'
  (its work from the start to the GPU's end), not their average;
- on top of it goes a lead, the time the system needs between a finished
  frame and the refresh that shows it, learned: it starts at
  FRAME_PACER_LATE_LEAD, grows by FRAME_PACER_LATE_LEAD_STEP each time a
  frame is shown later than it aimed for, and shrinks slowly while none is;
- if cost and lead together take more than FRAME_PACER_LATE_FIT of a
  refresh, there is no late start: the frame starts at once, as before;
- a frame that costs more than was allowed for (a fight begins), or is shown
  late, stops late starts for FRAME_PACER_LATE_HOLDOFF frames, until the
  window holds only the new costs. */

enum
{
	FRAME_PACER_LATE_WINDOW = 32,
	FRAME_PACER_LATE_HISTORY = 8,     /* frames seen before the first late start */
	FRAME_PACER_LATE_HOLDOFF = 120
};

#define FRAME_PACER_LATE_LEAD 0.003
#define FRAME_PACER_LATE_LEAD_STEP 0.002
#define FRAME_PACER_LATE_LEAD_MINIMUM 0.002
#define FRAME_PACER_LATE_FIT 0.75
/* frames shown on time before the lead shrinks by a quarter of a step */
#define FRAME_PACER_LATE_CALM 600

struct frame_pacer_late
{
	double costs[FRAME_PACER_LATE_WINDOW];
	int count, next;
	double lead;            /* 0: FRAME_PACER_LATE_LEAD */
	double allowed;         /* the cost the last late start allowed for; 0: it started at once */
	int holdoff;            /* frames still to start at once */
	unsigned long calm;     /* frames shown on time since the lead last changed */
	unsigned long late_starts, at_once, jumps, misses;
};

static inline double frame_pacer_late_worst(const struct frame_pacer_late *late)
{
	double worst = 0.0;
	int index;

	for (index = 0; index < late->count; index++)
		if (late->costs[index] > worst)
			worst = late->costs[index];
	return worst;
}

static inline double frame_pacer_late_lead(const struct frame_pacer_late *late)
{
	return late->lead > 0.0 ? late->lead : FRAME_PACER_LATE_LEAD;
}

/* How long to wait before starting a frame's work, in seconds (0: start at
once), and through aim, the refresh the frame then aims to be shown at (0:
none). now: the clock; refresh: the time of any one refresh of the display
on that clock, and period the time between them, both from the display link. */
static inline double frame_pacer_late_wait(struct frame_pacer_late *late, double now, double refresh, double period,
	double *aim)
{
	double budget, start;

	*aim = 0.0;
	late->allowed = 0.0;
	if (late->holdoff > 0)
	{
		late->holdoff--;
		late->at_once++;
		return 0.0;
	}
	if (late->count < FRAME_PACER_LATE_HISTORY || period < 1.0 / 240.0 || period > 1.0 / 20.0)
	{
		late->at_once++;
		return 0.0;
	}
	budget = frame_pacer_late_worst(late) + frame_pacer_late_lead(late);
	if (budget > FRAME_PACER_LATE_FIT * period)
	{
		late->at_once++;
		return 0.0;
	}
	/* the first refresh the frame can still make if it starts now */
	*aim = refresh + period * (double)(long)((now + budget - refresh) / period + 1.0);
	start = *aim - budget;
	late->allowed = budget - frame_pacer_late_lead(late);
	if (start <= now)
	{
		late->at_once++;
		return 0.0;
	}
	late->late_starts++;
	return start - now;
}

/* a frame's work took this long, from its start to the GPU's end */
static inline void frame_pacer_late_cost(struct frame_pacer_late *late, double cost)
{
	/* (a quarter second or more is a load or the background, not a frame) */
	if (cost <= 0.0 || cost >= 0.25)
		return;
	late->costs[late->next] = cost;
	late->next = (late->next + 1) % FRAME_PACER_LATE_WINDOW;
	if (late->count < FRAME_PACER_LATE_WINDOW)
		late->count++;
	if (late->allowed > 0.0 && cost > late->allowed)
	{
		late->jumps++;
		late->holdoff = FRAME_PACER_LATE_HOLDOFF;
	}
}

/* a frame that aimed for a refresh was shown at this time */
static inline void frame_pacer_late_shown(struct frame_pacer_late *late, double aim, double at, double period)
{
	if (aim <= 0.0 || at <= 0.0)
		return;
	if (at > aim + 0.5 * period)
	{
		late->misses++;
		late->holdoff = FRAME_PACER_LATE_HOLDOFF;
		late->lead = frame_pacer_late_lead(late) + FRAME_PACER_LATE_LEAD_STEP;
		if (late->lead > 0.5 * period)
			late->lead = 0.5 * period;
		late->calm = 0;
	}
	else if (++late->calm >= FRAME_PACER_LATE_CALM)
	{
		late->calm = 0;
		late->lead = frame_pacer_late_lead(late) - FRAME_PACER_LATE_LEAD_STEP / 4.0;
		if (late->lead < FRAME_PACER_LATE_LEAD_MINIMUM)
			late->lead = FRAME_PACER_LATE_LEAD_MINIMUM;
	}
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
