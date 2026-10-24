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
struct object_fields { short type, owner_team_index; float maximum_body_vitality, maximum_shield_vitality; };
struct unit_datum { struct object_fields object; struct { long player_index; } unit; };
struct network_game { byte cooperative_flags; };
static struct { long action_count, state_count; } replay;
static boolean coop_active = TRUE, client;
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
static boolean network_objects_client_has(long index) { return TRUE; }
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
/* PRODUCTION_FUNCTIONS */
/* A break effect deals damage synchronously, while its parent host hit is authorized. */
static void effect_new_from_object(long definition, long owner, long object, long player, float a, float b,
	void const *color, void const *field)
{
	struct damage_data damage = { 3, owner, 0, 0 };
	effect_dealt = network_damage_deals(&damage, 0, NONE, NONE, NONE, NULL, distributed_damage_authorized);
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
	puts("PASS: ally immunity, static shields/authority, replay rules");
	return 0;
}
