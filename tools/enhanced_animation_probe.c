/* The script extracts the real helpers, cache storage and lifecycle callbacks. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int boolean;
typedef float real;
typedef unsigned char byte;
#define TRUE 1
#define FALSE 0
#define NONE -1
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAXIMUM_NODES_PER_MODEL 64
#define NUMBEROF(a) (sizeof(a) / sizeof((a)[0]))
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(i) ((i) & 65535)
#define PIN(x, a, b) ((x) < (a) ? (a) : ((x) > (b) ? (b) : (x)))
#define VALID_INDEX(i, n) ((i) >= 0 && (i) < (n))
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); return 1; } } while (0)

struct model_node { short sibling, child, parent_node_index; };
struct tag_block { long count; struct model_node *address; };
struct model { struct tag_block nodes; };
struct tag_validation { int unused; };
static boolean tag_validate_contains(struct tag_validation *v, void *p, unsigned long n)
{ (void)v; (void)p; (void)n; return TRUE; }
static void tag_validate_correct(struct tag_validation *v, const char *f, ...)
{ (void)v; (void)f; }
static void tag_validate_refuse(struct tag_validation *v, const char *f, ...)
{ (void)v; (void)f; }
static int node_reads;
static struct model_node *probe_node(struct tag_block *block, long index)
{
	if (++node_reads > 128 || !VALID_INDEX(index, block->count))
	{
		fprintf(stderr, "ancestry walk did not terminate inside its node block\n");
		exit(1);
	}
	return &block->address[index];
}
#define TAG_BLOCK_GET_ELEMENT(block, index, type) probe_node(block, index)

static long now;
static long game_time_get(void) { return now; }
enum { _unit_state_throw_grenade = 1 };
struct unit_datum { struct { struct { long state, action; } animation; } unit; };
static struct unit_datum probe_unit;
static struct unit_datum *unit_get(long index) { (void)index; return &probe_unit; }
static boolean unit_animation_enhanced(long index) { (void)index; return TRUE; }
static struct { long used_time; } probe_globals;
static void *unit_globals = &probe_globals;
/* offsetof needs the name used by the actual map callback. */
struct unit_globals { long used_time; };
typedef void (*game_state_before_load_proc)(void);
static void game_sound_clear(void) {}
static void cluster_partitions_port_forget(void) {}
static void object_bounds_cache_invalidate(void) {}

#include "under_test.inc"

int main(int argc, char **argv)
{
	const long first = 0x10007, replacement = 0x20007;
	const char *test = argc == 2 ? argv[1] : "";
	struct model_node nodes[3] = {{NONE, 1, NONE}, {2, NONE, 0}, {NONE, NONE, 0}};
	struct model model = {{3, nodes}};
	struct tag_validation validation = {0};

	if (!strcmp(test, "hierarchy"))
	{
		CHECK(unit_node_is_at_or_below(&model, 1, 1));
		CHECK(!unit_node_is_at_or_below(&model, 2, 1));
		CHECK(unit_node_is_at_or_below(&model, 2, 0));
		return 0;
	}
	if (!strcmp(test, "root"))
	{
		nodes[0].parent_node_index = 0;
		CHECK(node_tree_check(&validation, (byte *)nodes, 3, sizeof(*nodes),
			offsetof(struct model_node, sibling), offsetof(struct model_node, child), offsetof(struct model_node, parent_node_index)));
		CHECK(nodes[0].parent_node_index == 0);
		CHECK(!unit_node_is_at_or_below(&model, 0, 1));
		return 0;
	}
	if (!strcmp(test, "bounds"))
	{
		CHECK(!unit_node_is_at_or_below(&model, 3, 1));
		CHECK(!unit_node_is_at_or_below(&model, -2, 1));
		return 0;
	}
	if (!strcmp(test, "cycle"))
	{
		nodes[1].parent_node_index = 2;
		nodes[2].parent_node_index = 1;
		CHECK(!unit_node_is_at_or_below(&model, 1, 0));
		return 0;
	}

	if (!strcmp(test, "first-tick"))
	{
		units_initialize_for_new_map();
		now = 0;
		CHECK(unit_grenade_throw_ease(grenade_throw_leg_weights, grenade_throw_leg_weight_times, first, 1.f) == 1.f);
		return 0;
	}
	now = 100;
	if (!strncmp(test, "ik-", 3))
	{
		probe_unit.unit.animation.action = 1;
		CHECK(unit_grenade_throw_releases_vehicle_ik(first));
		probe_unit.unit.animation.action = 0;
		if (!strcmp(test, "ik-salt"))
		{
			now = 101;
			CHECK(!unit_grenade_throw_releases_vehicle_ik(replacement));
		}
		else if (!strcmp(test, "ik-map-reset"))
		{
			units_initialize_for_new_map();
			now = 100;
			CHECK(!unit_grenade_throw_releases_vehicle_ik(first));
		}
		else if (!strcmp(test, "ik-load-reset"))
		{
			game_state_call_before_load_procs();
			now = 100;
			CHECK(!unit_grenade_throw_releases_vehicle_ik(first));
		}
		else if (!strcmp(test, "ik-rewind"))
		{
			now = 200;
			(void)unit_grenade_throw_releases_vehicle_ik(0x10008);
			now = 100;
			CHECK(!unit_grenade_throw_releases_vehicle_ik(first));
		}
		else if (!strcmp(test, "ik-grace"))
		{
			now = 106;
			CHECK(unit_grenade_throw_releases_vehicle_ik(first));
			now = 107;
			CHECK(!unit_grenade_throw_releases_vehicle_ik(first));
		}
		else CHECK(FALSE);
		return 0;
	}

	CHECK(unit_grenade_throw_ease(grenade_throw_leg_weights, grenade_throw_leg_weight_times, first, 1.f) == 1.f);
	CHECK(unit_grenade_throw_ease(grenade_throw_air_weights, grenade_throw_air_weight_times, first, 1.f) == 1.f);
	long selected = first;
	if (!strcmp(test, "salt") || !strcmp(test, "same-tick"))
	{
		selected = replacement;
		if (!strcmp(test, "salt")) now = 101;
	}
	else if (!strcmp(test, "map-reset")) units_initialize_for_new_map();
	else if (!strcmp(test, "load-reset")) game_state_call_before_load_procs();
	else if (!strcmp(test, "rewind"))
	{
		now = 200;
		(void)unit_grenade_throw_ease(grenade_throw_leg_weights, grenade_throw_leg_weight_times, 0x10008, 1.f);
		now = 100;
	}
	else if (!strcmp(test, "leg-ease"))
	{
		now = 101;
		real eased = unit_grenade_throw_ease(grenade_throw_leg_weights, grenade_throw_leg_weight_times, first, 0.f);
		CHECK(eased > 0.8333f && eased < 0.8334f);
		CHECK(unit_grenade_throw_ease(grenade_throw_leg_weights, grenade_throw_leg_weight_times, first, 1.f) == eased);
		return 0;
	}
	else CHECK(FALSE);
	CHECK(unit_grenade_throw_ease(grenade_throw_leg_weights, grenade_throw_leg_weight_times, selected, 0.f) == 0.f);
	CHECK(unit_grenade_throw_ease(grenade_throw_air_weights, grenade_throw_air_weight_times, selected, 0.f) == 0.f);
	return 0;
}
