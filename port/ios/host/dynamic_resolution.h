/* Dynamic resolution's pure planner. All durations are seconds; pixels are
content pixels, not attachment pixels. The host supplies presented frames in
order, with costs and level tags from the same frame. host_perf_hooks.c
feeds measured shadow frames without applying the suggested resolution.
port/ios/tests/dynamic_resolution_probe.c drives the rules without graphics. */
#pragma once

#include <math.h>
#include <string.h>

enum
{
	DYNAMIC_RESOLUTION_LEVELS = 28,
	/* Enough for two seconds at up to 480 presented frames per second.
	At higher rates the oldest samples are discarded first. */
	DYNAMIC_RESOLUTION_HISTORY = 1024,
	DYNAMIC_RESOLUTION_SETTLE = 4
};

enum dynamic_resolution_ignore_reason
{
	DYNAMIC_RESOLUTION_COMPILE = 1,
	DYNAMIC_RESOLUTION_SCENE = 2, /* scene-class change or either cutscene edge */
	DYNAMIC_RESOLUTION_LOAD = 4,
	DYNAMIC_RESOLUTION_RESUME = 8,
	DYNAMIC_RESOLUTION_TEXTURE = 16
};

enum dynamic_resolution_thermal
{
	DYNAMIC_RESOLUTION_NOMINAL,
	DYNAMIC_RESOLUTION_FAIR,
	DYNAMIC_RESOLUTION_SERIOUS,
	DYNAMIC_RESOLUTION_CRITICAL
};

enum dynamic_resolution_action
{
	DYNAMIC_RESOLUTION_HOLD,
	DYNAMIC_RESOLUTION_DROP,
	DYNAMIC_RESOLUTION_PANIC,
	DYNAMIC_RESOLUTION_RECOVER
};

struct dynamic_resolution_frame
{
	unsigned long long frame_id;
	int level; /* index returned by the planner when this frame was rendered */
	double elapsed; /* time since the preceding presentation; not a clock */
	double scene, upscale, hud, cpu, budget;
	int gpu_available;
	int late, deadline_miss;
	enum dynamic_resolution_thermal thermal;
	unsigned ignore; /* events beginning on this frame, not persistent settings */
};

struct dynamic_resolution_sample
{
	double at, pixels, scene, fixed;
	int level;
};

struct dynamic_resolution
{
	int lines[DYNAMIC_RESOLUTION_LEVELS], levels, level;
	double pixels[DYNAMIC_RESOLUTION_LEVELS];
	double recovery_a; /* smallest learned fixed cost, for conservative upward predictions */
	double a, b; /* seed from the pooled validation curve: seconds, seconds/pixel */
	struct dynamic_resolution_sample history[DYNAMIC_RESOLUTION_HISTORY];
	int count, next;
	double time, since_drop, since_step, since_recovery, good, recovery_delay;
	double recovery_headroom;
	int recovered, settle, ignore_frames;
	double ignore_seconds;
	unsigned ignore_reason;
	unsigned late_history, pressure_history;
	double pressure_scene[3], pressure_fixed[3];
	int consecutive_late;
	unsigned long cpu_bound_late;
	unsigned long long last_frame_id;
};

/* Width and height are the ceiling's content size. Fractional Q is rounded
independently at each level; the exact floor is appended if Q misses it.
An absolute floor above the ceiling pins the planner at the ceiling. */
static inline void dynamic_resolution_init(struct dynamic_resolution *plan,
	int width, int height, int absolute_floor, double a, double b)
{
	int step, floor_lines;
	memset(plan, 0, sizeof(*plan));
	if (width < 1) width = 1;
	if (height < 1) height = 1;
	floor_lines = (height + 1) / 2;
	if (absolute_floor > floor_lines) floor_lines = absolute_floor;
	if (floor_lines > height) floor_lines = height;
	for (step = 0; step < DYNAMIC_RESOLUTION_LEVELS; step++)
	{
		int lines = (int)(height * (1.0 - step / 27.0) + 0.5);
		if (lines < floor_lines) lines = floor_lines;
		if (!plan->levels || lines != plan->lines[plan->levels - 1])
		{
			int index = plan->levels++;
			int content_width = (int)((double)width * lines / height + 0.5);
			plan->lines[index] = lines;
			plan->pixels[index] = (double)content_width * lines;
		}
		if (lines == floor_lines) break;
	}
	plan->a = a >= 0.0 && isfinite(a) ? a : 0.0;
	plan->b = b >= 0.0 && isfinite(b) ? b : 0.0;
	plan->recovery_a = plan->a;
	plan->recovery_delay = 1.0;
	plan->since_recovery = 3.0;
}

/* Includes the current frame when called before dynamic_resolution_present.
Overlapping windows keep the longer remainder and combine their reasons. */
static inline void dynamic_resolution_ignore(struct dynamic_resolution *plan,
	int frames, unsigned reason)
{
	if (frames > plan->ignore_frames) plan->ignore_frames = frames;
	plan->ignore_reason |= reason;
}

static inline double dynamic_resolution_predict(const struct dynamic_resolution *plan,
	int level, double fixed)
{
	return plan->a + plan->b * plan->pixels[level] + fixed;
}

/* Expire by elapsed time, not frame count. */
static inline void dynamic_resolution_expire(struct dynamic_resolution *plan)
{
	while (plan->count)
	{
		int oldest = (plan->next - plan->count + DYNAMIC_RESOLUTION_HISTORY) % DYNAMIC_RESOLUTION_HISTORY;
		if (plan->time - plan->history[oldest].at <= 2.0 + 1e-9) break;
		plan->count--;
	}
}

static inline void dynamic_resolution_fit(struct dynamic_resolution *plan)
{
	double x = 0.0, y = 0.0, xx = 0.0, xy = 0.0, slope, intercept;
	int i, first = -1, distinct = 0;
	for (i = 0; i < plan->count; i++)
	{
		const struct dynamic_resolution_sample *sample = &plan->history[
			(plan->next - plan->count + i + DYNAMIC_RESOLUTION_HISTORY) % DYNAMIC_RESOLUTION_HISTORY];
		/* Megapixels keep the least-squares denominator well scaled. */
		double p = sample->pixels / 1e6;
		if (first < 0) first = sample->level;
		if (sample->level != first) distinct = 1;
		x += p; y += sample->scene; xx += p * p; xy += p * sample->scene;
	}
	if (!distinct || plan->count * xx - x * x <= 1e-12) return;
	slope = (plan->count * xy - x * y) / (plan->count * xx - x * x);
	intercept = (y - slope * x) / plan->count;
	/* A physical curve has nonnegative fixed and per-pixel costs. Use the
	boundary least-squares solution when the unconstrained fit crosses it. */
	if (slope < 0.0) { slope = 0.0; intercept = y / plan->count; }
	if (intercept < 0.0) { intercept = 0.0; slope = xy / xx; }
	plan->a = intercept;
	if (intercept < plan->recovery_a) plan->recovery_a = intercept;
	plan->b = slope / 1e6;
}

/* Nearest-rank p95 of per-frame predictions, preserving each frame's load
and U+H rather than hiding a fight behind the fitted mean. Hold a fixed and
scale each sample's variable scene cost to the proposed next level. Upward
predictions use the smallest learned fixed term: a workload change must not
turn variable scene work into an inflated fixed term and hide a spike. */
static inline double dynamic_resolution_p95(const struct dynamic_resolution *plan, int level)
{
	double values[DYNAMIC_RESOLUTION_HISTORY];
	int i, count = 0;
	for (i = 0; i < plan->count; i++)
	{
		const struct dynamic_resolution_sample *sample = &plan->history[
			(plan->next - plan->count + i + DYNAMIC_RESOLUTION_HISTORY) % DYNAMIC_RESOLUTION_HISTORY];
		double variable, value, a;
		int j;
		if (plan->time - sample->at > 0.5 + 1e-9) continue;
		a = plan->pixels[level] > sample->pixels ? plan->recovery_a : plan->a;
		variable = sample->scene - a;
		if (variable < 0.0) variable = 0.0;
		value = a + variable * plan->pixels[level] / sample->pixels + sample->fixed;
		for (j = count; j > 0 && values[j - 1] > value; j--) values[j] = values[j - 1];
		values[j] = value;
		count++;
	}
	return count ? values[(95 * count + 99) / 100 - 1] : INFINITY;
}

static inline enum dynamic_resolution_action dynamic_resolution_present(
	struct dynamic_resolution *plan, const struct dynamic_resolution_frame *frame)
{
	double dt = frame->elapsed, total = frame->scene + frame->upscale + frame->hud;
	double drop_target = frame->thermal >= DYNAMIC_RESOLUTION_SERIOUS ? 0.75 : 0.85;
	double recover_target = frame->thermal >= DYNAMIC_RESOLUTION_SERIOUS ? 0.85 : 0.95;
	int late = frame->late || frame->deadline_miss, valid, cpu_bound, trigger, target, settling, pressure, i;
	enum dynamic_resolution_action action = DYNAMIC_RESOLUTION_HOLD;
	plan->last_frame_id = frame->frame_id;
	if (frame->ignore & DYNAMIC_RESOLUTION_TEXTURE)
		dynamic_resolution_ignore(plan, 1, DYNAMIC_RESOLUTION_TEXTURE);
	if (frame->ignore & DYNAMIC_RESOLUTION_COMPILE)
		dynamic_resolution_ignore(plan, 4, DYNAMIC_RESOLUTION_COMPILE);
	if (frame->ignore & DYNAMIC_RESOLUTION_SCENE)
		dynamic_resolution_ignore(plan, 30, DYNAMIC_RESOLUTION_SCENE);
	if ((frame->ignore & (DYNAMIC_RESOLUTION_LOAD | DYNAMIC_RESOLUTION_RESUME)) || dt >= 0.25)
	{
		plan->ignore_seconds = 1.0;
		plan->ignore_reason |= frame->ignore;
		if (dt >= 0.25) plan->ignore_reason |= DYNAMIC_RESOLUTION_LOAD;
	}
	if (!isfinite(dt) || dt <= 0.0) return action;
	/* A load's wall time is not a second of presented frames. */
	if (dt >= 0.25) dt = 0.0;
	plan->time += dt;
	dynamic_resolution_expire(plan);
	settling = plan->settle > 0;
	if (settling) plan->settle--;
	if (plan->ignore_frames > 0 || plan->ignore_seconds > 1e-9)
	{
		if (plan->ignore_frames > 0) plan->ignore_frames--;
		if (plan->ignore_seconds > 0.0) plan->ignore_seconds -= dt;
		plan->late_history = plan->pressure_history = 0; plan->consecutive_late = 0; plan->good = 0.0;
		plan->recovery_headroom = 0.0;
		return action;
	}
	plan->ignore_reason = 0;
	if (!isfinite(frame->budget) || frame->budget <= 0.0 ||
		frame->level < 0 || frame->level >= plan->levels) return action;
	plan->since_drop += dt; plan->since_step += dt; plan->since_recovery += dt;
	valid = frame->gpu_available && isfinite(total) && isfinite(frame->scene) &&
		isfinite(frame->upscale) && isfinite(frame->hud) &&
		frame->scene >= 0.0 && frame->upscale >= 0.0 && frame->hud >= 0.0;
	cpu_bound = frame->cpu > 0.9 * frame->budget;
	if (late && cpu_bound && valid && total < frame->budget) plan->cpu_bound_late++;
	/* Four presentations drain the in-flight old level. A stale tag after
	that still cannot drive lateness decisions, but can inform the fit. */
	if (settling || frame->level != plan->level) late = 0;
	plan->late_history = ((plan->late_history << 1) | (late && !cpu_bound)) & 7;
	plan->consecutive_late = late && !cpu_bound ? plan->consecutive_late + 1 : 0;
	/* Two pressured frames out of three reject an isolated scene hitch while
	reacting to a sustained fight on its second frame. CPU work, stale levels
	and settling frames cannot corroborate GPU pressure. Keep recent lateness
	as a separate requirement, including the suspicious-timing panic below. */
	pressure = !settling && frame->level == plan->level && !cpu_bound && valid &&
		total > frame->budget - 0.0005;
	plan->pressure_history = ((plan->pressure_history << 1) | pressure) & 7;
	for (i = 2; i > 0; i--)
	{
		plan->pressure_scene[i] = plan->pressure_scene[i - 1];
		plan->pressure_fixed[i] = plan->pressure_fixed[i - 1];
	}
	plan->pressure_scene[0] = frame->scene;
	plan->pressure_fixed[0] = frame->upscale + frame->hud;
	trigger = pressure && plan->late_history &&
		(plan->pressure_history & (plan->pressure_history - 1));
	target = plan->level;
	if (plan->consecutive_late >= 3 && (!valid || total < frame->budget))
	{
		target += 2;
		if (target >= plan->levels) target = plan->levels - 1;
		plan->count = plan->next = 0;
		action = DYNAMIC_RESOLUTION_PANIC;
		trigger = 1;
	}
	else if (trigger)
	{
		/* Use the lower pressured cost, so one large spike cannot size the
		drop. Preserve its paired U+H and the fit's fixed scene term. */
		int sustained = 0;
		for (i = 1; i < 3; i++)
			if ((plan->pressure_history & (1u << i)) &&
				plan->pressure_scene[i] + plan->pressure_fixed[i] <
				plan->pressure_scene[sustained] + plan->pressure_fixed[sustained])
				sustained = i;
		plan->b = fmax(0.0, plan->pressure_scene[sustained] - plan->a) / plan->pixels[frame->level];
		for (target = plan->level; target < plan->levels - 1; target++)
			if (dynamic_resolution_predict(plan, target, plan->pressure_fixed[sustained]) <= drop_target * frame->budget)
				break;
		if (target > plan->level) action = DYNAMIC_RESOLUTION_DROP;
	}
	if (trigger)
	{
		plan->since_drop = 0.0; plan->good = 0.0;
		if (target > plan->level && plan->recovered && plan->since_recovery < 2.0)
		{
			plan->recovery_delay = fmin(8.0, plan->recovery_delay * 2.0);
			plan->recovered = 0;
		}
	}
	else if (valid && (!late || cpu_bound) && total <= recover_target * frame->budget)
	{
		plan->good += dt;
		if (plan->good + 1e-9 >= 10.0)
		{
			plan->recovery_delay = fmax(1.0, plan->recovery_delay / 2.0);
			plan->good = 0.0;
		}
	}
	else plan->good = 0.0;
	if (valid && action != DYNAMIC_RESOLUTION_PANIC)
	{
		struct dynamic_resolution_sample *sample = &plan->history[plan->next];
		sample->at = plan->time; sample->pixels = plan->pixels[frame->level];
		sample->scene = frame->scene; sample->fixed = frame->upscale + frame->hud;
		sample->level = frame->level;
		plan->next = (plan->next + 1) % DYNAMIC_RESOLUTION_HISTORY;
		if (plan->count < DYNAMIC_RESOLUTION_HISTORY) plan->count++;
		if (!trigger) dynamic_resolution_fit(plan);
	}
	/* A drop cooldown alone eventually expires during a bursty fight. Require
	continuous predicted headroom too, so a short lull cannot restart a climb
	every time the cooldown expires. Keep this across successful steps. */
	if (!trigger && valid && target > 0 &&
		dynamic_resolution_p95(plan, target - 1) <= recover_target * frame->budget)
		plan->recovery_headroom += dt;
	else plan->recovery_headroom = 0.0;
	if (!trigger && !settling && target > 0 && valid &&
		plan->since_drop + 1e-9 >= plan->recovery_delay && plan->since_step + 1e-9 >= 0.25 &&
		plan->recovery_headroom + 1e-9 >= plan->recovery_delay)
	{
		target--;
		action = DYNAMIC_RESOLUTION_RECOVER;
		plan->since_recovery = 0.0; plan->recovered = 1;
	}
	if (target != plan->level || action == DYNAMIC_RESOLUTION_PANIC)
	{
		plan->level = target; plan->settle = DYNAMIC_RESOLUTION_SETTLE;
		plan->since_step = 0.0;
		plan->late_history = plan->pressure_history = 0; plan->consecutive_late = 0;
	}
	return action;
}
