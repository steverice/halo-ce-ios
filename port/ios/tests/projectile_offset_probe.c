/* Your own projectiles drawn from the barrel (port/linux/game/projectile_offset.c,
included, not linked), with the probe's own muzzle, game clock, frame
fraction, local unit and hand.

Checks: a projectile spawned at the origin along +X, with the drawn muzzle
at (0.1, -0.3, -0.2), draws on its first frame offset by (0, -0.3, -0.2)
(lateral only); at 0.075 s by half that; at 0.15 s and later by zero; the
muzzle moving after the first frame changes nothing (the lateral is taken
once); a contrail point 2 units along the line, with the shot at 40 units a
second (passed at 0.05 s), offsets by 1 - smoothstep(0, 0.15, 0.05) of the
first offset, through the projectile and, once it is gone, through the
contrail's own index; a particle system's point likewise; a projectile
with another owner, or spawned while no hand aims, gets none; the 65th
record replaces the oldest; halo_projectile_offset_reset empties the table;
the node matrix copies are translated, the originals untouched, and the
ring gives 32 distinct slots before reusing one; after a checkpoint revert,
or the salt wrapping, a new projectile with a recorded index and salt draws
its own offset, not the stale record's; and a wall shot, gone before it is
first drawn, still draws its contrail and particle system from the muzzle
through the attachments recorded at its spawn; and a shot whose line meets
the world before the time's decay ends (a bullet at 324 units a second and
a rock 2.1 units away) draws in a straight line from the muzzle to the hit:
all of the offset at the muzzle, half of it halfway, none at the hit or
past it, for a contrail point and for the model at the age that puts it
there; the reach is asked for out to the speed times 0.15 s; a slow shot
takes the lesser of the time's share and the distance's; and a hit nearer
than the muzzle leaves all of the offset before it and none after. */
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef float real;
typedef struct { real i, j, k; } real_vector3d;
typedef struct { real x, y, z; } real_point3d;
struct real_matrix4x3
{
	real scale;
	real_vector3d forward, left, up;
	real_point3d position;
};
#define MAXIMUM_NODES_PER_MODEL 64

#define PROJECTILE_OFFSET_PROBE
#include "../../linux/game/projectile_offset.c"

/* the game's side, set by the probe */
static float probe_muzzle[3];
static int probe_muzzle_drawn;
static long probe_time;
static float probe_fraction;
static long probe_unit;
static int probe_hand;
static float probe_speed;

static int projectile_offset_muzzle(float muzzle[3])
{
	memcpy(muzzle, probe_muzzle, sizeof(probe_muzzle));
	return probe_muzzle_drawn;
}
static long projectile_offset_game_time(void) { return probe_time; }
static float projectile_offset_fraction(void) { return probe_fraction; }
static long projectile_offset_local_unit(void) { return probe_unit; }
static int projectile_offset_hand_aiming(void) { return probe_hand; }
static float projectile_offset_speed(long object_index) { (void)object_index; return probe_speed; }
/* where the shot's line first meets the world (negative: nowhere), and the
range the last spawn asked about */
static float probe_reach = -1.0f;
static float probe_range;
static float projectile_offset_reach(long owner_object_index, const float origin[3], const float direction[3],
	float range)
{
	(void)owner_object_index;
	(void)origin;
	(void)direction;
	probe_range = range;
	return probe_reach >= 0.0f && probe_reach <= range ? probe_reach : -1.0f;
}
/* the new projectile's attachments, as object_new made them */
static long probe_contrail = -1;
static long probe_system = -1;
static void projectile_offset_attachments(long object_index, long contrails[PROJECTILE_ATTACHMENTS],
	long particle_systems[PROJECTILE_ATTACHMENTS])
{
	(void)object_index;
	contrails[0] = probe_contrail;
	particle_systems[0] = probe_system;
}

static int failures;

static void check(int condition, const char *what)
{
	if (!condition)
	{
		printf("projectile offset probe: FAIL: %s\n", what);
		failures++;
	}
}

static int near3(const float a[3], float x, float y, float z)
{
	return fabsf(a[0] - x) < 1e-5f && fabsf(a[1] - y) < 1e-5f && fabsf(a[2] - z) < 1e-5f;
}

/* the model's age t seconds: game time and fraction with (time - birth +
fraction - 1) / 30 = t, from a birth at tick 100 */
static void set_age(float seconds)
{
	float ticks = seconds * 30.0f + 1.0f;

	probe_time = 100 + (long)floorf(ticks);
	probe_fraction = ticks - floorf(ticks);
}

int main(void)
{
	static const float origin[3] = { 0.0f, 0.0f, 0.0f };
	static const float along_x[3] = { 1.0f, 0.0f, 0.0f };
	const long unit = 0xe1230005;
	const long projectile = 0xe4560010;
	const long contrail = 0xa0010002;
	const long system = 0xb0020003;
	float offset[3];
	float point[3] = { 2.0f, 0.0f, 0.0f };
	float share;
	int index;

	halo_projectile_offset_reset();
	probe_unit = unit;
	probe_hand = 1;
	probe_speed = 40.0f;
	probe_muzzle[0] = 0.1f;
	probe_muzzle[1] = -0.3f;
	probe_muzzle[2] = -0.2f;
	probe_muzzle_drawn = 1;

	/* the spawn, during tick 100 */
	probe_time = 100;
	halo_projectile_spawned(projectile, unit, origin, along_x);

	/* its first drawn frame, the next tick's at fraction 0: age 0 */
	set_age(0.0f);
	check(halo_projectile_offset(projectile, offset) == 1, "first frame: recorded");
	check(near3(offset, 0.0f, -0.3f, -0.2f), "first frame: the lateral offset (0, -0.3, -0.2)");
	/* the muzzle moves on: the lateral was taken on the first frame */
	probe_muzzle[1] = 5.0f;
	set_age(0.075f);
	check(halo_projectile_offset(projectile, offset) == 1, "0.075 s: still offset");
	check(near3(offset, 0.0f, -0.15f, -0.1f), "0.075 s: half the offset");
	set_age(0.15f);
	check(halo_projectile_offset(projectile, offset) == 0 && near3(offset, 0.0f, 0.0f, 0.0f), "0.15 s: none");
	set_age(1.0f);
	check(halo_projectile_offset(projectile, offset) == 0 && near3(offset, 0.0f, 0.0f, 0.0f), "1 s: none");

	/* a contrail point 2 units along the line, passed at 2 / 40 = 0.05 s */
	share = 1.0f - (1.0f / 3.0f) * (1.0f / 3.0f) * (3.0f - 2.0f / 3.0f);
	check(halo_projectile_point_offset(projectile, contrail, -1, point, offset) == 1, "contrail point: offset");
	check(near3(offset, 0.0f, -0.3f * share, -0.2f * share), "contrail point: the share at 0.05 s");
	check(halo_projectile_point_offset(projectile, -1, system, point, offset) == 1 &&
		near3(offset, 0.0f, -0.3f * share, -0.2f * share), "system point: the share at 0.05 s");
	/* the projectile gone (its contrail's and system's object NONE) */
	check(halo_projectile_point_offset(-1, contrail, -1, point, offset) == 1 &&
		near3(offset, 0.0f, -0.3f * share, -0.2f * share), "contrail point, projectile gone: by its index");
	check(halo_projectile_point_offset(-1, -1, system, point, offset) == 1 &&
		near3(offset, 0.0f, -0.3f * share, -0.2f * share), "system point, projectile gone: by its index");
	point[0] = 6.0f; /* passed at 0.15 s */
	check(halo_projectile_point_offset(-1, contrail, -1, point, offset) == 0 && near3(offset, 0.0f, 0.0f, 0.0f),
		"a point passed at 0.15 s: none");
	point[0] = -1.0f; /* behind the origin: the whole offset */
	check(halo_projectile_point_offset(-1, contrail, -1, point, offset) == 1 &&
		near3(offset, 0.0f, -0.3f, -0.2f), "a point behind the origin: the whole offset");
	check(halo_projectile_point_offset(-1, 0xa0990002, -1, point, offset) == 0, "another contrail: none");

	/* another owner, or no hand: nothing recorded */
	probe_muzzle[1] = -0.3f;
	probe_time = 200;
	halo_projectile_spawned(0xe4570011, 0xe1240006, origin, along_x);
	halo_projectile_spawned(0xe4580012, -1, origin, along_x);
	probe_hand = 0;
	halo_projectile_spawned(0xe4590013, unit, origin, along_x);
	probe_hand = 1;
	probe_time = 201;
	probe_fraction = 0.0f;
	check(halo_projectile_offset(0xe4570011, offset) == 0, "another owner: none");
	check(halo_projectile_offset(0xe4580012, offset) == 0, "no owner: none");
	check(halo_projectile_offset(0xe4590013, offset) == 0, "spawned with no hand aiming: none");
	check(halo_projectile_point_offset(0xe4590013, 0xa0030004, -1, point, offset) == 0, "no hand: no point offset");

	/* the gun not drawn on the first frame (zoomed): none, and none later */
	probe_time = 300;
	halo_projectile_spawned(0xe45a0014, unit, origin, along_x);
	probe_muzzle_drawn = 0;
	probe_time = 301;
	check(halo_projectile_offset(0xe45a0014, offset) == 0, "gun not drawn: none");
	probe_muzzle_drawn = 1;
	check(halo_projectile_offset(0xe45a0014, offset) == 0, "gun drawn again: still none (taken once)");

	/* a checkpoint reverted past the birth: none */
	probe_time = 299;
	check(halo_projectile_offset(0xe45a0014, offset) == 0, "before its birth: none");

	/* a checkpoint revert: the reverted game reuses the index and salt of a
	projectile recorded after it, from another place; the new shot's offset */
	{
		static const float other_origin[3] = { 0.0f, 0.0f, 1.0f };
		const long reused = 0xe45b0015;

		halo_projectile_offset_reset();
		probe_time = 600;
		halo_projectile_spawned(reused, unit, origin, along_x);
		probe_time = 601;
		probe_fraction = 0.0f;
		check(halo_projectile_offset(reused, offset) == 1 && near3(offset, 0.0f, -0.3f, -0.2f), "before the revert");
		probe_time = 590; /* reverted */
		halo_projectile_spawned(reused, unit, other_origin, along_x);
		probe_time = 591;
		check(halo_projectile_offset(reused, offset) == 1 && near3(offset, 0.0f, -0.3f, -1.2f),
			"after a revert, the same index and salt: the new shot's offset");
		/* the same index and salt again without the time going back (the
		salt wrapping): the newer record, not the older */
		probe_time = 700;
		halo_projectile_spawned(reused, unit, origin, along_x);
		probe_time = 701;
		check(halo_projectile_offset(reused, offset) == 1 && near3(offset, 0.0f, -0.3f, -0.2f),
			"the same index and salt reused: the newest record");
	}

	/* a wall shot: the projectile dies in its first tick, before it is ever
	drawn, and its contrail's object is cleared; the contrail, known from the
	spawn, still bends from the muzzle */
	{
		const long wall_shot = 0xe45c0016;
		const long wall_contrail = 0xa0050006;
		const long wall_system = 0xb0060007;
		float near_point[3] = { 1.0f, 0.0f, 0.0f };

		halo_projectile_offset_reset();
		probe_contrail = wall_contrail;
		probe_system = wall_system;
		probe_time = 800;
		halo_projectile_spawned(wall_shot, unit, origin, along_x);
		probe_contrail = probe_system = -1;
		probe_time = 801;
		probe_fraction = 0.0f;
		share = 1.0f - (1.0f / 6.0f) * (1.0f / 6.0f) * (3.0f - 2.0f / 6.0f); /* 1 unit at 40 u/s: 0.025 s */
		check(halo_projectile_point_offset(-1, wall_contrail, -1, near_point, offset) == 1 &&
			near3(offset, 0.0f, -0.3f * share, -0.2f * share), "a wall shot's contrail, its projectile gone before drawn");
		check(halo_projectile_point_offset(-1, -1, wall_system, near_point, offset) == 1 &&
			near3(offset, 0.0f, -0.3f * share, -0.2f * share), "a wall shot's particle system likewise");
	}

	/* a bullet that lands 2.1 units away, long before the time's decay ends
	(48.6 units at 324 a second): the offset falls in a straight line from
	the muzzle (0.1 along) to the hit, so the drawn shot ends on the hit */
	{
		const long near_shot = 0xe45d0017;
		const long near_contrail = 0xa0070008;
		float at[3] = { 0.0f, 0.0f, 0.0f };

		halo_projectile_offset_reset();
		probe_speed = 324.0f;
		probe_reach = 2.1f;
		probe_contrail = near_contrail;
		probe_time = 900;
		halo_projectile_spawned(near_shot, unit, origin, along_x);
		probe_contrail = -1;
		check(fabsf(probe_range - 324.0f * 0.15f) < 1e-3f, "the reach is asked for out to the time's decay");
		probe_time = 901;
		probe_fraction = 0.0f;
		check(halo_projectile_offset(near_shot, offset) == 1 && near3(offset, 0.0f, -0.3f, -0.2f),
			"a near hit: the model's first frame has the whole offset");
		at[0] = 0.1f;
		check(halo_projectile_point_offset(-1, near_contrail, -1, at, offset) == 1 &&
			fabsf(offset[1] + 0.3f) < 1e-4f && fabsf(offset[2] + 0.2f) < 1e-4f, "a near hit: the whole offset at the muzzle");
		at[0] = 1.1f;
		check(halo_projectile_point_offset(-1, near_contrail, -1, at, offset) == 1 &&
			near3(offset, 0.0f, -0.15f, -0.1f), "a near hit: half the offset halfway from the muzzle to the hit");
		at[0] = 2.1f;
		check(halo_projectile_point_offset(-1, near_contrail, -1, at, offset) == 0 && near3(offset, 0.0f, 0.0f, 0.0f),
			"a near hit: none at the hit");
		at[0] = 3.0f;
		check(halo_projectile_point_offset(-1, near_contrail, -1, at, offset) == 0 && near3(offset, 0.0f, 0.0f, 0.0f),
			"a near hit: none past it");
		/* the model at the age that puts it halfway (1.1 units at 324 a second) */
		probe_fraction = 1.1f / 324.0f * 30.0f;
		check(halo_projectile_offset(near_shot, offset) == 1 && fabsf(offset[1] + 0.15f) < 1e-4f &&
			fabsf(offset[2] + 0.1f) < 1e-4f, "a near hit: the model halfway has half the offset");
		probe_fraction = 2.2f / 324.0f * 30.0f;
		check(halo_projectile_offset(near_shot, offset) == 0 && near3(offset, 0.0f, 0.0f, 0.0f),
			"a near hit: the model past the hit has none");

		/* a slow shot (40 units a second) with a hit 5.9 units away, inside
		its 6 units of decay: at 2 units the time's share is 0.7407 and the
		distance's (5.9 - 2) / (5.9 - 0.1); the lesser */
		halo_projectile_offset_reset();
		probe_speed = 40.0f;
		probe_reach = 5.9f;
		probe_contrail = near_contrail;
		probe_time = 950;
		halo_projectile_spawned(near_shot, unit, origin, along_x);
		probe_contrail = -1;
		probe_time = 951;
		probe_fraction = 0.0f;
		at[0] = 2.0f;
		share = (5.9f - 2.0f) / (5.9f - 0.1f);
		check(halo_projectile_point_offset(-1, near_contrail, -1, at, offset) == 1 &&
			near3(offset, 0.0f, -0.3f * share, -0.2f * share), "a slow shot: the lesser of the two shares");
		/* a hit 6.5 units away is past the 6 the time's decay covers: the time's share alone */
		halo_projectile_offset_reset();
		probe_reach = 6.5f;
		probe_contrail = near_contrail;
		probe_time = 960;
		halo_projectile_spawned(near_shot, unit, origin, along_x);
		probe_contrail = -1;
		probe_time = 961;
		share = 1.0f - (1.0f / 3.0f) * (1.0f / 3.0f) * (3.0f - 2.0f / 3.0f);
		check(halo_projectile_point_offset(-1, near_contrail, -1, at, offset) == 1 &&
			near3(offset, 0.0f, -0.3f * share, -0.2f * share), "a hit past the time's decay: the time's share");

		/* a hit nearer than the muzzle (the gun through a wall): all of the
		offset before it, none from it on */
		halo_projectile_offset_reset();
		probe_reach = 0.05f;
		probe_contrail = near_contrail;
		probe_time = 970;
		halo_projectile_spawned(near_shot, unit, origin, along_x);
		probe_contrail = -1;
		probe_time = 971;
		at[0] = 0.02f;
		check(halo_projectile_point_offset(-1, near_contrail, -1, at, offset) == 1 &&
			fabsf(offset[1] + 0.3f) < 1e-4f && fabsf(offset[2] + 0.2f) < 1e-4f, "a hit before the muzzle: the whole offset before it");
		at[0] = 0.05f;
		check(halo_projectile_point_offset(-1, near_contrail, -1, at, offset) == 0, "a hit before the muzzle: none from it on");
		probe_reach = -1.0f;
		probe_speed = 40.0f;
	}

	/* 65 records: the 65th replaces the oldest */
	halo_projectile_offset_reset();
	probe_time = 400;
	for (index = 0; index < 65; index++)
		halo_projectile_spawned(0x10000000 + index, unit, origin, along_x);
	probe_time = 401;
	probe_fraction = 0.0f;
	check(halo_projectile_offset(0x10000000, offset) == 0, "65 records: the oldest replaced");
	check(halo_projectile_offset(0x10000001, offset) == 1, "65 records: the second kept");
	check(halo_projectile_offset(0x10000000 + 64, offset) == 1, "65 records: the 65th kept");

	/* the reset empties it */
	halo_projectile_offset_reset();
	check(halo_projectile_offset(0x10000001, offset) == 0, "reset: the table empty");
	check(halo_projectile_offset(0x10000000 + 64, offset) == 0, "reset: the newest gone too");

	/* the node matrices: a translated copy, from 32 distinct slots in turn */
	{
		struct real_matrix4x3 nodes[2];
		struct real_matrix4x3 saved[2];
		const struct real_matrix4x3 *slots[33];
		int other;
		int distinct = 1;

		memset(nodes, 0, sizeof(nodes));
		nodes[0].scale = nodes[1].scale = 1.0f;
		nodes[0].forward.i = nodes[1].forward.i = 1.0f;
		nodes[0].position.x = 3.0f;
		nodes[1].position.x = 3.5f;
		nodes[1].position.z = 0.25f;
		memcpy(saved, nodes, sizeof(nodes));
		probe_time = 500;
		halo_projectile_spawned(projectile, unit, origin, along_x);
		probe_time = 501;
		probe_fraction = 0.0f;
		check(halo_projectile_matrices(0x77770001, nodes, 2) == nodes, "not recorded: the matrices unchanged");
		slots[0] = halo_projectile_matrices(projectile, nodes, 2);
		check(slots[0] != nodes, "recorded: a copy");
		check(fabsf(slots[0][0].position.x - 3.0f) < 1e-6f && fabsf(slots[0][0].position.y + 0.3f) < 1e-6f &&
			fabsf(slots[0][0].position.z + 0.2f) < 1e-6f && fabsf(slots[0][1].position.z - 0.05f) < 1e-6f,
			"the copy translated by the offset");
		check(slots[0][0].scale == 1.0f && slots[0][0].forward.i == 1.0f, "the copy's orientation kept");
		check(!memcmp(nodes, saved, sizeof(nodes)), "the object's own matrices never change");
		for (index = 1; index < 33; index++)
			slots[index] = halo_projectile_matrices(projectile, nodes, 2);
		for (index = 0; index < 32; index++)
		{
			for (other = index + 1; other < 32; other++)
			{
				if (slots[index] == slots[other])
					distinct = 0;
			}
		}
		check(distinct, "32 distinct slots");
		check(slots[32] == slots[0], "the 33rd reuses the first");
		set_age(0.2f);
		probe_time += 400; /* well past */
		check(halo_projectile_matrices(projectile, nodes, 2) == nodes, "past 0.15 s: the matrices unchanged");
	}

	if (failures)
	{
		printf("projectile offset probe: %d failures\n", failures);
		return 1;
	}
	printf("projectile offset probe: OK\n");
	return 0;
}
