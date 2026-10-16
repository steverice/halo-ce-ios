/* Engine-boundary fixture for tools/test_menu_cancel.py. The three production
functions inserted below own event consumption, handler actions and history. */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "menu_platform.h"

typedef int boolean;
typedef float real;
typedef int BOOL;
typedef unsigned char BYTE, CHAR;
typedef short SHORT;
#include <SDL3/SDL_mouse.h>
#include "sdl_platform.h"
enum { XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y };
enum { XINPUT_GAMEPAD_DPAD_UP = 1, XINPUT_GAMEPAD_DPAD_DOWN = 2,
	XINPUT_GAMEPAD_DPAD_LEFT = 4, XINPUT_GAMEPAD_DPAD_RIGHT = 8, XINPUT_GAMEPAD_BACK = 32 };
typedef struct { unsigned short wButtons; BYTE bAnalogButtons[8]; SHORT sThumbLX, sThumbLY; } XINPUT_GAMEPAD;
static BOOL text_typing;
static void typing_gamepad(const struct platform_input_state *input, XINPUT_GAMEPAD *pad) { abort(); }
/* KEYBOARD */
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define MAXIMUM_NUMBER_OF_LOCAL_PLAYERS 4
#define SHORT_MIN SHRT_MIN
#define SHORT_MAX SHRT_MAX
#define TEST_FLAG(flags, bit) (((flags) >> (bit)) & 1)
#define match_assert(file, line, condition) assert(condition)
#define match_vwarn(file, line, condition, message) assert(condition)
enum { _gamepad_analog_button_b = 1, _gamepad_binary_button_dpad_up = 8,
	_gamepad_binary_button_dpad_down, _gamepad_binary_button_dpad_left,
	_gamepad_binary_button_dpad_right, _gamepad_binary_button_back = 13,
	NUMBER_OF_GAMEPAD_BUTTONS = 16 };
enum { _ui_audio_feedback_none, _ui_audio_feedback_cursor,
	_ui_audio_feedback_forward, _ui_audio_feedback_back, _error_silent,
	_ui_widget_type_spinner_list, _ui_widget_type_column_list };
/* ENUMS */
struct tag_reference { long index; };
struct tag_block { long count; void *address; };
struct ui_widget_event_handler_reference {
	short event_type, function;
	long flags;
	char script[32];
	struct tag_reference widget_tag, sound_effect;
};
struct ui_widget_conditional_reference { long flags; struct tag_reference widget_tag; };
struct ui_widget_definition { long flags; struct tag_block event_handlers, conditional_widgets; };
struct widget_instance {
	short local_player_index, type, horizontal_offset, vertical_offset;
	boolean close_if_local_player_controller_present;
	unsigned long creation_time, milliseconds_to_auto_close, auto_close_fade_time;
	real alpha_modifier;
	long definition_tag_index;
	struct widget_instance *parent, *child, *focused_child, *next, *previous;
};
struct event_record {
	short type, controller_index;
	union { struct { short index, value; } button; struct { short x, y; } stick; } data;
};
struct widget_stack_data {
	long previous_widget_tag, focused_child_parent_widget_tag;
	short focused_child_index, local_player_index;
};
struct widget_stack_node { struct widget_stack_data data; struct widget_stack_node *next; };
static struct {
	struct widget_instance *active_widgets[4];
	struct widget_stack_node *widget_stack[4];
	unsigned long current_system_milliseconds;
} widget_globals;
static unsigned long dpad_event_times[4][4];
static struct ui_widget_definition definitions[3];
static struct ui_widget_event_handler_reference handlers[8];
static struct widget_instance instances[3];
static struct widget_stack_node history;
static int game_paused, returned_to_main, menu_source;

static struct widget_instance *widget_instance_get_topmost_parent(struct widget_instance *widget)
{ while (widget->parent) widget = widget->parent; return widget; }
static void ui_widget_delete(struct widget_instance *widget)
{
	assert(widget_globals.active_widgets[0] == widget);
	widget_globals.active_widgets[0] = NULL;
	game_paused = 0;
}
static void pop_widget(struct widget_stack_node **stack, struct widget_stack_data *data)
{ *data = (*stack)->data; *stack = (*stack)->next; }
static struct widget_instance *ui_widget_load_by_name_or_tag(void *name, long tag, void *parent,
	short player, long a, long b, long c)
{
	assert(tag >= 0 && tag < 3);
	instances[tag].local_player_index = player;
	widget_globals.active_widgets[0] = &instances[tag];
	return &instances[tag];
}
static void widget_instance_set_focused_child_by_index(long tag, struct widget_instance *widget, short index)
{ assert(tag == 0 && index == 0 && widget == &instances[0]); }
static struct ui_widget_definition *ui_widget_definition_get(long tag) { return &definitions[tag]; }
static int widget_takes_events_of_controller(struct widget_instance *widget, short controller) { return controller == 0; }
static int input_has_gamepad(short player) { return 1; }
static void ui_play_audio_feedback_sound(long feedback) {}
static void widget_instance_update_animation_parameters(struct widget_instance *widget) {}
static void main_goto_main_menu(void) { returned_to_main++; }
static int ui_widget_launch_widget(struct widget_instance *widget, long tag)
{
	/* Tag loading is the engine boundary. The real dispatcher must request
	the quit dialog, rather than deleting the root with empty history. */
	assert(tag == 2);
	ui_widget_delete(widget_instance_get_topmost_parent(widget));
	widget_globals.active_widgets[0] = &instances[2];
	return 1;
}
/* These branches require game tags or rendering and are outside cancel tests.
Abort if an unexpected action reaches one instead of silently accepting it. */
#define UNEXPECTED(name, signature) static void name signature { abort(); }
UNEXPECTED(widget_instance_give_focus_by_tag, (struct widget_instance *widget, long tag, short player))
UNEXPECTED(widget_instance_reload_recursive, (struct widget_instance *widget))
UNEXPECTED(ui_widget_reload_by_tag, (long tag))
UNEXPECTED(unspatialized_impulse_sound_new, (long tag, float volume))
UNEXPECTED(spinner_list_update, (struct widget_instance *widget))
UNEXPECTED(column_list_update, (struct widget_instance *widget, struct ui_widget_definition *definition))
UNEXPECTED(widget_instance_tab_to_previous_valid_widget, (struct widget_instance *widget))
UNEXPECTED(widget_instance_tab_to_next_valid_widget, (struct widget_instance *widget))
UNEXPECTED(widget_event_function_list_widget_goto_previous_item,
	(struct widget_instance *widget, struct event_record *event, boolean *deleted))
UNEXPECTED(widget_event_function_list_widget_goto_next_item,
	(struct widget_instance *widget, struct event_record *event, boolean *deleted))
UNEXPECTED(error, (int level, char const *message, ...))
static int hs_evaluate_by_name(char *script) { abort(); }
static int ui_widget_event_handler_function_invoke(struct widget_instance *widget,
	struct event_record *event, short function, boolean *deleted) { abort(); }
static struct widget_instance *widget_instance_find_by_tag_index(long tag) { abort(); }
/* PRODUCTION */

static void add_handler(short event, long flags)
{
	long index = definitions[0].event_handlers.count++;
	handlers[index] = (struct ui_widget_event_handler_reference) {
		.event_type = event, .flags = flags, .widget_tag.index = 2, .sound_effect.index = NONE
	};
}
static void reset(int can_quit)
{
	memset(&widget_globals, 0, sizeof(widget_globals));
	memset(definitions, 0, sizeof(definitions));
	memset(instances, 0, sizeof(instances));
	memset(handlers, 0, sizeof(handlers));
	unsetenv("HALO_APP_CAN_QUIT");
	if (can_quit) setenv("HALO_APP_CAN_QUIT", "1", 1);
	for (int i = 0; i < 3; i++) instances[i].definition_tag_index = i;
	definitions[0].flags = 1 << _widget_pass_unhandled_events_to_children_bit;
	definitions[0].event_handlers.address = handlers;
	/* ROOT HANDLERS */
	/* A focused child with no cancel handler catches erroneous forwarding. */
	instances[0].child = instances[0].focused_child = &instances[1];
	instances[1].parent = &instances[0];
	widget_globals.active_widgets[0] = &instances[0];
	game_paused = returned_to_main = 0;
}
static int press(short button)
{
	struct event_record event = { .type = _event_type_button, .controller_index = 0,
		.data.button = { .index = button, .value = 1 } };
	struct widget_instance *active = widget_globals.active_widgets[0];
	boolean deleted = FALSE;
	assert(active);
	widget_instance_process_one_event_recursive(active, &definitions[active->definition_tag_index], &event, &deleted);
	return deleted;
}
int main(void)
{
	for (menu_source = 0; menu_source < 2; menu_source++) {
	for (int button = 0; button < 2; button++) {
		short cancel = button ? _widget_event_back_button : _widget_event_b_button;
		reset(0);
		assert(!press(cancel));
		assert(widget_globals.active_widgets[0] == &instances[0]);
		assert(!press(cancel)); /* repeat must keep the interface */
		assert(widget_globals.active_widgets[0] == &instances[0]);
		reset(1);
		press(cancel);
		assert(widget_globals.active_widgets[0] == &instances[2]); /* desktop Quit */
		reset(0);
		instances[1].parent = NULL;
		widget_globals.active_widgets[0] = &instances[1];
		history = (struct widget_stack_node) { .data = { .previous_widget_tag = 0,
			.focused_child_parent_widget_tag = 0, .focused_child_index = 0, .local_player_index = 0 } };
		widget_globals.widget_stack[0] = &history;
		assert(press(cancel)); /* nested menu / online browser returns to root */
		assert(widget_globals.active_widgets[0] == &instances[0]);
		assert(widget_globals.widget_stack[0] == NULL);
		assert(!press(cancel)); /* cancel after returning from another screen */
		reset(0);
		instances[1].parent = NULL;
		widget_globals.active_widgets[0] = &instances[1];
		game_paused = 1;
		assert(press(cancel)); /* pause has gameplay, so empty history resumes */
		assert(widget_globals.active_widgets[0] == NULL && !game_paused);
		assert(!returned_to_main);
	}
	/* Keyboard Escape and Backspace map to B, and F1 maps to Back. Run the
	real translation before the same dispatcher used by physical gamepads. */
	int keys[] = { SDL_SCANCODE_ESCAPE, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_AC_BACK, SDL_SCANCODE_F1 };
	for (int key = 0; key < 4; key++) {
		struct platform_input_state input = {0};
		XINPUT_GAMEPAD pad = {0};
		input.keys[keys[key]] = 1;
		keyboard_gamepad(&input, &pad);
		short cancel = key == 3 ? _widget_event_back_button : _widget_event_b_button;
		assert(key == 3 ? (pad.wButtons & XINPUT_GAMEPAD_BACK) : pad.bAnalogButtons[XINPUT_GAMEPAD_B] == 255);
		reset(0);
		assert(!press(cancel));
		assert(widget_globals.active_widgets[0] == &instances[0]);
		reset(1);
		press(cancel);
		assert(widget_globals.active_widgets[0] == &instances[2]);
	}
	}
	puts("PASS: root B/Back consumed; desktop Quit, nested Back and pause resume preserved");
	return 0;
}
