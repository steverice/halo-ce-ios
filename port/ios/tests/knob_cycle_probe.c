#include "knob_cycle.h"
#include <assert.h>

static void current(const struct knob *knob, char *text, unsigned long size)
{
	if (!strcmp(knob->name, "display.shadow_map_size"))
		snprintf(text, size, "512");
	else if (!strcmp(knob->name, "display.render_height"))
		snprintf(text, size, "700");
	else if (!strcmp(knob->name, "display.effect_resolution"))
		snprintf(text, size, "true");
	else
		snprintf(text, size, "%s", knob->values[0]);
}

static int find(const char *name)
{
	for (int index = 0; index < KNOB_COUNT; index++)
		if (!strcmp(knob_table[index].name, name))
			return index;
	return -1;
}

int main(void)
{
	struct knob_state state;
	char label[96];
	const char *name = NULL, *value = NULL;
	int shadow = find("display.shadow_map_size");

	knob_state_reset(&state);
	assert(KNOB_COUNT == 7 && shadow >= 0);
	/* choosing a knob shows it at the setting's value and changes nothing */
	assert(!knob_cycle_press(&state, shadow, 0, current, label, sizeof(label), &name, &value));
	assert(!strcmp(label, "shadow map 512") && state.selected == shadow);
	/* left steps down, right up */
	assert(knob_cycle_press(&state, 0, -1, current, label, sizeof(label), &name, &value));
	assert(!strcmp(name, "display.shadow_map_size") && !strcmp(value, "256") && !strcmp(label, "shadow map 256"));
	assert(knob_cycle_press(&state, 0, 1, current, label, sizeof(label), &name, &value) && !strcmp(value, "512"));
	assert(knob_cycle_press(&state, 0, 1, current, label, sizeof(label), &name, &value) && !strcmp(value, "1024"));
	/* the end stops: the label stays, nothing is applied */
	assert(!knob_cycle_press(&state, 0, 1, current, label, sizeof(label), &name, &value));
	assert(!strcmp(label, "shadow map 1024"));
	/* the selection wraps both ways; knobs keep their own value */
	assert(!knob_cycle_press(&state, -(shadow + 1), 0, current, label, sizeof(label), &name, &value));
	assert(state.selected == KNOB_COUNT - 1);
	assert(!knob_cycle_press(&state, 1, 0, current, label, sizeof(label), &name, &value) && state.selected == 0);
	/* a number between the values takes the nearest, and a boolean its name */
	assert(!strcmp(label, "render lines 720"));
	state.selected = find("display.effect_resolution") - 1;
	assert(!knob_cycle_press(&state, 1, 0, current, label, sizeof(label), &name, &value));
	assert(!strcmp(label, "effect resolution on"));
	assert(knob_cycle_press(&state, 0, -1, current, label, sizeof(label), &name, &value) && !strcmp(value, "false"));
	state.selected = shadow;
	assert(!knob_cycle_press(&state, 0, 0, current, label, sizeof(label), &name, &value));
	assert(!strcmp(label, "shadow map 1024"));
	puts("PASS: knob cycle selects, steps, stops at the ends and wraps");
	return 0;
}
