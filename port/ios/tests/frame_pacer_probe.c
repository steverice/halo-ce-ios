/* The frame pacer's rules (port/ios/host/frame_pacer.h), driven with made-up
refreshes and presented times: no clock, no threads, no graphics.
tools/ios_test.py builds and runs it. */
#include "frame_pacer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(condition) \
	do \
	{ \
		if (!(condition)) \
		{ \
			printf("frame_pacer_probe: line %d: %s\n", __LINE__, #condition); \
			failures++; \
		} \
	} while (0)

static int near(double a, double b)
{
	return fabs(a - b) < 1e-9;
}

int main(void)
{
	/* the setting */
	CHECK(frame_pacer_in_flight_setting("", 2) == 2);
	CHECK(frame_pacer_in_flight_setting("1", 2) == 1);
	CHECK(frame_pacer_in_flight_setting("3", 2) == 3);
	CHECK(frame_pacer_in_flight_setting("4", 2) == 2);
	CHECK(frame_pacer_in_flight_setting("12", 2) == 2);
	CHECK(frame_pacer_in_flight_setting("system", 2) == 0);

	/* frames in flight: a limit of 2 lets two start, holds the third until one is shown */
	{
		struct frame_pacer_flight flight = { 2, 0 };

		CHECK(frame_pacer_flight_holds(&flight));
		CHECK(frame_pacer_flight_take(&flight));
		CHECK(frame_pacer_flight_take(&flight));
		CHECK(!frame_pacer_flight_take(&flight));
		frame_pacer_flight_give(&flight);
		CHECK(frame_pacer_flight_take(&flight));
		CHECK(flight.count == 2);
		frame_pacer_flight_give(&flight);
		frame_pacer_flight_give(&flight);
		/* (a give too many, after a slot timed out, doesn't go negative) */
		frame_pacer_flight_give(&flight);
		CHECK(flight.count == 0);
	}
	/* a limit of 1 is one at a time */
	{
		struct frame_pacer_flight flight = { 1, 0 };

		CHECK(frame_pacer_flight_take(&flight));
		CHECK(!frame_pacer_flight_take(&flight));
	}
	/* no limit: every frame starts and none holds a slot */
	{
		struct frame_pacer_flight flight = { 0, 0 };
		int index;

		CHECK(!frame_pacer_flight_holds(&flight));
		for (index = 0; index < 10; index++)
			CHECK(frame_pacer_flight_take(&flight));
		CHECK(flight.count == 0);
	}

	/* a frame's share of the display */
	CHECK(near(frame_pacer_share(1.0 / 60.0, 0.0), 1.0));
	CHECK(near(frame_pacer_share(1.0 / 120.0, 60.0), 2.0));
	CHECK(near(frame_pacer_share(1.0 / 120.0, 30.0), 4.0));
	CHECK(near(frame_pacer_share(1.0 / 60.0, 30.0), 2.0));
	/* (a cap above the display's rate is never asked about, and is one refresh) */
	CHECK(near(frame_pacer_share(1.0 / 60.0, 240.0), 1.0));

	/* the minimum time on screen: half a refresh short of the share */
	CHECK(near(frame_pacer_minimum_duration(1, 1, 1.0 / 60.0, 0.0), 0.5 / 60.0));
	CHECK(near(frame_pacer_minimum_duration(1, 1, 1.0 / 120.0, 60.0), 1.5 / 120.0));
	/* a 60 Hz display link over a 120 Hz panel (display.frame_rate 60): half a panel refresh short of 16.7 ms, so
	   the frame shows after two panel refreshes, not one; 30 there, after four */
	CHECK(near(frame_pacer_minimum_duration_panel(1, 1, 1.0 / 60.0, 60.0, 1.0 / 120.0), 1.5 / 120.0));
	CHECK(near(frame_pacer_minimum_duration_panel(1, 1, 1.0 / 60.0, 30.0, 1.0 / 120.0), 3.5 / 120.0));
	CHECK(near(frame_pacer_minimum_duration_panel(1, 1, 1.0 / 120.0, 0.0, 1.0 / 120.0), 0.5 / 120.0));
	CHECK(near(frame_pacer_minimum_duration_panel(1, 1, 1.0 / 60.0, 0.0, 0.0), 0.5 / 60.0));
	CHECK(frame_pacer_minimum_duration_panel(0, 1, 1.0 / 60.0, 60.0, 1.0 / 120.0) == 0.0);
	/* none when frames don't wait for the display (unlimited, a cap above the display), or it is off */
	CHECK(frame_pacer_minimum_duration(1, 0, 1.0 / 60.0, 0.0) == 0.0);
	CHECK(frame_pacer_minimum_duration(0, 1, 1.0 / 60.0, 0.0) == 0.0);
	/* none before the display's period is known, or when it isn't a display's */
	CHECK(frame_pacer_minimum_duration(1, 1, 0.0, 0.0) == 0.0);
	CHECK(frame_pacer_minimum_duration(1, 1, 1.0, 0.0) == 0.0);

	/* the 60 Hz blank at 60 Hz: every refresh, jitter and all */
	{
		struct frame_pacer_blank blank = { 0.0, 0 };
		int index, counted = 0;

		for (index = 0; index < 600; index++)
			counted += frame_pacer_blank_refresh(&blank, 100.0 + index / 60.0 + (index % 3 - 1) * 0.0002);
		CHECK(counted == 600);
	}
	/* at 120 Hz: every other refresh, 60 a second */
	{
		struct frame_pacer_blank blank = { 0.0, 0 };
		int index, counted = 0;

		for (index = 0; index < 1200; index++)
			counted += frame_pacer_blank_refresh(&blank, 100.0 + index / 120.0);
		CHECK(counted == 600);
		CHECK(blank.ticks == 600);
	}
	/* a pause (the background) is one blank, not a burst of the ones missed */
	{
		struct frame_pacer_blank blank = { 0.0, 0 };

		CHECK(frame_pacer_blank_refresh(&blank, 10.0));
		CHECK(frame_pacer_blank_refresh(&blank, 10.0 + 1.0 / 60.0));
		CHECK(frame_pacer_blank_refresh(&blank, 15.0));
		CHECK(!frame_pacer_blank_refresh(&blank, 15.0 + 1.0 / 120.0));
		CHECK(frame_pacer_blank_refresh(&blank, 15.0 + 1.0 / 60.0));
		CHECK(blank.ticks == 4);
	}
	/* a 30 Hz display: every refresh (never more blanks than refreshes) */
	{
		struct frame_pacer_blank blank = { 0.0, 0 };
		int index, counted = 0;

		for (index = 0; index < 300; index++)
			counted += frame_pacer_blank_refresh(&blank, 5.0 + index / 30.0);
		CHECK(counted == 300);
	}

	/* when frames were shown: even frames, one repeated frame, a frame never shown, a load */
	{
		struct frame_pacer_shown shown = { 0.0, 0, 0, 0, 0.0 };
		double period = 1.0 / 60.0, at = 50.0;
		int index;

		CHECK(frame_pacer_shown_add(&shown, at, period, 1.0) == 0.0);
		for (index = 0; index < 59; index++)
			CHECK(near(frame_pacer_shown_add(&shown, at += period, period, 1.0), period));
		CHECK(shown.intervals == 59 && shown.late == 0);
		/* two refreshes: late */
		CHECK(near(frame_pacer_shown_add(&shown, at += 2.0 * period, period, 1.0), 2.0 * period));
		CHECK(shown.late == 1 && near(shown.longest, 2.0 * period));
		/* never shown: not counted, and the next interval is from the last one shown */
		CHECK(frame_pacer_shown_add(&shown, 0.0, period, 1.0) == 0.0);
		CHECK(shown.frames == 61);
		CHECK(near(frame_pacer_shown_add(&shown, at += period, period, 1.0), period));
		/* two refreshes is on time for a frame whose share is two (a cap of 30 at 60 Hz) */
		frame_pacer_shown_add(&shown, at += 2.0 * period, period, 2.0);
		CHECK(shown.late == 1);
		/* a load isn't an interval */
		frame_pacer_shown_add(&shown, at += 3.0, period, 1.0);
		CHECK(shown.late == 1 && shown.intervals == 62 && near(shown.longest, 2.0 * period));
	}

	/* a late start: steady 4 ms frames at 60 Hz wait, and aim for the next refresh */
	{
		struct frame_pacer_late late = { { 0 } };
		double period = 1.0 / 60.0, aim, wait;
		int index;

		/* nothing until there is some history */
		CHECK(frame_pacer_late_wait(&late, 10.0, 10.0, period, &aim) == 0.0 && aim == 0.0);
		for (index = 0; index < FRAME_PACER_LATE_HISTORY; index++)
			frame_pacer_late_cost(&late, 0.004);
		/* just after a refresh at 10.0: start so that 4 ms of work and the 3 ms lead end at the next */
		wait = frame_pacer_late_wait(&late, 10.001, 10.0, period, &aim);
		CHECK(near(aim, 10.0 + period));
		CHECK(near(wait, period - 0.001 - 0.007));
		CHECK(near(late.allowed, 0.004));
		/* too late for that refresh: aim for the one after, never a negative wait */
		wait = frame_pacer_late_wait(&late, 10.0 + period - 0.002, 10.0, period, &aim);
		CHECK(near(aim, 10.0 + 2.0 * period));
		CHECK(wait > 0.0 && wait < period);
		/* a refresh time from long ago works the same (the display link's latest may be a few back) */
		wait = frame_pacer_late_wait(&late, 10.001, 10.0 - 7.0 * period, period, &aim);
		CHECK(near(aim, 10.0 + period));
		/* shown on time: nothing changes */
		frame_pacer_late_shown(&late, aim, aim + 0.001, period);
		CHECK(late.misses == 0 && late.holdoff == 0);
	}
	/* a fight begins: one frame costs more than was allowed for, and late starts stop for a while */
	{
		struct frame_pacer_late late = { { 0 } };
		double period = 1.0 / 60.0, aim;
		int index, waited = 0;

		for (index = 0; index < FRAME_PACER_LATE_WINDOW; index++)
			frame_pacer_late_cost(&late, 0.004);
		CHECK(frame_pacer_late_wait(&late, 20.001, 20.0, period, &aim) > 0.0);
		frame_pacer_late_cost(&late, 0.011);
		CHECK(late.jumps == 1 && late.holdoff == FRAME_PACER_LATE_HOLDOFF);
		for (index = 0; index < FRAME_PACER_LATE_HOLDOFF; index++)
		{
			CHECK(frame_pacer_late_wait(&late, 21.0 + index * period + 0.001, 20.0, period, &aim) == 0.0);
			frame_pacer_late_cost(&late, 0.011);
		}
		/* afterward the window holds only the new cost: 11 ms and the lead don't fit in three quarters of a refresh */
		CHECK(frame_pacer_late_wait(&late, 30.001, 20.0, period, &aim) == 0.0);
		/* the fight ends: once the window holds only light frames again, late starts come back */
		for (index = 0; index < FRAME_PACER_LATE_WINDOW; index++)
		{
			waited += frame_pacer_late_wait(&late, 31.0 + index * period + 0.001, 20.0, period, &aim) > 0.0;
			frame_pacer_late_cost(&late, 0.004);
		}
		CHECK(waited == 0);
		CHECK(frame_pacer_late_wait(&late, 40.001, 20.0, period, &aim) > 0.0);
	}
	/* a frame shown late: the lead grows, late starts stop for a while, and calm frames shrink it again slowly */
	{
		struct frame_pacer_late late = { { 0 } };
		double period = 1.0 / 60.0, aim;
		int index;

		for (index = 0; index < FRAME_PACER_LATE_WINDOW; index++)
			frame_pacer_late_cost(&late, 0.004);
		(void)frame_pacer_late_wait(&late, 50.001, 50.0, period, &aim);
		frame_pacer_late_shown(&late, aim, aim + period, period);
		CHECK(late.misses == 1 && late.holdoff == FRAME_PACER_LATE_HOLDOFF);
		CHECK(near(frame_pacer_late_lead(&late), FRAME_PACER_LATE_LEAD + FRAME_PACER_LATE_LEAD_STEP));
		for (index = 0; index < FRAME_PACER_LATE_CALM; index++)
			frame_pacer_late_shown(&late, aim, aim, period);
		CHECK(near(frame_pacer_late_lead(&late), FRAME_PACER_LATE_LEAD + 0.75 * FRAME_PACER_LATE_LEAD_STEP));
		/* the lead never passes half a refresh, however many misses */
		for (index = 0; index < 50; index++)
			frame_pacer_late_shown(&late, aim, aim + period, period);
		CHECK(frame_pacer_late_lead(&late) <= 0.5 * period + 1e-12);
	}
	/* frames that vary: the worst of the window decides, not the average */
	{
		struct frame_pacer_late late = { { 0 } };
		double period = 1.0 / 120.0, aim;
		int index;

		for (index = 0; index < FRAME_PACER_LATE_WINDOW; index++)
			frame_pacer_late_cost(&late, index % 8 == 0 ? 0.005 : 0.002);
		/* 5 ms and the lead are over three quarters of a 120 Hz refresh: at once */
		CHECK(frame_pacer_late_wait(&late, 60.0005, 60.0, period, &aim) == 0.0);
		/* a load (a quarter second) is not a frame's cost */
		frame_pacer_late_cost(&late, 0.5);
		CHECK(near(frame_pacer_late_worst(&late), 0.005));
	}

	if (failures)
		return 1;
	printf("frame_pacer_probe: ok\n");
	return 0;
}
