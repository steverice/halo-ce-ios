/*
SIM_EXPERIMENTS.C

The experimental simulation speed-ups (debug.collision_type_filter,
debug.sphere_query_type_filter, debug.static_light_relink), each off by
default. A setting is read again at the start of each game tick, so one can
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
};
#define SIM_EXPERIMENT_COUNT ((long)(sizeof(sim_experiment_names) / sizeof(sim_experiment_names[0])))

static boolean sim_experiment_values[SIM_EXPERIMENT_COUNT];
static long sim_experiment_tick = -2;

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
			sim_experiment_values[index] = config_boolean(sim_experiment_names[index]) != 0;
		}
	}

	return experiment >= 0 && experiment < SIM_EXPERIMENT_COUNT && sim_experiment_values[experiment];
}
