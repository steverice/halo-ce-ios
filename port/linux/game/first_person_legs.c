/*
FIRST_PERSON_LEGS.C

Your own legs in first person (Video Setup > FOV and Viewmodels > Legs): the
player's own body, which the game animates as everyone else sees it but does
not draw for its own player, drawn from the waist down. Look down while
standing, running, strafing or crouching and the legs move as the body's
animation moves them.

Only the drawing is changed: a copy of the body's pose, all but its hips and
legs folded into a lid over the waist, the whole set back a little behind
the eye so the camera never looks out through the hips. The body itself, its hit boxes and markers, are the game's
as ever. The legs are drawn in the world with the world's camera, so they
keep their place and size at any field of view.
*/

#include "cseries.h"
#include "real_math.h"
#include "objects/objects.h"
#include "models/model_definitions.h"
#include "units/units.h"
#include "units/unit_definitions.h"
#include "render/render.h"
#include "rasterizer/rasterizer.h"
#include <xtl.h>
#include "rasterizer/xbox/rasterizer_xbox.h"

#include "object_mesh.h"


#include <math.h>
#include <string.h>

/* port/linux/src/port_config.c */
int config_boolean(const char *name);
double config_real(const char *name);
unsigned long config_changes(void);

enum
{
	FIRST_PERSON_LEGS_MAXIMUM_NODES = 64,
	FIRST_PERSON_LEGS_MAXIMUM_RIM = 64,
};

/* how far the body is set back behind the eye at least, world units (about
12 cm), and at most (about 1 m) */
#define FIRST_PERSON_LEGS_SET_BACK 0.04f
#define FIRST_PERSON_LEGS_SET_BACK_MAXIMUM 0.33f
/* how far out of the view the waist's rim is kept (world units: 3 cm) */
#define FIRST_PERSON_LEGS_OUT_OF_VIEW 0.01f
/* the point the upper body folds into: the middle of the waist's rim, a
little above it (world units: about 3 cm), so the skin that joined hips to
chest closes the waist as a low lid, flush with the belt (the body's model is
a hollow shell: folded higher, it stood up as a cone before the eye; lower,
it sank into a funnel) */
#define FIRST_PERSON_LEGS_LID_UP 0.01f
/* in a vehicle's seat, the farthest the hips may sit from the camera, level
(world units: a Warthog passenger's sit about 0.22 from it), and the farthest
ahead of it along the way the body faces (a passenger's are behind it; hips
ahead of the eye would stand the legs in front of it) */
#define FIRST_PERSON_LEGS_SEAT_HIPS_MAXIMUM 0.3f
#define FIRST_PERSON_LEGS_SEAT_HIPS_AHEAD_MAXIMUM 0.05f
/* in a seat the game puts the camera on the vehicle's own marker, ahead of
where it seats the body: a Warthog passenger's hips are about 0.22 world
units (0.67 m) behind the eye, so looking down showed the footwell and the
waist somewhere behind you. The seated body is drawn moved ahead, along the
way it faces, by display.first_person_legs_seat_shift (0 to 1) of the way
that brings the hips under the eye, and never by more than this (world
units, 0.37 m): a Warthog passenger rides with the right knee up, and moved
0.2 that knee stood in the dash's side panel, while at 0.11 the thighs come
into the bottom of the view and nothing meets the dash */
#define FIRST_PERSON_LEGS_SEAT_SHIFT_MAXIMUM 0.12f
/* the seat flag "allows weapons" (unit_definitions.h names its neighbors, not
it): the rider keeps the weapon in hand and the first-person view of it, as
a Warthog's passenger and a Pelican's riders do. A seat without it is one a
script puts the unit in, like the cryo tube */
#define FIRST_PERSON_LEGS_SEAT_ALLOWS_WEAPONS_BIT 5

static real_matrix4x3 first_person_legs_matrices[FIRST_PERSON_LEGS_MAXIMUM_NODES];
/* the body's shadow: its whole pose, set back as the legs last were, so the
shadow lies under the legs that are drawn; and whose legs those were */
static real_matrix4x3 first_person_legs_shadow_matrices[FIRST_PERSON_LEGS_MAXIMUM_NODES];
static real_vector3d first_person_legs_last_set_back = { 0.f, 0.f, 0.f };
static long first_person_legs_last_object = NONE;
static real first_person_legs_seat_shift_share = 1.f;
static boolean first_person_legs_enabled(
	void)
{
	static unsigned long changes = (unsigned long)-1;
	static boolean enabled = FALSE;

	if (changes != config_changes())
	{
		real share = (real)config_real("display.first_person_legs_seat_shift");

		changes = config_changes();
		enabled = config_boolean("display.first_person_legs") != 0;
		first_person_legs_seat_shift_share = share != share ? 1.f : share < 0.f ? 0.f : share > 1.f ? 1.f : share;
	}
	return enabled;
}

/* whether a unit rides in a seat that shows its legs: a vehicle's seat
whose camera is the rider's own eyes (not one on the gun or behind the
vehicle) and that lets the rider keep the weapon out (a seat the player
takes, not one a script holds the unit in), once it sits (not while it
climbs in or out, when the body swings through the camera) */
static boolean first_person_legs_seated(
	long object_index)
{
	struct object_datum *object = object_get(object_index);
	struct object_datum *parent = object_try_and_get(object->object.parent_object_index);
	struct unit_datum *unit = unit_get(object_index);
	struct unit_definition *definition;
	struct unit_seat const *seat;

	if (!parent || parent->object.type != _object_type_vehicle || unit->unit.parent_seat_index == NONE)
		return FALSE;
	definition = unit_definition_get(parent->definition_index);
	if (!definition || !VALID_INDEX(unit->unit.parent_seat_index, definition->unit.seats.count))
		return FALSE;
	seat = TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, unit->unit.parent_seat_index, struct unit_seat);
	return TEST_FLAG(seat->flags, FIRST_PERSON_LEGS_SEAT_ALLOWS_WEAPONS_BIT) &&
		!TEST_FLAG(seat->flags, _unit_seat_third_person_camera_bit) &&
		!TEST_FLAG(seat->flags, _unit_seat_first_person_camera_bit) &&
		unit->unit.animation.state != _unit_state_entering_seat &&
		unit->unit.animation.state != _unit_state_exiting_seat;
}

/* the player's own unit, seen from inside it: its legs drawn (on foot or in a
seat that shows them,
alive, not hidden, and not while a script plays an animation on it: a
cutscene that keeps the camera in the unit's eyes poses the body for the
camera, not for a view from the waist) */
boolean first_person_legs_wanted(
	long object_index)
{
	struct object_datum *object = object_try_and_get(object_index);

	return first_person_legs_enabled() && object && object->object.type == _object_type_biped &&
		(object->object.parent_object_index == NONE || first_person_legs_seated(object_index)) &&
		!TEST_FLAG(object->object.damage_flags, _object_dead_bit) &&
		!TEST_FLAG(object->object.flags, _object_invisible_bit) &&
		!unit_is_playing_custom_animation(object_index);
}

/* whether a node's name has part in it, whatever its case (the game's
characters name theirs "bip01 l thigh", "Bip01 Pelvis") */
static boolean first_person_legs_name_has(
	char const *name,
	char const *part)
{
	char lower[TAG_STRING_LENGTH + 1];
	short index;

	for (index = 0; index < TAG_STRING_LENGTH && name[index]; index++)
		lower[index] = (char)(name[index] >= 'A' && name[index] <= 'Z' ? name[index] - 'A' + 'a' : name[index]);
	lower[index] = 0;
	return strstr(lower, part) != NULL;
}

/* a node of the legs: the hips, thighs, calves, feet and toes */
static boolean first_person_legs_node(
	char const *name)
{
	static char const *const parts[] = { "pelvis", "thigh", "calf", "foot", "toe", "leg" };
	short index;

	for (index = 0; index < (short)NUMBEROF(parts); index++)
	{
		if (first_person_legs_name_has(name, parts[index]))
			return TRUE;
	}
	return FALSE;
}

/* the middle of the waist's rim in the model's space: the hips' vertices
on triangles that run up into the body above them (FALSE: none), kept for
the mesh it was found in (a new map forgets it: first_person_legs_reset) */
static real_point3d first_person_legs_rim[FIRST_PERSON_LEGS_MAXIMUM_RIM];
static short first_person_legs_rim_count = 0;
static long first_person_legs_waist_key = NONE;
static long first_person_legs_waist_model = NONE;
static boolean first_person_legs_waist_found = FALSE;
static real_point3d first_person_legs_waist_middle;

/* a new map: the waist found, and the mesh it was found in, forgotten (the
new map's models have indices of their own) */
void first_person_legs_reset(
	void)
{
	first_person_legs_waist_key = NONE;
	first_person_legs_waist_model = NONE;
	first_person_legs_rim_count = 0;
	first_person_legs_last_object = NONE;
	object_mesh_reset();
}

static boolean first_person_legs_waist(
	struct object_mesh const *mesh,
	struct model const *model,
	short pelvis,
	real_point3d *middle)
{
	real_point3d sum = { 0.f, 0.f, 0.f };
	long found = 0;
	long index;

	if (mesh->key == first_person_legs_waist_key && mesh->model_index == first_person_legs_waist_model)
	{
		*middle = first_person_legs_waist_middle;
		return first_person_legs_waist_found;
	}
	first_person_legs_rim_count = 0;
	for (index = 0; index < mesh->triangle_count; index++)
	{
		boolean hips = FALSE;
		boolean above = FALSE;
		short corner;

		for (corner = 0; corner < 3; corner++)
		{
			struct object_mesh_vertex const *vertex = &mesh->vertices[mesh->triangles[index][corner]];
			short n;

			if (vertex->nodes[0] == pelvis && (vertex->nodes[1] == pelvis || vertex->weight >= 32767))
				hips = TRUE;
			for (n = 0; n < 2; n++)
			{
				if (vertex->nodes[n] < model->nodes.count && (n == 0 || vertex->weight < 32767) &&
					!first_person_legs_node(TAG_BLOCK_GET_ELEMENT(&model->nodes, vertex->nodes[n], struct model_node)->name))
				{
					above = TRUE;
				}
			}
		}
		if (!hips || !above)
			continue;
		for (corner = 0; corner < 3; corner++)
		{
			struct object_mesh_vertex const *vertex = &mesh->vertices[mesh->triangles[index][corner]];

			if (vertex->nodes[0] == pelvis && (vertex->nodes[1] == pelvis || vertex->weight >= 32767))
			{
				sum.x += vertex->position.x;
				sum.y += vertex->position.y;
				sum.z += vertex->position.z;
				found++;
				if (first_person_legs_rim_count < FIRST_PERSON_LEGS_MAXIMUM_RIM)
					first_person_legs_rim[first_person_legs_rim_count++] = vertex->position;
			}
		}
	}
	first_person_legs_waist_key = mesh->key;
	first_person_legs_waist_model = mesh->model_index;
	first_person_legs_waist_found = found > 0;
	if (found > 0)
	{
		first_person_legs_waist_middle.x = sum.x / found;
		first_person_legs_waist_middle.y = sum.y / found;
		first_person_legs_waist_middle.z = sum.z / found + FIRST_PERSON_LEGS_LID_UP;
	}
	*middle = first_person_legs_waist_middle;
	return first_person_legs_waist_found;
}

/* the pose to draw the legs with: the body's own (as it is drawn now), all
but the legs folded into the hips, the whole set back behind the eye; NULL:
none (a body without named legs) */
real_matrix4x3 const *first_person_legs_node_matrices(
	long object_index,
	real_matrix4x3 const *nodes)
{
	struct object_datum *object = object_try_and_get(object_index);
	struct object_definition *definition;
	struct model const *model;
	long count;
	short pelvis = NONE;
	short thighs = 0;
	short index;
	real_point3d fold;
	real_vector3d back;
	real_matrix4x3 skin;
	real set_back;
	/* (a seated body's move ahead: back with the least set back stays zero there) */
	real_vector3d seat_shift = { 0.f, 0.f, 0.f };

	if (!object || !nodes)
		return NULL;
	definition = object_definition_get(object->definition_index);
	if (!definition || definition->object.model.index == NONE)
		return NULL;
	model = model_definition_get(definition->object.model.index);
	count = object->object.node_matrices.size / (long)sizeof(real_matrix4x3);
	if (!model || count <= 0 || count > FIRST_PERSON_LEGS_MAXIMUM_NODES || model->nodes.count < count)
		return NULL;

	for (index = 0; index < count; index++)
	{
		struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);

		if (pelvis == NONE && first_person_legs_name_has(node->name, "pelvis"))
			pelvis = index;
		if (first_person_legs_name_has(node->name, "thigh"))
			thighs++;
	}
	if (pelvis == NONE || thighs == 0)
		return NULL;
	/* (the waist's middle as the hips are posed now) */
	{
		struct object_mesh const *mesh = object_mesh_get(object_index);
		real_point3d middle;

		if (!mesh || pelvis >= mesh->node_count || !first_person_legs_waist(mesh, model, pelvis, &middle))
			return NULL;
		matrix4x3_multiply(&nodes[pelvis], &mesh->inverse[pelvis], &skin);
		matrix4x3_transform_point(&skin, &middle, &fold);
	}

	/* (set back away from the way the eye looks, level) */
	back.i = -global_window_parameters.camera.forward.i;
	back.j = -global_window_parameters.camera.forward.j;
	back.k = 0.f;
	if (normalize3d(&back) <= 0.f)
		back = *global_zero_vector3d;

	/* (in a seat the body stays where it sits: set back, the legs would
	pass through the seat. Only with the hips under the camera, as a
	rider's are) */
	if (object->object.parent_object_index != NONE)
	{
		real_vector3d facing;
		real_vector3d hips;
		real level;

		hips.i = nodes[pelvis].position.x - global_window_parameters.camera.position.x;
		hips.j = nodes[pelvis].position.y - global_window_parameters.camera.position.y;
		hips.k = 0.f;
		if (hips.i * hips.i + hips.j * hips.j >
			FIRST_PERSON_LEGS_SEAT_HIPS_MAXIMUM * FIRST_PERSON_LEGS_SEAT_HIPS_MAXIMUM)
		{
			return NULL;
		}
		object_get_orientation(object_index, &facing, NULL);
		level = facing.i * facing.i + facing.j * facing.j;
		if (level > 0.0001f &&
			(hips.i * facing.i + hips.j * facing.j) / (real)sqrt(level) > FIRST_PERSON_LEGS_SEAT_HIPS_AHEAD_MAXIMUM)
		{
			return NULL;
		}
		back = *global_zero_vector3d;
		/* (ahead by as far as the hips sit behind the eye, along the way
		the body faces, level: the legs keep their pose and their side of
		the seat, and come under the view) */
		if (level > 0.0001f)
		{
			real behind = -(hips.i * facing.i + hips.j * facing.j) / (real)sqrt(level);
			real ahead = behind * first_person_legs_seat_shift_share;

			if (ahead > FIRST_PERSON_LEGS_SEAT_SHIFT_MAXIMUM)
				ahead = FIRST_PERSON_LEGS_SEAT_SHIFT_MAXIMUM;
			if (ahead > 0.f)
			{
				seat_shift.i = facing.i / (real)sqrt(level) * ahead;
				seat_shift.j = facing.j / (real)sqrt(level) * ahead;
			}
		}
	}

	/* (as far back as keeps the waist's rim out of the bottom of the view,
	as Halo 2 keeps its first person body's open top out of it: looking
	ahead, the least; looking down, the legs stand behind the eye, the
	thighs, knees and feet seen as they swing forward) */
	set_back = FIRST_PERSON_LEGS_SET_BACK;
	if (object->object.parent_object_index == NONE)
	{
		real_plane3d const *bottom = &global_window_parameters.frustum.world_planes[2];
		real toward = dot_product3d(&bottom->n, &back);
		short corner;

		if (toward > 0.05f)
		{
			for (corner = 0; corner < first_person_legs_rim_count; corner++)
			{
				real_point3d p;
				real distance;

				matrix4x3_transform_point(&skin, &first_person_legs_rim[corner], &p);
				distance = plane3d_distance_to_point(bottom, &p) + toward * FIRST_PERSON_LEGS_SET_BACK;
				if (distance < FIRST_PERSON_LEGS_OUT_OF_VIEW)
				{
					real needed = FIRST_PERSON_LEGS_SET_BACK + (FIRST_PERSON_LEGS_OUT_OF_VIEW - distance) / toward;

					if (needed > set_back)
						set_back = needed;
				}
			}
		}
		if (set_back > FIRST_PERSON_LEGS_SET_BACK_MAXIMUM)
			set_back = FIRST_PERSON_LEGS_SET_BACK_MAXIMUM;
	}

	first_person_legs_last_set_back.i = back.i * set_back + seat_shift.i;
	first_person_legs_last_set_back.j = back.j * set_back + seat_shift.j;
	first_person_legs_last_set_back.k = back.k * set_back;
	first_person_legs_last_object = object_index;

	for (index = 0; index < count; index++)
	{
		struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, index, struct model_node);
		real_matrix4x3 *matrix = &first_person_legs_matrices[index];

		*matrix = nodes[index];
		/* (the root kept: it is the body's frame, not a part of it) */
		if (node->parent_node_index != NONE && !first_person_legs_node(node->name))
		{
			matrix->position = fold;
			matrix->scale = 0.001f;
		}
		matrix->position.x += back.i * set_back + seat_shift.i;
		matrix->position.y += back.j * set_back + seat_shift.j;
		matrix->position.z += back.k * set_back;
	}
	return first_person_legs_matrices;
}

/* the pose to draw the body's shadow with, after its legs were drawn in the
same view: the whole body (the head and arms too: the ground shows the
shadow of all of it), set back as the legs were; NULL: none (no legs were
drawn for this body) */
real_matrix4x3 const *first_person_legs_shadow_node_matrices(
	long object_index,
	real_matrix4x3 const *nodes)
{
	struct object_datum *object = object_try_and_get(object_index);
	long count;
	short index;

	if (!object || !nodes || object_index != first_person_legs_last_object)
		return NULL;
	count = object->object.node_matrices.size / (long)sizeof(real_matrix4x3);
	if (count <= 0 || count > FIRST_PERSON_LEGS_MAXIMUM_NODES)
		return NULL;
	for (index = 0; index < count; index++)
	{
		real_matrix4x3 *matrix = &first_person_legs_shadow_matrices[index];

		*matrix = nodes[index];
		matrix->position.x += first_person_legs_last_set_back.i;
		matrix->position.y += first_person_legs_last_set_back.j;
		matrix->position.z += first_person_legs_last_set_back.k;
	}
	return first_person_legs_shadow_matrices;
}
