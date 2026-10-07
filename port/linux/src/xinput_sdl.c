/*
XINPUT_SDL.C

Xbox controllers and the debug keyboard for the Linux build.

Port 0 is always connected: it is the keyboard and mouse, merged with the
first SDL gamepad when one is present. Further SDL gamepads take ports 1-3.

In the game the keyboard and mouse are a control scheme of their own
(port/linux/include/halo_keyboard.h): config.toml's [controls] bind each of
the player's actions to up to two keys, mouse buttons or wheel turns
(Settings > Controls Setup changes them), and the game takes the actions
held (halo_keyboard_actions) with the controller's. They press none of its
buttons, but for the pause menu's and the scoreboard's (Start and Back).

In the menus the keys drive the controller, to move about them:
	arrows           D-pad               W A S D          left stick
	space, enter     A                   escape, backspace B
	delete, E        X                   tab              Y
	F1               back
(keys held as the game and the menus switch count only once let go of), the
on-screen keyboard takes what is typed, and the mouse is free and drives a
pointer
(port/linux/include/halo_ui_pointer.h, source/interface/ui_widget.c).
F11 switches between fullscreen and the window, and F12 releases or
recaptures the mouse, always.

Mouse aim does not go through the right stick: the game's look code asks
halo_linux_mouse_look for the motion since its last call and adds it to the
stick's facing change, so aiming is direct rather than rate based.

The game's debug keyboard exists only for the console. Backquote (which
opens it) always reaches the keystroke queue, everything else only while
the console is open, since the game also polls a few keys directly (escape
returns to the main menu). While the console is open the keyboard does not
drive the controller.
*/

#include "platform.h"
#include "sdl_platform.h"
#include "port_config.h"
/* port/linux/game/network_aim_assist.c */
extern unsigned char network_aim_assist_block_mouse(void);
#include "halo_keyboard.h"
#include "mp_benchmark_probe.h"
#include "benchmark_mp_arena.h"
#include "halo_spatial.h"
#include "knob_cycle.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORT_COUNT 4
#define VK_OEM_3_BACKQUOTE 0xc0

/* ---------- game hooks */

/* main/console.c */
extern unsigned char console_is_active(void);
/* input_replay.c */
void input_replay_filter(XINPUT_GAMEPAD *pad);
/* d3d8_device.c */
void halo_render_scale_step(int direction);
/* xbox_textures.c */
void halo_texture_upscale_toggle(void);

/* d3d8_device.c */
void halo_anti_aliasing_toggle(void);

/* while Back is held, R1 (the Xbox's Black) switches FXAA on and off
(halo_anti_aliasing_toggle), and the game doesn't see R1 */
static void anti_aliasing_chord(XINPUT_GAMEPAD *pad)
{
	static int held;
	int pressed = pad->bAnalogButtons[XINPUT_GAMEPAD_BLACK] > 64;

	if (!(pad->wButtons & XINPUT_GAMEPAD_BACK))
	{
		held = 0;
		return;
	}
	if (pressed && !held)
		halo_anti_aliasing_toggle();
	held = pressed;
	pad->bAnalogButtons[XINPUT_GAMEPAD_BLACK] = 0;
}

#ifdef HALO_IOS
int host_external_cycle(int direction);
int host_external_force(const char *mode);
void host_external_mode_label(char *text, unsigned long size);
void hud_print_message(short local_player_index, wchar_t const *message);

static void external_mode_chord(XINPUT_GAMEPAD *pad)
{
	static WORD previous;
	const char *modes = config_string("debug.external_mode");
	if (!modes || !*modes || !(pad->wButtons & XINPUT_GAMEPAD_BACK))
	{
		previous = 0;
		return;
	}
	WORD pressed = pad->wButtons & ~previous;
	previous = pad->wButtons;
	int direction = (pressed & XINPUT_GAMEPAD_DPAD_UP) ? 1 : (pressed & XINPUT_GAMEPAD_DPAD_DOWN) ? -1 : 0;
	if (direction)
	{
		char text[160];
		wchar_t message[160];
		host_external_force(modes);
		if (host_external_cycle(direction))
		{
			host_external_mode_label(text, sizeof(text));
			for (unsigned int i = 0; i < sizeof(message) / sizeof(*message); i++)
			{
				message[i] = (unsigned char)text[i];
				if (!text[i]) break;
			}
			message[159] = 0;
			hud_print_message(0, message);
		}
	}
	pad->wButtons &= (WORD)~(XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN);
}
#endif

/* While Back is held: debug.render_scale_dpad's D-pad left and right step
the render scale down and up, debug.upscaled_textures_toggle's D-pad up
switches between the level's upscaled and original textures, and with
either on the game doesn't see the D-pad */
static void back_dpad(XINPUT_GAMEPAD *pad)
{
	static int render_scale = -1, upscaled_textures;
	static WORD previous;
	WORD pressed;

	if (render_scale < 0)
	{
		render_scale = config_boolean("debug.render_scale_dpad");
		upscaled_textures = config_boolean("debug.upscaled_textures_toggle");
	}
	if (!render_scale && !upscaled_textures)
		return;
	if (!(pad->wButtons & XINPUT_GAMEPAD_BACK))
	{
		previous = 0;
		return;
	}
	pressed = pad->wButtons & ~previous;
	previous = pad->wButtons;
	if (render_scale && (pressed & XINPUT_GAMEPAD_DPAD_LEFT))
		halo_render_scale_step(-1);
	if (render_scale && (pressed & XINPUT_GAMEPAD_DPAD_RIGHT))
		halo_render_scale_step(1);
	if (upscaled_textures && (pressed & XINPUT_GAMEPAD_DPAD_UP))
		halo_texture_upscale_toggle();
	pad->wButtons &= (WORD)~(XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT |
		XINPUT_GAMEPAD_DPAD_RIGHT);
}
/* d3d8_device.c */
int halo_knob_apply(const char *name, const char *value);
void halo_knob_message(const char *text);

static void knob_current(const struct knob *knob, char *text, unsigned long size)
{
	if (knob->kind == KNOB_INTEGER)
		snprintf(text, size, "%ld", config_integer(knob->name));
	else if (knob->kind == KNOB_BOOLEAN)
		snprintf(text, size, "%s", config_boolean(knob->name) ? "true" : "false");
	else
		snprintf(text, size, "%s", config_string(knob->name));
}

/* debug.knob_cycle: while Back is held, the D-pad's up and down choose a quality setting (render lines, shadow map
size, anisotropy, model detail, anti-aliasing, effect and mirror resolution; knob_cycle.h) and left and right step
its value, applied from the next frame and shown on screen for two seconds. The game never sees the D-pad while
Back is held. It runs ahead of the other D-pad chords (debug.render_scale_dpad, debug.external_mode and
debug.sense_tuning_chord's D-pad pitch step), which get no D-pad presses while it is on. */
static void knob_cycle_chord(XINPUT_GAMEPAD *pad)
{
	static int enabled = -1;
	static WORD previous;
	static struct knob_state state;
	WORD pressed;
	int select_delta, value_delta;
	char label[96];
	const char *name = NULL, *value = NULL;

	if (enabled < 0)
	{
		enabled = config_boolean("debug.knob_cycle");
		knob_state_reset(&state);
	}
	if (!enabled)
		return;
	if (!(pad->wButtons & XINPUT_GAMEPAD_BACK))
	{
		previous = 0;
		return;
	}
	pressed = pad->wButtons & ~previous;
	previous = pad->wButtons;
	select_delta = (pressed & XINPUT_GAMEPAD_DPAD_DOWN) ? 1 : (pressed & XINPUT_GAMEPAD_DPAD_UP) ? -1 : 0;
	value_delta = (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : (pressed & XINPUT_GAMEPAD_DPAD_LEFT) ? -1 : 0;
	if (select_delta || value_delta)
	{
		if (knob_cycle_press(&state, select_delta, value_delta, knob_current, label, sizeof(label), &name, &value))
		{
			halo_knob_apply(name, value);
			platform_log("knob: %s=%s", name, value);
		}
		halo_knob_message(label);
	}
	pad->wButtons &= (WORD)~(XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT |
		XINPUT_GAMEPAD_DPAD_RIGHT);
}

/* controller aim's pitch offset (input.aim_pitch_offset,
port/linux/game/controller_aim_game.c), the live chord behind
debug.sense_tuning_chord: while a hand aims and Back is held (the Sense controllers' Create), the right stick
pushed past PITCH_CHORD_PUSH up or down (and let back inside
PITCH_CHORD_RELEASE before the next), or the D-pad's up or down (a
DualSense's), steps it by a degree, down tilting the barrel further down.
back_dpad runs first, so with debug.render_scale_dpad,
debug.upscaled_textures_toggle or debug.hud_distance_cycle on the D-pad is theirs and only the stick steps it.
The same chord with the right stick pushed right or left steps the hand's
aim assist (input.aim_assist) up or down by half, and a click of the right
stick cycles on-target steadying (input.aim_steady) off, default and strong,
so their settings compare in one run. While Back is held the game sees
neither the right stick nor its click (the zoom). A pull of the right
trigger with Back held takes the laser (input.laser) to its next mode, off,
close and on; that pull is the game's no more until the trigger is let go,
Back or not, so it never fires, and a trigger already down when Back goes
down keeps firing. The left trigger and the left shoulder (White; the other
hand's grip on a Sense controller) likewise step the aim ray's origin
(input.aim_origin_raise) a tenth toward the eyes and back toward the hand,
throwing no grenade and leaving the flashlight alone. A click of the left
stick switches the first-person gun's smoothing (input.weapon_smoothing) on
and off, and the game doesn't see that click (the crouch). The left stick
pushed right or left (and let back before the next) cycles the first-person
arms' mode (input.first_person_arms) forward or back; the game doesn't see
the left stick while Back is held (no walking). Y (Triangle on a Sense
controller) switches the reticle's easing across an outline
(input.reticle_ease) on and off, X (Square) steps the solved arms' turn
limit (input.arms_turn_limit) through 35 and 45 degrees, and A (Cross) the
arms' follow (input.arms_follow) through 0 and 0.5; the game sees none of
those presses (no grenade or weapon switch, no jump) */
#define PITCH_CHORD_PUSH 0.7f
#define PITCH_CHORD_RELEASE 0.3f
#define LASER_CHORD_PULL 128
#define LASER_CHORD_RELEASE 40
static void aim_pitch_offset_chord(XINPUT_GAMEPAD *pad)
{
	static int enabled = -1;
	static WORD previous;
	static int stick_armed = 1;
	static int assist_armed = 1;
	/* the triggers, left shoulder, Y, X and A */
	static const int pulls[6] = { XINPUT_GAMEPAD_RIGHT_TRIGGER, XINPUT_GAMEPAD_LEFT_TRIGGER, XINPUT_GAMEPAD_WHITE,
		XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_A };
	static int pull_down[6], pull_taken[6];
		WORD pressed;
	float stick;
	float assist_stick;
	int chord;
	int index;

	if (enabled < 0)
		enabled = config_boolean("debug.sense_tuning_chord");
	if (!enabled)
		return;
	chord = (pad->wButtons & XINPUT_GAMEPAD_BACK) && halo_controller_hand_aiming(0);
	for (index = 0; index < 6; index++)
	{
		int value = pad->bAnalogButtons[pulls[index]];

		if (value < LASER_CHORD_RELEASE)
			pull_down[index] = pull_taken[index] = 0;
		else if (value >= LASER_CHORD_PULL && !pull_down[index])
		{
			pull_down[index] = 1;
			if (chord)
			{
				if (index == 0)
					halo_controller_laser_cycle();
				else if (index == 3)
					halo_controller_reticle_ease_toggle();
				else if (index == 4)
					halo_controller_arms_turn_limit_cycle();
				else if (index == 5)
					halo_controller_arms_follow_cycle();
				else
					halo_controller_origin_raise_step(index == 1 ? 1 : -1);
				pull_taken[index] = 1;
			}
		}
		/* (taken, or a pull under way with Back held: the game sees nothing on the way to the chord) */
		if (pull_taken[index] || (chord && !pull_down[index]))
			pad->bAnalogButtons[pulls[index]] = 0;
	}
	if (!chord)
	{
		previous = 0;
		stick_armed = 1;
		assist_armed = 1;
		return;
	}
	assist_stick = (float)pad->sThumbRX / 32767.0f;
	if (assist_armed && fabsf(assist_stick) > PITCH_CHORD_PUSH)
	{
		halo_aim_assist_step(assist_stick > 0.0f ? 1 : -1);
		assist_armed = 0;
	}
	else if (!assist_armed && fabsf(assist_stick) < PITCH_CHORD_RELEASE)
		assist_armed = 1;
	pressed = pad->wButtons & ~previous;
	previous = pad->wButtons;
	stick = (float)pad->sThumbRY / 32767.0f;
	if (stick_armed && fabsf(stick) > PITCH_CHORD_PUSH)
	{
		halo_controller_pitch_offset_step(stick > 0.0f ? -1 : 1);
		stick_armed = 0;
	}
	else if (!stick_armed && fabsf(stick) < PITCH_CHORD_RELEASE)
		stick_armed = 1;
	if (pressed & XINPUT_GAMEPAD_DPAD_UP)
		halo_controller_pitch_offset_step(-1);
	if (pressed & XINPUT_GAMEPAD_DPAD_DOWN)
		halo_controller_pitch_offset_step(1);
	if (pressed & XINPUT_GAMEPAD_RIGHT_THUMB)
		halo_aim_steady_cycle();
	if (pressed & XINPUT_GAMEPAD_LEFT_THUMB)
		halo_controller_weapon_smoothing_toggle();
	{
		static int arms_armed = 1;
		float arms_stick = (float)pad->sThumbLX / 32767.0f;

		if (arms_armed && fabsf(arms_stick) > PITCH_CHORD_PUSH)
		{
			halo_controller_arms_cycle(arms_stick > 0.0f ? 1 : -1);
			arms_armed = 0;
		}
		else if (!arms_armed && fabsf(arms_stick) < PITCH_CHORD_RELEASE)
			arms_armed = 1;
	}
	pad->sThumbRX = 0;
	pad->sThumbRY = 0;
	pad->sThumbLX = 0;
	pad->sThumbLY = 0;
	pad->wButtons &= (WORD)~(XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_RIGHT_THUMB |
		XINPUT_GAMEPAD_LEFT_THUMB);
}
/* port/linux/game/menu_functions.c: two or more players on this machine
(co-op, or split screen in a network game) */
extern unsigned char pc_menu_split_players(void);

/* ---------- device tables */

XPP_DEVICE_TYPE XDEVICE_TYPE_GAMEPAD_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_MEMORY_UNIT_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE;

struct controller
{
	BOOL open;
	DWORD packet_number;
	XINPUT_GAMEPAD previous;
};

static struct controller controllers[PORT_COUNT];
static struct controller keyboard_device;
static DWORD reported_gamepads = 0;
static BOOL reported_keyboard = FALSE;

/* ---------- mouse */

static pthread_mutex_t mouse_lock = PTHREAD_MUTEX_INITIALIZER;
static float mouse_pending_x, mouse_pending_y;
static unsigned long mouse_polls_unconsumed = 0;
static float mouse_wheel_accumulated = 0.0f;
/* the wheel's switch (wheel_update): when the wheel last moved, until when
Y is held, and whether a scroll is under way */
static Uint64 wheel_moved_ms = 0;
static Uint64 wheel_press_until_ms = 0;
static BOOL wheel_scrolling = FALSE;
/* the way the scroll under way turns: 1 up (away), -1 down */
static int wheel_direction = 0;
/* when port 0's aim last moved, by the mouse and by the right stick
(halo_linux_mouse_aiming) */
static Uint64 mouse_aimed_ms = 0;
static Uint64 stick_aimed_ms = 0;

/* the right stick's deflection that counts as aiming with it, clear of a
worn stick's drift */
#define STICK_AIMING_DEFLECTION 8000

/* (input.mouse_vertical_sensitivity, read with it) */
static float vertical_sensitivity = 1.0f;

static float mouse_sensitivity(void)
{
	static float sensitivity;
	static unsigned long read_at = (unsigned long)-1;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		sensitivity = (float)config_real("input.mouse_sensitivity");
		if (sensitivity <= 0.0f)
			sensitivity = 1.0f;
		vertical_sensitivity = (float)config_real("input.mouse_vertical_sensitivity");
		if (vertical_sensitivity <= 0.0f)
			vertical_sensitivity = sensitivity;
	}
	return sensitivity;
}

/* radians of yaw and pitch for the mouse motion since the last call; the
game adds these to the facing change of the player on gamepad 0 */
int halo_linux_mouse_look(short gamepad_index, float *yaw, float *pitch)
{
	/* radians per pixel of relative motion at sensitivity 1 */
	const float scale = 0.0022f;
	static int invert;
	static unsigned long read_at = (unsigned long)-1;
	float x, y;

	*yaw = 0.0f;
	*pitch = 0.0f;
	if (gamepad_index != 0)
		return FALSE;
	if (read_at != config_changes())
	{
		read_at = config_changes();
		invert = config_boolean("input.invert_mouse");
	}
	pthread_mutex_lock(&mouse_lock);
	x = mouse_pending_x;
	y = mouse_pending_y;
	mouse_pending_x = 0.0f;
	mouse_pending_y = 0.0f;
	mouse_polls_unconsumed = 0;
	pthread_mutex_unlock(&mouse_lock);
	if (x == 0.0f && y == 0.0f)
		return FALSE;
	*yaw = -x * scale * mouse_sensitivity();
	*pitch = (invert ? y : -y) * scale * vertical_sensitivity;
	return TRUE;
}

/* whether the player on the gamepad aims with the mouse (it moved after the
right stick last did) and input.mouse_aim_assist is off: then the view's
magnetism leaves them be (player_control.c); the bullets' autoaim stays */
int halo_linux_mouse_aiming(short gamepad_index)
{
	static int aim_assist;
	static unsigned long read_at = (unsigned long)-1;
	int aiming;

	if (gamepad_index != 0)
		return FALSE;
	if (read_at != config_changes())
	{
		read_at = config_changes();
		aim_assist = config_boolean("input.mouse_aim_assist");
	}
	/* port: the host's policy blocks the mouse's magnetism whatever the setting */
	if (aim_assist && !network_aim_assist_block_mouse())
		return FALSE;
	pthread_mutex_lock(&mouse_lock);
	aiming = mouse_aimed_ms != 0 && mouse_aimed_ms >= stick_aimed_ms;
	pthread_mutex_unlock(&mouse_lock);
	return aiming;
}

/* collects the motion the game has not asked for yet; motion that nobody
consumes for a few polls (menus, cutscenes) is dropped so it cannot jerk
the view later */
static void mouse_poll(const struct platform_input_state *input)
{
	pthread_mutex_lock(&mouse_lock);
	if (++mouse_polls_unconsumed > 4)
	{
		mouse_pending_x = 0.0f;
		mouse_pending_y = 0.0f;
	}
	if (!input->mouse_released)
	{
		mouse_pending_x += input->mouse_dx;
		mouse_pending_y += input->mouse_dy;
		if (input->mouse_dx != 0.0f || input->mouse_dy != 0.0f)
			mouse_aimed_ms = SDL_GetTicks();
		mouse_wheel_accumulated += input->mouse_wheel;
		if (input->mouse_wheel != 0.0f)
			wheel_moved_ms = SDL_GetTicks();
	}
	pthread_mutex_unlock(&mouse_lock);
}

/* ---------- keyboard and mouse as a controller */

static BYTE analog(BOOL down)
{
	return down ? 0xff : 0x00;
}

/* the game's on-screen keyboard is up (platform_text_typing): the keys type
into it (XInputDebugGetKeystroke passes them to the game), but for the
arrows, which move about it, enter (Done, once let go of since it came up)
and escape (cancel) */
static BOOL text_typing;
static BOOL text_typing_enter_armed;
/* (the on-screen keyboard's, and a menu's text field's: menu_functions.c) */
static BOOL text_typing_keyboard, text_typing_field;

static void text_typing_update(void)
{
	BOOL typing = text_typing_keyboard || text_typing_field;

	if (typing && !text_typing)
		text_typing_enter_armed = FALSE;
	text_typing = typing;
}

void platform_text_typing(int typing)
{
	text_typing_keyboard = typing != 0;
	text_typing_update();
}

#ifdef HALO_IOS
/* (guest_sdl.c: the host's system keyboard) */
void halo_host_text_input(int on, const char *title, const char *text, int masked);
int halo_host_text_result(char *buffer, unsigned int size);
#endif
/* the field's text taken whole from the system's dialog: Done presses
Start once (typing_gamepad), as Enter does */
static BOOL text_typing_submit;

void platform_text_field(int typing, const char *title, const char *text, int masked)
{
#ifdef HALO_IOS
	/* (an Apple device's keyboard is the system's: on screen, up while the
	field is edited, its keys typing into it as a hardware keyboard's do; an
	Apple TV's, a dialog whose text platform_text_field_result gives;
	host_sdl.c) */
	if ((typing != 0) != text_typing_field)
		halo_host_text_input(typing != 0, title, text, masked);
#else
	(void)title;
	(void)text;
	(void)masked;
#endif
	text_typing_field = typing != 0;
	text_typing_submit = FALSE;
	text_typing_update();
}

int platform_text_field_result(char *text, int size)
{
#ifdef HALO_IOS
	int result = text_typing_field ? halo_host_text_result(text, (unsigned int)size) : 0;

	if (result == 1)
		text_typing_submit = TRUE;
	return result;
#else
	(void)text;
	(void)size;
	return 0;
#endif
}

static void typing_gamepad(const struct platform_input_state *input, XINPUT_GAMEPAD *pad)
{
	const unsigned char *k = input->keys;
	BOOL enter = k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER];

	if (k[SDL_SCANCODE_UP]) pad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
	if (k[SDL_SCANCODE_DOWN]) pad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
	if (k[SDL_SCANCODE_LEFT]) pad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
	if (k[SDL_SCANCODE_RIGHT]) pad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
	if (!enter)
		text_typing_enter_armed = TRUE;
	else if (text_typing_enter_armed)
		pad->wButtons |= XINPUT_GAMEPAD_START;
	if (text_typing_submit)
	{
		pad->wButtons |= XINPUT_GAMEPAD_START;
		text_typing_submit = FALSE;
	}
	pad->bAnalogButtons[XINPUT_GAMEPAD_B] |= analog(k[SDL_SCANCODE_ESCAPE]);
}

static void keyboard_gamepad(const struct platform_input_state *input, XINPUT_GAMEPAD *pad)
{
	const unsigned char *k = input->keys;
	BOOL mouse = !input->mouse_released;
	const unsigned char *m = input->mouse_buttons;
	int x = 0, y = 0;

	if (text_typing)
	{
		typing_gamepad(input, pad);
		return;
	}

	if (k[SDL_SCANCODE_D]) x++;
	if (k[SDL_SCANCODE_A]) x--;
	if (k[SDL_SCANCODE_W]) y++;
	if (k[SDL_SCANCODE_S]) y--;
	if (x || y)
	{
		/* full deflection, diagonals on the unit circle */
		float length = (x && y) ? 0.70710678f : 1.0f;

		pad->sThumbLX = (SHORT)(x * 32767 * length);
		pad->sThumbLY = (SHORT)(y * 32767 * length);
	}

	if (k[SDL_SCANCODE_UP]) pad->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
	if (k[SDL_SCANCODE_DOWN]) pad->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
	if (k[SDL_SCANCODE_LEFT]) pad->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
	if (k[SDL_SCANCODE_RIGHT]) pad->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
	if (k[SDL_SCANCODE_F1]) pad->wButtons |= XINPUT_GAMEPAD_BACK;

	/* (escape backs out, as backspace does: the pause menu's B resumes the
	game, the main menu's asks to quit; Start would choose, as A does) */
	pad->bAnalogButtons[XINPUT_GAMEPAD_A] |= analog(k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_RETURN] ||
		k[SDL_SCANCODE_KP_ENTER]);
	pad->bAnalogButtons[XINPUT_GAMEPAD_B] |= analog(k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_BACKSPACE] ||
		(mouse && m[SDL_BUTTON_X1]));
#ifdef HALO_ILP32
	/* the system back key (gesture or button) backs out of menus */
	pad->bAnalogButtons[XINPUT_GAMEPAD_B] |= analog(k[SDL_SCANCODE_AC_BACK]);
#endif
	pad->bAnalogButtons[XINPUT_GAMEPAD_X] |= analog(k[SDL_SCANCODE_DELETE] || k[SDL_SCANCODE_E]);
	pad->bAnalogButtons[XINPUT_GAMEPAD_Y] |= analog(k[SDL_SCANCODE_TAB]);
}

/* the keys held when the game and the menus switch count as up until let go
of: the escape that opens the pause menu does not also back out of it, nor
the one that closes it pause the game again */
static void keys_held_over_switch(struct platform_input_state *input)
{
	static unsigned char held[SDL_SCANCODE_COUNT];
	static int menus = -1;
	int scancode;

	if (menus != (input->menus != FALSE))
	{
		menus = input->menus != FALSE;
		memcpy(held, input->keys, sizeof(held));
	}
	for (scancode = 0; scancode < SDL_SCANCODE_COUNT; scancode++)
	{
		if (!input->keys[scancode])
			held[scancode] = 0;
		else if (held[scancode])
			input->keys[scancode] = 0;
	}
}

/* ---------- the keyboard and mouse's own controls */

#define MAXIMUM_BINDINGS 2

static const char *const binding_settings[NUMBER_OF_HALO_KEYBOARD_ACTIONS] =
{
	"controls.move_forward", "controls.move_backward", "controls.strafe_left", "controls.strafe_right",
	"controls.jump", "controls.crouch", "controls.fire", "controls.throw_grenade", "controls.melee",
	"controls.reload", "controls.zoom", "controls.switch_weapon", "controls.switch_grenade", "controls.action",
	"controls.flashlight", "controls.scoreboard", "controls.pause",
};

static const struct
{
	const char *name;
	int input;
} named_inputs[] =
{
	/* (SDL's names are "," and "Keypad ,", which a list of bindings, split
	at commas, cannot hold) */
	{ "Comma", SDL_SCANCODE_COMMA },
	{ "Keypad Comma", SDL_SCANCODE_KP_COMMA },
	{ "Mouse Left", INPUT_MOUSE + SDL_BUTTON_LEFT },
	{ "Mouse Right", INPUT_MOUSE + SDL_BUTTON_RIGHT },
	{ "Mouse Middle", INPUT_MOUSE + SDL_BUTTON_MIDDLE },
	{ "Mouse 4", INPUT_MOUSE + SDL_BUTTON_X1 },
	{ "Mouse 5", INPUT_MOUSE + SDL_BUTTON_X2 },
	{ "Wheel", INPUT_WHEEL },
	{ "Wheel Up", INPUT_WHEEL_UP },
	{ "Wheel Down", INPUT_WHEEL_DOWN },
};

/* the bindings, read again when config.toml changes; -1 for none */
static int bindings[NUMBER_OF_HALO_KEYBOARD_ACTIONS][MAXIMUM_BINDINGS];
static unsigned long bindings_read_at = (unsigned long)-1;
static unsigned long keyboard_actions_held;

/* (the C library's case: SDL's string functions are not the Android
guest's) */
static int same_name(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
	{
		if ((*a >= 'a' && *a <= 'z' ? *a - 32 : *a) != (*b >= 'a' && *b <= 'z' ? *b - 32 : *b))
			return FALSE;
	}
	return *a == *b;
}

int halo_input_from_name(const char *name)
{
	SDL_Scancode scancode;
	size_t index;

	for (index = 0; index < sizeof(named_inputs) / sizeof(named_inputs[0]); index++)
	{
		if (same_name(name, named_inputs[index].name))
			return named_inputs[index].input;
	}
	/* (the other mouse buttons, as halo_input_name names them) */
	if (!strncmp(name, "Mouse ", 6) && name[6] >= '1' && name[6] <= '9' && !name[7] &&
		name[6] - '0' < PLATFORM_MOUSE_BUTTON_COUNT)
	{
		return INPUT_MOUSE + (name[6] - '0');
	}
	scancode = SDL_GetScancodeFromName(name);
	return scancode != SDL_SCANCODE_UNKNOWN ? (int)scancode : -1;
}

void halo_input_name(int input, char *name, size_t size)
{
	size_t index;

	for (index = 0; index < sizeof(named_inputs) / sizeof(named_inputs[0]); index++)
	{
		if (named_inputs[index].input == input)
		{
			snprintf(name, size, "%s", named_inputs[index].name);
			return;
		}
	}
	if (input >= 0 && input < SDL_SCANCODE_COUNT && *SDL_GetScancodeName((SDL_Scancode)input))
		snprintf(name, size, "%s", SDL_GetScancodeName((SDL_Scancode)input));
	else if (input >= INPUT_MOUSE && input < INPUT_WHEEL)
		snprintf(name, size, "Mouse %d", input - INPUT_MOUSE);
	else
		snprintf(name, size, "%s", "");
}

static void bindings_read(void)
{
	int action;

	if (bindings_read_at == config_changes())
		return;
	bindings_read_at = config_changes();
	for (action = 0; action < NUMBER_OF_HALO_KEYBOARD_ACTIONS; action++)
	{
		const char *text = config_string(binding_settings[action]);
		int slot;

		for (slot = 0; slot < MAXIMUM_BINDINGS; slot++)
		{
			char name[64];
			size_t length;

			bindings[action][slot] = -1;
			while (*text == ' ' || *text == ',')
				text++;
			length = strcspn(text, ",");
			if (!length)
				continue;
			snprintf(name, sizeof(name), "%.*s", (int)length, text);
			while (*name && name[strlen(name) - 1] == ' ')
				name[strlen(name) - 1] = 0;
			bindings[action][slot] = halo_input_from_name(name);
			if (bindings[action][slot] < 0)
				platform_log("controls: %s has no key or button named \"%s\"", binding_settings[action], name);
			text += length;
		}
	}
}

static BOOL input_held(const struct platform_input_state *input, int code)
{
	BOOL wheel = SDL_GetTicks() < wheel_press_until_ms;

	if (code < 0)
		return FALSE;
	if (code < SDL_SCANCODE_COUNT)
		return input->keys[code] != 0;
	if (code < INPUT_WHEEL)
		return !input->mouse_released && input->mouse_buttons[code - INPUT_MOUSE];
	if (code == INPUT_WHEEL)
		return wheel;
	return wheel && wheel_direction == (code == INPUT_WHEEL_UP ? 1 : -1);
}

/* in the game: the actions held, and the controller's Start and Back for
the pause menu and the scoreboard */
static void keyboard_controls(const struct platform_input_state *input, XINPUT_GAMEPAD *pad)
{
	unsigned long held = 0;
	int action, slot;

	bindings_read();
	for (action = 0; action < NUMBER_OF_HALO_KEYBOARD_ACTIONS; action++)
	{
		for (slot = 0; slot < MAXIMUM_BINDINGS; slot++)
		{
			if (input_held(input, bindings[action][slot]))
				held |= 1UL << action;
		}
	}
	if (held & (1UL << HALO_KEYBOARD_PAUSE))
		pad->wButtons |= XINPUT_GAMEPAD_START;
	if (held & (1UL << HALO_KEYBOARD_SCOREBOARD))
		pad->wButtons |= XINPUT_GAMEPAD_BACK;
	keyboard_actions_held = held;
}

unsigned long halo_keyboard_actions(short controller_index)
{
	return controller_index == 0 ? keyboard_actions_held : 0;
}

/* A scroll of the wheel switches weapons once: it holds Y for WHEEL_PRESS_MS
once the wheel has turned a notch, and the scroll lasts until the wheel has
been still for WHEEL_SCROLL_GAP_MS. One notch often arrives as several events
over a few tens of milliseconds (high-resolution and smooth-scrolling
wheels), and one flick turns several notches; switching for each would bring
the same weapon straight back. Timed in milliseconds, not polls: polls come
once a frame, at the display's refresh rate. */
#define WHEEL_PRESS_MS 50
#define WHEEL_SCROLL_GAP_MS 200

/* debug.test_input "bot:<seed>": a scripted player for the automated
network tests (port/linux/game/network_test.c), different for each seed:
it walks and strafes in circles, turns, fires every few seconds, jumps now
and then and throws a grenade every seven seconds; "look:<seed>" stands
still, only turning and looking up and down (where remote players aim and
whether they stand); "walklook:<seed>" holds the right stick fully down
throughout (head-tracked stereo's first-person body, looking at the feet),
standing still for its first WALKLOOK_STAND_POLLS polls and then walking,
strafing, firing and jumping as "bot:" with the same seed, without its turn
or grenades; "zoom:<start>,<swaps>,<clicks>[,<lights>[,<unclicks>]]" stands
still and, from poll <start> on, presses Y <swaps> times (the next weapon),
then clicks the right stick <clicks> times (each a zoom level), then presses
the flashlight (White) <lights> times (zoomed: the sniper's night vision),
then clicks the right stick <unclicks> times more (past the last level: out
of the zoom), and holds still after: the zoom's checks on the Mac and in the
simulator */
static int test_input_holding_action;
static Uint64 test_input_holding_action_since;

/* the automated tests (port/linux/game/network_test.c): the scripted player
stands still, holding the action button (X: picking up, swapping weapons)
after a second */
void test_input_hold_action(int hold)
{
	if (hold && !test_input_holding_action)
		test_input_holding_action_since = SDL_GetTicks();
	test_input_holding_action = hold;
}

/* "walklook:": the polls (calls to XInputGetState) it stands looking down
before it walks */
#define WALKLOOK_STAND_POLLS 150

/* "zoom:": a press lasts this many polls, and the next waits this many
after it; a weapon swap's animation is given ZOOM_SWAP_POLLS before the
next press */
#define ZOOM_PRESS_POLLS 4
#define ZOOM_GAP_POLLS 20
#define ZOOM_SWAP_POLLS 90

static void test_input_gamepad(XINPUT_GAMEPAD *pad)
{
	static int checked;
	static int seed = -1;
	static int looking;
	static int walklooking;
	static unsigned long walklook_polls;
	static int zooming;
	static long zoom_start, zoom_swaps, zoom_clicks, zoom_lights, zoom_unclicks;
	static unsigned long zoom_polls;
	static int chording;
	static long chord_start, chord_period;
	static unsigned long chord_polls;
	double t;

	if (!checked)
	{
		const char *setting = config_string("debug.test_input");

		checked = 1;
		if (!strncmp(setting, "bot:", 4))
			seed = atoi(setting + 4);
		else if (!strcmp(setting, "bot"))
			seed = 0;
		else if (!strncmp(setting, "look:", 5))
		{
			seed = atoi(setting + 5);
			looking = 1;
		}
		else if (!strncmp(setting, "walklook:", 9))
		{
			seed = atoi(setting + 9);
			walklooking = 1;
		}
		/* "aachord:START,PERIOD": holds Back and presses R1 (the
		anti-aliasing chord) for a few polls from poll START and every
		PERIOD polls after, and nothing else */
		else if (!strncmp(setting, "aachord:", 8))
		{
			chording = 1;
			if (sscanf(setting + 8, "%ld,%ld", &chord_start, &chord_period) != 2 || chord_period < 10)
				chord_period = 300;
		}
		else if (!strncmp(setting, "zoom:", 5))
		{
			zooming = 1;
			int fields = sscanf(setting + 5, "%ld,%ld,%ld,%ld,%ld", &zoom_start, &zoom_swaps, &zoom_clicks,
				&zoom_lights, &zoom_unclicks);

			if (fields < 3)
				zoom_start = zoom_swaps = zoom_clicks = 0;
			if (fields < 4)
				zoom_lights = 0;
			if (fields < 5)
				zoom_unclicks = 0;
		}
	}
	if (chording)
	{
		long since = (long)chord_polls++ - chord_start;

		if (since >= 0 && since % chord_period < 6)
		{
			pad->wButtons |= XINPUT_GAMEPAD_BACK;
			if (since % chord_period >= 2)
				pad->bAnalogButtons[XINPUT_GAMEPAD_BLACK] = 255;
		}
		return;
	}
	if (zooming)
	{
		/* counted by polls, as "walklook:"; the swaps' presses, then the
		clicks', then the lights', then the unclicks', each ZOOM_PRESS_POLLS
		long */
		long since = (long)zoom_polls++ - zoom_start;
		long swaps_end = zoom_swaps * ZOOM_SWAP_POLLS;
		long clicks_end = swaps_end + zoom_clicks * (ZOOM_PRESS_POLLS + ZOOM_GAP_POLLS);
		long lights_end = clicks_end + zoom_lights * (ZOOM_PRESS_POLLS + ZOOM_GAP_POLLS);

		if (since < 0)
			return;
		if (since < swaps_end)
		{
			if (since % ZOOM_SWAP_POLLS < ZOOM_PRESS_POLLS)
				pad->bAnalogButtons[XINPUT_GAMEPAD_Y] = 255;
		}
		else if (since - swaps_end < zoom_clicks * (ZOOM_PRESS_POLLS + ZOOM_GAP_POLLS))
		{
			if ((since - swaps_end) % (ZOOM_PRESS_POLLS + ZOOM_GAP_POLLS) < ZOOM_PRESS_POLLS)
				pad->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
		}
		else if (since - clicks_end < zoom_lights * (ZOOM_PRESS_POLLS + ZOOM_GAP_POLLS))
		{
			if ((since - clicks_end) % (ZOOM_PRESS_POLLS + ZOOM_GAP_POLLS) < ZOOM_PRESS_POLLS)
				pad->bAnalogButtons[XINPUT_GAMEPAD_WHITE] = 255;
		}
		else if (since - lights_end < zoom_unclicks * (ZOOM_PRESS_POLLS + ZOOM_GAP_POLLS))
		{
			if ((since - lights_end) % (ZOOM_PRESS_POLLS + ZOOM_GAP_POLLS) < ZOOM_PRESS_POLLS)
				pad->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
		}
		return;
	}
	if (seed < 0)
		return;
	if (test_input_holding_action)
	{
		/* (standing still, the button held from a second on) */
		if (SDL_GetTicks() - test_input_holding_action_since >= 1000)
			pad->bAnalogButtons[XINPUT_GAMEPAD_X] = 255;
		return;
	}
	t = (double)SDL_GetTicks() / 1000.0 + seed * 1.7;
	if (looking)
	{
		pad->sThumbRX = (SHORT)(sin(t * 0.5) * 14000.0);
		pad->sThumbRY = (SHORT)(sin(t * 0.3) * 32000.0);
		return;
	}
	if (walklooking)
	{
		/* counted by polls, not the clock, so the standing frames are the
		same with debug.fixed_timestep at any speed */
		pad->sThumbRY = -32768;
		if (walklook_polls < WALKLOOK_STAND_POLLS)
		{
			walklook_polls++;
			return;
		}
		pad->sThumbLY = (SHORT)(sin(t * 0.9) * 32000.0);
		pad->sThumbLX = (SHORT)(cos(t * 0.6 + seed) * 20000.0);
		if (fmod(t, 3.0) < 0.3)
			pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = 255;
		if (fmod(t, 5.0) < 0.1)
			pad->bAnalogButtons[XINPUT_GAMEPAD_A] = 255;
		return;
	}
	pad->sThumbLY = (SHORT)(sin(t * 0.9) * 32000.0);
	pad->sThumbLX = (SHORT)(cos(t * 0.6 + seed) * 20000.0);
	pad->sThumbRX = (SHORT)(sin(t * 0.4) * 14000.0);
	if (fmod(t, 3.0) < 0.3)
		pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = 255;
	if (fmod(t, 5.0) < 0.1)
		pad->bAnalogButtons[XINPUT_GAMEPAD_A] = 255;
	if (fmod(t, 7.0) < 0.2)
		pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = 255;
}

/* This probe is restricted to a four-local-player test match. Game ticks and
controller ports pick the actions, so display frame rate cannot shift them. */
static struct mp_benchmark_probe_latch mp_probe_latches[PORT_COUNT];
static int mp_probe_latches_ready;

void mp_benchmark_probe_reset_controllers(void)
{
	short port;

	for (port = 0; port < PORT_COUNT; port++)
		mp_benchmark_probe_latch_reset(&mp_probe_latches[port]);
	mp_probe_latches_ready = 1;
}

void mp_benchmark_probe_action_counts(struct mp_benchmark_probe_counts counts[4])
{
	short port;

	for (port = 0; port < PORT_COUNT; port++)
	{
		counts[port].fire = mp_probe_latches[port].fire_requests;
		counts[port].jump = mp_probe_latches[port].jump_requests;
		counts[port].grenade = mp_probe_latches[port].grenade_requests;
	}
}

static void mp_benchmark_probe_gamepad(XINPUT_GAMEPAD *pad, short port)
{
	struct mp_benchmark_probe_pad input;

	if (!mp_probe_latches_ready)
		mp_benchmark_probe_reset_controllers();
	mp_benchmark_probe_sample(mp_benchmark_probe_tick(), port, &mp_probe_latches[port], &input);
	benchmark_mp_arena_gamepad(port, mp_benchmark_probe_tick(), &input);
	memset(pad, 0, sizeof(*pad));
	pad->sThumbLX = input.left_x;
	pad->sThumbLY = input.left_y;
	pad->sThumbRX = input.right_x;
	pad->sThumbRY = input.right_y;
	pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = input.fire ? 255 : 0;
	pad->bAnalogButtons[XINPUT_GAMEPAD_A] = input.jump ? 255 : 0;
	pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = input.grenade ? 255 : 0;
}

static enum mp_benchmark_probe_input_policy mp_benchmark_probe_or_test_gamepad(XINPUT_GAMEPAD *pad, short port)
{
	enum mp_benchmark_probe_input_policy policy = mp_benchmark_probe_current_policy();

	if (policy == mp_benchmark_probe_scripted_input)
		mp_benchmark_probe_gamepad(pad, port);
	else if (policy == mp_benchmark_probe_neutral_input)
		memset(pad, 0, sizeof(*pad));
	else
		test_input_gamepad(pad);
	return policy;
}

static void wheel_update(void)
{
	Uint64 now = SDL_GetTicks();

	pthread_mutex_lock(&mouse_lock);
	if (!wheel_scrolling)
	{
		if (fabsf(mouse_wheel_accumulated) >= 1.0f)
		{
			wheel_scrolling = TRUE;
			wheel_direction = mouse_wheel_accumulated > 0.0f ? 1 : -1;
			wheel_press_until_ms = now + WHEEL_PRESS_MS;
		}
	}
	else if (now >= wheel_press_until_ms && now - wheel_moved_ms >= WHEEL_SCROLL_GAP_MS)
	{
		wheel_scrolling = FALSE;
		mouse_wheel_accumulated = 0.0f;
	}
	pthread_mutex_unlock(&mouse_lock);
}

/* ---------- SDL gamepads */

/* the SDL gamepads in connection order, at most one per port */
static int sdl_gamepads(SDL_Gamepad *gamepads[PORT_COUNT])
{
	SDL_JoystickID *ids;
	int count = 0, index, found = 0;

	memset(gamepads, 0, sizeof(SDL_Gamepad *) * PORT_COUNT);
	ids = SDL_GetGamepads(&count);
	if (!ids)
		return 0;
#ifdef HALO_ILP32
	{
		/* iOS can list input devices with a few gamepad buttons (the
		emulator's keyboard, some phones' key devices) as generic gamepads:
		recognised controllers take the first ports */
		int pass;

		for (pass = 0; pass < 2; pass++)
		{
			for (index = 0; index < count && found < PORT_COUNT; index++)
			{
				SDL_Gamepad *gamepad = SDL_GetGamepadFromID(ids[index]);
				SDL_GamepadType type;
				BOOL recognised;

				if (!gamepad)
					continue;
				type = SDL_GetGamepadType(gamepad);
				recognised = type != SDL_GAMEPAD_TYPE_UNKNOWN && type != SDL_GAMEPAD_TYPE_STANDARD;
				if (recognised == (pass == 0))
					gamepads[found++] = gamepad;
			}
		}
	}
#else
	for (index = 0; index < count && found < PORT_COUNT; index++)
	{
		SDL_Gamepad *gamepad = SDL_GetGamepadFromID(ids[index]);

		if (gamepad)
			gamepads[found++] = gamepad;
	}
#endif
	SDL_free(ids);
	return found;
}

/* whether one gamepad is port 1's (port_gamepad) */
static BOOL lone_gamepad_split;

/* no button of the gamepad held, its sticks and triggers at rest */
static BOOL gamepad_idle(SDL_Gamepad *gamepad)
{
	int index;

	for (index = 0; index < SDL_GAMEPAD_BUTTON_COUNT; index++)
	{
		if (SDL_GetGamepadButton(gamepad, (SDL_GamepadButton)index))
			return FALSE;
	}
	for (index = 0; index < SDL_GAMEPAD_AXIS_COUNT; index++)
	{
		if (abs(SDL_GetGamepadAxis(gamepad, (SDL_GamepadAxis)index)) > 8000)
			return FALSE;
	}
	return TRUE;
}

/* the gamepad of a port: the first shares port 0 with the keyboard, but for
two or more players with one gamepad (co-op, split screen), port 1 has it
(the keyboard's player is 1, the gamepad's 2). It changes port only at rest:
a button held across the change would be pressed again on the other port
(the B that leaves a profile screen leaving the game as player 1's) */
static SDL_Gamepad *port_gamepad(SDL_Gamepad *gamepads[PORT_COUNT], int count, int port)
{
	if (count == 1)
	{
		BOOL split = pc_menu_split_players() != 0;

		if (split != lone_gamepad_split && gamepad_idle(gamepads[0]))
			lone_gamepad_split = split;
		if (lone_gamepad_split)
			return port == 1 ? gamepads[0] : NULL;
	}
	return port < count ? gamepads[port] : NULL;
}

static SHORT stick(Sint16 value, BOOL flip)
{
	int result = flip ? -(int)value - 1 : value;

	if (result < -32768) result = -32768;
	if (result > 32767) result = 32767;
	return (SHORT)result;
}

static void merge_button(XINPUT_GAMEPAD *pad, int analog_index, BOOL down)
{
	if (down)
		pad->bAnalogButtons[analog_index] = 0xff;
}

/* source/input/input_xbox.c's GAMEPAD_STICK_DEAD_RANGE: the game ignores
each stick axis below this, of 32767, and rescales the rest */
#define GAME_STICK_DEAD_RANGE 9000

/* the value of one axis that the game's dead zone (fix_dead_zone) turns into
wanted, from -1 to 1 */
static SHORT through_game_dead_zone(double wanted)
{
	double dead = GAME_STICK_DEAD_RANGE / 32767.0, value;

	if (wanted == 0.0)
		return 0;
	value = dead + (1.0 - dead) * fabs(wanted);
	if (value > 1.0)
		value = 1.0;
	return (SHORT)lround((wanted < 0.0 ? -value : value) * 32767.0);
}

/* input.stick_dead_zone: below the Xbox's own 0.27, a round dead zone of that
size replaces the game's square one, whose first 27% of travel on each axis
does nothing and snaps small diagonals to an axis. The stick's direction is
kept and its length rescaled from the dead zone's edge, then each axis is
pushed out so that the game's dead zone, applied after, gives exactly that */
static void stick_dead_zone(SHORT *x, SHORT *y)
{
	static double zone = -1.0;
	double fx = *x / 32767.0, fy = *y / 32767.0, length, scale;

	if (zone < 0.0)
		zone = config_real("input.stick_dead_zone");
	if (zone >= GAME_STICK_DEAD_RANGE / 32767.0 - 0.01)
		return;
	if (zone < 0.0)
		zone = 0.0;
	length = sqrt(fx * fx + fy * fy);
	if (length <= zone)
	{
		*x = *y = 0;
		return;
	}
	scale = ((length > 1.0 ? 1.0 : length) - zone) / (1.0 - zone) / length;
	*x = through_game_dead_zone(fx * scale);
	*y = through_game_dead_zone(fy * scale);
}

static void sdl_gamepad_state(SDL_Gamepad *gamepad, XINPUT_GAMEPAD *pad)
{
	static const struct
	{
		SDL_GamepadButton button;
		WORD mask;
	} digital[] =
	{
		{ SDL_GAMEPAD_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP },
		{ SDL_GAMEPAD_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN },
		{ SDL_GAMEPAD_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT },
		{ SDL_GAMEPAD_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
		{ SDL_GAMEPAD_BUTTON_START, XINPUT_GAMEPAD_START },
		{ SDL_GAMEPAD_BUTTON_BACK, XINPUT_GAMEPAD_BACK },
		{ SDL_GAMEPAD_BUTTON_LEFT_STICK, XINPUT_GAMEPAD_LEFT_THUMB },
		{ SDL_GAMEPAD_BUTTON_RIGHT_STICK, XINPUT_GAMEPAD_RIGHT_THUMB },
	};
	int index;
	int left_trigger, right_trigger;

	for (index = 0; index < (int)(sizeof(digital) / sizeof(digital[0])); index++)
	{
		if (SDL_GetGamepadButton(gamepad, digital[index].button))
			pad->wButtons |= digital[index].mask;
	}
	merge_button(pad, XINPUT_GAMEPAD_A, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH));
	merge_button(pad, XINPUT_GAMEPAD_B, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST));
	merge_button(pad, XINPUT_GAMEPAD_X, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST));
	merge_button(pad, XINPUT_GAMEPAD_Y, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH));
	/* the Duke's white and black buttons sit where later pads have shoulders */
	merge_button(pad, XINPUT_GAMEPAD_WHITE, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
	merge_button(pad, XINPUT_GAMEPAD_BLACK, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));

	left_trigger = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * 255 / 32767;
	right_trigger = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * 255 / 32767;
	if (left_trigger > pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER])
		pad->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = (BYTE)left_trigger;
	if (right_trigger > pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER])
		pad->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = (BYTE)right_trigger;

	/* a stick only overrides the keyboard when it is pushed further */
	{
		SHORT left_x = stick(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX), FALSE);
		SHORT left_y = stick(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY), TRUE);
		SHORT right_x = stick(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX), FALSE);
		SHORT right_y = stick(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY), TRUE);

		stick_dead_zone(&left_x, &left_y);
		stick_dead_zone(&right_x, &right_y);
		if (abs(left_x) > abs(pad->sThumbLX)) pad->sThumbLX = left_x;
		if (abs(left_y) > abs(pad->sThumbLY)) pad->sThumbLY = left_y;
		if (abs(right_x) > abs(pad->sThumbRX)) pad->sThumbRX = right_x;
		if (abs(right_y) > abs(pad->sThumbRY)) pad->sThumbRY = right_y;
	}
}

/* controller aim's Sense controllers (host_spatial.m maps them onto the
Xbox gamepad by the spec's table, read at the poses' instant, into the game
sample): merged into port 0 the way sdl_gamepad_state merges a gamepad, the
buttons ORed, the larger analog value, a stick only where pushed further.
In menus Circle (the table's X, action and reload) is B, as the Xbox's menus
take B for back, and the grip's B (melee) is left out */
static void sense_gamepad_state(BOOL menus, XINPUT_GAMEPAD *pad)
{
	const struct halo_spatial_sample *sample = halo_controller_game_sample();
	BYTE analog[8];
	SHORT left_x, left_y, right_x, right_y;
	int index;

	if (!sample || sample->gamepad_valid != 1)
		return;
	memcpy(analog, sample->gamepad.analog, sizeof(analog));
	if (menus)
	{
		analog[XINPUT_GAMEPAD_B] = analog[XINPUT_GAMEPAD_X];
		analog[XINPUT_GAMEPAD_X] = 0;
	}
	pad->wButtons |= sample->gamepad.buttons;
	for (index = 0; index < 8; index++)
	{
		if (analog[index] > pad->bAnalogButtons[index])
			pad->bAnalogButtons[index] = analog[index];
	}
	left_x = sample->gamepad.thumb[0];
	left_y = sample->gamepad.thumb[1];
	right_x = sample->gamepad.thumb[2];
	right_y = sample->gamepad.thumb[3];
	stick_dead_zone(&left_x, &left_y);
	stick_dead_zone(&right_x, &right_y);
	if (abs(left_x) > abs(pad->sThumbLX)) pad->sThumbLX = left_x;
	if (abs(left_y) > abs(pad->sThumbLY)) pad->sThumbLY = left_y;
	if (abs(right_x) > abs(pad->sThumbRX)) pad->sThumbRX = right_x;
	if (abs(right_y) > abs(pad->sThumbRY)) pad->sThumbRY = right_y;
}

/* ---------- XAPI */

VOID WINAPI XInitDevices(DWORD preallocation_type_count, PXDEVICE_PREALLOC_TYPE preallocation_types)
{
	(void)preallocation_type_count;
	(void)preallocation_types;
	platform_sdl_initialize();
}

static DWORD connected_gamepads(void)
{
	SDL_Gamepad *gamepads[PORT_COUNT];
	int count = sdl_gamepads(gamepads);
	DWORD mask = XDEVICE_PORT0_MASK;
	int port;

	/* the first pad shares port 0 with the keyboard (port_gamepad) */
	for (port = 1; port < PORT_COUNT; port++)
	{
		if (port_gamepad(gamepads, count, port))
			mask |= 1UL << port;
	}
	/* The private four-player probe needs handles before the match starts. */
	return mp_benchmark_probe_device_mask(mask, mp_benchmark_probe_requested());
}

BOOL WINAPI XGetDeviceChanges(PXPP_DEVICE_TYPE device_type, PDWORD insertions, PDWORD removals)
{
	*insertions = 0;
	*removals = 0;
	if (device_type == XDEVICE_TYPE_GAMEPAD)
	{
		DWORD connected = connected_gamepads();

		*insertions = connected & ~reported_gamepads;
		*removals = reported_gamepads & ~connected;
		reported_gamepads = connected;
	}
	else if (device_type == XDEVICE_TYPE_DEBUG_KEYBOARD)
	{
		if (!reported_keyboard)
		{
			*insertions = 1;
			reported_keyboard = TRUE;
		}
	}
	return *insertions || *removals;
}

HANDLE WINAPI XInputOpen(PXPP_DEVICE_TYPE device_type, DWORD port, DWORD slot,
	PXINPUT_POLLING_PARAMETERS polling_parameters)
{
	(void)slot;
	(void)polling_parameters;
	if (device_type == XDEVICE_TYPE_GAMEPAD && port < PORT_COUNT)
	{
		memset(&controllers[port], 0, sizeof(controllers[port]));
		controllers[port].open = TRUE;
		return (HANDLE)&controllers[port];
	}
	if (device_type == XDEVICE_TYPE_DEBUG_KEYBOARD && port == 0)
	{
		keyboard_device.open = TRUE;
		return (HANDLE)&keyboard_device;
	}
	SetLastError(ERROR_DEVICE_NOT_CONNECTED);
	return NULL;
}

VOID WINAPI XInputClose(HANDLE device)
{
	struct controller *controller = (struct controller *)device;

	if (controller)
		controller->open = FALSE;
}

static int controller_port(HANDLE device)
{
	int port;

	for (port = 0; port < PORT_COUNT; port++)
	{
		if (device == (HANDLE)&controllers[port] && controllers[port].open)
			return port;
	}
	return -1;
}

/* display.late_look (port/linux/game/render_interpolation.c): the right
stick as the game was last given it for player one, and when; and the stick
as it is now, read again without the game hearing of it. 0 when there is no
controller to read, or the input is a test's or a replay's, which must play
the same every time. */
double halo_frame_trace_milliseconds(void);

static struct
{
	double read_ms;              /* halo_frame_trace_milliseconds' clock; 0 before the first read */
	float x, y;
	int valid;
} look_read;

/* how long ago the game last read player one's controller, in seconds (0
before it has): what a re-read just before the camera could gain, whether or
not there is a stick to re-read */
float halo_look_read_age(void)
{
	return look_read.read_ms > 0.0 ? (float)((halo_frame_trace_milliseconds() - look_read.read_ms) / 1000.0) : 0.0f;
}

int halo_look_stick_latest(float *read_x, float *read_y, float *now_x, float *now_y, float *age_seconds)
{
	SDL_Gamepad *gamepads[PORT_COUNT];
	SDL_Gamepad *gamepad;
	int count;

	if (!look_read.valid)
		return 0;
	platform_pump_events();
	count = sdl_gamepads(gamepads);
	gamepad = port_gamepad(gamepads, count, 0);
	if (!gamepad)
		return 0;
	*read_x = look_read.x;
	*read_y = look_read.y;
	*now_x = (float)stick(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX), FALSE) / 32767.0f;
	*now_y = (float)stick(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY), TRUE) / 32767.0f;
	*age_seconds = halo_look_read_age();
	return 1;
}

DWORD WINAPI XInputGetState(HANDLE device, PXINPUT_STATE state)
{
	int port = controller_port(device);
	SDL_Gamepad *gamepads[PORT_COUNT];
	int count;

	/* the first read after a Present starts a frame: the frame pacer may
	wait here, before anything is read (d3d8_device.c, halo_frame_start) */
	halo_frame_start();
	memset(state, 0, sizeof(*state));
	if (port < 0)
		return ERROR_DEVICE_NOT_CONNECTED;
	platform_pump_events();
	count = sdl_gamepads(gamepads);
	if (port == 0)
	{
		struct platform_input_state input;
		enum mp_benchmark_probe_input_policy probe_policy;

		/* the tracked controllers' poses and buttons of this instant, for
		this frame (controller_aim_game.c; once a frame) */
		halo_controller_sample_game();
		platform_input_read(&input, TRUE);
		mouse_poll(&input);
		wheel_update();
		keyboard_actions_held = 0;
		keys_held_over_switch(&input);
		if (!console_is_active())
		{
			if (input.menus)
				keyboard_gamepad(&input, &state->Gamepad);
			else
				keyboard_controls(&input, &state->Gamepad);
		}
		if (port_gamepad(gamepads, count, 0))
			sdl_gamepad_state(gamepads[0], &state->Gamepad);
		/* the Sense controllers, before the test input and the replay
		filter, so recordings see them; Create (Back) held with the right
		stick reaches the pitch offset's chord below, not the game */
		sense_gamepad_state(input.menus != FALSE, &state->Gamepad);
		probe_policy = mp_benchmark_probe_or_test_gamepad(&state->Gamepad, port);
		if (abs(state->Gamepad.sThumbRX) > STICK_AIMING_DEFLECTION ||
			abs(state->Gamepad.sThumbRY) > STICK_AIMING_DEFLECTION)
		{
			pthread_mutex_lock(&mouse_lock);
			stick_aimed_ms = SDL_GetTicks();
			pthread_mutex_unlock(&mouse_lock);
		}
		knob_cycle_chord(&state->Gamepad);
#ifdef HALO_IOS
		external_mode_chord(&state->Gamepad);
#endif
		back_dpad(&state->Gamepad);
		aim_pitch_offset_chord(&state->Gamepad);
		anti_aliasing_chord(&state->Gamepad);

		/* (display.late_look: what the stick was when the game read it; not
		a test's or a replay's input) */
		look_read.valid = port_gamepad(gamepads, count, 0) != NULL && !platform_fixed_timestep() &&
			probe_policy == mp_benchmark_probe_ordinary_input &&
			!config_string("debug.test_input")[0] && !config_string("debug.input_replay")[0];
		look_read.x = (float)state->Gamepad.sThumbRX / 32767.0f;
		look_read.y = (float)state->Gamepad.sThumbRY / 32767.0f;
		look_read.read_ms = halo_frame_trace_milliseconds();
		if (probe_policy == mp_benchmark_probe_ordinary_input)
			input_replay_filter(&state->Gamepad);
	}
	else if (port < config_integer("debug.network_test_players") ||
		(port > 0 && port < PORT_COUNT && mp_benchmark_probe_requested()))
	{
		/* (split screen without a second gamepad: the scripted player of
		debug.test_input plays this controller too, ahead of any gamepad,
		the Mac's touch controls included; the private suite owns all four
		virtual ports without changing the user's configured player count) */
		mp_benchmark_probe_or_test_gamepad(&state->Gamepad, port);
	}
	else if (port_gamepad(gamepads, count, port))
	{
		sdl_gamepad_state(port_gamepad(gamepads, count, port), &state->Gamepad);
	}

	if (memcmp(&state->Gamepad, &controllers[port].previous, sizeof(state->Gamepad)))
	{
		controllers[port].packet_number++;
		controllers[port].previous = state->Gamepad;
	}
	state->dwPacketNumber = controllers[port].packet_number;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputSetState(HANDLE device, PXINPUT_FEEDBACK feedback)
{
	int port = controller_port(device);
	SDL_Gamepad *gamepads[PORT_COUNT];
	int count;

	if (!feedback)
		return ERROR_INVALID_PARAMETER;
	feedback->Header.dwStatus = ERROR_SUCCESS;
	if (port < 0)
		return ERROR_DEVICE_NOT_CONNECTED;
	count = sdl_gamepads(gamepads);
	if (port_gamepad(gamepads, count, port))
	{
		/* the game refreshes the motors every frame; rumble a little longer
		than that so they do not stutter */
		SDL_RumbleGamepad(port_gamepad(gamepads, count, port), feedback->Rumble.wLeftMotorSpeed,
			feedback->Rumble.wRightMotorSpeed, 100);
	}
	/* the Sense controllers, merged into port 0, are not SDL's gamepads:
	the motors reach them through controller aim's feedback to the host */
	if (port == 0)
		halo_controller_rumble(feedback->Rumble.wLeftMotorSpeed, feedback->Rumble.wRightMotorSpeed);
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugInitKeyboardQueue(PXINPUT_DEBUG_KEYQUEUE_PARAMETERS parameters)
{
	(void)parameters;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugGetKeystroke(PXINPUT_DEBUG_KEYSTROKE keystroke)
{
	struct platform_keystroke next;

	memset(keystroke, 0, sizeof(*keystroke));
	while (platform_next_keystroke(&next))
	{
		BOOL key_up = (next.flags & XINPUT_DEBUG_KEYSTROKE_FLAG_KEYUP) != 0;

		/* key ups always pass, so no key is left latched down; while typing,
		escape does not (it cancels, as B: the game's own escape leaves the
		menus, main.c) */
		if (key_up || next.virtual_key == VK_OEM_3_BACKQUOTE || console_is_active() ||
			(text_typing && next.virtual_key != 0x1B /* escape */))
		{
			keystroke->VirtualKey = next.virtual_key;
			keystroke->Ascii = next.ascii;
			keystroke->Flags = next.flags;
			return ERROR_SUCCESS;
		}
	}
	return ERROR_HANDLE_EOF;
}
