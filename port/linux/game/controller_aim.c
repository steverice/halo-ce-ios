/* Controller aim's math (controller_aim.h). */
#include <string.h>

#include "controller_aim.h"

#define TWO_PI (2.0f * 3.14159265f)
#define PI 3.14159265f

/* tracking loss: the last aim held for the grace (struct controller_track's
hold_seconds), then eased to the head over EASE_SECONDS; a recovery eases back
over EASE_SECONDS too */
#define EASE_SECONDS 0.200f
/* the tracking a recovery needs first (struct controller_track's
dwell_step): longer for a loss within DWELL_GROWS_WITHIN of the last
recovery, back to the first after DWELL_RESETS_AFTER without a loss */
static const float DWELL_SECONDS[3] = { 0.100f, 0.500f, 1.000f };
#define DWELL_GROWS_WITHIN 5.0f
#define DWELL_RESETS_AFTER 10.0f
/* a message or a buzz at most this often */
#define MESSAGE_EVERY 5.0f
/* orientation-only dims the reticle after this long */
#define ORIENTATION_DIM_AFTER 2.0f
/* the since_* counters stop here ("large"), where a float still counts milliseconds */
#define SINCE_LARGE 1000.0f
/* the press latch's facing step: pi / 16384 */
#define FACING_STEPS 16384.0f

/* angle wrapped into -pi..pi */
static float wrapped(float angle)
{
	return remainderf(angle, TWO_PI);
}

/* linear progress into 0..1, smoothstepped */
static float smoothstep(float progress)
{
	progress = fminf(fmaxf(progress, 0.0f), 1.0f);
	return progress * progress * (3.0f - 2.0f * progress);
}

static float length3(const float v[3])
{
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/* v turned by angle about +Z */
static void turned_about_z(const float v[3], float angle, float out[3])
{
	float c = cosf(angle), s = sinf(angle);
	float x = v[0] * c - v[1] * s;
	float y = v[0] * s + v[1] * c;

	out[0] = x;
	out[1] = y;
	out[2] = v[2];
}

float controller_body_yaw(struct controller_body *body, float look_yaw, float head_yaw, int head_level)
{
	/* past 85 degrees the head's yaw swings by its roll: the body
	holds, so the aim stays put in the room while you look at your feet */
	if (head_level)
	{
		body->yaw = wrapped(look_yaw - head_yaw);
		body->known = 1;
	}
	return body->known ? body->yaw : 0.0f;
}

void controller_location_to_world(const struct halo_spatial_location *location, float body_yaw,
	const float eye[3], float pitch_offset, float origin[3], float forward[3], float up[3])
{
	float c = cosf(pitch_offset), s = sinf(pitch_offset);
	float pitched_forward[3], pitched_up[3], position[3];
	int axis;

	/* about the location's left: forward toward -up, positive down */
	for (axis = 0; axis < 3; axis++)
	{
		pitched_forward[axis] = location->forward[axis] * c - location->up[axis] * s;
		pitched_up[axis] = location->up[axis] * c + location->forward[axis] * s;
	}
	turned_about_z(pitched_forward, body_yaw, forward);
	turned_about_z(pitched_up, body_yaw, up);
	turned_about_z(location->position, body_yaw, position);
	for (axis = 0; axis < 3; axis++)
		origin[axis] = eye[axis] + position[axis] / CONTROLLER_METERS_PER_UNIT;
}

void controller_sightline(struct controller_near *near, const float eye[3], const float aim_point[3],
	const float barrel_forward[3], float time_delta, float direction[3])
{
	float to_point[3], length, distance;
	int axis;

	for (axis = 0; axis < 3; axis++)
		to_point[axis] = aim_point[axis] - eye[axis];
	/* close up the eye-to-P direction swings over small hand motion: P is
	low-passed there, and follows at once beyond. The filter runs on P less
	the eye, so the eye's own motion (strafing, a seat) never makes it lag */
	if (near->known && length3(to_point) < CONTROLLER_NEAR_UNITS)
	{
		float follow = 1.0f - expf(-fmaxf(time_delta, 0.0f) / CONTROLLER_NEAR_SECONDS);

		for (axis = 0; axis < 3; axis++)
			near->point[axis] += (to_point[axis] - near->point[axis]) * follow;
	}
	else
	{
		for (axis = 0; axis < 3; axis++)
			near->point[axis] = to_point[axis];
		near->known = 1;
	}
	for (axis = 0; axis < 3; axis++)
		to_point[axis] = near->point[axis];
	distance = length3(to_point);
	/* The projectile leaves on the eye's line, not the hand's. Blending
	that line toward the parallel barrel ray makes it miss P at close
	range, precisely where their parallax is largest. Keep convergence
	throughout the near filter; only a hit at the eye has no direction. */
	if (distance <= 1e-6f)
	{
		length = length3(barrel_forward);
		for (axis = 0; axis < 3; axis++)
			direction[axis] = length > 0.0f ? barrel_forward[axis] / length : (axis == 0);
		return;
	}
	for (axis = 0; axis < 3; axis++)
		direction[axis] = to_point[axis] / distance;
}

void controller_angles(const float direction[3], float *yaw, float *pitch)
{
	float level = sqrtf(direction[0] * direction[0] + direction[1] * direction[1]);

	*yaw = atan2f(direction[1], direction[0]);
	*pitch = fminf(fmaxf(atan2f(direction[2], level), -CONTROLLER_PITCH_LIMIT), CONTROLLER_PITCH_LIMIT);
}

short controller_facing_pack(int hand, float delta_yaw)
{
	long steps;

	if (!hand)
		return 0;
	steps = lroundf(wrapped(delta_yaw) * (FACING_STEPS / PI));
	/* +pi is -pi; the 15 bits hold -16384..16383 */
	if (steps >= (long)FACING_STEPS)
		steps -= 2 * (long)FACING_STEPS;
	if (steps < -(long)FACING_STEPS)
		steps = -(long)FACING_STEPS;
	return (short)(steps * 2 + 1);
}

int controller_facing_unpack(short pad, float *delta_yaw)
{
	int hand = pad & 1;

	/* (pad - hand) / 2 is the arithmetic shift, exactly: pad - hand is even */
	*delta_yaw = hand ? (float)((pad - hand) / 2) * (PI / FACING_STEPS) : 0.0f;
	return hand;
}

float controller_yaw_blend(float from, float to, float blend)
{
	return wrapped(from + blend * wrapped(to - from));
}

void controller_track_reset(struct controller_track *track)
{
	track->blend = 1.0f;
	track->hold_seconds = CONTROLLER_LOSS_GRACE_DEFAULT;
	track->lost = 0;
	track->lost_for = 0.0f;
	track->tracked_for = 0.0f;
	track->orientation_for = 0.0f;
	track->since_recovery = SINCE_LARGE;
	track->since_loss = SINCE_LARGE;
	track->since_message = SINCE_LARGE;
	track->dwell_step = 0;
}

/* a message or a buzz now unless one came within MESSAGE_EVERY: 1 when it may */
static int message_allowed(struct controller_track *track)
{
	if (track->since_message < MESSAGE_EVERY)
		return 0;
	track->since_message = 0.0f;
	return 1;
}

void controller_track_update(struct controller_track *track, int state, int held, float time_delta,
	struct controller_track_out *out)
{
	/* the hand aims on orientation alone (the last good position); a
	recovery needs position and orientation */
	int tracked = held && (state == HALO_SPATIAL_TRACKED || state == HALO_SPATIAL_LOW_ACCURACY);
	int aiming = tracked || (held && state == HALO_SPATIAL_ORIENTATION);
	float previous_blend = track->blend;

	time_delta = fmaxf(time_delta, 0.0f);
	out->message = 0;
	out->buzz = 0;
	track->since_recovery = fminf(track->since_recovery + time_delta, SINCE_LARGE);
	track->since_loss = fminf(track->since_loss + time_delta, SINCE_LARGE);
	track->since_message = fminf(track->since_message + time_delta, SINCE_LARGE);
	if (!track->lost && !aiming)
	{
		/* a controller at the edge of tracking would flap: each loss soon
		after a recovery needs longer tracking before the next */
		if (track->since_loss >= DWELL_RESETS_AFTER)
			track->dwell_step = 0;
		else if (track->since_recovery < DWELL_GROWS_WITHIN && track->dwell_step < 2)
			track->dwell_step++;
		track->since_loss = 0.0f;
		track->lost = 1;
		track->lost_for = 0.0f;
		track->tracked_for = 0.0f;
	}
	if (track->lost)
	{
		track->lost_for += time_delta;
		track->tracked_for = tracked ? track->tracked_for + time_delta : 0.0f;
		if (track->tracked_for >= DWELL_SECONDS[track->dwell_step])
		{
			track->lost = 0;
			track->since_recovery = 0.0f;
			/* the head had taken the aim: say the hand has it back */
			if (track->blend <= 0.0f && message_allowed(track))
				out->message = 2;
		}
	}
	/* each ease is monotonic from wherever the last left the blend: a loss
	during a recovery's ease holds there until the loss's own ease reaches it */
	if (track->lost)
	{
		track->blend = fminf(track->blend, 1.0f - smoothstep((track->lost_for - track->hold_seconds) / EASE_SECONDS));
		if (track->blend <= 0.0f && previous_blend > 0.0f && message_allowed(track))
		{
			out->message = 1;
			out->buzz = 1;
		}
	}
	else
		track->blend = fmaxf(track->blend, smoothstep(track->since_recovery / EASE_SECONDS));
	track->orientation_for = held && state == HALO_SPATIAL_ORIENTATION ? track->orientation_for + time_delta : 0.0f;
	out->blend = track->blend;
	out->use_held = track->lost;
	out->dim = track->lost || track->orientation_for >= ORIENTATION_DIM_AFTER;
}

float controller_loss_grace(float setting, float debug)
{
	float grace = debug >= 0.0f ? debug : setting;

	return fminf(fmaxf(grace, 0.0f), CONTROLLER_LOSS_GRACE_MAXIMUM);
}

float controller_gate_ease(float *value, int target, float time_delta, int snap)
{
	float goal = target ? 1.0f : 0.0f;
	float step = fmaxf(time_delta, 0.0f) / EASE_SECONDS;

	if (snap)
		*value = goal;
	else if (*value < goal)
		*value = fminf(*value + step, goal);
	else
		*value = fmaxf(*value - step, goal);
	return smoothstep(*value);
}

int controller_latch_update(struct controller_latch *latch, long tick, int game_running, unsigned long flags_now,
	unsigned long fire_mask, float *yaw, float *pitch, short *pad)
{
	/* while the clock is stopped no tick will take them */
	if (latch->tick != tick || !game_running)
	{
		latch->tick = tick;
		latch->pending = 0;
		latch->latched = 0;
	}
	if (!game_running)
		return 0;
	if (flags_now & fire_mask & ~latch->pending)
	{
		if (!latch->latched)
		{
			latch->latched = 1;
			latch->yaw = *yaw;
			latch->pitch = *pitch;
			latch->pad = *pad;
		}
	}
	latch->pending |= flags_now;
	if (latch->latched)
	{
		*yaw = latch->yaw;
		*pitch = latch->pitch;
		*pad = latch->pad;
	}
	return latch->latched;
}

int controller_needs_reset(long last_unit, long unit, long last_time, long time)
{
	return unit != last_unit || time < last_time || (time == 0 && last_time != 0);
}

/* v less its share along unit, normalized into out; 0 when nothing is left */
static int squared_to(const float v[3], const float unit[3], float out[3])
{
	float along = v[0] * unit[0] + v[1] * unit[1] + v[2] * unit[2];
	float length;
	int axis;

	for (axis = 0; axis < 3; axis++)
		out[axis] = v[axis] - along * unit[axis];
	length = length3(out);
	if (!(length > 1e-4f))
		return 0;
	for (axis = 0; axis < 3; axis++)
		out[axis] /= length;
	return 1;
}

int controller_weapon_frame(const float camera_forward[3], const float camera_up[3], const float sightline[3],
	const float barrel_up[3], float blend, float forward_out[3], float up_out[3])
{
	float forward[3], up_mix[3], up[3];
	int axis;

	blend = fminf(fmaxf(blend, 0.0f), 1.0f);
	if (blend <= 0.0f)
		memcpy(forward, camera_forward, sizeof(forward));
	else if (blend >= 1.0f)
		memcpy(forward, sightline, sizeof(forward));
	else
	{
		/* along the arc: the angle between them shared by the blend */
		float cosine = fminf(fmaxf(camera_forward[0] * sightline[0] + camera_forward[1] * sightline[1] +
			camera_forward[2] * sightline[2], -1.0f), 1.0f);
		float angle = acosf(cosine);
		float sine = sinf(angle);

		if (sine < 1e-4f)
		{
			/* (straight behind: no one arc) */
			if (cosine < 0.0f)
				return 0;
			memcpy(forward, sightline, sizeof(forward));
		}
		else
		{
			float from = sinf((1.0f - blend) * angle) / sine, to = sinf(blend * angle) / sine;

			for (axis = 0; axis < 3; axis++)
				forward[axis] = from * camera_forward[axis] + to * sightline[axis];
		}
	}
	for (axis = 0; axis < 3; axis++)
		up_mix[axis] = (1.0f - blend) * camera_up[axis] + blend * barrel_up[axis];
	/* (the barrel's up along forward, straight up or down: the camera's) */
	if (!squared_to(up_mix, forward, up) && !squared_to(camera_up, forward, up))
		return 0;
	memcpy(forward_out, forward, sizeof(forward));
	memcpy(up_out, up, sizeof(up));
	return 1;
}

float controller_range_ease_clamp(float seconds)
{
	if (seconds != seconds)
		return CONTROLLER_RANGE_EASE_DEFAULT;
	return fminf(fmaxf(seconds, 0.0f), CONTROLLER_RANGE_EASE_MAXIMUM);
}

float controller_range_ease(struct controller_range *state, const float forward[3], const float position[3],
	float range, float seconds, float time_delta)
{
	float inverse, turned[3], moved[3], speed, fast;
	int axis;

	if (!(range > 1e-3f))
		range = 1e-3f;
	inverse = 1.0f / range;
	if (!(seconds > 0.0f) || range < CONTROLLER_NEAR_UNITS)
	{
		state->known = 0;
		return range;
	}
	for (axis = 0; axis < 3; axis++)
		moved[axis] = state->known ? position[axis] - state->position[axis] : 0.0f;
	memcpy(state->position, position, sizeof(state->position));
	if (!state->known || !(time_delta > 0.0f) || time_delta > 0.25f || length3(moved) > CONTROLLER_RANGE_EASE_JUMP_METERS)
	{
		state->inverse = inverse;
		memcpy(state->forward, forward, sizeof(state->forward));
		state->known = 1;
		return range;
	}
	/* how fast the ray turns (degrees a second, by the chord between this
	direction and the last): the easing's time shrinks to nothing as it
	nears the fast speed */
	for (axis = 0; axis < 3; axis++)
		turned[axis] = forward[axis] - state->forward[axis];
	memcpy(state->forward, forward, sizeof(state->forward));
	speed = length3(turned) * (180.0f / PI) / time_delta;
	fast = smoothstep((speed - CONTROLLER_RANGE_EASE_SLOW_DEGREES) /
		(CONTROLLER_RANGE_EASE_FAST_DEGREES - CONTROLLER_RANGE_EASE_SLOW_DEGREES));
	seconds *= 1.0f - fast;
	if (!(seconds > 1e-4f))
	{
		state->inverse = inverse;
		return range;
	}
	state->inverse += (inverse - state->inverse) * (1.0f - expf(-time_delta / seconds));
	return 1.0f / state->inverse;
}

/* an angle held within limit: itself up to CONTROLLER_WEAPON_LIMIT_KNEE of
it, easing to the limit beyond (the slope is continuous at the knee) */
static float limited_angle(float angle, float limit)
{
	float knee = limit * CONTROLLER_WEAPON_LIMIT_KNEE, size = fabsf(angle), room = limit - knee;

	if (size <= knee || !(room > 0.0f))
		return fminf(fmaxf(angle, -limit), limit);
	size = knee + room * (1.0f - expf(-(size - knee) / room));
	return angle < 0.0f ? -size : size;
}

void controller_weapon_limit(const float camera_forward[3], const float camera_up[3], float turn_limit,
	float roll_limit, float forward[3], float up[3])
{
	float f[3], u[3], r[3], length, along, across, rise, turn, elevation, sink = 0.0f, limited[3], level[3], roll;
	int axis;

	/* the camera's frame: forward, up made square to it, right = forward x up */
	length = length3(camera_forward);
	if (!(length > 1e-6f))
		return;
	for (axis = 0; axis < 3; axis++)
		f[axis] = camera_forward[axis] / length;
	if (!squared_to(camera_up, f, u))
		return;
	r[0] = f[1] * u[2] - f[2] * u[1];
	r[1] = f[2] * u[0] - f[0] * u[2];
	r[2] = f[0] * u[1] - f[1] * u[0];
	along = forward[0] * f[0] + forward[1] * f[1] + forward[2] * f[2];
	across = forward[0] * r[0] + forward[1] * r[1] + forward[2] * r[2];
	rise = forward[0] * u[0] + forward[1] * u[1] + forward[2] * u[2];
	/* (behind the camera has no turn to limit: the tracking's own loss takes the aim long before) */
	if (!(along > 1e-3f))
		return;
	turn = atan2f(across, along);
	elevation = atan2f(rise, sqrtf(along * along + across * across));
	if (turn_limit > 0.0f)
	{
		float past = fabsf(turn) - turn_limit;

		if (past > 0.0f)
			sink = fminf(past * 1.5f, CONTROLLER_WEAPON_LIMIT_SINK_DEGREES * (PI / 180.0f));
		turn = limited_angle(turn, turn_limit);
		elevation = limited_angle(elevation, CONTROLLER_WEAPON_LIMIT_ELEVATION_DEGREES * (PI / 180.0f)) - sink;
		elevation = fmaxf(elevation, -89.0f * (PI / 180.0f));
		for (axis = 0; axis < 3; axis++)
			limited[axis] = cosf(elevation) * (cosf(turn) * f[axis] + sinf(turn) * r[axis]) + sinf(elevation) * u[axis];
	}
	else
		memcpy(limited, forward, sizeof(limited));
	/* the up: carried to the limited forward (made square to it), then its
	roll off the view's up, about that forward, held within roll_limit */
	if (!squared_to(up, limited, up))
	{
		if (!squared_to(u, limited, up))
			return;
	}
	memcpy(forward, limited, sizeof(limited));
	if (roll_limit > 0.0f && squared_to(u, forward, level))
	{
		float side[3], held;

		/* side = forward x level; the up is cos(roll) level + sin(roll) side */
		side[0] = forward[1] * level[2] - forward[2] * level[1];
		side[1] = forward[2] * level[0] - forward[0] * level[2];
		side[2] = forward[0] * level[1] - forward[1] * level[0];
		roll = atan2f(up[0] * side[0] + up[1] * side[1] + up[2] * side[2],
			up[0] * level[0] + up[1] * level[1] + up[2] * level[2]);
		held = limited_angle(roll, roll_limit);
		for (axis = 0; axis < 3; axis++)
			up[axis] = cosf(held) * level[axis] + sinf(held) * side[axis];
	}
}

float controller_weapon_smooth_clamp(float seconds)
{
	if (seconds != seconds)
		return CONTROLLER_WEAPON_SMOOTH_DEFAULT;
	return fminf(fmaxf(seconds, 0.0f), CONTROLLER_WEAPON_SMOOTH_MAXIMUM);
}

/* v turned by yaw about +Z */
static void turned(const float v[3], float yaw, float out[3])
{
	float c = cosf(yaw), s = sinf(yaw);
	float x = v[0] * c - v[1] * s, y = v[0] * s + v[1] * c;

	out[0] = x;
	out[1] = y;
	out[2] = v[2];
}

void controller_weapon_smooth(struct controller_weapon_filter *filter, float body_yaw, float seconds,
	float time_delta, float sightline[3], float barrel_up[3])
{
	float forward[3], up[3], mixed[3], error[3], angle, fast, follow, length;
	int axis;

	if (!(seconds > 0.0f))
	{
		filter->known = 0;
		return;
	}
	turned(sightline, -body_yaw, forward);
	turned(barrel_up, -body_yaw, up);
	if (!filter->known || !(time_delta > 0.0f) || time_delta > 0.25f)
	{
		memcpy(filter->forward, forward, sizeof(forward));
		memcpy(filter->up, up, sizeof(up));
		filter->known = 1;
		return;
	}
	for (axis = 0; axis < 3; axis++)
		error[axis] = forward[axis] - filter->forward[axis];
	angle = length3(error) * (180.0f / PI);
	for (axis = 0; axis < 3; axis++)
		error[axis] = up[axis] - filter->up[axis];
	angle = fmaxf(angle, length3(error) * (180.0f / PI));
	fast = smoothstep((angle - CONTROLLER_WEAPON_SMOOTH_FREE_DEGREES) /
		(CONTROLLER_WEAPON_SMOOTH_FULL_DEGREES - CONTROLLER_WEAPON_SMOOTH_FREE_DEGREES));
	follow = 1.0f - expf(-time_delta / (seconds * (1.0f - fast) + CONTROLLER_WEAPON_SMOOTH_FAST_SECONDS * fast));
	for (axis = 0; axis < 3; axis++)
	{
		filter->forward[axis] += (forward[axis] - filter->forward[axis]) * follow;
		mixed[axis] = filter->up[axis] + (up[axis] - filter->up[axis]) * follow;
	}
	length = length3(filter->forward);
	if (length > 1e-6f)
		for (axis = 0; axis < 3; axis++)
			filter->forward[axis] /= length;
	if (!(length > 1e-6f) || !squared_to(mixed, filter->forward, filter->up))
	{
		/* (opposite ends, or the up along the forward: start again from the live frame) */
		memcpy(filter->forward, forward, sizeof(forward));
		memcpy(filter->up, up, sizeof(up));
		return;
	}
	turned(filter->forward, body_yaw, sightline);
	turned(filter->up, body_yaw, barrel_up);
}

/* the zoomed picture's projection: 0 behind or square to the camera or
for a degenerate camera; with clamp, each tangent onto the picture's edge;
without, 0 past limit times the tangents */
static int zoom_picture_project(const float direction[3], const float zoom_forward[3], const float zoom_up[3],
	const float tangents[2], float magnification, float limit, int clamp, float shown[3])
{
	float forward[3], up[3], right[3], length, along, across, upward, x, y, z;
	int axis;

	/* the zoomed camera's frame: forward unit, up made square to it, right
	forward x up (the game's +Y is left) */
	length = sqrtf(zoom_forward[0] * zoom_forward[0] + zoom_forward[1] * zoom_forward[1] +
		zoom_forward[2] * zoom_forward[2]);
	if (!(length > 1e-6f))
		return 0;
	for (axis = 0; axis < 3; axis++)
		forward[axis] = zoom_forward[axis] / length;
	along = zoom_up[0] * forward[0] + zoom_up[1] * forward[1] + zoom_up[2] * forward[2];
	for (axis = 0; axis < 3; axis++)
		up[axis] = zoom_up[axis] - along * forward[axis];
	length = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
	if (!(length > 1e-6f))
		return 0;
	for (axis = 0; axis < 3; axis++)
		up[axis] /= length;
	right[0] = forward[1] * up[2] - forward[2] * up[1];
	right[1] = forward[2] * up[0] - forward[0] * up[2];
	right[2] = forward[0] * up[1] - forward[1] * up[0];
	/* the direction's tangents in the zoomed picture; behind or square to
	the camera is outside it */
	along = direction[0] * forward[0] + direction[1] * forward[1] + direction[2] * forward[2];
	if (!(along > 1e-6f))
		return 0;
	across = (direction[0] * right[0] + direction[1] * right[1] + direction[2] * right[2]) / along;
	upward = (direction[0] * up[0] + direction[1] * up[1] + direction[2] * up[2]) / along;
	if (clamp)
	{
		across = fmaxf(-tangents[0], fminf(tangents[0], across));
		upward = fmaxf(-tangents[1], fminf(tangents[1], upward));
	}
	else if (!(fabsf(across) <= tangents[0] * limit) || !(fabsf(upward) <= tangents[1] * limit))
		return 0;
	/* the picture spans the view, so a tangent shows magnification times larger */
	if (!(magnification >= 1.0f))
		magnification = 1.0f;
	x = across * magnification;
	y = upward * magnification;
	z = -1.0f;
	length = sqrtf(x * x + y * y + z * z);
	shown[0] = x / length;
	shown[1] = y / length;
	shown[2] = z / length;
	return 1;
}

int controller_zoom_project(const float direction[3], const float zoom_forward[3], const float zoom_up[3],
	const float tangents[2], float magnification, int inside, float shown[3])
{
	/* the edge at 100% of each tangent while inside; back inside only
	within CONTROLLER_ZOOM_REENTRY of it, so a hand at the edge can't flap */
	return zoom_picture_project(direction, zoom_forward, zoom_up, tangents, magnification,
		inside ? 1.0f : CONTROLLER_ZOOM_REENTRY, 0, shown);
}

int controller_zoom_project_sight(const float direction[3], const float zoom_forward[3], const float zoom_up[3],
	const float tangents[2], float magnification, const struct controller_zoom_sight *sight, int inside,
	float shown[3])
{
	float limit = inside ? 1.0f : CONTROLLER_ZOOM_REENTRY, at[3], across, upward;

	if (!zoom_picture_project(direction, zoom_forward, zoom_up, tangents, magnification, limit, 0, at))
		return 0;
	/* where the reticle is drawn, as tangents off the view's center (at is
	unit, ahead: z is negative) */
	across = at[0] / -at[2];
	upward = at[1] / -at[2];
	if (!(across <= sight->right * limit) || !(-across <= sight->left * limit) ||
		!(upward <= sight->up * limit) || !(-upward <= sight->down * limit) ||
		!(sqrtf(across * across + upward * upward) <= sight->off_center * limit))
		return 0;
	memcpy(shown, at, sizeof(at));
	return 1;
}

int controller_zoom_project_clamped(const float direction[3], const float zoom_forward[3], const float zoom_up[3],
	const float tangents[2], float magnification, float shown[3])
{
	return zoom_picture_project(direction, zoom_forward, zoom_up, tangents, magnification, 1.0f, 1, shown);
}

void controller_zoom_stabilize(struct controller_zoom_filter *filter,
	struct halo_spatial_location *location, float magnification, float steady, float time_delta,
	int tracking_lost)
{
	float error[3], angle, intentional, seconds, follow, length, up[3];
	int axis, timed = time_delta > 0.0f && !(time_delta > 0.25f), active;

	/* the steadying eases from none after every reset */
	if (!filter->known || tracking_lost)
		filter->steady = 0.0f;
	if (!(steady > 0.0f))
		steady = 0.0f;
	if (timed && !tracking_lost)
	{
		filter->steady += (steady - filter->steady) * (1.0f - expf(-time_delta /
			(steady > filter->steady ? CONTROLLER_STEADY_RISE_SECONDS : CONTROLLER_STEADY_FALL_SECONDS)));
		/* (the tail of the ease out: 5 ms of low-pass left unzoomed) */
		if (steady == 0.0f && filter->steady < 0.05f)
			filter->steady = 0.0f;
	}
	active = magnification > 1.0f || filter->steady > 0.0f;
	if (tracking_lost || !active || !filter->known || filter->magnification != magnification || !timed)
	{
		filter->location = *location;
		filter->magnification = magnification;
		filter->known = active && !tracking_lost;
		return;
	}
	/* Chord length retains small-angle precision where acos(dot) rounds
	to zero. Work in the room so head turns and world snap turns never lag. */
	for (axis = 0; axis < 3; axis++)
		error[axis] = location->forward[axis] - filter->location.forward[axis];
	angle = length3(error) * (180.0f / PI);
	for (axis = 0; axis < 3; axis++)
		error[axis] = location->up[axis] - filter->location.up[axis];
	angle = fmaxf(angle, length3(error) * (180.0f / PI));
	intentional = smoothstep((angle - 0.20f) / 0.20f);
	seconds = magnification > 1.0f ? 0.03f + 0.01f * fminf(magnification, 10.0f) : 0.0f;
	if (filter->steady > 0.0f)
		seconds = fminf(seconds + filter->steady * (CONTROLLER_STEADY_SECONDS +
			CONTROLLER_STEADY_SECONDS_PER_MAGNIFICATION * fminf(fmaxf(magnification, 1.0f), 10.0f)),
			CONTROLLER_STEADY_SECONDS_LIMIT);
	seconds = seconds * (1.0f - intentional) + 0.002f * intentional;
	follow = intentional >= 1.0f ? 1.0f : 1.0f - expf(-time_delta / seconds);
	for (axis = 0; axis < 3; axis++)
	{
		/* Position is head-relative: filtering it would drag the barrel
		with a head translation. Keep its live value. */
		filter->location.position[axis] = location->position[axis];
		filter->location.forward[axis] += (location->forward[axis] - filter->location.forward[axis]) * follow;
		up[axis] = filter->location.up[axis] + (location->up[axis] - filter->location.up[axis]) * follow;
	}
	length = length3(filter->location.forward);
	if (!(length > 1e-6f))
	{
		filter->location = *location;
		return;
	}
	for (axis = 0; axis < 3; axis++)
		filter->location.forward[axis] /= length;
	if (!squared_to(up, filter->location.forward, filter->location.up))
	{
		filter->location = *location;
		return;
	}
	/* up x forward is left in the game's axes. */
	for (axis = 0; axis < 3; axis++)
	{
		int next = (axis + 1) % 3, last = (axis + 2) % 3;
		filter->location.left[axis] = filter->location.up[next] * filter->location.forward[last] -
			filter->location.up[last] * filter->location.forward[next];
	}
	*location = filter->location;
}

float controller_steady_clamp(float setting)
{
	if (setting != setting)
		return CONTROLLER_STEADY_DEFAULT;
	return fminf(fmaxf(setting, 0.0f), CONTROLLER_STEADY_MAXIMUM);
}

float controller_steady_strength(float setting, float assist, float on_target, int hand_aims, int fixed_rules)
{
	if (!hand_aims || fixed_rules || !(on_target > 0.0f) || !(assist > 0.0f))
		return 0.0f;
	return controller_steady_clamp(setting) * fminf(assist, 2.0f) * fminf(on_target, 1.0f);
}

float controller_steady_by_zoom(float unzoomed, float at_2x, float at_8x, float magnification)
{
	unzoomed = controller_steady_clamp(unzoomed);
	at_2x = controller_steady_clamp(at_2x);
	at_8x = controller_steady_clamp(at_8x);
	if (!(magnification > 1.0f))
		return unzoomed;
	if (magnification <= 2.0f)
		return at_2x;
	if (magnification >= 8.0f)
		return at_8x;
	return at_2x + (at_8x - at_2x) * (magnification - 2.0f) / 6.0f;
}

float controller_steady_cycle(float setting)
{
	setting = controller_steady_clamp(setting);
	if (setting < CONTROLLER_STEADY_DEFAULT)
		return CONTROLLER_STEADY_DEFAULT;
	if (setting < CONTROLLER_STEADY_MAXIMUM)
		return CONTROLLER_STEADY_MAXIMUM;
	return 0.0f;
}

int controller_laser_mode(const char *text)
{
	if (!strcmp(text, "close"))
		return CONTROLLER_LASER_CLOSE;
	if (!strcmp(text, "on") || !strcmp(text, "true") || !strcmp(text, "always"))
		return CONTROLLER_LASER_ON;
	return CONTROLLER_LASER_OFF;
}

const char *controller_laser_mode_name(int mode)
{
	return mode == CONTROLLER_LASER_CLOSE ? "close" : mode == CONTROLLER_LASER_ON ? "on" : "off";
}

int controller_laser_cycle(int mode)
{
	return mode == CONTROLLER_LASER_OFF ? CONTROLLER_LASER_CLOSE : mode == CONTROLLER_LASER_CLOSE ?
		CONTROLLER_LASER_ON : CONTROLLER_LASER_OFF;
}

float controller_laser_distance_clamp(float meters)
{
	if (meters != meters)
		return CONTROLLER_LASER_DISTANCE_DEFAULT;
	return fminf(fmaxf(meters, CONTROLLER_LASER_DISTANCE_MINIMUM), CONTROLLER_LASER_DISTANCE_MAXIMUM);
}

float controller_laser_opacity(int mode, float distance, float reticle_meters, int past_cover)
{
	float inside;

	if (past_cover || mode == CONTROLLER_LASER_ON)
		return 1.0f;
	if (mode != CONTROLLER_LASER_CLOSE || reticle_meters != reticle_meters)
		return 0.0f;
	inside = controller_laser_distance_clamp(distance) - reticle_meters;
	return fminf(fmaxf(inside / CONTROLLER_LASER_FADE_METERS, 0.0f), 1.0f);
}

float controller_laser_ease(float opacity, float target, float seconds)
{
	if (!(seconds > 0.0f) || opacity != opacity)
		return target;
	opacity += (target - opacity) * (1.0f - expf(-seconds / CONTROLLER_LASER_EASE_SECONDS));
	return target <= 0.0f && opacity < CONTROLLER_LASER_OPACITY_MINIMUM ? 0.0f : opacity;
}

float controller_origin_raise_clamp(float raise)
{
	if (raise != raise)
		return 0.0f;
	return fminf(fmaxf(raise, 0.0f), 1.0f);
}

float controller_origin_raise_step(float raise, int direction)
{
	float tenths = roundf(controller_origin_raise_clamp(raise) / CONTROLLER_ORIGIN_RAISE_STEP) + (direction > 0 ? 1.0f : -1.0f);

	return fminf(fmaxf(tenths, 0.0f), roundf(1.0f / CONTROLLER_ORIGIN_RAISE_STEP)) * CONTROLLER_ORIGIN_RAISE_STEP;
}

void controller_origin_raise(float origin[3], const float head[3], float raise)
{
	int axis;

	if (!(raise > 0.0f))
		return;
	if (raise > 1.0f)
		raise = 1.0f;
	for (axis = 0; axis < 3; axis++)
		origin[axis] = raise >= 1.0f ? head[axis] : origin[axis] + (head[axis] - origin[axis]) * raise;
}
