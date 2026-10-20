/* The Sense controllers onto the Xbox gamepad (host_spatial_gamepad.h). */
#include "host_spatial_gamepad.h"
#include <math.h>
#include <string.h>

/* XINPUT_GAMEPAD's (port/include/xdk/xdk_xbox.h): the buttons' bits and the
analog buttons' order */
#define GAMEPAD_START 0x0010
#define GAMEPAD_BACK 0x0020
#define GAMEPAD_LEFT_THUMB 0x0040
#define GAMEPAD_RIGHT_THUMB 0x0080
enum { ANALOG_A, ANALOG_B, ANALOG_X, ANALOG_Y, ANALOG_BLACK, ANALOG_WHITE, ANALOG_LEFT_TRIGGER, ANALOG_RIGHT_TRIGGER };

/* a grip (a button GameController may read as a value) is pressed past half */
#define GRIP_PRESSED 0.5f

/* an analog button: 255 pressed, 0 released */
static uint8_t pressed(int down)
{
	return down ? 255 : 0;
}

/* a trigger, 0 to 1, as 0 to 255 */
static uint8_t trigger(float value)
{
	if (!(value > 0.0f))
		return 0;
	if (value >= 1.0f)
		return 255;
	return (uint8_t)lroundf(value * 255.0f);
}

/* a stick axis, -1 to 1, as signed 16-bit (1 is 32767, -1 is -32767) */
static int16_t stick(float value)
{
	if (!(value == value))
		return 0;
	if (value > 1.0f)
		value = 1.0f;
	if (value < -1.0f)
		value = -1.0f;
	return (int16_t)lroundf(value * 32767.0f);
}

void host_spatial_gamepad_map(const struct host_spatial_controls *left, const struct host_spatial_controls *right,
	int aiming_hand, struct halo_spatial_gamepad *gamepad)
{
	/* the aiming hand's controller takes the right side of the table, the
	other the left: a left-handed player's left controller is the table's
	right */
	const struct host_spatial_controls *aiming = aiming_hand ? right : left;
	const struct host_spatial_controls *other = aiming_hand ? left : right;

	memset(gamepad, 0, sizeof(*gamepad));
	/* fire and the grenade */
	gamepad->analog[ANALOG_RIGHT_TRIGGER] = trigger(aiming->trigger);
	gamepad->analog[ANALOG_LEFT_TRIGGER] = trigger(other->trigger);
	/* melee and the flashlight */
	gamepad->analog[ANALOG_B] = pressed(aiming->grip > GRIP_PRESSED);
	gamepad->analog[ANALOG_WHITE] = pressed(other->grip > GRIP_PRESSED);
	/* jump and action/reload under the aiming thumb; switch weapon and
	switch grenades under the other */
	gamepad->analog[ANALOG_A] = pressed(aiming->face_low);
	gamepad->analog[ANALOG_X] = pressed(aiming->face_high);
	gamepad->analog[ANALOG_Y] = pressed(other->face_low);
	gamepad->analog[ANALOG_BLACK] = pressed(other->face_high);
	/* move and crouch; turn and zoom; start and back */
	if (other->stick_click)
		gamepad->buttons |= GAMEPAD_LEFT_THUMB;
	if (aiming->stick_click)
		gamepad->buttons |= GAMEPAD_RIGHT_THUMB;
	if (aiming->menu)
		gamepad->buttons |= GAMEPAD_START;
	if (other->menu)
		gamepad->buttons |= GAMEPAD_BACK;
	gamepad->thumb[0] = stick(other->stick_x);
	gamepad->thumb[1] = stick(other->stick_y);
	gamepad->thumb[2] = stick(aiming->stick_x);
	gamepad->thumb[3] = stick(aiming->stick_y);
}

int host_spatial_button_clock_choose(struct host_spatial_button_clock *clock, double anchor_timestamp, double now,
	double *read_time)
{
	double age = now - anchor_timestamp;
	int fresh = anchor_timestamp > 0.0 && age >= 0.0 && age <= HOST_SPATIAL_ALIGNED_SECONDS;

	if (!fresh)
	{
		clock->aligned = 0;
		clock->fresh_samples = 0;
	}
	else if (!clock->aligned && ++clock->fresh_samples >= HOST_SPATIAL_FRESH_SAMPLES)
		clock->aligned = 1;
	*read_time = clock->aligned ? anchor_timestamp : now;
	/* never back past the last read */
	if (*read_time < clock->last_read)
		*read_time = clock->last_read;
	clock->last_read = *read_time;
	return clock->aligned;
}

void host_spatial_button_clock_failed(struct host_spatial_button_clock *clock, double now)
{
	clock->aligned = 0;
	clock->fresh_samples = 0;
	if (now > clock->last_read)
		clock->last_read = now;
}

/* a level, 0 to 1 (a NaN is 0) */
static float unit(float value)
{
	if (!(value > 0.0f))
		return 0.0f;
	return value < 1.0f ? value : 1.0f;
}

void host_spatial_rumble_map(float left_motor, float right_motor, int aiming_hand, float strength,
	struct host_spatial_rumble *rumble)
{
	/* as the buttons: the aiming hand's controller is the pad's right side */
	int right_side = aiming_hand ? 1 : 0, left_side = !right_side;

	if (!(strength > 0.0f))
		strength = 0.0f;
	if (strength > HOST_SPATIAL_RUMBLE_STRENGTH_MAXIMUM)
		strength = HOST_SPATIAL_RUMBLE_STRENGTH_MAXIMUM;
	rumble->intensity[left_side] = unit(unit(left_motor) * strength);
	rumble->sharpness[left_side] = HOST_SPATIAL_RUMBLE_LOW_SHARPNESS;
	rumble->intensity[right_side] = unit(unit(right_motor) * strength);
	rumble->sharpness[right_side] = HOST_SPATIAL_RUMBLE_HIGH_SHARPNESS;
}

int host_spatial_rumble_fresh(double sent, double now)
{
	return now - sent < HOST_SPATIAL_RUMBLE_STALE_SECONDS;
}
