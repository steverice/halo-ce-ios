#ifndef KNOB_CYCLE_H
#define KNOB_CYCLE_H

/* debug.knob_cycle: the quality settings the chord (xinput_sdl.c) cycles while the game runs, each with the values it
steps through in order (the last is the heaviest) and the text shown for one. The state is the knob chosen and where
each knob stands; the chord's presses come in as a step in the knob chosen (up and down) or in its value (left and
right). Nothing is saved: the next launch takes the settings again. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KNOB_VALUE_MAXIMUM 6

enum knob_kind { KNOB_INTEGER, KNOB_BOOLEAN, KNOB_STRING };

struct knob
{
	const char *label;
	const char *name;
	enum knob_kind kind;
	int count;
	const char *values[KNOB_VALUE_MAXIMUM];
	const char *shown[KNOB_VALUE_MAXIMUM];
};

/* The render height does not apply under foveated head-tracked stereo (the Compositor's rate maps size the eyes);
the shadow map and effect targets are allocated again at their new size on first use, and the old ones stay
allocated until the app quits. Not here because they cannot change while the game runs: display.upscaled_textures,
display.compressed_textures, display.renderer and display.foveation. */
static const struct knob knob_table[] = {
	{ "render lines", "display.render_height", KNOB_INTEGER, 5, { "0", "540", "720", "900", "1080" },
		{ "profile", "540", "720", "900", "1080" } },
	{ "shadow map", "display.shadow_map_size", KNOB_INTEGER, 4, { "128", "256", "512", "1024" },
		{ "128", "256", "512", "1024" } },
	{ "anisotropy", "display.anisotropic_filtering", KNOB_INTEGER, 5, { "1", "2", "4", "8", "16" },
		{ "1x", "2x", "4x", "8x", "16x" } },
	{ "model detail", "display.model_lod", KNOB_STRING, 2, { "auto", "max" }, { "auto", "max" } },
	{ "anti-aliasing", "display.anti_aliasing", KNOB_STRING, 3, { "off", "fxaa", "smaa" }, { "off", "FXAA", "SMAA" } },
	{ "effect resolution", "display.effect_resolution", KNOB_STRING, 3, { "xbox", "half", "full" }, { "xbox", "half", "full" } },
};
#define KNOB_COUNT ((int)(sizeof(knob_table) / sizeof(*knob_table)))

struct knob_state
{
	int selected;
	/* each knob's value, -1 until it is first asked for */
	int value[KNOB_COUNT];
};

static inline void knob_state_reset(struct knob_state *state)
{
	int index;

	state->selected = 0;
	for (index = 0; index < KNOB_COUNT; index++)
		state->value[index] = -1;
}

/* the position of the setting's value text among the knob's values: an exact match, else for numbers the nearest */
static inline int knob_value_find(const struct knob *knob, const char *text)
{
	int index, best = 0;
	long wanted = atol(text), distance = -1;

	for (index = 0; index < knob->count; index++)
		if (!strcmp(knob->values[index], text))
			return index;
	if (knob->kind != KNOB_INTEGER)
		return 0;
	for (index = 0; index < knob->count; index++)
	{
		long gap = labs(atol(knob->values[index]) - wanted);

		if (distance < 0 || gap < distance)
		{
			distance = gap;
			best = index;
		}
	}
	return best;
}

/* The chord's press: select_delta moves to the next or previous knob (round the table), value_delta moves the knob
chosen to the next or previous value (stopping at the ends). current(knob, text, size) writes the setting's value as
it stands, for a knob not yet touched. label gets the text to show; the return is 1 when a value changed and
name/value are what to apply, else 0 (a knob only chosen, or already at its end). */
static inline int knob_cycle_press(struct knob_state *state, int select_delta, int value_delta,
	void (*current)(const struct knob *knob, char *text, unsigned long size), char *label, unsigned long label_size,
	const char **name, const char **value)
{
	const struct knob *knob;
	int changed = 0;

	if (select_delta)
		state->selected = (state->selected + select_delta + KNOB_COUNT) % KNOB_COUNT;
	knob = &knob_table[state->selected];
	if (state->value[state->selected] < 0)
	{
		char text[64] = "";

		current(knob, text, sizeof(text));
		state->value[state->selected] = knob_value_find(knob, text);
	}
	if (value_delta)
	{
		int next = state->value[state->selected] + value_delta;

		if (next >= 0 && next < knob->count)
		{
			state->value[state->selected] = next;
			*name = knob->name;
			*value = knob->values[next];
			changed = 1;
		}
	}
	snprintf(label, label_size, "%s %s", knob->label, knob->shown[state->value[state->selected]]);
	return changed;
}

#endif
