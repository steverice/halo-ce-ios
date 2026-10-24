/* Production damage policy in a small world: no maps, renderer or network. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef int boolean;
typedef unsigned char byte;
typedef struct { float x, y, z; } real_point3d;
typedef struct { float i, j, k; } real_vector3d;
#define NONE (-1)
#define TRUE 1
#define FALSE 0
#define FLAG(bit) (1u << (bit))
#define SET_FLAG(bits, bit, value) ((bits) = ((bits) & ~FLAG(bit)) | ((value) ? FLAG(bit) : 0))
#define TEST_FLAG(bits, bit) (((bits) & FLAG(bit)) != 0)
enum { _object_type_biped, _object_type_vehicle, _object_type_scenery, _object_type_device,
	_object_type_weapon, _object_mask_unit = 3, _object_mask_scenery = 4, _object_mask_device = 8 };
enum { _game_connection_local, _game_connection_network_server, _game_connection_network_client };
enum { _damage_kill_instantly_bit };
/* COOP_FLAGS */
#define STATIC_TARGET_TYPES (_object_mask_scenery | _object_mask_device)
struct damage_data { long owner_player_index, owner_object_index; short owner_team_index; unsigned flags; };
struct object_fields { short type, owner_team_index; float maximum_body_vitality, maximum_shield_vitality; real_vector3d forward; long owner_player_index; };
struct unit_datum { struct object_fields object; struct { long player_index; } unit; };
struct network_game { byte cooperative_flags; };
static struct { long action_count, state_count; } replay;
static boolean coop_active = TRUE, client;
static unsigned host_version = 38;
static unsigned network_game_host_version(void) { return host_version; }
static struct network_game game;
static struct unit_datum objects[8];
static boolean network_coop_active(void) { return coop_active; }
static boolean network_game_distributed_client(void) { return client; }
static struct network_game *network_game_get_game(void) { return &game; }
static struct unit_datum *object_try_and_get_and_verify_type(long index, unsigned mask)
{
	return index >= 0 && index < 8 && TEST_FLAG(mask, objects[index].object.type) ? &objects[index] : NULL;
}
static float object_get_maximum_body_vitality(long index, boolean ignore) { return objects[index].object.maximum_body_vitality; }
static float object_get_maximum_shield_vitality(long index, boolean ignore) { return objects[index].object.maximum_shield_vitality; }
static boolean game_team_is_enemy(short a, short b) { return a == NONE || b == NONE || a != b; }
enum { MAXIMUM_HIT_REPORTS_PER_TICK = 8 };
struct distributed_hit_report {
	long object_index, host_time;
	struct damage_data damage;
	real_point3d target_position;
	real_vector3d object_normal;
	short node_index, region_index, material_index;
	boolean has_normal;
};
struct player_datum { boolean quit_out_of_game; };
static struct distributed_hit_report damage_reports[MAXIMUM_HIT_REPORTS_PER_TICK];
static int damage_report_count, material_count, connection;
static boolean damage_dealing_report, distributed_damage_authorized, effect_dealt;
static struct player_datum remote;
#define csmemset memset
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(index) ((index) & 0xffff)
static int game_connection(void) { return connection; }
static struct player_datum *player_try_and_get(long index) { return &remote; }
static boolean distributed_player_is_local(long index) { return index == 3; }
static long distributed_living_unit(struct player_datum *player) { return NONE; }
static boolean distributed_damage_is_recoil(struct damage_data const *damage, long index) { return FALSE; }
static void damage_replay_player_effect(long index, struct damage_data *damage, float amount) { }
static boolean network_objects_client_has(long index) { return index < 3; }
static void distributed_damage_from_data(struct damage_data const *damage, struct damage_data *result) { *result = *damage; }
static void object_get_origin(long index, real_point3d *point) { memset(point, 0, sizeof(*point)); }
static long distributed_latest_host_time(void) { return 10; }
static long distributed_player_machine(short index) { return 9; }
static void distributed_damage_material(struct damage_data *damage, long index, short material) { material_count++; }
static void effect_new_from_object(long definition, long owner, long object, long player, float a, float b,
	void const *color, void const *field);
enum { _network_game_server_state_pregame };
struct network_game_server { struct network_game game; short state; };
static struct network_game_server server;
static int settings_sent;
static struct network_game_server *global_network_game_server_get(void) { return &server; }
static boolean network_game_server_send_game_data_pregame(struct network_game_server *s) { settings_sent++; return TRUE; }
static void network_event(char const *message) { assert(0); }
enum { _effect_vector_normal, _effect_vector_incident, _effect_vector_negative_incident,
	_effect_vector_reflected, _effect_vector_gravity, NUMBER_OF_EFFECT_MARKERS };
static real_vector3d down = {0, 0, -1}, forward = {1, 0, 0}, zero;
static real_vector3d const *global_down3d = &down, *global_forward3d = &forward, *global_zero_vector3d = &zero;
static struct unit_datum *object_get(long index) { return &objects[index]; }
static float normalize3d(real_vector3d *vector) { return 1; }
static void scale_vector3d(real_vector3d const *a, float scale, real_vector3d *b) { *b = *a; }
static void vector_from_points3d(real_point3d const *a, real_point3d const *b, real_vector3d *result) { *result = zero; }
static void reflect_vector3d(real_vector3d const *a, real_vector3d const *b, real_vector3d *result) { *result = *a; }
static void effect_new_attached_from_markers(long definition, long owner, long object, short node, short count,
	char const **names, real_point3d const *points, real_vector3d const *forwards, float a, float b,
	void const *color, void const *field);
static void effect_new_unattached_from_markers(long definition, long owner, real_vector3d const *velocity, short count,
	char const **names, real_point3d const *points, real_vector3d const *forwards, float a, float b,
	void const *color, void const *field, boolean extra);
static boolean attached_seen, unattached_seen;
/* PRODUCTION_FUNCTIONS */
/* A break effect deals damage synchronously, while its parent host hit is authorized. */
static void effect_new_from_object(long definition, long owner, long object, long player, float a, float b,
	void const *color, void const *field)
{
	struct damage_data damage = { objects[owner].object.owner_player_index, owner, 0, 0 };
	effect_dealt = network_damage_deals(&damage, 0, NONE, NONE, NONE, NULL, distributed_damage_authorized);
}
static void effect_new_attached_from_markers(long definition, long owner, long object, short node, short count,
	char const **names, real_point3d const *points, real_vector3d const *forwards, float a, float b,
	void const *color, void const *field)
{
	attached_seen = TRUE;
	effect_new_from_object(definition, owner, object, NONE, a, b, color, field);
}
static void effect_new_unattached_from_markers(long definition, long owner, real_vector3d const *velocity, short count,
	char const **names, real_point3d const *points, real_vector3d const *forwards, float a, float b,
	void const *color, void const *field, boolean extra)
{
	unattached_seen = TRUE;
	effect_new_from_object(definition, owner, owner, NONE, a, b, color, field);
}
int main(void)
{
	struct damage_data damage = { 3, 1, 0, 0 };
	objects[0].object = (struct object_fields){ _object_type_biped, 0, 1, 1 };
	objects[0].unit.player_index = NONE;
	objects[1].object = (struct object_fields){ _object_type_vehicle, 0, 1, 0 };
	objects[1].unit.player_index = NONE;
	objects[2].object = (struct object_fields){ _object_type_biped, 0, 1, 1 };
	objects[2].unit.player_index = 4;
	assert(!input_replay_playing());
	/* FORMATS.md: independent inverted bits yield 00, 02, 03, 01. */
	assert(server.game.cooperative_flags == 0);
	network_game_server_port_set_cooperative_ally_friendly_fire(FALSE);
	assert(server.game.cooperative_flags == 2);
	network_game_server_port_set_cooperative_player_collisions(FALSE);
	assert(server.game.cooperative_flags == 3);
	network_game_server_port_set_cooperative_ally_friendly_fire(TRUE);
	assert(server.game.cooperative_flags == 1 && settings_sent == 3);
	server.game.cooperative_flags |= 0x80;
	network_game_server_port_set_cooperative_ally_friendly_fire(FALSE);
	assert(server.game.cooperative_flags == 0x83); /* preserve other flags */

	assert(!network_coop_protects_ally(&damage, 0)); /* default ON */
	game.cooperative_flags = 2;
	assert(network_coop_protects_ally(&damage, 0));
	assert(network_coop_protects_ally(&damage, 1));
	assert(!network_coop_protects_ally(&damage, 2)); /* separate player policy */
	damage.owner_team_index = 1;
	assert(!network_coop_protects_ally(&damage, 0)); /* enemies */
	damage.owner_team_index = 0; damage.owner_player_index = NONE;
	assert(!network_coop_protects_ally(&damage, 0)); /* AI-on-AI */
	damage.owner_player_index = 3; damage.flags = FLAG(_damage_kill_instantly_bit);
	assert(!network_coop_protects_ally(&damage, 0)); /* scripted death */
	damage.flags = 0; coop_active = FALSE;
	assert(!network_coop_protects_ally(&damage, 0)); /* local split screen */
	coop_active = TRUE; replay.action_count = 20;
	assert(input_replay_playing() && !network_coop_protects_ally(&damage, 0));
	replay.action_count = 0;
	replay.state_count = 20;
	assert(input_replay_playing() && !network_coop_protects_ally(&damage, 0));
	replay.state_count = 0;
	objects[3].object = (struct object_fields){ _object_type_scenery, 0, 0, 1 };
	objects[4].object = (struct object_fields){ _object_type_device, 0, 1, 0 };
	objects[5].object = (struct object_fields){ _object_type_scenery, 0, 0, 0 };
	assert(static_target(3) && static_target(4) && !static_target(5) && !static_target(0));
	objects[3].object.owner_player_index = 3;
	damage.owner_object_index = 3;
	assert(network_damage_static_source(&damage));
	connection = _game_connection_network_server;
	damage.owner_player_index = 7; /* remote-owned static destruction */
	assert(network_damage_deals(&damage, 0, NONE, NONE, NONE, NULL, FALSE));
	connection = _game_connection_network_client;
	damage.owner_player_index = 3;
	assert(!network_damage_deals(&damage, 0, NONE, NONE, NONE, NULL, FALSE) && !damage_report_count);
	assert(network_damage_deals(&damage, 3, NONE, NONE, NONE, NULL, TRUE)); /* host static event */
	client = TRUE;
	distributed_damage_authorized = TRUE;
	damage_effect_new_on_object(9, 3);
	assert(!effect_dealt && !damage_report_count && distributed_damage_authorized);
	{
		real_point3d position = {0, 0, 0};
		damage_effect_new_at_location(9, 3, 0, &position, &forward, NULL);
		assert(attached_seen && !effect_dealt && !damage_report_count && distributed_damage_authorized);
		damage_effect_new_at_location(9, 3, NONE, &position, &forward, &forward);
		assert(unattached_seen && !effect_dealt && !damage_report_count && distributed_damage_authorized);
	}

	/* Destruction keeps the source's player credit without handing that
	player authority. Exercise client-owned static destruction through the
	same constructors used by body/shield depletion. */
	for (long source = 3; source <= 4; source++)
	{
		objects[source].object.owner_player_index = 7;
		damage.owner_object_index = source;
		damage.owner_player_index = 7;
		connection = _game_connection_network_server;
		client = FALSE;
		damage_report_count = 0;
		for (int reporting = FALSE; reporting <= TRUE; reporting++)
		{
			damage_dealing_report = reporting;
			assert(network_damage_deals(&damage, 0, NONE, NONE, NONE, NULL, FALSE));
			damage_effect_new_on_object(9, source);
			assert(effect_dealt && !damage_report_count && distributed_damage_authorized);
		}
		connection = _game_connection_network_client;
		client = TRUE;
		/* Neither the remote owner nor a local owner may report secondary damage. */
		for (int owner = 3; owner <= 7; owner += 4)
		{
			objects[source].object.owner_player_index = owner;
			damage.owner_player_index = owner;
			assert(!network_damage_deals(&damage, 0, NONE, NONE, NONE, NULL, FALSE));
			damage_effect_new_on_object(9, source);
			assert(!effect_dealt && !damage_report_count && distributed_damage_authorized);
		}
		damage_dealing_report = FALSE;
	}

	damage.owner_object_index = 1;
	assert(!network_damage_static_source(&damage)); /* player vehicle */
	client = TRUE;
	assert(network_damage_updates_shields(3) && network_damage_updates_shields(4));
	assert(!network_damage_updates_shields(0));
	replay.action_count = 1;
	assert(!static_target(3) && static_target(4)); /* pre-0.5.0 body-only */
	assert(!network_damage_static_source(&damage) && !network_damage_updates_shields(3));
	client = FALSE;
	assert(network_damage_updates_shields(0));
	/* Negotiated legacy hosts keep their pre-33/34 agreements. */
	replay.action_count = 0;
	client = TRUE;
	connection = _game_connection_network_client;
	damage.owner_player_index = 3;
	for (host_version = 29; host_version <= 38; host_version++)
	{
		assert(network_coop_protects_ally(&damage, 0) == (host_version >= 33));
		assert(static_target(3) == (host_version >= 34));
		assert(static_target(4));
		assert(network_damage_updates_shields(3) == (host_version >= 34));
		assert(network_damage_updates_shields(4) == (host_version >= 34));
		assert(!network_damage_updates_shields(0));
		damage.owner_object_index = 3;
		assert(network_damage_static_source(&damage) == (host_version >= 34));
		damage_report_count = 0;
		assert(!network_damage_deals(&damage, 0, NONE, NONE, NONE, NULL, FALSE));
		assert(damage_report_count == (host_version < 34)); /* old source reporting */
		damage.owner_object_index = 1;
		damage_report_count = 0;
		assert(!network_damage_deals(&damage, 3, NONE, NONE, NONE, NULL, FALSE));
		assert(damage_report_count == (host_version >= 34)); /* shield target eligibility */
		damage_report_count = 0;
		assert(!network_damage_deals(&damage, 4, NONE, NONE, NONE, NULL, FALSE));
		assert(damage_report_count == 1); /* body target in every agreement */
		distributed_damage_authorized = TRUE;
		damage_report_count = 0;
		damage_effect_new_on_object(9, 3);
		assert(effect_dealt == (host_version < 34)); /* old nested replay behavior */
		assert(distributed_damage_authorized && !damage_report_count);
		{
			real_point3d position = {0, 0, 0};
			damage_effect_new_at_location(9, 3, 0, &position, &forward, NULL);
			assert(effect_dealt == (host_version < 34) && distributed_damage_authorized);
			damage_effect_new_at_location(9, 3, NONE, &position, &forward, &forward);
			assert(effect_dealt == (host_version < 34) && distributed_damage_authorized);
		}
	}
	/* Hosting uses the current agreement, independent of an old remembered host. */
	host_version = 29;
	connection = _game_connection_network_server;
	client = FALSE;
	assert(network_coop_protects_ally(&damage, 0));
	assert(static_target(3) && network_damage_updates_shields(3));
	damage.owner_object_index = 3;
	assert(network_damage_static_source(&damage));
	puts("PASS: ally immunity, static shields/authority, replay rules");
	return 0;
}
