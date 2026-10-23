/*
SIM_EXPERIMENTS.C

The simulation speed-ups (debug.collision_type_filter,
debug.sphere_query_type_filter, debug.static_light_relink,
debug.collision_candidates). All default to on. A setting is read again at the start of each game tick, so one can
be flipped while the game runs (a console toggle or a settings change) and
the simulation keeps one value for the whole tick. Every one changes how
much work a query does and not what it returns.
*/

#include "cseries.h"

int config_boolean(const char *name);
long game_time_get(void);

static const char *const sim_experiment_names[] =
{
	"debug.collision_type_filter",
	"debug.sphere_query_type_filter",
	"debug.static_light_relink",
	"debug.collision_candidates",
};
#define SIM_EXPERIMENT_COUNT ((long)(sizeof(sim_experiment_names) / sizeof(sim_experiment_names[0])))

static boolean sim_experiment_values[SIM_EXPERIMENT_COUNT];
static long sim_experiment_tick = -2;
static unsigned sim_experiment_mask;
static unsigned sim_experiment_overrides;

/* debug.immediate_merge is on in every build and every benchmark case; the setting stays as a switch for diagnosis,
read once a tick like the others, but a catalog case cannot set it. */
static boolean sim_switch_value;
static long sim_switch_tick = -2;

boolean sim_switch_immediate_merge(void)
{
	long tick = game_time_get();

	if (tick != sim_switch_tick)
	{
		sim_switch_tick = tick;
		sim_switch_value = config_boolean("debug.immediate_merge") != 0;
	}

	return sim_switch_value;
}

/* the same answer for the renderer, which can draw before game time exists */
boolean sim_switch_immediate_merge_now(void)
{
	return config_boolean("debug.immediate_merge") != 0;
}

/* A benchmark case's values and the bits it explicitly sets; a zero mask leaves the configuration alone. */
void sim_experiment_override(
	unsigned values,
	unsigned mask)
{
	sim_experiment_overrides = values;
	sim_experiment_mask = mask;
	sim_experiment_tick = -2;
}

boolean sim_experiment_enabled(
	short experiment)
{
	long tick = game_time_get();

	if (tick != sim_experiment_tick)
	{
		long index;

		sim_experiment_tick = tick;
		for (index = 0; index < SIM_EXPERIMENT_COUNT; index++)
		{
			sim_experiment_values[index] = (sim_experiment_mask & (1u << index)) != 0 ?
				(sim_experiment_overrides & (1u << index)) != 0 : config_boolean(sim_experiment_names[index]) != 0;
		}
	}

	return experiment >= 0 && experiment < SIM_EXPERIMENT_COUNT && sim_experiment_values[experiment];
}

/* the same answer for code that runs outside the game's ticks, the renderer (the loading screen draws before game time
exists): the setting and the case's choice as of now, with no game time to ask */
boolean sim_experiment_enabled_now(
	short experiment)
{
	if (experiment < 0 || experiment >= SIM_EXPERIMENT_COUNT)
	{
		return FALSE;
	}

	return (sim_experiment_mask & (1u << experiment)) != 0 ?
		(sim_experiment_overrides & (1u << experiment)) != 0 : config_boolean(sim_experiment_names[experiment]) != 0;
}
