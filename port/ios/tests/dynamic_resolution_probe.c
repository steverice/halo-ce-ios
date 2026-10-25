/* Synthetic presented frames exercise the planner without clocks or graphics. */
#include "dynamic_resolution.h"
#include <assert.h>
#include <stdio.h>

#define CLOSE(a, b) assert(fabs((a) - (b)) < 1e-9)

static struct dynamic_resolution_frame frame(struct dynamic_resolution *p, double hz,
	double scene, double fixed, int late)
{
	struct dynamic_resolution_frame f = {0};
	f.frame_id = p->last_frame_id + 1;
	f.level = p->level;
	f.elapsed = f.budget = 1.0 / hz;
	f.scene = scene; f.upscale = fixed * 0.4; f.hud = fixed * 0.6;
	f.cpu = f.budget * 0.5;
	f.gpu_available = 1; f.late = late;
	return f;
}

static enum dynamic_resolution_action present(struct dynamic_resolution *p, double hz,
	double a, double b, double fixed, int late)
{
	struct dynamic_resolution_frame f = frame(p, hz, a + b * p->pixels[p->level], fixed, late);
	return dynamic_resolution_present(p, &f);
}

/* Feed a second eligible frame: one late sample alone must hold. */
static enum dynamic_resolution_action pressure(struct dynamic_resolution *p,
	struct dynamic_resolution_frame *f)
{
	assert(dynamic_resolution_present(p, f) == DYNAMIC_RESOLUTION_HOLD);
	f->frame_id++;
	return dynamic_resolution_present(p, f);
}

static enum dynamic_resolution_action fight(struct dynamic_resolution *p, double hz,
	double a, double b, double fixed)
{
	struct dynamic_resolution_frame f = frame(p, hz, a + b * p->pixels[p->level], fixed, 1);
	return pressure(p, &f);
}

static void levels_and_examples(void)
{
	struct dynamic_resolution p;
	struct dynamic_resolution_frame f;
	dynamic_resolution_init(&p, 1920, 1080, 540, 0.001, 5.3e-9);
	assert(p.levels == 15); /* 1080, 1040, ... 560, then the exact 540 floor */
	assert(p.lines[1] == 1040 && p.lines[14] == 540);
	f = frame(&p, 60, 0.012, 0.002, 0);
	assert(dynamic_resolution_present(&p, &f) == DYNAMIC_RESOLUTION_HOLD);
	f = frame(&p, 60, 0.017, 0.002, 1);
	assert(pressure(&p, &f) == DYNAMIC_RESOLUTION_DROP);
	assert(p.lines[p.level] == 880);
	CLOSE(p.b, 0.016 / 2073600.0);
	CLOSE(p.a, 0.001);
	CLOSE(0.001 + 5.3e-9 * p.pixels[p.level] + 0.002, 0.010294496);
	/* Rule 7's 0.75 B limit gives 800, not the example's 840: at 840
	the predicted 12.679 ms exceeds the 12.5 ms target. */
	dynamic_resolution_init(&p, 1920, 1080, 540, 0.001, 5.3e-9);
	f.thermal = DYNAMIC_RESOLUTION_SERIOUS;
	assert(pressure(&p, &f) == DYNAMIC_RESOLUTION_DROP);
	assert(p.lines[p.level] == 800);
	assert(dynamic_resolution_predict(&p, p.level, 0.002) <= 0.0125);
	assert(dynamic_resolution_predict(&p, p.level - 1, 0.002) > 0.0125);

	dynamic_resolution_init(&p, 2622, 1206, 540, 0.0006, 0.0047 / (2622.0 * 1206));
	assert(p.levels == 15 && p.lines[1] == 1161 && p.lines[2] == 1117);
	assert(p.lines[4] == 1027 && p.lines[14] == 603);
	assert(p.pixels[14] == 1311 * 603);
	f = frame(&p, 120, 0.0053, 0.0012, 0);
	assert(dynamic_resolution_present(&p, &f) == DYNAMIC_RESOLUTION_HOLD);
	f = frame(&p, 120, 0.0075, 0.0012, 1);
	assert(pressure(&p, &f) == DYNAMIC_RESOLUTION_DROP);
	assert(p.lines[p.level] == 1027);
	CLOSE(p.b, 0.0069 / (2622.0 * 1206));
	dynamic_resolution_init(&p, 3840, 2160, 540, 0, 0);
	assert(p.lines[p.levels - 1] == 1080);
	dynamic_resolution_init(&p, 960, 540, 450, 0, 0);
	assert(p.lines[p.levels - 1] == 450);
	dynamic_resolution_init(&p, 1920, 1081, 0, 0, 0);
	assert(p.lines[p.levels - 1] == 541);
	dynamic_resolution_init(&p, 1920, 1080, 2000, 0, 0);
	assert(p.levels == 1);
}

static void ceiling_and_cpu(void)
{
	struct dynamic_resolution p;
	struct dynamic_resolution_frame f;
	int i;
	dynamic_resolution_init(&p, 1920, 1080, 540, 0.001, 5.3e-9);
	for (i = 0; i < 600; i++)
	{
		f = frame(&p, 60, 1.0 / 60 - 0.00201, 0.002, 0);
		assert(dynamic_resolution_present(&p, &f) == DYNAMIC_RESOLUTION_HOLD);
		assert(p.level == 0);
	}
	p.level = 5;
	for (i = 0; i < 180; i++)
	{
		f = frame(&p, 60, 0.004, 0.002, 1); f.cpu = 0.016;
		assert(dynamic_resolution_present(&p, &f) != DYNAMIC_RESOLUTION_DROP);
	}
	assert(p.level == 0 && p.cpu_bound_late == 180);
	for (i = 0; i < 10; i++)
	{
		f = frame(&p, 60, 0.020, 0.002, 1); f.cpu = 0.016;
		assert(dynamic_resolution_present(&p, &f) == DYNAMIC_RESOLUTION_HOLD);
	}
	assert(p.cpu_bound_late == 180);
}

static void panic_and_settle(void)
{
	struct dynamic_resolution p;
	struct dynamic_resolution_frame f;
	int i, unavailable;
	for (unavailable = 0; unavailable <= 1; unavailable++)
	{
		dynamic_resolution_init(&p, 2622, 1206, 540, 0.0006, 1e-9);
		for (i = 0; i < 3; i++)
		{
			f = frame(&p, 120, 0.0058, 0.0012, 1);
			f.gpu_available = !unavailable;
			assert(dynamic_resolution_present(&p, &f) ==
				(i == 2 ? DYNAMIC_RESOLUTION_PANIC : DYNAMIC_RESOLUTION_HOLD));
		}
		assert(p.lines[p.level] == 1117);
		assert(p.count == 0 && p.next == 0 && p.since_drop == 0.0);
		assert(p.settle == 4 && p.consecutive_late == 0);
		for (i = 0; i < 4; i++)
		{
			f = frame(&p, 120, 0.020, 0.0012, 1); f.level = 0;
			assert(dynamic_resolution_present(&p, &f) == DYNAMIC_RESOLUTION_HOLD);
		}
		assert(p.settle == 0 && p.level == 2);
		f = frame(&p, 120, 0.020, 0.0012, 1); f.level = 0;
		assert(dynamic_resolution_present(&p, &f) == DYNAMIC_RESOLUTION_HOLD);
		f.level = p.level; f.deadline_miss = 1; f.late = 0;
		assert(pressure(&p, &f) == DYNAMIC_RESOLUTION_DROP);
	}
}

static void ignore_windows(void)
{
	struct dynamic_resolution p;
	struct dynamic_resolution_frame f;
	unsigned reasons[] = {DYNAMIC_RESOLUTION_COMPILE, DYNAMIC_RESOLUTION_SCENE,
		DYNAMIC_RESOLUTION_LOAD, DYNAMIC_RESOLUTION_RESUME, DYNAMIC_RESOLUTION_TEXTURE};
	int r, i;
	for (r = 0; r < 5; r++)
	{
		int frames = r == 4 ? 1 : r == 0 ? 4 : r == 1 ? 30 : 60;
		dynamic_resolution_init(&p, 1920, 1080, 540, 0.001, 5e-9);
		for (i = 0; i < frames; i++)
		{
			f = frame(&p, 60, 0.020, 0.002, 1);
			f.ignore = i == 0 ? reasons[r] : 0;
			assert(dynamic_resolution_present(&p, &f) == DYNAMIC_RESOLUTION_HOLD);
			assert(p.count == 0 && p.level == 0);
		}
		f.ignore = 0;
		assert(pressure(&p, &f) == DYNAMIC_RESOLUTION_DROP);
	}
	dynamic_resolution_init(&p, 1920, 1080, 540, 0.001, 5e-9);
	f = frame(&p, 60, 0.3, 0.002, 1); f.elapsed = 0.25;
	assert(dynamic_resolution_present(&p, &f) == DYNAMIC_RESOLUTION_HOLD);
	CLOSE(p.ignore_seconds, 1.0);
	CLOSE(p.time, 0.0);
	dynamic_resolution_ignore(&p, 30, DYNAMIC_RESOLUTION_SCENE);
	dynamic_resolution_ignore(&p, 4, DYNAMIC_RESOLUTION_COMPILE);
	assert(p.ignore_frames == 30);
	assert((p.ignore_reason & (DYNAMIC_RESOLUTION_SCENE | DYNAMIC_RESOLUTION_COMPILE)) == 3);
}

static void recovery_seconds(void)
{
	int rates[] = {60, 90, 120}, r, i, recoveries;
	struct dynamic_resolution p;
	for (r = 0; r < 3; r++)
	{
		int hz = rates[r];
		double scale = 60.0 / hz;
		dynamic_resolution_init(&p, 1920, 1080, 540, 0.001 * scale, 5.3e-9 * scale);
		assert(fight(&p, hz, 0.001 * scale, 0.016 / 2073600 * scale, 0.002 * scale) == DYNAMIC_RESOLUTION_DROP);
		/* At 60 Hz this is the worked fight's 880 -> 1080 recovery. */
		recoveries = 0;
		for (i = 1; i <= 3 * hz; i++)
		{
			enum dynamic_resolution_action action = present(&p, hz, 0.001 * scale, 5.3e-9 * scale, 0.002 * scale, 0);
			if (action == DYNAMIC_RESOLUTION_RECOVER)
			{
				int cadence = (hz + 3) / 4;
				assert(i == hz + recoveries * cadence);
				recoveries++;
			}
		}
		assert(p.level == 0 && recoveries > 0);
		if (hz == 60) assert(recoveries == 5);
	}
}

static void fit_and_percentile(void)
{
	struct dynamic_resolution p;
	struct dynamic_resolution_frame f;
	int i;
	dynamic_resolution_init(&p, 1920, 1080, 540, 0.001, 0);
	for (i = 0; i < 60; i++)
	{
		f = frame(&p, 60, 0, 0.002, 0); f.level = i % 2;
		f.scene = 0.001 + 5e-9 * p.pixels[f.level];
		dynamic_resolution_present(&p, &f);
	}
	CLOSE(p.a, 0.001); CLOSE(p.b * 1e6, 0.005);
	assert(p.count == 60);
	CLOSE(dynamic_resolution_p95(&p, 0), 0.001 + 5e-9 * p.pixels[0] + 0.002);
	/* A >5% tail must prevent recovery even with a low median. */
	for (i = 0; i < 4; i++) present(&p, 60, 0.030, 0, 0.002, 0);
	assert(dynamic_resolution_p95(&p, 0) > 0.025);
	p.time += 2.1;
	dynamic_resolution_expire(&p);
	assert(p.count == 0 && isinf(dynamic_resolution_p95(&p, 0)));
	/* Without two levels the seed is retained. */
	dynamic_resolution_init(&p, 1920, 1080, 540, 0.001, 5e-9);
	for (i = 0; i < 30; i++) present(&p, 60, 0.002, 4e-9, 0.002, 0);
	CLOSE(p.a, 0.001); CLOSE(p.b * 1e6, 0.005);
}

static void lateness_window(void)
{
	struct dynamic_resolution p;
	struct dynamic_resolution_frame f;
	int age, i;
	for (age = 1; age <= 3; age++)
	{
		dynamic_resolution_init(&p, 1920, 1080, 540, 0.001, 5e-9);
		f = frame(&p, 60, 0.020, 0.002, 1);
		assert(dynamic_resolution_present(&p, &f) == DYNAMIC_RESOLUTION_HOLD);
		for (i = 1; i < age; i++)
		{
			f = frame(&p, 60, 0.009, 0.002, 0);
			assert(dynamic_resolution_present(&p, &f) == DYNAMIC_RESOLUTION_HOLD);
		}
		f = frame(&p, 60, 0.020, 0.002, 0);
		assert(dynamic_resolution_present(&p, &f) ==
			(age <= 2 ? DYNAMIC_RESOLUTION_DROP : DYNAMIC_RESOLUTION_HOLD));
	}
	/* Exactly the CPU cutoff is eligible; above it cannot drop. */
	dynamic_resolution_init(&p, 1920, 1080, 540, 0.001, 5e-9);
	f = frame(&p, 60, 0.020, 0.002, 1); f.cpu = 0.9 * f.budget;
	assert(pressure(&p, &f) == DYNAMIC_RESOLUTION_DROP);
}

static void thermal_and_floor(void)
{
	struct dynamic_resolution p;
	struct dynamic_resolution_frame f;
	int thermal, i;
	for (thermal = 0; thermal <= 3; thermal++)
	{
		dynamic_resolution_init(&p, 1920, 1080, 540, 0.001, 5.3e-9);
		f = frame(&p, 60, 0.017, 0.002, 1); f.thermal = thermal;
		assert(pressure(&p, &f) == DYNAMIC_RESOLUTION_DROP);
		assert(p.lines[p.level] == (thermal < 2 ? 880 : 800));
		p.level = 1; p.settle = 0; p.count = p.next = 0;
		for (i = 0; i < 120; i++)
		{
			f = frame(&p, 60, 0.0125, 0.002, 0); f.thermal = thermal;
			dynamic_resolution_present(&p, &f);
		}
		assert(p.level == (thermal < 2 ? 0 : 1));
	}
	dynamic_resolution_init(&p, 1920, 1080, 540, 0.001, 5e-9);
	f = frame(&p, 60, 0.1, 0.002, 1);
	assert(pressure(&p, &f) == DYNAMIC_RESOLUTION_DROP);
	assert(p.level == p.levels - 1);
	for (i = 0; i < 120; i++) present(&p, 60, 0.1, 0, 0.002, 1);
	assert(p.level == p.levels - 1 && p.since_drop == 0.0);
}

static void dampening(void)
{
	struct dynamic_resolution p;
	int cycle, i;
	double delays[] = {2.0, 4.0, 8.0, 8.0};
	dynamic_resolution_init(&p, 1920, 1080, 540, 0.001, 5.3e-9);
	p.level = 1;
	for (cycle = 0; cycle < 4; cycle++)
	{
		for (i = 0; p.level && i < 600; i++)
			present(&p, 60, 0.001, 5.3e-9, 0.002, 0);
		assert(p.level == 0);
		for (i = 0; i < 4; i++) present(&p, 60, 0.001, 5.3e-9, 0.002, 0);
		assert(fight(&p, 60, 0.001, 0.016 / 2073600, 0.002) == DYNAMIC_RESOLUTION_DROP);
		assert(p.recovery_delay == delays[cycle]);
	}
	p.level = 0; p.good = 0.0;
	for (i = 0; i < 599; i++) present(&p, 60, 0.001, 5.3e-9, 0.002, 0);
	assert(p.recovery_delay == 8.0);
	present(&p, 60, 0.001, 5.3e-9, 0.002, 0);
	assert(p.recovery_delay == 4.0);
	for (i = 0; i < 600; i++) present(&p, 60, 0.001, 5.3e-9, 0.002, 0);
	assert(p.recovery_delay == 2.0);
}

static void bursts(void)
{
	struct dynamic_resolution p;
	int cycle, i, recoveries = 0, drops = 0;
	dynamic_resolution_init(&p, 1920, 1080, 540, 0.001, 5.3e-9);
	/* Repeated two-second fights, separated by one-second lulls. Costs scale
	with the chosen content, so late feedback disappears after a drop. */
	for (cycle = 0; cycle < 12; cycle++)
	{
		for (i = 0; i < 180; i++)
		{
			double b = i < 120 ? 0.016 / 2073600 : 5.3e-9;
			int late = 0.001 + b * p.pixels[p.level] + 0.002 > 1.0 / 60;
			enum dynamic_resolution_action action = present(&p, 60, 0.001, b, 0.002, late);
			if (cycle > 0) assert(action == DYNAMIC_RESOLUTION_HOLD);
			if (action == DYNAMIC_RESOLUTION_RECOVER) recoveries++;
			if (action == DYNAMIC_RESOLUTION_DROP) drops++;
		}
	}
	assert(drops == 1 && recoveries == 2);
	assert(p.lines[p.level] == 960);

}

/* Device traces showed isolated 20–39 ms scene spikes over about 9 ms.
A one-frame estimate must neither trigger nor size a sustained-pressure drop. */
static void isolated_spikes(void)
{
	struct dynamic_resolution p;
	int spike, i;
	for (spike = 20; spike <= 39; spike++)
	{
		dynamic_resolution_init(&p, 1441, 1080, 540, 0, 0);
		for (i = 0; i < 180; i++)
		{
			struct dynamic_resolution_frame f = frame(&p, 60,
				i == 60 ? spike / 1000.0 : 0.009, 0.003, i == 60);
			assert(dynamic_resolution_present(&p, &f) == DYNAMIC_RESOLUTION_HOLD);
			assert(p.level == 0);
		}
	}
}

/* The twelve recorded 1080/60 drops, replayed as isolated spikes. The
stderr stream logs actions and every 600th frame, so this is a pattern
replay over the measured ~9 ms baseline, not a full presentation trace. */
static void recorded_spikes(void)
{
	static const struct { int scene, fixed, cpu, thermal; } spikes[] = {
		{19075, 3580, 9098, 0},
		{33148, 3003, 12171, 0},
		{35068, 3191, 7579, 1},
		{39047, 3091, 7910, 1},
		{33450, 3498, 12808, 2},
		{29697, 3225, 7304, 2},
		{27716, 3354, 4132, 2},
		{35937, 3038, 10819, 2},
		{34777, 4078, 12234, 2},
		{33881, 3078, 11981, 2},
		{33036, 2941, 6838, 0},
		{26576, 3093, 5947, 0},
	};
	struct dynamic_resolution p;
	unsigned spike;
	int i;
	for (spike = 0; spike < sizeof(spikes) / sizeof(spikes[0]); spike++)
	{
		dynamic_resolution_init(&p, 1441, 1080, 540, 0, 0);
		for (i = 0; i < 240; i++)
		{
			struct dynamic_resolution_frame f = frame(&p, 60, 0.009, 0.003, i == 60);
			f.budget = 0.016670;
			f.thermal = spikes[spike].thermal;
			if (i == 60)
			{
				f.scene = spikes[spike].scene / 1e6;
				f.upscale = 0;
				f.hud = spikes[spike].fixed / 1e6;
				f.cpu = spikes[spike].cpu / 1e6;
				f.elapsed *= 2;
			}
			assert(dynamic_resolution_present(&p, &f) == DYNAMIC_RESOLUTION_HOLD);
			assert(p.level == 0);
		}
	}
}

static void sustained_pressure(void)
{
	struct dynamic_resolution p;
	struct dynamic_resolution_frame f;
	int scene, i, expected = 0;
	for (scene = 18; scene <= 20; scene++)
	{
		dynamic_resolution_init(&p, 1441, 1080, 540, 0, 0);
		f = frame(&p, 60, scene / 1000.0, 0.003, 1);
		assert(pressure(&p, &f) == DYNAMIC_RESOLUTION_DROP);
		assert(p.level > 0);
	}
	/* Increasing just one spike must not enlarge the drop. */
	for (scene = 20; scene <= 39; scene++)
	{
		dynamic_resolution_init(&p, 1441, 1080, 540, 0, 0);
		f = frame(&p, 60, 0.020, 0.003, 1);
		assert(dynamic_resolution_present(&p, &f) == DYNAMIC_RESOLUTION_HOLD);
		f = frame(&p, 60, scene / 1000.0, 0.003, 1);
		assert(dynamic_resolution_present(&p, &f) == DYNAMIC_RESOLUTION_DROP);
		if (scene == 20) expected = p.level;
		assert(p.level == expected);
	}
	/* Even severe measured pressure waits for corroboration, then reaches floor. */
	dynamic_resolution_init(&p, 1441, 1080, 540, 0, 0);
	f = frame(&p, 60, 0.080, 0.003, 1);
	assert(pressure(&p, &f) == DYNAMIC_RESOLUTION_DROP);
	assert(p.level == p.levels - 1);
	/* CPU-attributed spikes cannot supply either qualifying pressure sample. */
	dynamic_resolution_init(&p, 1441, 1080, 540, 0, 0);
	for (i = 0; i < 30; i++)
	{
		f = frame(&p, 60, i % 3 == 0 ? 0.020 : 0.039, 0.003, 1);
		if (i % 3) f.cpu = f.budget;
		assert(dynamic_resolution_present(&p, &f) == DYNAMIC_RESOLUTION_HOLD);
	}
}

int main(void)
{
	isolated_spikes();
	recorded_spikes();
	sustained_pressure();
	levels_and_examples();
	ceiling_and_cpu();
	panic_and_settle();
	ignore_windows();
	recovery_seconds();
	fit_and_percentile();
	lateness_window();
	thermal_and_floor();
	dampening();
	bursts();
	puts("dynamic_resolution_probe: all checks passed");
	return 0;
}
