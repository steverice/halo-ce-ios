/*
FIRST_PERSON_BODY.C

The Master Chief's body below the view in head-tracked stereo
(port/linux/src/halo_stereo.h; the stereo spec's "The first-person body in
head-tracked stereo" and "First-person scale and the body"). The game hides
the player's own unit in first person (render_objects.c's
object_is_first_person_camera); looking down in HEAD mode then shows no
feet. With display.first_person_body, the eye passes draw that unit anyway,
after the first-person weapon and under its stencil (render_objects.c),
with a render-only copy of its node matrices:

- the head and neck are gone: the subtree of the node named neck, less the
  clavicles hung from it, collapsed at scale 0 to a cap BODY_NECK_RAISE
  above the neck node, along spine1-to-neck, so the collar's outside faces
  close over the opening (Task 7f's lesson at the waist: collapsed to the
  ring itself, or below it into the chest, the shell showed its inside). A
  model with no neck node collapses only its head, to its parent's
  position;
- while the first-person weapon shows, each arm from the upper arm down
  collapses to the upper arm too, or the third-person arms would double
  the first-person ones;
- the aiming pose bends the spine forward under the camera as the look
  pitches down, which put the chest under the eyes and in front of the feet
  (Task 12j's Mac captures): the upper body (the spine's subtree) turns
  back rigidly about the spine node, about the facing's left, until the
  spine-to-neck line leans no more than BODY_UPRIGHT_LEAN_DEGREES forward;
- then, if a kept node is still within BODY_CAMERA_CLEARANCE of the camera,
  the upper body turns back further by the least whole degree that clears
  it, scaled down for a shortfall under BODY_CLAMP_RAMP so the turn grows
  smoothly from nothing (the shoulders rest about that far from the eyes,
  and a full turn for a millimeter popped);
- the whole copy set back along the unit's horizontal facing by
  display.first_person_body_offset (world units): the game's camera sits
  on the body's axis, where the pelvis hides the feet straight down; set
  back, the feet show past the front of the chest, as a person's do past
  their belly. The shoulders come nearer the eyes than the eye passes'
  near plane (0.0625 units), so the body's draws clamp their depth there
  instead of being cut open (halo_first_person_body_set_depth_clamp; at
  0.08 without it, looking straight down showed the shell's inside). In a
  seat the camera is the seat's camera marker on the vehicle, not a point
  on the body, so the set-back there is
  display.first_person_body_seat_offset instead, along the unit's world
  facing (object_get_orientation: the seat marker's).

The shadow takes a second copy, the full silhouette (head and arms
included) set back the same way, so the feet's shadow lies under the drawn
feet. The collapsed neck leaves the body's shell open, so render_objects.c
draws the body a second time with the cull reversed, which only the shell's
inside faces pass, in a flat dark color (halo_first_person_body_fill).

The body draws only while the unit is in a pose the game drives under the
camera: no custom animation, not dead, and either on foot with its pelvis
within BODY_PELVIS_MAXIMUM_DISTANCE of the camera horizontally, or in a
vehicle's first-person seat (display.first_person_body_seats; not a seat
whose camera is on the gun or behind the vehicle) with its pelvis within
BODY_SEAT_PELVIS_MAXIMUM_DISTANCE of the seat's camera, and not climbing
into or out of it. a10's cryo pod is a vehicle's seat, but its pelvis sits
ahead of the camera, past BODY_SEAT_PELVIS_MAXIMUM_FORWARD, so it takes none.
debug.gpu_stats logs the first skip of each stretch, and once a second the
camera and the drawn pelvis and feet in the camera's frame; and once a
second while the unit sits in a seat, whether or not a body draws, the
seat, its vehicle and the pose in the seat's frame (the seat log).

Scale 0 is enough: render_model multiplies each matrix by the node's
runtime_default_inverse_matrix, which keeps the position and the zero
scale, and the GPU's constants fold the scale into the 3x3.

The node matrices themselves are never changed, because markers,
attachments and collision read them; the copies are handed only to
render_model. Each lives in its own static array, never a stack array:
transparent geometry keeps the node matrices' pointer until the eye's
transparent pass (rasterizer_xbox_transparent_geometry.c), and the body (its
inside faces' pass recomputes the same copy) and its shadow draw once each
per eye pass, on one thread.

The nodes are found by name, case-insensitively on the name's last word
("bip01 spine", "bip01 neck", "bip01 l foot"), and the subtrees through
the parent links. The lookup is cached by the model's node_list_checksum (a
tag index is only valid for one map), and on the first lookup of each model
its node names are logged under debug.gpu_stats.
*/

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef FIRST_PERSON_BODY_PROBE
#include "cseries.h"
#include "math/real_math.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"
#include "models/model_definitions.h"
#include "camera/director.h"
#include "game/game.h"
#include "game/players.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "render/render.h"
#include "units/unit_definitions.h"
#include "units/units.h"
#include "../src/halo_stereo.h"

/* port/linux/src/port_config.c */
int config_boolean(const char *name);
double config_real(const char *name);
/* port/linux/src/sdl_platform.c (the host's on iOS) */
void platform_log(const char *format, ...);
#endif

/* models cached at once: in practice the player's one biped, and another
after a map with another (the Elite's or a marine's in a scripted swap) */
#define BODY_CACHE_SIZE 8
/* display.first_person_body_offset's default and range, world units */
#define BODY_OFFSET_DEFAULT 0.08f
#define BODY_OFFSET_MAXIMUM 0.2f
/* the farthest the pelvis may sit from the camera horizontally, world
units: walking it stays within about 0.05; in the cryo pod it's 0.175 */
#define BODY_PELVIS_MAXIMUM_DISTANCE 0.15f
/* display.first_person_body_seat_offset's default, world units */
#define BODY_SEAT_OFFSET_DEFAULT 0.0f
/* in a seat, the farthest the pelvis may sit from the seat's camera
horizontally, world units: the Warthog passenger's sits 0.215 from it,
seated (Task 7f-2's Mac log), plus 0.05, rounded up to 0.05. The frame the
director hands the camera into the seat, with the pelvis 0.35 to 0.41 to
the side, is skipped */
#define BODY_SEAT_PELVIS_MAXIMUM_DISTANCE 0.3f
/* in a seat, the farthest the pelvis may sit ahead of the seat's camera
along the facing, world units: a passenger's is behind the camera marker,
while in a10's cryo pod (a vehicle, levels\a10\devices\cryotube) it's
0.175 ahead, with the camera in front of the Chief's chest */
#define BODY_SEAT_PELVIS_MAXIMUM_FORWARD 0.05f
/* the nearest a kept node may come to the camera, world units: the spine's
bend clamp turns the upper body back until every kept node is this far */
#define BODY_CAMERA_CLEARANCE 0.1f
/* the bend clamp's steps and its farthest turn back, degrees */
#define BODY_CLAMP_STEP_DEGREES 1
#define BODY_CLAMP_MAXIMUM_DEGREES 90
/* the shortfall under the clearance, world units, at which the clamp's
turn reaches the full clearing angle; less takes that share of it */
#define BODY_CLAMP_RAMP 0.01f
/* how far above the neck node the collapsed neck and head sit, along
spine1-to-neck, world units: Task 7f's raise at the waist, applied at the
neck */
#define BODY_NECK_RAISE 0.03f
/* the most the spine-to-neck line may lean forward from the world's up,
degrees: the cyborg stands leaning about 19 and the aiming pose bends it to
about 31 looking straight down (Task 12j's Mac log); upright, its chest
stays behind the feet and its shoulders clear of the near plane (at 10,
the collar still crossed it) */
#define BODY_UPRIGHT_LEAN_DEGREES 0.0f

enum body_skip
{
	BODY_DRAWS,
	BODY_SKIP_SEATED,
	BODY_SKIP_CUSTOM_ANIMATION,
	BODY_SKIP_DEAD,
	BODY_SKIP_PELVIS_FAR,
	BODY_SKIP_MODEL,
	BODY_SKIP_SEAT_TRANSITION,
	NUMBER_OF_BODY_SKIPS
};

static const char *const body_skip_names[NUMBER_OF_BODY_SKIPS] = {
	"draws",
	"the unit is in a seat that takes no body",
	"the unit plays a custom animation",
	"the unit is dead",
	"the unit's pelvis is too far from under the camera",
	"the unit's model nodes aren't recognized",
	"the unit is climbing into or out of a seat",
};

struct body_model
{
	unsigned long checksum;
	long node_count;              /* 0: an empty slot */
	int recognized;               /* a spine, or failing that a head */
	short spine;                  /* NONE if not found */
	short pelvis;                 /* the spine's parent, or the root */
	short neck;                   /* NONE if not found */
	short neck_base;              /* spine1, or the neck's parent; NONE if neither */
	short feet[2];                /* NONE if not found */
	/* for the seat log (NONE if not found): the knees (calf), the chest
	(spine1) and the head */
	short calves[2];
	short spine1;
	short head;
	/* per node: the node whose position it collapses to (the neck's
	subtree, or with no neck the head's), or NONE */
	signed char target[MAXIMUM_NODES_PER_MODEL];
	/* per node: its upper arm, the node it collapses to with
	collapse_arms, or NONE */
	signed char arm_target[MAXIMUM_NODES_PER_MODEL];
	/* per node: in the spine's subtree (the spine itself included), which
	the bend clamp turns */
	unsigned char upper_body[MAXIMUM_NODES_PER_MODEL];
};

static struct body_model body_cache[BODY_CACHE_SIZE];
static int body_cache_next;
static int body_setting = -1; /* display.first_person_body, read once */
static int body_seats_setting = -1; /* display.first_person_body_seats, read once */
/* display.first_person_body_offset and display.first_person_body_seat_offset,
each read once */
static float body_offset = -1.0f, body_seat_offset = -1.0f;
static real_matrix4x3 body_matrices[MAXIMUM_NODES_PER_MODEL];
static real_matrix4x3 body_shadow_matrices[MAXIMUM_NODES_PER_MODEL];
/* the last body copy's nearest kept upper-body node to the camera, before
and after the bend clamp, and the clamp's turn in degrees, for the log */
static float body_nearest_before = -1.0f, body_nearest_after = -1.0f;
static float body_clamp_degrees;
/* the spine-to-neck line's forward lean before the upright turn, degrees */
static float body_lean_degrees;
/* render_objects.c's inside-faces pass (halo_first_person_body_set_fill) */
static int body_fill;
/* render_objects.c's body passes (halo_first_person_body_set_depth_clamp) */
static int body_depth_clamp;

/* the name's last word ("bip01 l upperarm": "upperarm") is word (lower
case), ignoring the name's case */
static int node_name_is(const char *name, const char *word)
{
	const char *last = strrchr(name, ' ');

	for (name = last ? last + 1 : name; *name && *word; name++, word++) {
		char c = *name >= 'A' && *name <= 'Z' ? (char)(*name - 'A' + 'a') : *name;

		if (c != *word)
			return 0;
	}
	return !*name && !*word;
}

static void log_node_names(const struct model *model, long model_index)
{
	char line[256];
	size_t length = 0;
	short node_index;

	platform_log("first_person_body: model %ld (node list checksum 0x%08lx), %d nodes:", model_index,
		(unsigned long)model->node_list_checksum, (int)model->nodes.count);
	line[0] = '\0';
	for (node_index = 0; node_index < model->nodes.count; node_index++) {
		const struct model_node *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, node_index, struct model_node);
		char entry[64];
		size_t entry_length;

		snprintf(entry, sizeof(entry), "%s%d %s (parent %d)", length ? ", " : "", (int)node_index, node->name,
			(int)node->parent_node_index);
		entry_length = strlen(entry);
		if (length + entry_length >= sizeof(line)) {
			platform_log("first_person_body:   %s", line);
			snprintf(entry, sizeof(entry), "%d %s (parent %d)", (int)node_index, node->name,
				(int)node->parent_node_index);
			entry_length = strlen(entry);
			length = 0;
		}
		memcpy(line + length, entry, entry_length + 1);
		length += entry_length;
	}
	if (length)
		platform_log("first_person_body:   %s", line);
}

/* the nearest ancestor of node_index named word, the node itself included,
or NONE */
static short body_ancestor_named(const struct model *model, short node_index, const char *word)
{
	short count = (short)model->nodes.count;
	short ancestor = node_index;
	short steps;

	for (steps = 0; steps < count && ancestor >= 0 && ancestor < count; steps++) {
		const struct model_node *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, ancestor, struct model_node);

		if (node_name_is(node->name, word))
			return ancestor;
		ancestor = node->parent_node_index;
	}
	return NONE;
}

/* whether node_index is in the subtree of neck (the neck included) with
no node named clavicle between them: the neck and head, not the arms the
cyborg hangs from its neck */
static int body_in_neck(const struct model *model, short node_index, short neck)
{
	short count = (short)model->nodes.count;
	short ancestor = node_index;
	short steps;

	for (steps = 0; steps < count && ancestor >= 0 && ancestor < count; steps++) {
		const struct model_node *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, ancestor, struct model_node);

		if (ancestor == neck)
			return 1;
		if (node_name_is(node->name, "clavicle"))
			return 0;
		ancestor = node->parent_node_index;
	}
	return 0;
}

/* the model's collapse targets, pelvis and feet, from its node names and
parent links */
static const struct body_model *body_model_get(long model_index)
{
	const struct model *model = model_definition_get(model_index);
	struct body_model *body;
	short count = (short)model->nodes.count;
	short node_index;
	short spine = NONE, spine1 = NONE, neck = NONE, head = NONE;
	int feet = 0, calves = 0;
	int slot;

	for (slot = 0; slot < BODY_CACHE_SIZE; slot++) {
		body = &body_cache[slot];
		if (body->node_count == count && count > 0 && body->checksum == model->node_list_checksum)
			return body;
	}
	body = &body_cache[body_cache_next];
	body_cache_next = (body_cache_next + 1) % BODY_CACHE_SIZE;
	memset(body, 0, sizeof(*body));
	body->checksum = model->node_list_checksum;
	body->node_count = count;
	body->feet[0] = body->feet[1] = NONE;
	body->calves[0] = body->calves[1] = NONE;
	body->spine = body->spine1 = body->head = NONE;
	if (count <= 0 || count > MAXIMUM_NODES_PER_MODEL) {
		platform_log("first_person_body: model %ld has %d nodes (at most %d); no body",
			model_index, (int)count, MAXIMUM_NODES_PER_MODEL);
		return body;
	}

	if (config_boolean("debug.gpu_stats"))
		log_node_names(model, model_index);
	for (node_index = 0; node_index < count; node_index++) {
		const struct model_node *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, node_index, struct model_node);

		if (spine == NONE && node_name_is(node->name, "spine"))
			spine = node_index;
		if (spine1 == NONE && node_name_is(node->name, "spine1"))
			spine1 = node_index;
		if (neck == NONE && node_name_is(node->name, "neck"))
			neck = node_index;
		if (head == NONE && node_name_is(node->name, "head"))
			head = node_index;
		if (feet < 2 && node_name_is(node->name, "foot"))
			body->feet[feet++] = node_index;
		if (calves < 2 && node_name_is(node->name, "calf"))
			body->calves[calves++] = node_index;
	}
	for (node_index = 0; node_index < count; node_index++) {
		short ancestor;

		body->target[node_index] = NONE;
		body->arm_target[node_index] = (signed char)body_ancestor_named(model, node_index, "upperarm");
		body->upper_body[node_index] = spine != NONE && body_ancestor_named(model, node_index, "spine") == spine;
		if (neck != NONE) {
			/* the neck and head, to the cap above the neck node (the
			collapse places it) */
			if (body_in_neck(model, node_index, neck))
				body->target[node_index] = (signed char)neck;
		} else if ((ancestor = body_ancestor_named(model, node_index, "head")) != NONE) {
			/* no neck: the head, to its parent's position (where it is, if it
			has none) */
			const struct model_node *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, ancestor, struct model_node);
			short parent = node->parent_node_index;

			body->target[node_index] = (signed char)(parent >= 0 && parent < count ? parent : ancestor);
		}
	}
	body->spine = spine;
	body->spine1 = spine1;
	body->head = head;
	body->neck = neck;
	body->neck_base = NONE;
	if (neck != NONE) {
		const struct model_node *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, neck, struct model_node);
		short parent = node->parent_node_index;

		body->neck_base = spine1 != NONE ? spine1 : parent >= 0 && parent < count ? parent : NONE;
	}
	if (spine != NONE) {
		const struct model_node *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, spine, struct model_node);

		body->pelvis = node->parent_node_index >= 0 && node->parent_node_index < count ? node->parent_node_index : 0;
	}
	body->recognized = spine != NONE || head != NONE;
	if (!body->recognized)
		platform_log("first_person_body: model %ld (node list checksum 0x%08lx) has no spine or head node by "
			"name; no body", model_index, (unsigned long)model->node_list_checksum);
	return body;
}

/* an offset setting's value in 0 to BODY_OFFSET_MAXIMUM; NaN gives the
fallback */
static float body_offset_clamp(double value, float fallback)
{
	if (value != value)
		return fallback;
	if (value < 0.0)
		return 0.0f;
	if (value > BODY_OFFSET_MAXIMUM)
		return BODY_OFFSET_MAXIMUM;
	return (float)value;
}

/* the set-back: display.first_person_body_offset on foot, or seated
display.first_person_body_seat_offset, clamped, each read once */
static float body_offset_get(int seated)
{
	if (seated) {
		if (body_seat_offset < 0.0f)
			body_seat_offset = body_offset_clamp(config_real("display.first_person_body_seat_offset"),
				BODY_SEAT_OFFSET_DEFAULT);
		return body_seat_offset;
	}
	if (body_offset < 0.0f)
		body_offset = body_offset_clamp(config_real("display.first_person_body_offset"), BODY_OFFSET_DEFAULT);
	return body_offset;
}

/* position turned by angle (radians) about axis (unit) through pivot */
static void body_turn_point(real_point3d *position, const real_point3d *pivot, const real_vector3d *axis, float angle)
{
	real_vector3d v = { position->x - pivot->x, position->y - pivot->y, position->z - pivot->z };
	real_vector3d turned;
	float c = cosf(angle), s = sinf(angle);
	float dot = axis->i * v.i + axis->j * v.j + axis->k * v.k;

	/* Rodrigues: v cos + (axis x v) sin + axis (axis . v)(1 - cos) */
	turned.i = v.i * c + (axis->j * v.k - axis->k * v.j) * s + axis->i * dot * (1.0f - c);
	turned.j = v.j * c + (axis->k * v.i - axis->i * v.k) * s + axis->j * dot * (1.0f - c);
	turned.k = v.k * c + (axis->i * v.j - axis->j * v.i) * s + axis->k * dot * (1.0f - c);
	position->x = pivot->x + turned.i;
	position->y = pivot->y + turned.j;
	position->z = pivot->z + turned.k;
}

static void body_turn_vector(real_vector3d *vector, const real_vector3d *axis, float angle)
{
	static const real_point3d origin = { 0.0f, 0.0f, 0.0f };
	real_point3d point = { vector->i, vector->j, vector->k };

	body_turn_point(&point, &origin, axis, angle);
	vector->i = point.x;
	vector->j = point.y;
	vector->k = point.z;
}

/* whether the copy's node is kept (drawn at scale 1) */
static int body_kept(const struct body_model *body, short node_index, int collapse_arms)
{
	return body->target[node_index] == NONE && (!collapse_arms || body->arm_target[node_index] == NONE);
}

/* the nearest of the kept upper-body nodes to the camera, with the upper
body turned by angle about axis through pivot */
static float body_nearest(const struct body_model *body, const real_matrix4x3 *copy, short node_count,
	int collapse_arms, const float camera[3], const real_point3d *pivot, const real_vector3d *axis, float angle)
{
	float nearest = 1e9f;
	short node_index;

	for (node_index = 0; node_index < node_count; node_index++) {
		real_point3d position = copy[node_index].position;
		float dx, dy, dz, distance;

		if (!body->upper_body[node_index] || !body_kept(body, node_index, collapse_arms))
			continue;
		if (angle != 0.0f)
			body_turn_point(&position, pivot, axis, angle);
		dx = position.x - camera[0];
		dy = position.y - camera[1];
		dz = position.z - camera[2];
		distance = sqrtf(dx * dx + dy * dy + dz * dz);
		if (distance < nearest)
			nearest = distance;
	}
	return nearest;
}

/* the upper body (the spine's subtree) turned rigidly by angle about axis
through pivot */
static void body_turn_upper(const struct body_model *body, real_matrix4x3 *copy, short node_count,
	const real_point3d *pivot, const real_vector3d *axis, float angle)
{
	short node_index;

	for (node_index = 0; node_index < node_count; node_index++) {
		if (!body->upper_body[node_index])
			continue;
		body_turn_point(&copy[node_index].position, pivot, axis, angle);
		body_turn_vector(&copy[node_index].forward, axis, angle);
		body_turn_vector(&copy[node_index].left, axis, angle);
		body_turn_vector(&copy[node_index].up, axis, angle);
	}
}

/* the share of the clearing turn the clamp applies for a shortfall under
BODY_CAMERA_CLEARANCE: growing from 0 to 1 over BODY_CLAMP_RAMP */
static float body_clamp_ramp(float shortfall)
{
	return shortfall <= 0.0f ? 0.0f : shortfall >= BODY_CLAMP_RAMP ? 1.0f : shortfall / BODY_CLAMP_RAMP;
}

/* the upright turn and the bend clamp on the set-back copy, before the
collapse: the upper body turned back about the spine node, about the
facing's left, first until the spine-to-neck line leans no more than
BODY_UPRIGHT_LEAN_DEGREES forward, then by the least whole degree that
keeps every kept node BODY_CAMERA_CLEARANCE from the camera (or the turn up
to BODY_CLAMP_MAXIMUM_DEGREES that keeps them farthest), times
body_clamp_ramp of the shortfall */
static void body_clamp_bend(const struct body_model *body, real_matrix4x3 *copy, short node_count, int collapse_arms,
	const float facing[3], const float camera[3])
{
	float length = sqrtf(facing[0] * facing[0] + facing[1] * facing[1]);
	real_point3d pivot;
	real_vector3d left;
	float best = 0.0f, best_nearest, applied;
	int degrees, best_degrees = 0;

	body_nearest_before = body_nearest_after = -1.0f;
	body_clamp_degrees = 0.0f;
	body_lean_degrees = 0.0f;
	if (body->spine == NONE || body->spine >= node_count || length <= 1e-4f)
		return;
	pivot = copy[body->spine].position;
	/* the facing's left (up x forward); a negative turn about it lifts the
	forward toward the up: the chest turns back */
	left.i = -facing[1] / length;
	left.j = facing[0] / length;
	left.k = 0.0f;
	if (body->neck != NONE && body->neck < node_count) {
		float dx = copy[body->neck].position.x - pivot.x;
		float dy = copy[body->neck].position.y - pivot.y;
		float dz = copy[body->neck].position.z - pivot.z;
		float ahead = (dx * facing[0] + dy * facing[1]) / length;

		body_lean_degrees = atan2f(ahead, dz) * 57.29578f;
		if (body_lean_degrees > BODY_UPRIGHT_LEAN_DEGREES)
			body_turn_upper(body, copy, node_count, &pivot, &left,
				-(body_lean_degrees - BODY_UPRIGHT_LEAN_DEGREES) * 0.017453293f);
	}
	best_nearest = body_nearest_before = body_nearest(body, copy, node_count, collapse_arms, camera, &pivot, &left, 0.0f);
	for (degrees = BODY_CLAMP_STEP_DEGREES; best_nearest < BODY_CAMERA_CLEARANCE &&
		degrees <= BODY_CLAMP_MAXIMUM_DEGREES; degrees += BODY_CLAMP_STEP_DEGREES) {
		float angle = -(float)degrees * 0.017453293f;
		float nearest = body_nearest(body, copy, node_count, collapse_arms, camera, &pivot, &left, angle);

		if (nearest > best_nearest) {
			best_nearest = nearest;
			best = angle;
			best_degrees = degrees;
		}
		if (nearest >= BODY_CAMERA_CLEARANCE)
			break;
	}
	applied = best * body_clamp_ramp(BODY_CAMERA_CLEARANCE - body_nearest_before);
	body_clamp_degrees = (float)best_degrees * body_clamp_ramp(BODY_CAMERA_CLEARANCE - body_nearest_before);
	if (applied != 0.0f)
		body_turn_upper(body, copy, node_count, &pivot, &left, applied);
	body_nearest_after = applied == best ? best_nearest :
		body_nearest(body, copy, node_count, collapse_arms, camera, &pivot, &left, 0.0f);
}

const real_matrix4x3 *halo_first_person_body_matrices(long model_index, const real_matrix4x3 *matrices,
	short node_count, const float facing[3], const float camera[3], int collapse_arms, int seated, int shadow)
{
	const struct body_model *body;
	real_matrix4x3 *copy = shadow ? body_shadow_matrices : body_matrices;
	float length = sqrtf(facing[0] * facing[0] + facing[1] * facing[1]);
	float back[2] = { 0.0f, 0.0f };
	real_point3d cap = { 0.0f, 0.0f, 0.0f };
	short node_index;

	if (model_index == NONE || !matrices || node_count <= 0)
		return matrices;
	body = body_model_get(model_index);
	if (!body->recognized)
		return matrices;
	if (node_count > body->node_count)
		node_count = (short)body->node_count;
	/* the set-back: the offset backward along the facing made horizontal
	(none if the facing is vertical) */
	if (length > 1e-4f) {
		back[0] = -body_offset_get(seated) * facing[0] / length;
		back[1] = -body_offset_get(seated) * facing[1] / length;
	}

	memcpy(copy, matrices, (size_t)node_count * sizeof(*matrices));
	for (node_index = 0; node_index < node_count; node_index++) {
		copy[node_index].position.x += back[0];
		copy[node_index].position.y += back[1];
	}
	if (shadow)
		return copy;
	body_clamp_bend(body, copy, node_count, collapse_arms, facing, camera);
	/* the neck's cap, from the (turned) neck and its base */
	if (body->neck != NONE && body->neck < node_count) {
		cap = copy[body->neck].position;
		if (body->neck_base != NONE && body->neck_base < node_count) {
			float dx = cap.x - copy[body->neck_base].position.x;
			float dy = cap.y - copy[body->neck_base].position.y;
			float dz = cap.z - copy[body->neck_base].position.z;
			float distance = sqrtf(dx * dx + dy * dy + dz * dz);

			if (distance > 1e-5f) {
				cap.x += BODY_NECK_RAISE * dx / distance;
				cap.y += BODY_NECK_RAISE * dy / distance;
				cap.z += BODY_NECK_RAISE * dz / distance;
			}
		}
	}
	/* the collapse, to the cap or the (turned) targets' positions: any
	other target is kept (a head's parent) or collapses to itself (an upper
	arm), so the order doesn't matter */
	for (node_index = 0; node_index < node_count; node_index++) {
		short target = collapse_arms && body->arm_target[node_index] != NONE ?
			body->arm_target[node_index] : body->target[node_index];

		if (target != NONE && target < node_count) {
			copy[node_index].scale = 0.0f;
			copy[node_index].position = target == body->neck ? cap : copy[target].position;
		}
	}
	return copy;
}

/* the pelvis's horizontal offset from the camera (offset[0], offset[1]);
0 if the model's nodes aren't recognized */
static int body_pelvis_offset(long model_index, const real_matrix4x3 *matrices, short node_count,
	const float camera[3], float offset[2])
{
	const struct body_model *body;

	if (model_index == NONE || !matrices || node_count <= 0)
		return 0;
	body = body_model_get(model_index);
	if (!body->recognized || body->pelvis >= node_count)
		return 0;
	offset[0] = matrices[body->pelvis].position.x - camera[0];
	offset[1] = matrices[body->pelvis].position.y - camera[1];
	return 1;
}

/* why the body doesn't draw for its pose, from its node matrices and the
camera's position: its model, or its pelvis's horizontal distance from the
camera */
static enum body_skip body_pose_skip(long model_index, const real_matrix4x3 *matrices, short node_count,
	const float camera[3])
{
	float offset[2];

	if (!body_pelvis_offset(model_index, matrices, node_count, camera, offset))
		return BODY_SKIP_MODEL;
	if (offset[0] * offset[0] + offset[1] * offset[1] > BODY_PELVIS_MAXIMUM_DISTANCE * BODY_PELVIS_MAXIMUM_DISTANCE)
		return BODY_SKIP_PELVIS_FAR;
	return BODY_DRAWS;
}

/* why a seated unit's pose takes no body, or BODY_DRAWS: not a vehicle's
seat (the pod), the seats setting off, a seat whose camera is on the gun
or behind the vehicle, the enter or exit animation, or the pelvis away
from the seat's camera */
static enum body_skip body_seat_pose_skip(int seats_setting, int vehicle_parent, int camera_elsewhere,
	int in_transition, long model_index, const real_matrix4x3 *matrices, short node_count,
	const float camera[3], const float facing[3])
{
	float offset[2];
	float length = sqrtf(facing[0] * facing[0] + facing[1] * facing[1]);

	if (!seats_setting || !vehicle_parent || camera_elsewhere)
		return BODY_SKIP_SEATED;
	if (in_transition)
		return BODY_SKIP_SEAT_TRANSITION;
	if (!body_pelvis_offset(model_index, matrices, node_count, camera, offset))
		return BODY_SKIP_MODEL;
	if (offset[0] * offset[0] + offset[1] * offset[1] >
		BODY_SEAT_PELVIS_MAXIMUM_DISTANCE * BODY_SEAT_PELVIS_MAXIMUM_DISTANCE)
		return BODY_SKIP_PELVIS_FAR;
	/* ahead of the camera along the facing made horizontal: the pod */
	if (length > 1e-4f && (offset[0] * facing[0] + offset[1] * facing[1]) / length > BODY_SEAT_PELVIS_MAXIMUM_FORWARD)
		return BODY_SKIP_PELVIS_FAR;
	return BODY_DRAWS;
}

#ifndef FIRST_PERSON_BODY_PROBE
/* the local player's unit, in the director's first person (not a scripted
camera's first-person object, which render_objects.c also hides) */
static int first_person_body_local_unit(long object_index)
{
	long player_index;

	if (render.local_player_index == NONE)
		return 0;
	player_index = local_player_get_player_index(render.local_player_index);
	if (player_index == NONE)
		return 0;
	return player_get(player_index)->unit_index == object_index &&
		director_get_perspective(render.local_player_index) == _director_perspective_first_person;
}

static long body_model_index(long object_index)
{
	return object_definition_get(object_get(object_index)->definition_index)->object.model.index;
}

static short body_node_count(long object_index, long model_index)
{
	struct object_datum *object = object_get(object_index);
	short count = (short)(object->object.node_matrices.size / (short)sizeof(real_matrix4x3));
	short model_count = (short)model_definition_get(model_index)->nodes.count;

	return count < model_count ? count : model_count;
}

/* " name (f r d)": node's position in the seat's frame (forward along
facing, right, down; facing horizontal and unit length) from camera, or
nothing for a node not found */
static size_t body_seat_log_node(char *line, size_t length, size_t size, const char *name,
	const real_matrix4x3 *matrices, short node_count, short node, const float camera[3], const float facing[2])
{
	float dx, dy, dz;

	if (length >= size || node == NONE || node >= node_count)
		return length;
	dx = matrices[node].position.x - camera[0];
	dy = matrices[node].position.y - camera[1];
	dz = matrices[node].position.z - camera[2];
	return length + (size_t)snprintf(line + length, size - length, " %s (%.3f %.3f %.3f)", name,
		dx * facing[0] + dy * facing[1], dx * facing[1] - dy * facing[0], -dz);
}

/* debug.gpu_stats, once a second of game time while the unit has a parent,
whether or not a body draws: the seat (with its yaw bounds off its marker, in
degrees: 0 to 0 has none), its parent, the animation state, the
camera, and the pelvis, spine, knees, feet, spine1 and head in the seat's
frame (forward along the unit's world facing made horizontal, right, down),
with the facing's angle from the root parent's forward (positive to its
left). The first time a vehicle is logged, its whole seats block, each
seat's label and flags. Its own tick, apart from the on-foot log's */
static void first_person_body_seat_log(long object_index, const float camera[3])
{
	static long last_tick = -1;
	static long logged_definitions[BODY_CACHE_SIZE];
	static int logged_count;
	static int stats = -1;
	struct object_datum *object = object_get(object_index);
	struct object_datum *parent = object_get(object->object.parent_object_index);
	struct unit_datum *unit = unit_get(object_index);
	const struct render_camera *view = &render.camera;
	long tick = game_time_get();
	long model_index = body_model_index(object_index);
	long root_index = object->object.parent_object_index;
	const struct unit_seat *seat;
	const real_vector3d *root_forward;
	real_vector3d forward;
	float facing[2], length, root_length, angle = 0.0f;
	char line[1024];
	size_t size = sizeof(line), written;
	int i;

	if (stats < 0)
		stats = config_boolean("debug.gpu_stats") != 0;
	if (!stats || (last_tick >= 0 && tick >= last_tick && tick - last_tick < 30))
		return;
	last_tick = tick;
	if (!TEST_FLAG(_object_mask_unit, parent->object.type) || unit->unit.parent_seat_index == NONE) {
		platform_log("first_person_body: seated on %s (type %d), not in a unit's seat",
			tag_get_name(parent->definition_index), (int)parent->object.type);
		return;
	}
	for (i = 0; i < logged_count && logged_definitions[i] != parent->definition_index; i++)
		;
	if (i == logged_count && logged_count < BODY_CACHE_SIZE) {
		const struct tag_block *seats = &unit_definition_get(parent->definition_index)->unit.seats;
		short seat_index;

		logged_definitions[logged_count++] = parent->definition_index;
		written = (size_t)snprintf(line, size, "first_person_body: seats of %s (type %d):",
			tag_get_name(parent->definition_index), (int)parent->object.type);
		for (seat_index = 0; seat_index < seats->count && written < size; seat_index++) {
			const struct unit_seat *each = TAG_BLOCK_GET_ELEMENT(seats, seat_index, struct unit_seat);

			written += (size_t)snprintf(line + written, size - written, "%s %d \"%s\" 0x%lx",
				seat_index ? "," : "", (int)seat_index, each->label, (unsigned long)each->flags);
		}
		platform_log("%s", line);
	}
	seat = TAG_BLOCK_GET_ELEMENT(&unit_definition_get(parent->definition_index)->unit.seats,
		unit->unit.parent_seat_index, struct unit_seat);
	while (object_get(root_index)->object.parent_object_index != NONE)
		root_index = object_get(root_index)->object.parent_object_index;
	root_forward = &object_get(root_index)->object.forward;
	object_get_orientation(object_index, &forward, NULL);
	length = sqrtf(forward.i * forward.i + forward.j * forward.j);
	facing[0] = length > 1e-4f ? forward.i / length : 1.0f;
	facing[1] = length > 1e-4f ? forward.j / length : 0.0f;
	root_length = sqrtf(root_forward->i * root_forward->i + root_forward->j * root_forward->j);
	if (root_length > 1e-4f)
		angle = atan2f(root_forward->i * facing[1] - root_forward->j * facing[0],
			root_forward->i * facing[0] + root_forward->j * facing[1]) * 57.29578f;
	written = (size_t)snprintf(line, size, "first_person_body: seat \"%s\" flags 0x%lx yaw %.1f to %.1f on %s "
		"(type %d), state %d: camera (%.3f %.3f %.3f) pitch %.1f; in the seat's frame (forward along the facing made "
		"horizontal, right, down):", seat->label, (unsigned long)seat->flags, seat->yaw_minimum * 57.29578f,
		seat->yaw_maximum * 57.29578f, tag_get_name(parent->definition_index),
		(int)parent->object.type, (int)unit->unit.animation.state, camera[0], camera[1], camera[2],
		asinf(view->forward.k > 1.0f ? 1.0f : view->forward.k < -1.0f ? -1.0f : view->forward.k) * 57.29578f);
	if (model_index != NONE) {
		const struct body_model *body = body_model_get(model_index);
		const real_matrix4x3 *matrices = object_get_node_matrices(object_index);
		short node_count = body_node_count(object_index, model_index);

		if (body->recognized) {
			written = body_seat_log_node(line, written, size, "pelvis", matrices, node_count, body->pelvis, camera,
				facing);
			written = body_seat_log_node(line, written, size, "spine", matrices, node_count, body->spine, camera,
				facing);
			for (i = 0; i < 2; i++)
				written = body_seat_log_node(line, written, size, "knee", matrices, node_count, body->calves[i],
					camera, facing);
			for (i = 0; i < 2; i++)
				written = body_seat_log_node(line, written, size, "foot", matrices, node_count, body->feet[i],
					camera, facing);
			written = body_seat_log_node(line, written, size, "spine1", matrices, node_count, body->spine1, camera,
				facing);
			written = body_seat_log_node(line, written, size, "head", matrices, node_count, body->head, camera,
				facing);
		}
	}
	if (written < size)
		snprintf(line + written, size - written, "; facing %.1f degrees from the vehicle's forward", angle);
	platform_log("%s", line);
}

/* why the unit's pose doesn't take a body: a custom animation, death, a
seat that takes none (body_seat_pose_skip: a10's pod among them), or its
pelvis away from under the camera */
static enum body_skip first_person_body_pose(long object_index)
{
	struct object_datum *object = object_get(object_index);
	long model_index = body_model_index(object_index);
	float camera[3] = { render.camera.position.x, render.camera.position.y, render.camera.position.z };

	if (object->object.parent_object_index != NONE)
		first_person_body_seat_log(object_index, camera);
	if (unit_is_playing_custom_animation(object_index))
		return BODY_SKIP_CUSTOM_ANIMATION;
	if (TEST_FLAG(object->object.damage_flags, _object_dead_bit))
		return BODY_SKIP_DEAD;
	if (model_index == NONE)
		return BODY_SKIP_MODEL;
	if (object->object.parent_object_index != NONE) {
		struct object_datum *parent = object_get(object->object.parent_object_index);
		struct unit_datum *unit = unit_get(object_index);
		const struct unit_seat *seat;
		real_vector3d forward;
		float facing[3];

		if (body_seats_setting < 0)
			body_seats_setting = config_boolean("display.first_person_body_seats") != 0;
		if (!TEST_FLAG(_object_mask_unit, parent->object.type) || unit->unit.parent_seat_index == NONE)
			return BODY_SKIP_SEATED;
		seat = TAG_BLOCK_GET_ELEMENT(&unit_definition_get(parent->definition_index)->unit.seats,
			unit->unit.parent_seat_index, struct unit_seat);
		object_get_orientation(object_index, &forward, NULL);
		facing[0] = forward.i;
		facing[1] = forward.j;
		facing[2] = forward.k;
		return body_seat_pose_skip(body_seats_setting, parent->object.type == _object_type_vehicle,
			TEST_FLAG(seat->flags, _unit_seat_third_person_camera_bit) ||
				TEST_FLAG(seat->flags, _unit_seat_first_person_camera_bit),
			unit->unit.animation.state == _unit_state_entering_seat ||
				unit->unit.animation.state == _unit_state_exiting_seat,
			model_index, object_get_node_matrices(object_index), body_node_count(object_index, model_index),
			camera, facing);
	}
	return body_pose_skip(model_index, object_get_node_matrices(object_index),
		body_node_count(object_index, model_index), camera);
}

/* debug.gpu_stats, once a second of game time: the camera, and the drawn
pelvis and feet in the camera's frame (forward, right, down), and the
pelvis's forward against the facing */
void halo_first_person_body_log(long object_index, const struct real_matrix4x3 *drawn, short node_count)
{
	static long last_tick = -1;
	static int stats = -1;
	struct object_datum *object = object_get(object_index);
	const struct render_camera *camera = &render.camera;
	const struct body_model *body;
	real_vector3d right;
	char line[384];
	size_t length;
	long tick = game_time_get();
	long model_index;
	int i;

	if (stats < 0)
		stats = config_boolean("debug.gpu_stats") != 0;
	if (!stats || (last_tick >= 0 && tick >= last_tick && tick - last_tick < 30))
		return;
	model_index = body_model_index(object_index);
	if (model_index == NONE)
		return;
	last_tick = tick;
	body = body_model_get(model_index);
	/* right = forward x up */
	right.i = camera->forward.j * camera->up.k - camera->forward.k * camera->up.j;
	right.j = camera->forward.k * camera->up.i - camera->forward.i * camera->up.k;
	right.k = camera->forward.i * camera->up.j - camera->forward.j * camera->up.i;
	length = (size_t)snprintf(line, sizeof(line), "first_person_body: camera (%.3f %.3f %.3f) pitch %.1f,"
		" offset %.3f; in the camera's frame (forward, right, down):", camera->position.x, camera->position.y,
		camera->position.z, asinf(camera->forward.k > 1.0f ? 1.0f : camera->forward.k < -1.0f ? -1.0f :
		camera->forward.k) * 57.29578f, body_offset_get(object->object.parent_object_index != NONE));
	for (i = 0; i < 3 && length < sizeof(line); i++) {
		short node = i == 0 ? body->pelvis : body->feet[i - 1];
		float dx, dy, dz;

		if (node == NONE || node >= node_count)
			continue;
		dx = drawn[node].position.x - camera->position.x;
		dy = drawn[node].position.y - camera->position.y;
		dz = drawn[node].position.z - camera->position.z;
		length += (size_t)snprintf(line + length, sizeof(line) - length, " %s (%.3f %.3f %.3f)",
			i == 0 ? "pelvis" : "foot",
			dx * camera->forward.i + dy * camera->forward.j + dz * camera->forward.k,
			dx * right.i + dy * right.j + dz * right.k,
			-(dx * camera->up.i + dy * camera->up.j + dz * camera->up.k));
	}
	if (body_nearest_before >= 0.0f && length < sizeof(line))
		length += (size_t)snprintf(line + length, sizeof(line) - length,
			"; the spine leaned %.1f degrees forward; the chest's nearest kept node %.3f from the camera "
			"(%.3f before a bend clamp of %.1f degrees, ramped)",
			body_lean_degrees, body_nearest_after, body_nearest_before, body_clamp_degrees);
	if (body->pelvis < node_count && length < sizeof(line)) {
		const real_vector3d *pelvis_forward = &drawn[body->pelvis].forward;
		float facing_length = sqrtf(object->object.forward.i * object->object.forward.i +
			object->object.forward.j * object->object.forward.j);
		float pelvis_length = sqrtf(pelvis_forward->i * pelvis_forward->i + pelvis_forward->j * pelvis_forward->j);

		if (facing_length > 1e-4f && pelvis_length > 1e-4f) {
			float cosine = (object->object.forward.i * pelvis_forward->i +
				object->object.forward.j * pelvis_forward->j) / (facing_length * pelvis_length);

			snprintf(line + length, sizeof(line) - length, "; pelvis forward %.1f degrees from the facing",
				acosf(cosine > 1.0f ? 1.0f : cosine < -1.0f ? -1.0f : cosine) * 57.29578f);
		}
	}
	platform_log("%s", line);
}
#else
/* the probe's (port/ios/tests/first_person_body_probe.c) */
static int first_person_body_local_unit(long object_index);
static enum body_skip first_person_body_pose(long object_index);
#endif

int halo_first_person_body(long object_index)
{
	static enum body_skip last_skip = BODY_DRAWS;
	const struct halo_stereo_frame *frame = halo_stereo_frame();
	enum body_skip skip;
	int layer;

	if (object_index == NONE || frame->eye_count != 2 ||
		(frame->mode != HALO_STEREO_HEAD && frame->mode != HALO_STEREO_SIDE_BY_SIDE))
		return 0;
	layer = halo_stereo_current_layer();
	if (layer != 0 && layer != 1)
		return 0;
	/* the film (HEAD mode's is SCREEN already; the side-by-side view's
	isn't) and the side-by-side view's SCREEN gameplay draw as before */
	if (halo_stereo_film() || halo_stereo_screen_gameplay())
		return 0;
	if (!first_person_body_local_unit(object_index))
		return 0;
	if (body_setting < 0)
		body_setting = config_boolean("display.first_person_body") != 0;
	if (!body_setting)
		return 0;
	skip = first_person_body_pose(object_index);
	if (skip != last_skip) {
		if (skip != BODY_DRAWS && config_boolean("debug.gpu_stats"))
			platform_log("first_person_body: no body: %s", body_skip_names[skip]);
		last_skip = skip;
	}
	return skip == BODY_DRAWS;
}

void halo_first_person_body_set_fill(int on)
{
	body_fill = on;
}

int halo_first_person_body_fill(void)
{
	return body_fill;
}

void halo_first_person_body_set_depth_clamp(int on)
{
	body_depth_clamp = on;
}

int halo_first_person_body_depth_clamp(void)
{
	return body_depth_clamp;
}
