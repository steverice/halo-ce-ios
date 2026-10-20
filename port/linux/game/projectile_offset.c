/*
PROJECTILE_OFFSET.C

Controller aim, milestone 1 (the spec's section 2, "The projectile's
origin", option (b)): while a hand aims, the local
player's own projectiles are drawn as if they left the drawn gun's muzzle.
The simulation still fires them from the eye along the hand's aim (the shot
everyone else sees, and the one the tick moves); only the drawing moves.

As a projectile is created (weapons.c, trigger_create_projectiles), the
shot's origin, direction and speed are recorded here when its owner is
local player 0's unit and a hand aims. The first time the projectile, or
anything attached to it, is drawn, its lateral offset is taken: the drawn
muzzle (the first-person weapon's "primary trigger" marker, moved as the
rasterizer moves the gun's draws for display.weapon_offset_down and
display.weapon_offset_back) less the nearest point on the shot's line to it.
It is drawn offset by that, decaying by 1 - smoothstep(0, 0.15 s, age), so
within 0.15 s it is back on the eye's line. A bullet lands long before
that (at 324 units a second, on anything within 48 units), so the offset
also falls in a straight line along the shot, from all of it at the drawn
muzzle to none where the shot's line first meets the world (the reach,
tested once at the spawn, out to the distance the 0.15 s covers). The
lesser of the two shares is drawn, so the tracer runs from the muzzle to
the hit. With the time's share alone it ran beside the eye's line all the
way and ended the whole lateral offset (about 0.05 units, 15 cm) off the
hit: beside it on a surface that faces the shot, and inside the surface or
short of it where the surface slants.

What reads it, render-only, each offsetting a local copy and never the
game's data:
- the model (render_objects.c): a copy of the node matrices translated by
the offset (halo_projectile_matrices), and the bounding sphere's center;
- attached particles (render_particles.c) and the projectile's lights and
lens flares (object_lights.c): the offset at the projectile's age
(halo_projectile_offset);
- contrails (render_contrails.c) and a particle system's particles
(particle_systems.c): each point by the offset at the age the projectile
had when it passed it, its distance along the line over the shot's speed
(halo_projectile_point_offset), so a tracer bends from the muzzle onto the
eye's line. A contrail or system is matched by its projectile while that
lives, and its own datum index is kept in the record then, so its points
keep their offsets once the projectile is gone.

Unattached effect particles (smoke a projectile leaves in the world) keep
their simulated place: nothing links them to their projectile at render
time. Child projectiles from a detonation never pass through
trigger_create_projectiles and keep theirs.

The record is written during the tick but read only by the drawing: the
tick never reads it, and the drawing writes nothing the tick reads (the
reach's collision test reads the world and writes only the record). With no
hand aiming nothing is recorded, every lookup returns 0 at once, and every
frame is unchanged. The age is the model's, a tick behind the simulation
(render_interpolation.c): (game time - birth tick + the frame's fraction -
1) / 30 s.

port/ios/tests/projectile_offset_probe.c includes this file with the game's
types in the shapes it reads, and its own muzzle, clock, owner and hand.
*/

#include <math.h>
#include <string.h>

#ifndef PROJECTILE_OFFSET_PROBE
#include "cseries.h"
#include "math/real_math.h"
#include "models/model_definitions.h"
#include "objects/objects.h"
#include "game/game.h"
#include "game/players.h"
#include "interface/first_person_weapons.h"
#include "render/render.h"
#include "physics/collisions.h"
#include "units/units.h"
#include "../src/halo_stereo.h"

/* port/linux/game/render_interpolation.c */
real render_interpolation_fraction(void);
/* port/linux/game/controller_aim_game.c */
int halo_controller_hand_aiming(short local_player_index);
int halo_controller_weapon_frame(short local_player_index, const float camera_forward[3],
	const float camera_up[3], float forward[3], float up[3]);
/* port/linux/src/sdl_platform.c (the host's on iOS) */
void platform_log(const char *format, ...);
#endif

/* ---------- constants */

/* records kept, the oldest replaced: an automatic weapon's rounds over a
few seconds, long enough for its contrails to fade */
#define PROJECTILE_RECORDS 64
/* contrails and particle systems kept per record */
#define PROJECTILE_ATTACHMENTS 4
/* seconds over which the drawn offset decays (the spec's 0.15 s) */
#define PROJECTILE_OFFSET_SECONDS 0.15f
#define PROJECTILE_TICKS_PER_SECOND 30.0f
/* render-only node matrix copies used in turn: transparent models keep the
pointer until the transparent pass, so each drawn projectile needs its own */
#define PROJECTILE_MATRIX_SLOTS 32
/* lateral offsets logged per map, to check the muzzle against the drawing */
#define PROJECTILE_LOG_LIMIT 8

/* ---------- the game's side (defined at the end, or by the probe) */

/* the drawn muzzle, world units; 0 when the gun isn't drawn */
static int projectile_offset_muzzle(float muzzle[3]);
static long projectile_offset_game_time(void);
static float projectile_offset_fraction(void);
/* local player 0's unit, or NONE */
static long projectile_offset_local_unit(void);
static int projectile_offset_hand_aiming(void);
/* the new projectile's speed, world units a second */
static float projectile_offset_speed(long object_index);
/* the shot's reach: how far along its line (unit direction, from origin)
it first meets what a projectile collides with, the owner's ultimate parent
ignored as the projectile ignores it; negative when that's nothing within
range (world units) */
static float projectile_offset_reach(long owner_object_index, const float origin[3], const float direction[3],
	float range);
/* the new projectile's contrails and particle systems (datum indices; at
most PROJECTILE_ATTACHMENTS of each, the rest left -1) */
static void projectile_offset_attachments(long object_index, long contrails[PROJECTILE_ATTACHMENTS],
	long particle_systems[PROJECTILE_ATTACHMENTS]);

/* ---------- the record */

struct projectile_record
{
	long object_index; /* -1 (NONE): empty */
	long birth_tick;
	float origin[3];
	float direction[3];
	float speed;
	float reach; /* along the line to where the shot lands; negative: nothing within the time's decay */
	int lateral_taken;
	float lateral[3];
	float muzzle_along; /* the drawn muzzle's place along the line, taken with the lateral */
	long contrail_indices[PROJECTILE_ATTACHMENTS];
	long particle_system_indices[PROJECTILE_ATTACHMENTS];
};

static struct projectile_record projectile_records[PROJECTILE_RECORDS];
static int projectile_records_ready;
static int projectile_next_record;
/* the newest birth tick, for the quick test that nothing is young enough
for halo_projectile_offset; and whether anything was recorded since the
last reset, for the point offsets' quick test */
static long projectile_newest_birth_tick;
static int projectile_any_recorded;
static int projectile_cached_record = -1;
static int projectile_logged;

static struct real_matrix4x3 projectile_matrices[PROJECTILE_MATRIX_SLOTS][MAXIMUM_NODES_PER_MODEL];
static int projectile_next_matrix_slot;

static void records_empty(void)
{
	int record;
	int attachment;

	for (record = 0; record < PROJECTILE_RECORDS; record++)
	{
		memset(&projectile_records[record], 0, sizeof(projectile_records[record]));
		projectile_records[record].object_index = -1;
		for (attachment = 0; attachment < PROJECTILE_ATTACHMENTS; attachment++)
		{
			projectile_records[record].contrail_indices[attachment] = -1;
			projectile_records[record].particle_system_indices[attachment] = -1;
		}
	}
	projectile_next_record = 0;
	projectile_any_recorded = 0;
	projectile_cached_record = -1;
	projectile_logged = 0;
	projectile_records_ready = 1;
}

void halo_projectile_offset_reset(void)
{
	records_empty();
}

void halo_projectile_spawned(long object_index, long owner_object_index, const float origin[3],
	const float direction[3])
{
	struct projectile_record *record;
	long now;
	float length;
	int slot;
	int attachment;

	/* the owner first: every unit's every shot comes here */
	if (object_index == -1 || owner_object_index == -1 || owner_object_index != projectile_offset_local_unit() ||
		!projectile_offset_hand_aiming())
	{
		return;
	}
	length = sqrtf(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
	if (!(length > 0.0f))
		return;
	now = projectile_offset_game_time();
	/* a game time before the newest record's birth: a checkpoint was
	reverted, and the reverted game reuses the indices (and salts) of what
	was recorded after it */
	if (!projectile_records_ready || (projectile_any_recorded && now < projectile_newest_birth_tick))
		records_empty();
	/* the same index and salt recorded before (a revert, or the salt
	wrapping): that record is this projectile's now */
	for (slot = 0; slot < PROJECTILE_RECORDS && projectile_records[slot].object_index != object_index; slot++)
		;
	if (slot < PROJECTILE_RECORDS)
	{
		record = &projectile_records[slot];
	}
	else
	{
		record = &projectile_records[projectile_next_record];
		projectile_next_record = (projectile_next_record + 1) % PROJECTILE_RECORDS;
	}
	memset(record, 0, sizeof(*record));
	record->object_index = object_index;
	record->birth_tick = now;
	record->origin[0] = origin[0];
	record->origin[1] = origin[1];
	record->origin[2] = origin[2];
	record->direction[0] = direction[0] / length;
	record->direction[1] = direction[1] / length;
	record->direction[2] = direction[2] / length;
	record->speed = projectile_offset_speed(object_index);
	record->reach = projectile_offset_reach(owner_object_index, record->origin, record->direction,
		record->speed * PROJECTILE_OFFSET_SECONDS);
	for (attachment = 0; attachment < PROJECTILE_ATTACHMENTS; attachment++)
	{
		record->contrail_indices[attachment] = -1;
		record->particle_system_indices[attachment] = -1;
	}
	/* its contrails and particle systems as created (object_new makes the
	attachments before it returns), so a projectile that dies before it is
	first drawn still has its tracer drawn from the muzzle */
	projectile_offset_attachments(object_index, record->contrail_indices, record->particle_system_indices);
	projectile_newest_birth_tick = record->birth_tick;
	projectile_any_recorded = 1;
	if (projectile_cached_record >= 0 && projectile_records[projectile_cached_record].object_index != object_index)
		projectile_cached_record = -1;
}

static int record_find_object(long object_index)
{
	int record;

	if (object_index == -1)
		return -1;
	if (projectile_cached_record >= 0 && projectile_records[projectile_cached_record].object_index == object_index)
		return projectile_cached_record;
	for (record = 0; record < PROJECTILE_RECORDS; record++)
	{
		if (projectile_records[record].object_index == object_index)
			return projectile_cached_record = record;
	}
	return -1;
}

static int attachment_held(const long *indices, long index)
{
	int attachment;

	for (attachment = 0; attachment < PROJECTILE_ATTACHMENTS; attachment++)
	{
		if (indices[attachment] == index)
			return 1;
	}
	return 0;
}

static void attachment_keep(long *indices, long index)
{
	int attachment;

	if (index == -1 || attachment_held(indices, index))
		return;
	for (attachment = 0; attachment < PROJECTILE_ATTACHMENTS; attachment++)
	{
		if (indices[attachment] == -1)
		{
			indices[attachment] = index;
			return;
		}
	}
}

static int record_find_attachment(long contrail_index, long particle_system_index)
{
	int record;

	if (projectile_cached_record >= 0 &&
		((contrail_index != -1 &&
			attachment_held(projectile_records[projectile_cached_record].contrail_indices, contrail_index)) ||
		(particle_system_index != -1 &&
			attachment_held(projectile_records[projectile_cached_record].particle_system_indices, particle_system_index))))
	{
		return projectile_cached_record;
	}
	for (record = 0; record < PROJECTILE_RECORDS; record++)
	{
		if (projectile_records[record].object_index == -1)
			continue;
		if ((contrail_index != -1 && attachment_held(projectile_records[record].contrail_indices, contrail_index)) ||
			(particle_system_index != -1 &&
				attachment_held(projectile_records[record].particle_system_indices, particle_system_index)))
		{
			return projectile_cached_record = record;
		}
	}
	return -1;
}

/* on the projectile's first drawn frame: the drawn muzzle less the nearest
point on the shot's line to it (zero when the gun isn't drawn, kept so) */
static void record_take_lateral(struct projectile_record *record)
{
	float muzzle[3];
	float along;
	int axis;

	if (record->lateral_taken)
		return;
	record->lateral_taken = 1;
	if (!projectile_offset_muzzle(muzzle))
		return;
	along = (muzzle[0] - record->origin[0]) * record->direction[0] +
		(muzzle[1] - record->origin[1]) * record->direction[1] +
		(muzzle[2] - record->origin[2]) * record->direction[2];
	record->muzzle_along = along;
	for (axis = 0; axis < 3; axis++)
		record->lateral[axis] = muzzle[axis] - (record->origin[axis] + record->direction[axis] * along);
#ifndef PROJECTILE_OFFSET_PROBE
	if (projectile_logged < PROJECTILE_LOG_LIMIT)
	{
		projectile_logged++;
		platform_log("projectile offset: tick %ld speed %.1f lateral %.3f %.3f %.3f (%.3f units)", record->birth_tick,
			record->speed, record->lateral[0], record->lateral[1], record->lateral[2],
			sqrtf(record->lateral[0] * record->lateral[0] + record->lateral[1] * record->lateral[1] +
				record->lateral[2] * record->lateral[2]));
	}
#endif
}

/* 1 - smoothstep(0, PROJECTILE_OFFSET_SECONDS, age): the share of the
lateral offset left at an age; 0 from PROJECTILE_OFFSET_SECONDS on */
static float offset_share(float age_seconds)
{
	float t;

	if (!(age_seconds < PROJECTILE_OFFSET_SECONDS))
		return 0.0f;
	if (age_seconds <= 0.0f)
		return 1.0f;
	t = age_seconds / PROJECTILE_OFFSET_SECONDS;
	return 1.0f - t * t * (3.0f - 2.0f * t);
}

/* the distance's share of the lateral offset at a point along the shot's
line: all of it up to the drawn muzzle, none from the reach on, a straight
line between (the drawn shot's path from the muzzle to its hit); all of it
when the shot's line met nothing. A reach nearer than the muzzle (the gun
through a wall) leaves no span: all of it before the reach, none after */
static float reach_share(const struct projectile_record *record, float along)
{
	float span;

	if (record->reach < 0.0f)
		return 1.0f;
	if (!(along < record->reach))
		return 0.0f;
	span = record->reach - record->muzzle_along;
	if (!(along > record->muzzle_along) || !(span > 0.0f))
		return 1.0f;
	return (record->reach - along) / span;
}

/* the offset of a point the projectile passed at age_seconds, along world
units down the shot's line: the lateral by the lesser of the time's share
and the distance's */
static int record_offset(struct projectile_record *record, float age_seconds, float along, float offset[3])
{
	float share = offset_share(age_seconds);
	float reach;

	record_take_lateral(record);
	reach = reach_share(record, along);
	if (reach < share)
		share = reach;
	offset[0] = record->lateral[0] * share;
	offset[1] = record->lateral[1] * share;
	offset[2] = record->lateral[2] * share;
	return share > 0.0f && (offset[0] != 0.0f || offset[1] != 0.0f || offset[2] != 0.0f);
}

int halo_projectile_offset(long object_index, float offset[3])
{
	long now;
	float age;
	int record;

	offset[0] = offset[1] = offset[2] = 0.0f;
	if (!projectile_any_recorded || object_index == -1)
		return 0;
	now = projectile_offset_game_time();
	/* nothing is young enough */
	if ((float)(now - projectile_newest_birth_tick - 1) >= PROJECTILE_OFFSET_SECONDS * PROJECTILE_TICKS_PER_SECOND)
		return 0;
	record = record_find_object(object_index);
	/* (a game time before its birth: a checkpoint reverted past it) */
	if (record < 0 || now < projectile_records[record].birth_tick)
		return 0;
	age = ((float)(now - projectile_records[record].birth_tick) + projectile_offset_fraction() - 1.0f) /
		PROJECTILE_TICKS_PER_SECOND;
	if (!(age < PROJECTILE_OFFSET_SECONDS))
		return 0;
	/* (the model is where its age puts it along the line) */
	return record_offset(&projectile_records[record], age, age > 0.0f ? age * projectile_records[record].speed : 0.0f,
		offset);
}

int halo_projectile_point_offset(long object_index, long contrail_index, long particle_system_index,
	const float point[3], float offset[3])
{
	struct projectile_record *found;
	float along;
	int record;

	offset[0] = offset[1] = offset[2] = 0.0f;
	if (!projectile_any_recorded)
		return 0;
	record = record_find_object(object_index);
	if (record >= 0)
	{
		/* while the projectile lives: keep its contrail's and system's own
		indices, for after it is gone */
		attachment_keep(projectile_records[record].contrail_indices, contrail_index);
		attachment_keep(projectile_records[record].particle_system_indices, particle_system_index);
	}
	else
	{
		record = record_find_attachment(contrail_index, particle_system_index);
		if (record < 0)
			return 0;
	}
	found = &projectile_records[record];
	if (!(found->speed > 0.0f) || projectile_offset_game_time() < found->birth_tick)
		return 0;
	along = (point[0] - found->origin[0]) * found->direction[0] +
		(point[1] - found->origin[1]) * found->direction[1] +
		(point[2] - found->origin[2]) * found->direction[2];
	return record_offset(found, along / found->speed, along, offset);
}

const struct real_matrix4x3 *halo_projectile_matrices(long object_index, const struct real_matrix4x3 *matrices,
	short node_count)
{
	struct real_matrix4x3 *copy;
	float offset[3];
	short node;

	if (!matrices || node_count <= 0 || node_count > MAXIMUM_NODES_PER_MODEL ||
		!halo_projectile_offset(object_index, offset))
	{
		return matrices;
	}
	copy = projectile_matrices[projectile_next_matrix_slot];
	projectile_next_matrix_slot = (projectile_next_matrix_slot + 1) % PROJECTILE_MATRIX_SLOTS;
	for (node = 0; node < node_count; node++)
	{
		copy[node] = matrices[node];
		copy[node].position.x += offset[0];
		copy[node].position.y += offset[1];
		copy[node].position.z += offset[2];
	}
	return copy;
}

/* ---------- the game's side */

#ifndef PROJECTILE_OFFSET_PROBE
/* the first-person weapon's "primary trigger" marker as drawn this frame
(the gun's node matrices turn to the hand's aim, so the marker is
the turned gun's), less the rasterizer's view move for the weapon's draws:
rasterizer_first_person_offset (rasterizer_xbox.c) draws the gun moved down
by display.weapon_offset_down along the gun's up and back by
display.weapon_offset_back along its forward, the turned gun's while a hand
aims (halo_controller_weapon_frame), the camera's otherwise. In a
first-person seat the same gun is drawn and the same marker read */
static int projectile_offset_muzzle(float muzzle[3])
{
	long unit_index = player_control_get_unit_index(0);
	struct unit_datum *unit = unit_index != NONE ? unit_try_and_get(unit_index) : NULL;
	long weapon_index;
	struct object_marker marker;
	struct halo_stereo_eye weapon_eye;
	float down;
	float back;

	if (!unit || unit->unit.current_weapon_index < 0 ||
		unit->unit.current_weapon_index >= MAXIMUM_WEAPONS_PER_UNIT)
	{
		return 0;
	}
	weapon_index = unit->unit.weapon_object_indices[unit->unit.current_weapon_index];
	if (weapon_index == NONE || first_person_weapon_get_local_index(weapon_index) != 0 ||
		first_person_weapon_get_marker_by_name(weapon_index, "primary trigger", &marker, 1) < 1)
	{
		return 0;
	}
	muzzle[0] = marker.matrix.position.x;
	muzzle[1] = marker.matrix.position.y;
	muzzle[2] = marker.matrix.position.z;
	/* the rasterizer's test (rasterizer_set_frustum_z): a SCREEN mode eye
	draws the gun through its own eye instead of moving it */
	if (!halo_stereo_first_person_eye(&weapon_eye) && halo_stereo_weapon_offset(&down, &back))
	{
		float forward[3] = { render.camera.forward.i, render.camera.forward.j, render.camera.forward.k };
		float up[3] = { render.camera.up.i, render.camera.up.j, render.camera.up.k };
		float turned_forward[3];
		float turned_up[3];
		int axis;

		if (halo_controller_weapon_frame(0, forward, up, turned_forward, turned_up) == 1)
		{
			memcpy(forward, turned_forward, sizeof(forward));
			memcpy(up, turned_up, sizeof(up));
		}
		for (axis = 0; axis < 3; axis++)
			muzzle[axis] -= up[axis] * down + forward[axis] * back;
	}
	return 1;
}

static long projectile_offset_game_time(void)
{
	return game_time_get();
}

static float projectile_offset_fraction(void)
{
	return render_interpolation_fraction();
}

static long projectile_offset_local_unit(void)
{
	return player_control_get_unit_index(0);
}

static int projectile_offset_hand_aiming(void)
{
	return halo_controller_hand_aiming(0);
}

static void projectile_offset_attachments(long object_index, long contrails[PROJECTILE_ATTACHMENTS],
	long particle_systems[PROJECTILE_ATTACHMENTS])
{
	struct object_datum *object = object_try_and_get(object_index);
	int contrail_count = 0;
	int system_count = 0;
	int attachment;

	if (!object)
		return;
	for (attachment = 0; attachment < MAXIMUM_NUMBER_OF_ATTACHMENTS_PER_OBJECT; attachment++)
	{
		long index = object->object.attachment_indices[attachment];

		if (index == NONE)
			continue;
		if (object->object.attachment_types[attachment] == _object_attachment_type_contrail &&
			contrail_count < PROJECTILE_ATTACHMENTS)
		{
			contrails[contrail_count++] = index;
		}
		else if (object->object.attachment_types[attachment] == _object_attachment_type_particle_system &&
			system_count < PROJECTILE_ATTACHMENTS)
		{
			particle_systems[system_count++] = index;
		}
	}
}

static float projectile_offset_reach(long owner_object_index, const float origin[3], const float direction[3],
	float range)
{
	struct collision_result collision;
	real_point3d start;
	real_vector3d vector;

	if (!(range > 0.0f))
		return -1.0f;
	start.x = origin[0];
	start.y = origin[1];
	start.z = origin[2];
	vector.i = direction[0] * range;
	vector.j = direction[1] * range;
	vector.k = direction[2] * range;
	/* a projectile's own test and what it ignores (projectiles.c) */
	if (!collision_test_vector(_collision_test_for_projectiles_flags, &start, &vector,
		object_get_ultimate_parent(owner_object_index), &collision))
	{
		return -1.0f;
	}
	return (collision.point.x - start.x) * direction[0] + (collision.point.y - start.y) * direction[1] +
		(collision.point.z - start.z) * direction[2];
}

/* read as the projectile is created: its velocity (world units a tick) is
the shot's, its definition's initial velocity on the owner's */
static float projectile_offset_speed(long object_index)
{
	struct object_datum *object = object_try_and_get(object_index);

	if (!object)
		return 0.0f;
	return magnitude3d(&object->object.translational_velocity) * PROJECTILE_TICKS_PER_SECOND;
}
#endif
