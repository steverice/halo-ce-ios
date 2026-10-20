/* The Sense controllers' buttons onto the Xbox gamepad
(port/ios/host/host_spatial_gamepad.c, included): every row of the spec's
table right-handed, the mirror for input.aim_hand = "left", analog buttons
255 or 0, the triggers' scale and the sticks as signed 16-bit; and the
buttons' read clock (aligned or live, never stepping back); and the game's
rumble onto the two controllers (each motor's side and sharpness, the mirror,
input.controller_rumble's scale, and when stale levels stop). */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../host/host_spatial_gamepad.c"

/* XINPUT_GAMEPAD's (port/include/xdk/xdk_xbox.h) */
enum { A, B, X, Y, BLACK, WHITE, LEFT_TRIGGER, RIGHT_TRIGGER };
#define DPAD_MASK 0x000f
#define START 0x0010
#define BACK 0x0020
#define LEFT_THUMB 0x0040
#define RIGHT_THUMB 0x0080

static int failures;

static void check(int condition, const char *what)
{
	if (!condition)
	{
		printf("FAILED: %s\n", what);
		failures++;
	}
}

static const char *const analog_names[8] = { "A", "B", "X", "Y", "black", "white", "left trigger", "right trigger" };

/* the map of one press, aiming with hand (0 left, 1 right); side 0 presses
on the left controller, 1 on the right */
static struct halo_spatial_gamepad press(int hand, int side, const struct host_spatial_controls *controls)
{
	struct host_spatial_controls idle, left, right;
	struct halo_spatial_gamepad gamepad;

	memset(&idle, 0, sizeof(idle));
	left = side == 0 ? *controls : idle;
	right = side == 1 ? *controls : idle;
	memset(&gamepad, 0x5a, sizeof(gamepad));
	host_spatial_gamepad_map(&left, &right, hand, &gamepad);
	return gamepad;
}

/* only analog[index] is set, to value; no button, the sticks at rest */
static void expect_analog(const struct halo_spatial_gamepad *gamepad, int index, int value, const char *what)
{
	char text[160];
	int other;

	snprintf(text, sizeof(text), "%s gives %s %d (got %d)", what, analog_names[index], value, gamepad->analog[index]);
	check(gamepad->analog[index] == value, text);
	for (other = 0; other < 8; other++)
		if (other != index && gamepad->analog[other])
		{
			snprintf(text, sizeof(text), "%s leaves %s alone (got %d)", what, analog_names[other],
				gamepad->analog[other]);
			check(0, text);
		}
	snprintf(text, sizeof(text), "%s presses no button (got 0x%04x)", what, gamepad->buttons);
	check(gamepad->buttons == 0, text);
	snprintf(text, sizeof(text), "%s leaves the sticks at rest", what);
	check(!gamepad->thumb[0] && !gamepad->thumb[1] && !gamepad->thumb[2] && !gamepad->thumb[3], text);
}

/* only the button mask is set; no analog button, the sticks at rest */
static void expect_button(const struct halo_spatial_gamepad *gamepad, int mask, const char *what)
{
	char text[160];
	int index, analog = 0;

	snprintf(text, sizeof(text), "%s gives buttons 0x%04x (got 0x%04x)", what, mask, gamepad->buttons);
	check(gamepad->buttons == mask, text);
	for (index = 0; index < 8; index++)
		analog |= gamepad->analog[index];
	snprintf(text, sizeof(text), "%s presses no analog button", what);
	check(!analog, text);
	snprintf(text, sizeof(text), "%s leaves the sticks at rest", what);
	check(!gamepad->thumb[0] && !gamepad->thumb[1] && !gamepad->thumb[2] && !gamepad->thumb[3], text);
}

/* the table's rows, for one aiming hand: the aiming controller's side is
the right one's when hand is 1 */
static void table(int hand)
{
	int aiming = hand, off = !hand;
	const char *const hand_names[2] = { "left-handed", "right-handed" };
	struct host_spatial_controls controls;
	struct halo_spatial_gamepad gamepad;
	char what[96];

#define ROW(side, field, value, label) \
	do { \
		memset(&controls, 0, sizeof(controls)); \
		controls.field = value; \
		gamepad = press(hand, side, &controls); \
		snprintf(what, sizeof(what), "%s: %s", hand_names[hand], label); \
	} while (0)

	/* the triggers, scaled; full, 255 */
	ROW(aiming, trigger, 0.6f, "the aiming trigger at 0.6");
	expect_analog(&gamepad, RIGHT_TRIGGER, 153, what);
	ROW(aiming, trigger, 1.0f, "the aiming trigger full");
	expect_analog(&gamepad, RIGHT_TRIGGER, 255, what);
	ROW(off, trigger, 0.6f, "the other trigger at 0.6");
	expect_analog(&gamepad, LEFT_TRIGGER, 153, what);
	ROW(off, trigger, 1.0f, "the other trigger full");
	expect_analog(&gamepad, LEFT_TRIGGER, 255, what);
	/* the grips: melee and the flashlight, 255 pressed */
	ROW(aiming, grip, 1.0f, "the aiming grip (melee)");
	expect_analog(&gamepad, B, 255, what);
	ROW(off, grip, 1.0f, "the other grip (flashlight)");
	expect_analog(&gamepad, WHITE, 255, what);
	/* the face buttons */
	ROW(aiming, face_low, 1, "the aiming low face button (Cross: jump)");
	expect_analog(&gamepad, A, 255, what);
	ROW(aiming, face_high, 1, "the aiming high face button (Circle: action and reload)");
	expect_analog(&gamepad, X, 255, what);
	ROW(off, face_low, 1, "the other low face button (Square: switch weapon)");
	expect_analog(&gamepad, Y, 255, what);
	ROW(off, face_high, 1, "the other high face button (Triangle: switch grenades)");
	expect_analog(&gamepad, BLACK, 255, what);
	/* the stick clicks and the menus */
	ROW(off, stick_click, 1, "the other stick's click (crouch)");
	expect_button(&gamepad, LEFT_THUMB, what);
	ROW(aiming, stick_click, 1, "the aiming stick's click (zoom)");
	expect_button(&gamepad, RIGHT_THUMB, what);
	ROW(aiming, menu, 1, "the aiming menu button (Options: start)");
	expect_button(&gamepad, START, what);
	ROW(off, menu, 1, "the other menu button (Create: back)");
	expect_button(&gamepad, BACK, what);
	/* the sticks: the other moves (left thumb), the aiming one turns (right
	thumb), signed 16-bit, up positive */
	memset(&controls, 0, sizeof(controls));
	controls.stick_x = 1.0f;
	controls.stick_y = -1.0f;
	gamepad = press(hand, off, &controls);
	snprintf(what, sizeof(what), "%s: the other stick right and down is the left thumb (32767, -32767)",
		hand_names[hand]);
	check(gamepad.thumb[0] == 32767 && gamepad.thumb[1] == -32767 && !gamepad.thumb[2] && !gamepad.thumb[3], what);
	snprintf(what, sizeof(what), "%s: the other stick presses nothing", hand_names[hand]);
	check(!gamepad.buttons && !gamepad.analog[A] && !gamepad.analog[LEFT_TRIGGER], what);
	controls.stick_x = -1.0f;
	controls.stick_y = 1.0f;
	gamepad = press(hand, aiming, &controls);
	snprintf(what, sizeof(what), "%s: the aiming stick left and up is the right thumb (-32767, 32767)",
		hand_names[hand]);
	check(!gamepad.thumb[0] && !gamepad.thumb[1] && gamepad.thumb[2] == -32767 && gamepad.thumb[3] == 32767, what);
	controls.stick_x = 0.5f;
	controls.stick_y = -0.25f;
	gamepad = press(hand, aiming, &controls);
	snprintf(what, sizeof(what), "%s: the aiming stick at (0.5, -0.25) is (16384, -8192) within one (got %d, %d)",
		hand_names[hand], gamepad.thumb[2], gamepad.thumb[3]);
	check(gamepad.thumb[2] >= 16383 && gamepad.thumb[2] <= 16384 && gamepad.thumb[3] >= -8192 &&
		gamepad.thumb[3] <= -8191, what);
	/* past the ends, held at them */
	controls.stick_x = 1.5f;
	controls.stick_y = -1.5f;
	gamepad = press(hand, aiming, &controls);
	snprintf(what, sizeof(what), "%s: a stick past its ends is held at 32767 and -32767", hand_names[hand]);
	check(gamepad.thumb[2] == 32767 && gamepad.thumb[3] == -32767, what);
#undef ROW
}

/* nothing held: an all-zero gamepad, whatever the buffer held before */
static void idle(void)
{
	struct host_spatial_controls left, right;
	struct halo_spatial_gamepad gamepad, zero;
	int hand;

	memset(&left, 0, sizeof(left));
	memset(&right, 0, sizeof(right));
	memset(&zero, 0, sizeof(zero));
	for (hand = 0; hand < 2; hand++)
	{
		memset(&gamepad, 0xa5, sizeof(gamepad));
		host_spatial_gamepad_map(&left, &right, hand, &gamepad);
		check(!memcmp(&gamepad, &zero, sizeof(zero)), hand ? "right-handed: nothing held is all zero" :
			"left-handed: nothing held is all zero");
	}
}

/* analog buttons are 255 or 0: a grip read as an analog value presses past
half, and a released trigger is 0 */
static void analog_buttons(void)
{
	struct host_spatial_controls left, right;
	struct halo_spatial_gamepad gamepad;

	memset(&left, 0, sizeof(left));
	memset(&right, 0, sizeof(right));
	right.grip = 0.4f;
	left.grip = 0.4f;
	host_spatial_gamepad_map(&left, &right, 1, &gamepad);
	check(gamepad.analog[B] == 0 && gamepad.analog[WHITE] == 0, "a grip under half isn't pressed");
	right.grip = 0.7f;
	left.grip = 0.7f;
	host_spatial_gamepad_map(&left, &right, 1, &gamepad);
	check(gamepad.analog[B] == 255 && gamepad.analog[WHITE] == 255, "a grip past half is 255");
	right.face_low = 7;
	host_spatial_gamepad_map(&left, &right, 1, &gamepad);
	check(gamepad.analog[A] == 255, "any nonzero face button is 255");
}

/* everything at once, right-handed: each row lands on its own button, with
the buttons' bits ORed and every analog button set */
static void everything(void)
{
	struct host_spatial_controls left, right;
	struct halo_spatial_gamepad gamepad;
	int index;

	left.trigger = right.trigger = 1.0f;
	left.grip = right.grip = 1.0f;
	left.face_low = left.face_high = right.face_low = right.face_high = 1;
	left.stick_click = left.menu = right.stick_click = right.menu = 1;
	left.stick_x = 0.0f;
	left.stick_y = 1.0f;
	right.stick_x = 1.0f;
	right.stick_y = 0.0f;
	host_spatial_gamepad_map(&left, &right, 1, &gamepad);
	check(gamepad.buttons == (START | BACK | LEFT_THUMB | RIGHT_THUMB), "everything held: start, back, both thumbs");
	check(!(gamepad.buttons & DPAD_MASK), "the Sense controllers have no D-pad");
	for (index = 0; index < 8; index++)
		check(gamepad.analog[index] == 255, "everything held: every analog button 255");
	check(gamepad.thumb[0] == 0 && gamepad.thumb[1] == 32767 && gamepad.thumb[2] == 32767 && gamepad.thumb[3] == 0,
		"everything held: the left stick up, the right stick right");
	check(gamepad.pad == 0, "the padding is zero");
}

/* the buttons' read clock: live to start, aligned only after three fresh
samples, live at once on a stale anchor or a failed read, and the read
time never steps back */
static void button_clock(void)
{
	struct host_spatial_button_clock clock;
	double read_time, previous = 0.0, now;
	int index, aligned;
	char what[160];
	/* a sequence of (now, anchor) over 30 samples 16 ms apart: fresh (20 ms
	old), then stale (no new anchors for 200 ms, the anchor frozen), then
	fresh again, with the anchors 60 ms old, older than the last live read */
	struct { double now, anchor; int aligned; } steps[] =
	{
		{ 1.000, 0.000, 0 },   /* no anchor yet: live */
		{ 1.016, 0.996, 0 },   /* fresh 1 */
		{ 1.032, 1.012, 0 },   /* fresh 2 */
		{ 1.048, 1.028, 1 },   /* fresh 3: aligned */
		{ 1.064, 1.044, 1 },
		{ 1.080, 1.044, 1 },   /* 36 ms old: still fresh */
		{ 1.160, 1.044, 0 },   /* 116 ms old: live */
		{ 1.176, 1.116, 0 },   /* fresh 1 (60 ms old) */
		{ 1.192, 1.132, 0 },   /* fresh 2 */
		{ 1.208, 1.148, 1 },   /* fresh 3: aligned, held at 1.192 (the last live read) */
		{ 1.224, 1.164, 1 },   /* held at 1.192 */
		{ 1.240, 1.200, 1 },   /* past it: 1.200 */
		{ 1.256, 1.216, 1 },
		{ 1.272, 1.290, 0 },   /* an anchor ahead of now (another clock): live */
		{ 1.288, 1.268, 0 },   /* fresh 1 */
	};

	memset(&clock, 0, sizeof(clock));
	for (index = 0; index < (int)(sizeof(steps) / sizeof(steps[0])); index++)
	{
		aligned = host_spatial_button_clock_choose(&clock, steps[index].anchor, steps[index].now, &read_time);
		snprintf(what, sizeof(what), "the clock's step %d is %s (got %s)", index, steps[index].aligned ? "aligned" :
			"live", aligned ? "aligned" : "live");
		check(aligned == steps[index].aligned, what);
		snprintf(what, sizeof(what), "the clock's step %d never reads before the last (%.3f after %.3f)", index,
			read_time, previous);
		check(read_time >= previous, what);
		previous = read_time;
	}
	/* the held and released reads */
	memset(&clock, 0, sizeof(clock));
	for (index = 0; index < 10; index++)
		host_spatial_button_clock_choose(&clock, 1.176 + 0.016 * index - 0.060, 1.176 + 0.016 * index, &read_time);
	check(read_time > 1.176 + 0.016 * 9 - 0.061 && read_time < 1.176 + 0.016 * 9 - 0.059,
		"aligned long enough, the read is the anchor's own time");
	memset(&clock, 0, sizeof(clock));
	host_spatial_button_clock_choose(&clock, 0.0, 1.0, &read_time);
	check(read_time == 1.0, "a live read is at now");
	/* a failed aligned read: live at once, three fresh samples to come back */
	memset(&clock, 0, sizeof(clock));
	now = 2.0;
	for (index = 0; index < 4; index++, now += 0.016)
		host_spatial_button_clock_choose(&clock, now - 0.010, now, &read_time);
	check(clock.aligned == 1, "four fresh samples are aligned");
	host_spatial_button_clock_failed(&clock, now);
	check(clock.aligned == 0 && clock.last_read == now, "a failed read goes live, at now");
	previous = clock.last_read;
	for (index = 0; index < 3; index++)
	{
		now += 0.016;
		aligned = host_spatial_button_clock_choose(&clock, now - 0.030, now, &read_time);
		check(read_time >= previous, "after a failed read, the read never steps back");
		previous = read_time;
	}
	check(aligned == 1, "three fresh samples after a failed read are aligned again");
}

static int near(float value, float wanted)
{
	return fabsf(value - wanted) < 1e-5f;
}

/* the game's rumble: each motor on its side's controller with its own
sharpness, mirrored with the aiming hand, scaled and clamped */
static void rumble(void)
{
	struct host_spatial_rumble out;
	float zero = 0.0f, not_a_number = zero / zero;

	host_spatial_rumble_map(0.25f, 0.5f, 1, 1.0f, &out);
	check(near(out.intensity[0], 0.25f) && near(out.intensity[1], 0.5f),
		"right-handed, the left motor is the left controller's and the right motor the right's");
	check(near(out.sharpness[0], HOST_SPATIAL_RUMBLE_LOW_SHARPNESS) &&
		near(out.sharpness[1], HOST_SPATIAL_RUMBLE_HIGH_SHARPNESS),
		"right-handed, the left controller is dull and the right sharp");
	host_spatial_rumble_map(0.25f, 0.5f, 0, 1.0f, &out);
	check(near(out.intensity[1], 0.25f) && near(out.intensity[0], 0.5f),
		"left-handed, the motors swap controllers");
	check(near(out.sharpness[1], HOST_SPATIAL_RUMBLE_LOW_SHARPNESS) &&
		near(out.sharpness[0], HOST_SPATIAL_RUMBLE_HIGH_SHARPNESS),
		"left-handed, the sharp motor stays with the aiming hand");
	host_spatial_rumble_map(0.0f, 0.0f, 1, 1.0f, &out);
	check(out.intensity[0] == 0.0f && out.intensity[1] == 0.0f, "still motors are silent");
	host_spatial_rumble_map(1.0f, 0.0f, 1, 1.0f, &out);
	check(near(out.intensity[0], 1.0f) && out.intensity[1] == 0.0f, "one motor alone leaves the other hand silent");
	/* the strength */
	host_spatial_rumble_map(0.5f, 1.0f, 1, 0.0f, &out);
	check(out.intensity[0] == 0.0f && out.intensity[1] == 0.0f, "strength 0 is off");
	host_spatial_rumble_map(0.5f, 1.0f, 1, 0.5f, &out);
	check(near(out.intensity[0], 0.25f) && near(out.intensity[1], 0.5f), "strength 0.5 halves both");
	host_spatial_rumble_map(0.25f, 0.75f, 1, 2.0f, &out);
	check(near(out.intensity[0], 0.5f) && near(out.intensity[1], 1.0f), "strength 2 doubles, to at most 1");
	host_spatial_rumble_map(0.25f, 0.25f, 1, 100.0f, &out);
	check(near(out.intensity[0], 0.5f) && near(out.intensity[1], 0.5f), "a strength over the maximum is the maximum");
	host_spatial_rumble_map(0.5f, 0.5f, 1, -1.0f, &out);
	check(out.intensity[0] == 0.0f && out.intensity[1] == 0.0f, "a negative strength is off");
	host_spatial_rumble_map(0.5f, 0.5f, 1, not_a_number, &out);
	check(out.intensity[0] == 0.0f && out.intensity[1] == 0.0f, "a NaN strength is off");
	/* motors outside 0 to 1 */
	host_spatial_rumble_map(7.0f, -3.0f, 1, 1.0f, &out);
	check(near(out.intensity[0], 1.0f) && out.intensity[1] == 0.0f, "motors are clamped to 0 to 1");
	host_spatial_rumble_map(not_a_number, not_a_number, 1, 1.0f, &out);
	check(out.intensity[0] == 0.0f && out.intensity[1] == 0.0f, "NaN motors are silent");
	/* a stalled game's levels go stale */
	check(host_spatial_rumble_fresh(10.0, 10.0) && host_spatial_rumble_fresh(10.0, 10.1),
		"levels a frame or a hitch old still play");
	check(!host_spatial_rumble_fresh(10.0, 10.0 + HOST_SPATIAL_RUMBLE_STALE_SECONDS) &&
		!host_spatial_rumble_fresh(10.0, 60.0), "levels a stalled game left behind don't");
}

int main(void)
{
	rumble();
	button_clock();
	table(1);
	table(0);
	idle();
	analog_buttons();
	everything();
	if (failures)
	{
		printf("spatial gamepad probe: %d FAILED\n", failures);
		return 1;
	}
	printf("spatial gamepad probe: OK\n");
	return 0;
}
