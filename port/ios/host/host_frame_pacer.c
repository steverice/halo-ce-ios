/* The frame pacer (host_frame_pacer.h): the waiting, the settings and the
log around frame_pacer.h's rules. */
#include "host_frame_pacer.h"

#if HALO_FRAME_PACING

#include "frame_pacer.h"
#include "host_config.h"
#include "ios_host.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

/* host_gpu.c's, which writes the guest's log */
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* frames in flight when display.frames_in_flight is empty: port/ios/README.md
has the measurements behind it */
#define IN_FLIGHT_DEFAULT 2
/* a frame goes ahead without a slot after this long: the window may be
hidden, and a presented notice that never comes mustn't stop the game */
#define SLOT_WAIT_NANOSECONDS 100000000L
/* the display isn't refreshing (the background) after this long */
#define BLANK_WAIT_NANOSECONDS 50000000L

static struct
{
	int started;
	int system_wanted;               /* display.frame_source = "system", and frames wait for the display */
	int system_source;               /* the renderer's system pacer runs: no slots or spacing of ours */
	int in_flight;                   /* display.frames_in_flight (0: no limit) */
	int spacing;                     /* display.frame_spacing */
	int times;                       /* debug.present_times */
	double load;                     /* debug.frame_load_cpu, in seconds */
	pthread_mutex_t lock;
	pthread_cond_t shown_or_ticked;
	struct frame_pacer_flight flight;
	struct frame_pacer_blank blank;
	struct frame_pacer_shown shown;
	double period;                   /* the display's refresh period, seconds; 0 unknown */
	double refresh;                  /* the time of the display's latest refresh; 0 none yet */
	int late_start;                  /* display.late_start */
	struct frame_pacer_late late;
	double frame_started;            /* when this frame's work began (frame_pacer_frame_start); 0 unknown */
	double frame_aim;                /* the refresh this frame's late start aimed at; 0 none */
	double gpu_time;                 /* the renderer's latest (frame_pacer_gpu_time) */
	int waited_for_display;          /* the last frame presented waited for the display */
	double load_every;               /* debug.frame_load_every, seconds: the made-up load is on in every other span */
	double load_from;
	unsigned long reported;          /* shown.frames at the last report */
} pacer = { .lock = PTHREAD_MUTEX_INITIALIZER, .shown_or_ticked = PTHREAD_COND_INITIALIZER };

/* CACurrentMediaTime's clock, which a display link's and a drawable's times use */
static double now_seconds(void)
{
	return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
}

static void deadline_after(struct timespec *deadline, long nanoseconds)
{
	clock_gettime(CLOCK_REALTIME, deadline);
	deadline->tv_nsec += nanoseconds;
	if (deadline->tv_nsec >= 1000000000L)
	{
		deadline->tv_nsec -= 1000000000L;
		deadline->tv_sec++;
	}
}

void frame_pacer_start(int fixed_timestep)
{
	char value[16];
	int limit;

	if (fixed_timestep)
		return;
	host_config_string("display.frames_in_flight", "", value, sizeof(value));
	limit = frame_pacer_in_flight_setting(value, IN_FLIGHT_DEFAULT);
#if TARGET_OS_SIMULATOR
	/* a simulator's drawables never say when they were shown */
	limit = 0;
#endif
	host_config_string("display.frame_spacing", "true", value, sizeof(value));
	pthread_mutex_lock(&pacer.lock);
	/* (started again after frame_pacer_stop: frames from before hold no slot) */
	memset(&pacer.flight, 0, sizeof(pacer.flight));
	pacer.flight.limit = pacer.in_flight = limit;
	pacer.spacing = strcmp(value, "false") != 0;
	host_config_string("display.frame_source", "", value, sizeof(value));
	/* (the system's pacer shows one frame a refresh: no cap, no "unlimited") */
	pacer.system_wanted = !strcmp(value, "system") && !host_frame_rate_unlimited() && host_frame_rate_cap() == 0.0;
	host_config_string("debug.present_times", "false", value, sizeof(value));
	pacer.times = !strcmp(value, "true");
	pacer.load = host_config_real("debug.frame_load_cpu", 0.0) / 1e3;
	pacer.load_every = host_config_real("debug.frame_load_every", 0.0);
	pacer.load_from = now_seconds();
	/* (measured on the Mac only so far: off elsewhere unless asked for) */
	host_config_string("display.late_start", "", value, sizeof(value));
#if TARGET_OS_MACCATALYST || HALO_NATIVE_MAC
	pacer.late_start = strcmp(value, "false") != 0;
#else
	pacer.late_start = !strcmp(value, "true");
#endif
	pacer.started = 1;
	pthread_mutex_unlock(&pacer.lock);
	platform_log("frame pacer: %d frame%s in flight%s, frame spacing %s%s (display.frames_in_flight, display.frame_spacing)",
		limit, limit == 1 ? "" : "s", limit ? "" : " (no limit: the renderer's own queue)", pacer.spacing ? "on" : "off",
		pacer.times ? "; debug.present_times" : "");
	platform_log("frame pacer: late start %s (display.late_start)", pacer.late_start ? "on" : "off");
	if (pacer.load > 0.0)
		platform_log("frame pacer: debug.frame_load_cpu spins %.1f ms in every frame%s", pacer.load * 1e3,
			pacer.load_every > 0.0 ? ", on and off (debug.frame_load_every)" : "");
}

void frame_pacer_stop(void)
{
	pthread_mutex_lock(&pacer.lock);
	pacer.started = 0;
	pthread_cond_broadcast(&pacer.shown_or_ticked);
	pthread_mutex_unlock(&pacer.lock);
}

void frame_pacer_frame_start(void)
{
	double now, wait = 0.0, aim = 0.0;

	if (!pacer.started)
		return;
	now = now_seconds();
	pthread_mutex_lock(&pacer.lock);
	/* only for frames shown in step with the display, by the pacer's own
	rules: drawing faster than the display there is nothing to be in time
	for, and the system's pacer keeps its own time; and only while a frame's
	share is one refresh (a cap or a scene class's rate below the display's
	spaces frames out, which the late start's aim does not follow) */
	if (pacer.late_start && pacer.waited_for_display && !pacer.system_source && pacer.refresh > 0.0 &&
		frame_pacer_share(pacer.period, host_frame_rate_target()) == 1.0)
		wait = frame_pacer_late_wait(&pacer.late, now, pacer.refresh, pacer.period, &aim);
	pthread_mutex_unlock(&pacer.lock);
	if (wait > 0.0)
	{
		struct timespec duration = { 0, (long)(wait * 1e9) };

		nanosleep(&duration, NULL);
	}
	pacer.frame_aim = aim;
	pacer.frame_started = now_seconds();
}

void frame_pacer_gpu_time(double seconds)
{
	pacer.gpu_time = seconds;
}

int frame_pacer_late_start_toggle(void)
{
	int now_on;

	pthread_mutex_lock(&pacer.lock);
	now_on = pacer.late_start = !pacer.late_start;
	pthread_mutex_unlock(&pacer.lock);
	platform_log("frame pacer: late start %s (toggled)", now_on ? "on" : "off");
	return now_on;
}

int frame_pacer_wants_system_source(void)
{
	return pacer.started && pacer.system_wanted;
}

void frame_pacer_system_source(void)
{
	pthread_mutex_lock(&pacer.lock);
	pacer.system_source = 1;
	pacer.flight.limit = 0;
	pacer.spacing = 0;
	pthread_mutex_unlock(&pacer.lock);
	platform_log("frame pacer: the system's pacer hands out the frames (display.frame_source = \"system\"); "
		"the frames in flight are its to keep, and frame spacing is off");
}

int frame_pacer_in_flight(void)
{
	return pacer.in_flight;
}

void frame_pacer_refresh(double timestamp, double period)
{
	pthread_mutex_lock(&pacer.lock);
	pacer.period = period;
	pacer.refresh = timestamp;
	if (pacer.started && frame_pacer_blank_refresh(&pacer.blank, timestamp))
		pthread_cond_broadcast(&pacer.shown_or_ticked);
	pthread_mutex_unlock(&pacer.lock);
}

void frame_pacer_frame_begin(struct frame_pacer_frame *frame, unsigned long number, double began, int waits_for_display)
{
	memset(frame, 0, sizeof(*frame));
	frame->number = number;
	frame->began = began;
	if (!pacer.started)
		return;
	/* (a made-up load, to see what the settings do when a frame's work
	doesn't fit in a refresh; with debug.frame_load_every, on in every other
	span of that many seconds, as a fight coming and going) */
	if (pacer.load > 0.0 &&
		(pacer.load_every <= 0.0 || ((long)((now_seconds() - pacer.load_from) / pacer.load_every) & 1)))
	{
		double until = now_seconds() + pacer.load;

		while (now_seconds() < until)
			;
	}
	/* the frame's work so far, from the game's start of it (the delay to
	the display is counted from there, where the controllers were read) */
	if (pacer.frame_started > 0.0)
	{
		frame->began = pacer.frame_started;
		frame->aim = pacer.frame_aim;
		pthread_mutex_lock(&pacer.lock);
		frame_pacer_late_cost(&pacer.late, now_seconds() - pacer.frame_started + pacer.gpu_time);
		pacer.waited_for_display = waits_for_display != 0;
		pthread_mutex_unlock(&pacer.lock);
	}
	frame->waits_for_display = waits_for_display != 0;
	frame->watched = pacer.times != 0;
	if (!waits_for_display)
		return;
	pthread_mutex_lock(&pacer.lock);
	if (frame_pacer_flight_holds(&pacer.flight))
	{
		struct timespec deadline;
		int taken;

		deadline_after(&deadline, SLOT_WAIT_NANOSECONDS);
		while (!(taken = frame_pacer_flight_take(&pacer.flight)))
			if (pthread_cond_timedwait(&pacer.shown_or_ticked, &pacer.lock, &deadline))
				break;
		if (taken)
			frame->slot = frame->watched = 1;
	}
	pthread_mutex_unlock(&pacer.lock);
}

double frame_pacer_present(const struct frame_pacer_frame *frame)
{
	double period;

	if (!pacer.started)
		return 0.0;
	pthread_mutex_lock(&pacer.lock);
	period = pacer.period;
	pthread_mutex_unlock(&pacer.lock);
	/* (a scene class's rate counts: a cutscene at 30 is spaced for 30) */
	return frame_pacer_minimum_duration(pacer.spacing, frame->waits_for_display, period, host_frame_rate_target());
}

void frame_pacer_presented(const struct frame_pacer_frame *frame, double at)
{
	double interval, period;
	unsigned long frames = 0, intervals = 0, late = 0;
	double longest = 0.0;
	int noticed = at <= 0.0;

	/* (no time from the system: when it told us) */
	if (noticed)
		at = now_seconds();
	pthread_mutex_lock(&pacer.lock);
	if (frame->slot)
	{
		frame_pacer_flight_give(&pacer.flight);
		pthread_cond_broadcast(&pacer.shown_or_ticked);
	}
	period = pacer.period;
	/* (judged against the rate in force: under a scene class's 30 a frame's share is two refreshes) */
	interval = frame_pacer_shown_add(&pacer.shown, at, period, frame_pacer_share(period, host_frame_rate_target()));
	frame_pacer_late_shown(&pacer.late, frame->aim, at, period);
	if (pacer.shown.frames >= pacer.reported + 600)
	{
		frames = pacer.shown.frames - pacer.reported;
		intervals = pacer.shown.intervals;
		late = pacer.shown.late;
		longest = pacer.shown.longest;
		pacer.reported = pacer.shown.frames;
		pacer.shown.intervals = pacer.shown.late = 0;
		pacer.shown.longest = 0.0;
	}
	pthread_mutex_unlock(&pacer.lock);
	if (pacer.times)
	{
		platform_log("present time: frame %lu at %.3f ms, %.3f ms after the one before, %.3f ms after it began%s",
			frame->number, at * 1e3, interval * 1e3, frame->began > 0.0 ? (at - frame->began) * 1e3 : 0.0,
			noticed ? " (noticed: the system gave no time)" : "");
	}
	if (frames)
	{
		struct frame_pacer_late starts;

		pthread_mutex_lock(&pacer.lock);
		starts = pacer.late;
		pacer.late.late_starts = pacer.late.at_once = pacer.late.jumps = pacer.late.misses = 0;
		pthread_mutex_unlock(&pacer.lock);
		platform_log("frame pacer: of the last %lu frames shown, %lu came over 1.5 times their share of the display after the "
			"one before (longest %.1f ms, of %lu intervals at %.1f Hz); late starts %lu, at once %lu, cost jumps %lu, "
			"shown late %lu, worst cost %.1f ms, lead %.1f ms", frames, late, longest * 1e3, intervals,
			period > 0.0 ? 1.0 / period : 0.0, starts.late_starts, starts.at_once, starts.jumps, starts.misses,
			frame_pacer_late_worst(&starts) * 1e3, frame_pacer_late_lead(&starts) * 1e3);
	}
}

void frame_pacer_frame_dropped(struct frame_pacer_frame *frame)
{
	if (!frame->slot)
		return;
	frame->slot = 0;
	pthread_mutex_lock(&pacer.lock);
	frame_pacer_flight_give(&pacer.flight);
	pthread_cond_broadcast(&pacer.shown_or_ticked);
	pthread_mutex_unlock(&pacer.lock);
}

uint32_t frame_pacer_vertical_blank_wait(void)
{
	struct timespec deadline;
	unsigned long seen;
	uint32_t came;

	pthread_mutex_lock(&pacer.lock);
	seen = pacer.blank.ticks;
	if (!seen)
	{
		pthread_mutex_unlock(&pacer.lock);
		return 0;
	}
	deadline_after(&deadline, BLANK_WAIT_NANOSECONDS);
	while (pacer.blank.ticks == seen)
		if (pthread_cond_timedwait(&pacer.shown_or_ticked, &pacer.lock, &deadline))
			break;
	came = pacer.blank.ticks != seen;
	pthread_mutex_unlock(&pacer.lock);
	return came;
}

#endif
