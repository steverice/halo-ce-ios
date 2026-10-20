/* The hand's aim assist strength (input.aim_assist), through the game's own
autoaim code: tools/ios_test.py takes player_aim_projectile, the functions
it computes a target with and the two cone functions it bends a shot with
from source/game/aim_assist.c and source/math/real_math.c as they stand
(aim_assist_under_test.inc), and this probe compiles them against a small
fake world with port/linux/game/aim_assist_strength.c (included, not
linked).

The world: a player whose eye is at (0, 0, 0.6), one enemy 20 units ahead
(its autoaim pill 0.7 tall and 0.2 wide, its body 0.25 wide), a wall 60
units ahead, and a weapon with a 2 degree autoaim cone out to 40 units, a 6
degree magnetism cone and a 1 degree deviation cone. A shot is fired from
the eye, its aim turned some degrees to the left of the enemy; it hits when
its line after player_aim_projectile passes through the body.

Checks: a shot 1.5 degrees off (half a unit wide of the enemy) hits at
strength 1 and misses at 0; at 0.5 it misses and a shot 0.9 degrees off
hits; a shot 2.5 degrees off misses at 1 and hits at 2; at 0 a shot whose
line already passes through the enemy hits unbent and counts as fully aimed
at (the reticle's red), and one 1.5 degrees off doesn't; a network game or
a multiplayer game type bends the 1.5 degree shot at every setting, and
doesn't bend the 2.5 degree one at 2; so does a stick's shot; a setting out
of range clamps; at strength 1 the weapon's angles pass through bit for
bit; a weapon without autoaim gets none at 2; and the live chord's steps
run 0 to 2 by halves and hold at the ends. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../linux/game/aim_assist_strength.c"

/* ---------- the game's types and helpers the code under test reads */

typedef float real;
typedef unsigned char boolean;
typedef struct { real i, j, k; } real_vector3d;
typedef struct { real x, y, z; } real_point3d;
#define TRUE 1
#define FALSE 0
#define NONE -1
#define _real_epsilon 0.0001f
#define MAX(a,b) ((a)>(b)?(a):(b))
#define CEILING(n,ceiling) ((n)>(ceiling)?(ceiling):(n))
#define PIN(n,floor,ceiling) ((n)<(floor) ? (floor) : CEILING((n),(ceiling)))
#define TEST_FLAG(flags, bit) (((flags) & (1u << (bit))) != 0)
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(index) ((index) & 0xffff)
#define TAG_BLOCK_GET_ELEMENT(block, index, type) ((type *)(block)->address)

static int failures;
static void check(int yes, const char *what)
{
	if (!yes) { printf("FAILED: %s\n", what); failures++; }
}

#define match_assert(file, line, condition) check((condition) != 0, "the game's assertion: " #condition)
#define match_assert_valid_real_normal3d(file, line, vector) \
	check(fabsf(magnitude3d(vector) - 1.0f) < 0.001f, "the game's assertion: a unit vector")

static real sine(real angle) { return sinf(angle); }
static real cosine(real angle) { return cosf(angle); }
static real arccosine(real x) { return acosf(x); }
static real dot_product3d(real_vector3d const *a, real_vector3d const *b) { return a->i * b->i + a->j * b->j + a->k * b->k; }
static real magnitude_squared3d(real_vector3d const *v) { return dot_product3d(v, v); }
static real magnitude3d(real_vector3d const *v) { return sqrtf(magnitude_squared3d(v)); }
static real_vector3d *cross_product3d(real_vector3d const *a, real_vector3d const *b, real_vector3d *result)
{
	real_vector3d out = { a->j * b->k - a->k * b->j, a->k * b->i - a->i * b->k, a->i * b->j - a->j * b->i };

	*result = out;
	return result;
}
static real_vector3d *scale_vector3d(real_vector3d const *v, real scale, real_vector3d *result)
{
	result->i = v->i * scale;
	result->j = v->j * scale;
	result->k = v->k * scale;
	return result;
}
static real_vector3d *vector_from_points3d(real_point3d const *a, real_point3d const *b, real_vector3d *result)
{
	result->i = b->x - a->x;
	result->j = b->y - a->y;
	result->k = b->z - a->z;
	return result;
}
static void set_real_point3d(real_point3d *point, real x, real y, real z)
{
	point->x = x;
	point->y = y;
	point->z = z;
}
/* (as real_math.h's) */
static real normalize3d(real_vector3d *v)
{
	real magnitude = magnitude3d(v);

	if (!(_real_epsilon > fabsf(magnitude)))
		scale_vector3d(v, 1.0f / magnitude, v);
	else
		magnitude = 0.0f;
	return magnitude;
}
#define fast_normalize3d normalize3d
static boolean limit3d(real_vector3d *vector, real length)
{
	real dot = magnitude_squared3d(vector);

	if (dot > length * length)
	{
		scale_vector3d(vector, length / sqrtf(dot), vector);
		return TRUE;
	}
	return FALSE;
}
static real_vector3d *perpendicular3d(real_vector3d const *a, real_vector3d *result)
{
	real_vector3d other = { 0.0f, 0.0f, 1.0f };

	if (fabsf(a->k) > 0.9f)
	{
		other.i = 1.0f;
		other.k = 0.0f;
	}
	return cross_product3d(a, &other, result);
}

/* ---------- the fake world */

enum { PLAYER_UNIT = 1, ENEMY_UNIT = 2, WEAPON = 3, PLAYER = 0 };
enum { _weapon_aim_assists_only_when_zoomed_bit = 4, _unit_is_inconsequential_bit = 13 };
enum { _collision_user_aim_assist = 7, MAXIMUM_COLLISION_USER_STACK_DEPTH = 8 };
enum { _collision_test_for_projectiles_flags = 0 };

#define DEGREES_TO_RADIANS(degrees) ((degrees) * 3.14159265f / 180.0f)
#define ENEMY_DISTANCE 20.0f
#define ENEMY_PILL_WIDTH 0.2f
#define ENEMY_BODY_WIDTH 0.25f
#define ENEMY_HEIGHT 0.7f
#define WALL_DISTANCE 60.0f
#define EYE_HEIGHT 0.6f

struct aim_assist_parameters
{
	real autoaim_angle;
	real autoaim_distance;
	real magnetism_angle;
	real magnetism_distance;
	real deviation_angle;
	real unused;
};
struct unit_datum
{
	long definition_index;
	struct { real_point3d position; } object;
	struct { short current_weapon_index; } unit;
};
struct unit_definition { struct { unsigned long flags; } unit; };
struct weapon_datum { long definition_index; };
struct weapon_definition { struct { unsigned long flags; struct aim_assist_parameters aim_assist_parameters; } weapon; };
struct game_globals_player_control { real magnetism_inconsequential_target_scale; };
struct game_globals { struct { void *address; } player_control; };
struct player_datum { long unit_index; short team_index; long aim_assist_unit_index; long aim_assist_timestamp; };
struct collision_result { real_point3d point; };

static struct unit_datum player_unit = { 0, { { 0.0f, 0.0f, 0.0f } }, { 0 } };
static struct unit_datum enemy_unit = { 0, { { ENEMY_DISTANCE, 0.0f, 0.0f } }, { 0 } };
static struct unit_definition unit_definition;
static struct weapon_datum weapon;
static struct weapon_definition weapon_definition = { { 0, {
	DEGREES_TO_RADIANS(2.0f), 40.0f, DEGREES_TO_RADIANS(6.0f), 50.0f, DEGREES_TO_RADIANS(1.0f), 0.0f } } };
static struct game_globals_player_control player_control = { 1.0f };
static struct game_globals game_globals = { { &player_control } };
static struct player_datum player = { PLAYER_UNIT, 0, NONE, 0 };
static long global_current_collision_user_depth = 1;
static long global_current_collision_users[MAXIMUM_COLLISION_USER_STACK_DEPTH];
static real zoom_magnification = 1.0f;

/* what the probe sets for each case: input.aim_assist, whether a hand aims
the shot, and whether the game's rules are fixed (a network game, or a
multiplayer game type) */
static float probe_setting = 1.0f;
static int probe_hand = 1;
static int probe_fixed;

static struct unit_datum *unit_get(long unit_index) { return unit_index == ENEMY_UNIT ? &enemy_unit : &player_unit; }
static long unit_inventory_get_weapon(long unit_index, short weapon_index) { return WEAPON; }
static struct weapon_datum *weapon_get(long weapon_index) { return &weapon; }
static struct weapon_definition *weapon_definition_get(long definition_index) { return &weapon_definition; }
static real weapon_get_zoom_magnification(long weapon_index, short zoom_level) { return zoom_magnification; }
static struct unit_definition *unit_definition_get(long definition_index) { return &unit_definition; }
static struct game_globals *scenario_get_game_globals(void) { return &game_globals; }
static struct player_datum *player_get(long player_index) { return &player; }
static long unit_get_aiming_unit_index(long unit_index) { return unit_index; }
static short unit_get_zoom_level(long unit_index) { return NONE; }
static long game_time_get(void) { return 30; }
static void biped_get_autoaim_pill(long object_index, real_point3d *base, real_vector3d *height, real *width)
{
	set_real_point3d(base, ENEMY_DISTANCE, 0.0f, 0.0f);
	height->i = height->j = 0.0f;
	height->k = ENEMY_HEIGHT;
	*width = ENEMY_PILL_WIDTH;
}
static short director_camera_deterministic(long unit_index, real_point3d *position, real_vector3d *forward);

/* a line from origin along a unit direction against the enemy's body, an
upright cylinder: 1 and how far along the line it enters */
static int line_hits_enemy(real_point3d const *origin, real_vector3d const *direction, real *distance)
{
	real along = direction->i * direction->i + direction->j * direction->j;
	real offset_i = origin->x - ENEMY_DISTANCE;
	real offset_j = origin->y;
	real half = offset_i * direction->i + offset_j * direction->j;
	real discriminant = half * half - along * (offset_i * offset_i + offset_j * offset_j - ENEMY_BODY_WIDTH * ENEMY_BODY_WIDTH);
	real t, height;

	if (along <= 0.0f || discriminant < 0.0f)
		return 0;
	t = (-half - sqrtf(discriminant)) / along;
	height = origin->z + direction->k * t;
	if (t <= 0.0f || height < 0.0f || height > ENEMY_HEIGHT)
		return 0;
	*distance = t;
	return 1;
}

/* the projectiles' collision test: the enemy's body, else the wall, else
nothing (the game then leaves collision.point as it was; here the vector's end) */
static boolean collision_test_vector(long flags, real_point3d const *origin, real_vector3d const *vector,
	long ignore_object_index, struct collision_result *collision)
{
	real_vector3d direction = *vector;
	real length = normalize3d(&direction);
	real distance = length;
	boolean hit = FALSE;

	if (line_hits_enemy(origin, &direction, &distance) && distance <= length)
		hit = TRUE;
	else if (direction.i > 0.0f && (WALL_DISTANCE - origin->x) / direction.i <= length)
	{
		distance = (WALL_DISTANCE - origin->x) / direction.i;
		hit = TRUE;
	}
	else
		distance = length;
	set_real_point3d(&collision->point, origin->x + direction.i * distance, origin->y + direction.j * distance,
		origin->z + direction.k * distance);
	return hit;
}

/* the targeting search for a world of one enemy, in full view: the game's
aim_assist() finds candidates in the cone and the range of the larger of
the autoaim and magnetism limits (find_aim_assist_targets), computes each
with aim_assist_compute_target (the real one, below) and keeps the best
with a clear line of sight */
struct aim_assist_target;
static boolean aim_assist(struct aim_assist_parameters const *parameters, real_point3d const *position,
	real_vector3d const *direction, long ignore_object_index, short ignore_team_index,
	struct aim_assist_target *target);

/* the port's side of the hooks, as controller_aim_game.c has them, from the
probe's setting, hand flag and rules */
float halo_aim_assist_player_strength(long player_absolute_index)
{
	return aim_assist_strength_effective(probe_setting, probe_hand, probe_fixed);
}

void halo_aim_assist_apply(float strength, float *autoaim_angle, float *deviation_angle)
{
	aim_assist_strength_apply(strength, autoaim_angle, deviation_angle);
}

/* ---------- the game's code (tools/ios_test.py writes it from the sources) */

#include "aim_assist_under_test.inc"

static boolean aim_assist(struct aim_assist_parameters const *parameters, real_point3d const *position,
	real_vector3d const *direction, long ignore_object_index, short ignore_team_index,
	struct aim_assist_target *target)
{
	real distance = MAX(parameters->autoaim_distance, parameters->magnetism_distance);
	real angle = MAX(parameters->autoaim_angle, parameters->magnetism_angle);

	return distance > 0.0f && angle > 0.0f &&
		aim_assist_compute_target(parameters, ENEMY_UNIT, position, direction, target);
}

/* ---------- the cases */

/* the aim this many degrees to the left of the enemy's middle, level */
static real_vector3d probe_aim;
static void aim_off(float degrees)
{
	probe_aim.i = cosf(DEGREES_TO_RADIANS(degrees));
	probe_aim.j = sinf(DEGREES_TO_RADIANS(degrees));
	probe_aim.k = 0.0f;
}

static short director_camera_deterministic(long unit_index, real_point3d *position, real_vector3d *forward)
{
	set_real_point3d(position, 0.0f, 0.0f, EYE_HEIGHT);
	*forward = probe_aim;
	return 0;
}

struct shot
{
	int hit;        /* the shot's line passes through the enemy's body */
	float bent;     /* degrees player_aim_projectile turned it */
	long target;    /* what it returned */
};

static struct shot fire(float degrees_off, float setting, int hand, int fixed)
{
	real_point3d origin = { 0.0f, 0.0f, EYE_HEIGHT };
	real_vector3d direction;
	struct shot shot;
	real distance;

	probe_setting = setting;
	probe_hand = hand;
	probe_fixed = fixed;
	aim_off(degrees_off);
	direction = probe_aim;
	shot.target = player_aim_projectile(PLAYER, &origin, &direction);
	shot.bent = acosf(fminf(1.0f, dot_product3d(&direction, &probe_aim))) * 180.0f / 3.14159265f;
	shot.hit = line_hits_enemy(&origin, &direction, &distance);
	printf("%.1f degrees off, input.aim_assist %.1f, %s, %s: bent %.3f degrees, %s\n", degrees_off, setting,
		hand ? "hand" : "stick", fixed ? "multiplayer" : "single player", shot.bent, shot.hit ? "hits" : "misses");
	return shot;
}

/* the targeting query's autoaim level at a strength (what turns the reticle red at 1) */
static float query_level(float degrees_off, float strength)
{
	struct aim_assist_parameters parameters;
	struct aim_assist_target target;
	real_point3d eye = { 0.0f, 0.0f, EYE_HEIGHT };

	aim_off(degrees_off);
	check(unit_get_aim_assist_parameters(PLAYER_UNIT, NONE, &parameters), "the weapon has aim assist parameters");
	aim_assist_strength_apply(strength, &parameters.autoaim_angle, &parameters.deviation_angle);
	if (!aim_assist(&parameters, &eye, &probe_aim, PLAYER_UNIT, 0, &target))
		return 0.0f;
	return target.autoaim_level;
}

int main(void)
{
	struct shot shot;
	float autoaim, deviation, setting;
	int step;

	/* the game's own cone, and none */
	shot = fire(1.5f, 1.0f, 1, 0);
	check(shot.hit && shot.target == ENEMY_UNIT && shot.bent > 0.5f, "1.5 degrees off at strength 1: bent onto the enemy");
	shot = fire(1.5f, 0.0f, 1, 0);
	check(!shot.hit && shot.bent < 0.01f, "1.5 degrees off at strength 0: unbent, a miss");
	check(shot.target == ENEMY_UNIT, "strength 0 keeps the target the magnetism cone finds (homing projectiles)");

	/* half and double */
	check(!fire(1.5f, 0.5f, 1, 0).hit, "1.5 degrees off at strength 0.5: a miss");
	check(fire(0.9f, 0.5f, 1, 0).hit, "0.9 degrees off at strength 0.5: a hit");
	check(!fire(0.9f, 0.0f, 1, 0).hit, "0.9 degrees off at strength 0: a miss");
	check(!fire(2.5f, 1.0f, 1, 0).hit, "2.5 degrees off at strength 1: a miss");
	check(fire(2.5f, 2.0f, 1, 0).hit, "2.5 degrees off at strength 2: a hit (the deviation cone widens with the autoaim cone)");
	check(!fire(5.0f, 2.0f, 1, 0).hit, "5 degrees off at strength 2: a miss");

	/* on the enemy at strength 0: nothing to bend, and the reticle's red */
	shot = fire(0.3f, 0.0f, 1, 0);
	check(shot.hit && shot.bent < 0.06f, "0.3 degrees off (on the body) at strength 0: a hit, unbent");
	check(query_level(0.3f, 0.0f) == 1.0f, "strength 0: an aim through the enemy is fully aimed at (the red reticle)");
	check(query_level(1.5f, 0.0f) == 0.0f, "strength 0: an aim 1.5 degrees off isn't");
	check(query_level(1.5f, 1.0f) == 1.0f, "strength 1: an aim 1.5 degrees off is fully aimed at");
	check(query_level(2.5f, 1.0f) < 1.0f && query_level(2.5f, 2.0f) == 1.0f,
		"strength 2: the red reticle's cone widens with the shot's");

	/* multiplayer, and a stick: the game's own whatever the setting */
	check(fire(1.5f, 0.0f, 1, 1).hit, "multiplayer ignores input.aim_assist = 0: the 1.5 degree shot bends");
	check(!fire(2.5f, 2.0f, 1, 1).hit, "multiplayer ignores input.aim_assist = 2: the 2.5 degree shot doesn't");
	check(fire(1.5f, 0.0f, 0, 0).hit, "a stick's shot ignores input.aim_assist = 0");
	check(!fire(2.5f, 2.0f, 0, 0).hit, "a stick's shot ignores input.aim_assist = 2");
	shot = fire(1.5f, 1.0f, 1, 0);
	check(fire(1.5f, 0.0f, 1, 1).bent == shot.bent && fire(1.5f, 2.0f, 0, 0).bent == shot.bent,
		"multiplayer's and a stick's shots bend exactly as strength 1's");

	/* the setting's range */
	check(aim_assist_strength_clamp(-1.0f) == 0.0f && aim_assist_strength_clamp(9.0f) == AIM_ASSIST_STRENGTH_MAXIMUM &&
		aim_assist_strength_clamp(nanf("")) == AIM_ASSIST_STRENGTH_DEFAULT && aim_assist_strength_clamp(0.25f) == 0.25f,
		"the setting clamps to 0..2, and isn't a number: 1");
	check(aim_assist_strength_effective(9.0f, 1, 0) == AIM_ASSIST_STRENGTH_MAXIMUM &&
		aim_assist_strength_effective(9.0f, 1, 1) == 1.0f && aim_assist_strength_effective(0.0f, 0, 0) == 1.0f,
		"the effective strength: the clamped setting for a hand in single player, else 1");

	/* strength 1 changes nothing, and no autoaim stays none */
	autoaim = DEGREES_TO_RADIANS(2.0f) * (1.0f / 3.0f);
	deviation = autoaim;
	aim_assist_strength_apply(1.0f, &autoaim, &deviation);
	check(autoaim == DEGREES_TO_RADIANS(2.0f) * (1.0f / 3.0f) && deviation == autoaim, "strength 1: the angles bit for bit");
	autoaim = 0.0f;
	deviation = DEGREES_TO_RADIANS(1.0f);
	aim_assist_strength_apply(2.0f, &autoaim, &deviation);
	check(autoaim == 0.0f && deviation == DEGREES_TO_RADIANS(1.0f), "a weapon without autoaim gets none at strength 2");
	autoaim = DEGREES_TO_RADIANS(2.0f);
	deviation = DEGREES_TO_RADIANS(2.0f);
	aim_assist_strength_apply(0.0f, &autoaim, &deviation);
	check(autoaim == AIM_ASSIST_ON_TARGET_ANGLE && deviation == DEGREES_TO_RADIANS(2.0f),
		"strength 0: the on-target floor, and the game's deviation cone");

	/* the live chord's steps */
	setting = 1.0f;
	for (step = 0; step < 4; step++)
		setting = aim_assist_strength_step(setting, 1);
	check(setting == 2.0f, "steps up hold at 2");
	for (step = 0; step < 3; step++)
		setting = aim_assist_strength_step(setting, -1);
	check(setting == 0.5f, "three steps down from 2: 0.5");
	check(aim_assist_strength_step(0.5f, -1) == 0.0f && aim_assist_strength_step(0.0f, -1) == 0.0f, "steps down hold at 0");
	check(aim_assist_strength_step(0.7f, 1) == 1.0f, "a setting between steps moves onto the next");

	if (failures)
	{
		printf("%d FAILED\n", failures);
		return 1;
	}
	printf("aim assist probe passed\n");
	return 0;
}
