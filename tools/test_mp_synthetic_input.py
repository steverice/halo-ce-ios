#!/usr/bin/env python3
"""Run the host's real distributed-input queue and tick snapshot with suite actions."""

from pathlib import Path
import shutil
import subprocess
import tempfile
from unittest import SkipTest


ROOT = Path(__file__).resolve().parents[1]
QUEUES = (ROOT / "source/game/player_queues_new.c").read_text()
NETWORK_TEST = (ROOT / "port/linux/game/network_test.c").read_text()
UNITS = (ROOT / "source/units/units.c").read_text()
UNIT_HEADER = (ROOT / "source/units/units.h").read_text()
WEAPON_HEADER = (ROOT / "source/items/weapons.h").read_text()
ITEMS = (ROOT / "source/items/items.c").read_text()
OBJECTS = (ROOT / "source/objects/objects.c").read_text()


def function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for position in range(opening, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[start:position + 1]
    raise ValueError(f"Unclosed source function: {signature}")


WEAPON_FIXTURES = r'''

#define TICKS_PER_SECOND 30
#define WEAPON_DEFINITION_TAG 1
#define _object_dead_bit 0
#define _unit_add_weapon_starting 1
enum {_item_attached_to_unit_bit,_item_belongs_to_player_bit,_item_has_nonzero_angular_velocity_bit,
_item_on_structure_bit,_item_on_object_bit,_item_does_not_accelerate_bit};
struct player_datum {long unit_index;};
struct unit_datum {struct {unsigned long damage_flags;} object;struct {long weapon_object_indices[4],player_index;short current_weapon_index;} unit;};
struct weapon_datum {long definition_index;struct {long parent_object_index,owner_player_index;struct {int leaf_index,cluster_index;} location;} object;struct {unsigned long flags;} item;};
struct object_placement_data {long definition,owner;};
static struct player_datum test_players[128];
static struct unit_datum test_units[128],moved_unit;
static int move_on_create,unit_moved;
static struct weapon_datum test_weapons[2048];
static long next_weapon=1;static int missing_definition,fail_attach,created;
static struct player_datum *player_try_and_get(long index) {return &test_players[DATUM_INDEX_TO_ABSOLUTE_INDEX(index)];}
static struct unit_datum *unit_try_and_get(long index) {return index==NONE?NULL:unit_moved&&DATUM_INDEX_TO_ABSOLUTE_INDEX(index)==1?&moved_unit:&test_units[DATUM_INDEX_TO_ABSOLUTE_INDEX(index)];}
static struct weapon_datum *weapon_try_and_get(long index) {return index<=0||index>=next_weapon?NULL:&test_weapons[index];}
static long tag_loaded(int tag,const char *name) {
    (void)tag;
    const char *names[]={"pistol","assault rifle","plasma rifle","plasma pistol","needler","shotgun"};
    for(int i=0;i<6;i++)if(!strcmp(strrchr(name,92)+1,names[i]))return missing_definition==i+1?NONE:i+1;
    abort();
}
static const char *tag_get_name(long index) {(void)index;return "test weapon";}
static void unit_delete_all_weapons(long index) {for(int i=0;i<4;i++)if(i!=unit_try_and_get(index)->unit.current_weapon_index)unit_try_and_get(index)->unit.weapon_object_indices[i]=NONE;}
static void object_placement_data_new(struct object_placement_data *p,long definition,long owner) {p->definition=definition;p->owner=owner;}
static long object_new(struct object_placement_data *p) {long i=next_weapon++;created++;if(move_on_create) {moved_unit=test_units[1];memset(&test_units[1],0xff,sizeof(test_units[1]));unit_moved=1;move_on_create=0;}test_weapons[i].definition_index=p->definition;test_weapons[i].object.parent_object_index=NONE;return i;}
static int network_objects_may_delete(long index) {(void)index;return 1;}
static void object_delete_initial_recursive(long index,int siblings) {(void)index;(void)siblings;}
static void object_delete_recursive(long index,int siblings) {
    (void)siblings;
    test_weapons[index].definition_index=NONE;
    for(int actor=0;actor<128;actor++) {
        struct unit_datum *u=unit_try_and_get((1L<<16)|actor);
        for(int slot=0;slot<4;slot++)if(u->unit.weapon_object_indices[slot]==index) {u->unit.weapon_object_indices[slot]=NONE;if(u->unit.current_weapon_index==slot)u->unit.current_weapon_index=NONE;}
    }
}
__IMMEDIATE_DELETE__
#define item_datum weapon_datum
static struct weapon_datum *item_get(long index) {return weapon_try_and_get(index);}
static struct unit_datum *unit_get(long index) {return unit_try_and_get(index);}
static void object_set_garbage(long index,int garbage) {(void)index;(void)garbage;}
static void scenario_location_award_bonus(void *location) {(void)location;}
__ITEM_INVENTORY__
static int unit_add_weapon_to_inventory(long unit,long weapon,long mode) {
    (void)mode;if(fail_attach)return 0;
    struct unit_datum *u=unit_try_and_get(unit);
    for(int i=0;i<4;i++)if(u->unit.weapon_object_indices[i]==NONE) {u->unit.weapon_object_indices[i]=weapon;item_in_unit_inventory(weapon,unit);return 1;}
    return 0;
}

'''

HARNESS = r'''
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int boolean;
typedef float real;
typedef unsigned short word;
#define TRUE 1
#define FALSE 0
#define NONE -1
#define FLAG(bit) (1UL << (bit))
#define TEST_FLAG(flags, bit) (((flags) & FLAG(bit)) != 0)
#define SET_FLAG(flags, bit, value) do { if (value) (flags) |= FLAG(bit); else (flags) &= ~FLAG(bit); } while (0)
__UNIT_CONTROL_ENUM__
__WEAPON_CONTROL_ENUM__
#define MAXIMUM_NUMBER_OF_PLAYERS 128
#define BENCHMARK_MP_MAXIMUM_PLAYERS 128
#define MAXIMUM_LOCAL_PLAYERS 4
#define MAXIMUM_WEAPONS_PER_UNIT 4
#define NUMBER_OF_UNIT_GRENADE_TYPES 2
#define DISTRIBUTED_INPUT_HISTORY 4
#define DISTRIBUTED_INPUT_SILENCE_TICKS 10
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(index) ((int)((index) & 0xffff))
#define csmemset memset
#define csmemcpy memcpy
#define match_assert(file, line, expression) do { (void)(file); (void)(line); if (!(expression)) abort(); } while (0)
enum { _game_connection_local, _game_connection_network_server };
enum { _network_test_off, _network_test_host };
static char last_log[512];
static void platform_log(const char *format, ...) { va_list args;va_start(args,format);vsnprintf(last_log,sizeof(last_log),format,args);va_end(args); }
struct real_vector2d { float i, j; };
struct real_euler_angles2d { float yaw, pitch; };
struct player_action {
    unsigned long control_flags;
    struct real_euler_angles2d desired_facing;
    struct real_vector2d throttle;
    float primary_trigger;
    short desired_weapon_index, desired_grenade_index, desired_zoom_level, pad;
};
struct server_update { word action_count; short pad; struct player_action actions[MAXIMUM_NUMBER_OF_PLAYERS]; };
struct update { long update_number; struct server_update update; };
struct update_server_queue_datum { short identifier, pad; long next_update_number; struct player_action current_action; };
struct data_array { int count; struct update_server_queue_datum *data; };
static struct update_server_queue_datum queues[128];
static struct data_array queue_array = { 128, queues };
static struct { int initialized; long next_update_number_to_build; struct data_array *queues; } update_server_globals = {
    TRUE, 0, &queue_array
};
static struct { boolean valid; long tick, received_time; } update_server_distributed_inputs[MAXIMUM_NUMBER_OF_PLAYERS];
static unsigned long update_server_pending_control_flags[MAXIMUM_NUMBER_OF_PLAYERS];
static struct update current_update;
static struct server_update observed;
static long game_tick, machine_players[128][MAXIMUM_LOCAL_PLAYERS];
__TRACE_TYPE__
static struct { int suite_owned, players_wanted, suite_network_players, mode;
    unsigned long suite_setup_input_ticks;boolean suite_setup_input_reported;
    long suite_weapon_units[128];int suite_weapon_ready[128];
    long suite_weapon_definitions[6];int suite_weapon_definitions_ready;
    struct network_test_weapon_trace suite_weapon_trace[128]; } network_test;
static int game_connection(void) { return _game_connection_network_server; }
static boolean game_time_initialized(void) { return TRUE; }
static long game_time_get(void) { return game_tick; }
static int input_replay_sim_isolated(void) { return FALSE; }
static long player_control_take_frame_action_flags(void) { return 0; }
static struct {struct {struct player_action actions[MAXIMUM_LOCAL_PLAYERS];} saved_action_collection;} update_client_globals;
static long local_player_get_player_index(short local) { return local==0?machine_players[0][0]:NONE; }
static void *datum_try_and_get(struct data_array *array,long player_index);
__TAKE_LOCAL_ACTIONS__
static struct update *update_server_get_update(long number) { (void)number; return &current_update; }
static void update_client_handle_server_update(const struct server_update *update, long number) {
    (void)number; observed = *update;
}
static void *datum_try_and_get(struct data_array *array, long player_index) {
    int absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
    if (absolute < 0 || absolute >= array->count ||
        array->data[absolute].identifier != (short)(player_index >> 16)) return NULL;
    return &array->data[absolute];
}
static long *machine_get_player_list(long machine_index) {
    return machine_index >= 0 && machine_index < 128 ? machine_players[machine_index] : NULL;
}
static int valid_real(float value) { return isfinite(value); }
static int arena_result=1;
static int benchmark_mp_arena_tick(long tick) { (void)tick; return arena_result; }
static int arena_enabled;
static int benchmark_mp_arena_enabled(void) { return arena_enabled; }
static int benchmark_mp_arena_action(int actor,long tick,struct player_action *action) { (void)actor;(void)tick;(void)action;return 0; }
void network_test_suite_synthetic_input_tick(long tick);
__WEAPON_FIXTURES__
__BOT_WEAPON__
__ACTION_VALID__
__ADD_TICKS__
__QUEUE_READY__
__HANDLE_INPUT__
__SYNTHETIC_INPUT__
__NEXT_UPDATE__
static short actual_unit_desired_weapon(short requested) {
    struct {struct {short desired_weapon_index;} unit;} body,*unit=&body;
    struct {short weapon_index;} data,*control_data=&data;
    unit->unit.desired_weapon_index=NONE;control_data->weapon_index=requested;
    __UNIT_DESIRED_WEAPON__
    return unit->unit.desired_weapon_index;
}
static long actual_unit_primary_weapon_flags(unsigned long control_flags) {
    struct { struct { unsigned long control_flags; } unit; } body, *unit = &body;
    unit->unit.control_flags = control_flags;
    long flags = 0;
    __UNIT_PRIMARY_GATE__
    return flags;
}
static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "synthetic input line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)
int main(void) {
    int i;
    for(i=0;i<128;i++) {
        test_players[i].unit_index=(1L<<16)|i;
        test_units[i].unit.player_index=test_players[i].unit_index;
        test_units[i].unit.current_weapon_index=NONE;
        unit_delete_all_weapons(test_players[i].unit_index);
    }
    memset(machine_players, 0xff, sizeof(machine_players));
    queue_array.count = 2;
    network_test.suite_network_players = 2;
    machine_players[1][0] = (1L << 16) | 1;
    queues[0].identifier = queues[1].identifier = 1;
    network_test.suite_owned = network_test.players_wanted = 1;
    network_test.mode = _network_test_host;
    /* A spawned player already holds a starting weapon. The ordinary
       delete-all API intentionally preserves it; this workload replaces it too. */
    {
        struct object_placement_data starting={99,test_players[1].unit_index};
        long old=object_new(&starting);
        CHECK(unit_add_weapon_to_inventory(test_players[1].unit_index,old,1));
        test_units[1].unit.current_weapon_index=0;
    }
    game_tick = 10;
    update_server_next_update();
    CHECK(observed.action_count == 2 && observed.actions[1].throttle.j > 0.5f &&
        observed.actions[1].primary_trigger > 0.5f);
    CHECK(TEST_FLAG(observed.actions[1].control_flags, _unit_control_weapon_primary_trigger_bit));
    CHECK(TEST_FLAG(actual_unit_primary_weapon_flags(observed.actions[1].control_flags), _weapon_control_primary_trigger_bit));
    CHECK(test_weapons[1].definition_index==NONE);
    CHECK(test_units[1].unit.weapon_object_indices[0]!=NONE&&test_units[1].unit.weapon_object_indices[1]!=NONE);
    CHECK(test_units[1].unit.weapon_object_indices[2]==NONE&&test_units[1].unit.weapon_object_indices[3]==NONE);
    CHECK(update_server_distributed_inputs[1].valid && update_server_distributed_inputs[1].tick == 10);
    {
        struct player_action duplicate = queues[1].current_action;
        word flags[DISTRIBUTED_INPUT_HISTORY] = { 0 };
        duplicate.throttle.j = -1.0f;
        update_server_handle_distributed_input(machine_players[1][0], 10, &duplicate, flags, DISTRIBUTED_INPUT_HISTORY);
        CHECK(queues[1].current_action.throttle.j > 0.5f);
    }
    game_tick = 60; /* release phase must clear both the analog pull and digital fire flag */
    update_server_next_update();
    CHECK(observed.actions[1].primary_trigger == 0.0f);
    CHECK(!TEST_FLAG(observed.actions[1].control_flags, _unit_control_weapon_primary_trigger_bit));
    CHECK(!actual_unit_primary_weapon_flags(observed.actions[1].control_flags));
    network_test.suite_owned = 0;
    memset(&queues[1].current_action, 0, sizeof(queues[1].current_action));
    game_tick = 61;
    update_server_next_update();
    CHECK(observed.actions[1].throttle.j == 0.0f && observed.actions[1].primary_trigger == 0.0f);
    network_test.suite_owned = 1;
    queues[1].identifier = 0; /* missing datum queue must stay inert */
    game_tick = 62;
    update_server_next_update();
    CHECK(observed.actions[1].throttle.j == 0.0f && observed.actions[1].primary_trigger == 0.0f);
    for (int count = 16; count <= 128; count *= 2) {
        memset(queues, 0, sizeof(queues));
        memset(update_server_distributed_inputs, 0, sizeof(update_server_distributed_inputs));
        queue_array.count = count;
        network_test.suite_network_players = count;
        network_test.mode = _network_test_host;
        for (i = 0; i < count; i++) {
            queues[i].identifier = i + 1;
            machine_players[i][0] = ((long)(i + 1) << 16) | i;
        }
        game_tick = 100;
        update_server_next_update();
        CHECK(observed.action_count == count);
        CHECK(observed.actions[1].desired_weapon_index >= 0);
        {
            short prior[128];int changes[180]={0},transitions=0;long first_definition[128];
            int initial_created=created;
            for(i=1;i<count;i++) {
                short slot=observed.actions[i].desired_weapon_index;
                CHECK(slot<4);
                prior[i]=actual_unit_desired_weapon(slot);
                first_definition[i]=test_weapons[test_units[i].unit.weapon_object_indices[slot]].definition_index;
                CHECK(first_definition[i]>0);
            }
            for(int offset=1;offset<=180;offset++) {
                game_tick=100+offset;update_server_next_update();
                for(i=1;i<count;i++) {
                    short slot=actual_unit_desired_weapon(observed.actions[i].desired_weapon_index);
                    CHECK(slot>=0&&slot<4);
                    if(slot!=prior[i]) {
                        long definition=test_weapons[test_units[i].unit.weapon_object_indices[slot]].definition_index;
                        CHECK(definition!=first_definition[i]);
                        changes[offset-1]++;transitions++;prior[i]=slot;
                    }
                    CHECK((observed.actions[i].primary_trigger>0)==(game_tick%90<24));
                    CHECK(!!actual_unit_primary_weapon_flags(observed.actions[i].control_flags)==(game_tick%90<24));
                }
            }
            CHECK(transitions==count-1&&created==initial_created);
            for(i=0;i<180;i++)CHECK(changes[i]<=1); /* actors change at separate ticks */
        }
        for (i = 1; i < count; i++) {
            CHECK(observed.actions[i].throttle.j > 0.5f);
            CHECK(isfinite(observed.actions[i].desired_facing.yaw));
            CHECK(TEST_FLAG(actual_unit_primary_weapon_flags(observed.actions[i].control_flags), _weapon_control_primary_trigger_bit));
            CHECK(update_server_distributed_inputs[i].tick == game_tick);
        }
        memset(update_server_distributed_inputs,0,sizeof(update_server_distributed_inputs));
        game_tick = 150;
        update_server_next_update();
        for (i = 1; i < count; i++) {
            CHECK(!actual_unit_primary_weapon_flags(observed.actions[i].control_flags));
            CHECK(observed.actions[i].primary_trigger == 0.0f);
        }
    }
    /* Death stays inert, a salted respawn gets a new valid two-weapon loadout,
       and missing or foreign inventory fails rather than requesting a stale slot. */
    {
        long player=machine_players[1][0],unit=test_players[1].unit_index;
        int initial_created=created;
        test_units[1].object.damage_flags=1;
        CHECK(network_test_suite_bot_weapon(1,player,400)==NONE&&created==initial_created);
        test_units[1].object.damage_flags=0;
        test_players[1].unit_index=unit+(1L<<16);
        CHECK(network_test_suite_bot_weapon(1,player,400)>=0&&created==initial_created+2);
        long weapon=test_units[1].unit.weapon_object_indices[1];
        test_weapons[weapon].object.owner_player_index=NONE;
        CHECK(network_test_suite_bot_weapon(1,player,401)==-2);
        test_weapons[weapon].object.owner_player_index=test_units[1].unit.player_index;
        missing_definition=2;network_test.suite_weapon_definitions_ready=0;
        CHECK(network_test_suite_bot_weapon(1,player,402)==-2);
        missing_definition=0;
        test_players[1].unit_index+=(1L<<16);fail_attach=1;
        CHECK(network_test_suite_bot_weapon(1,player,403)==-2);
        for(i=0;i<4;i++)CHECK(test_units[1].unit.weapon_object_indices[i]==NONE);
        fail_attach=0;
        CHECK(network_test_suite_bot_weapon(1,player,404)>=0);
        test_players[1].unit_index+=(1L<<16);move_on_create=1;
        CHECK(network_test_suite_bot_weapon(1,player,405)>=0&&unit_moved);
        test_units[1]=moved_unit;unit_moved=0;
    }
    network_test.mode = _network_test_host;
    queues[127].identifier = 0; /* reject the whole batch before touching earlier valid queues */
    for (i = 1; i < 127; i++) queues[i].current_action.throttle.j = -0.25f;
    network_test_suite_synthetic_input_tick(200);
    CHECK(network_test.mode == _network_test_off);
    for (i = 1; i < 127; i++) CHECK(queues[i].current_action.throttle.j == -0.25f);
    queues[127].identifier = 128;
    network_test_suite_synthetic_input_tick(201);
    for (i = 1; i < 127; i++) CHECK(queues[i].current_action.throttle.j == -0.25f);
    /* Machine IDs and actual salted datum slots need not have the same index. */
    network_test.mode = _network_test_host;
    for (i = 0; i < 128; i++) {
        queues[i].identifier = i + 1;
        memset(&queues[i].current_action, 0, sizeof(queues[i].current_action));
    }
    for (i = 0; i < 128; i++) {
        int slot = (i * 7 + 5) % 128;
        machine_players[i][0] = ((long)(slot + 1) << 16) | slot;
    }
    network_test_suite_synthetic_input_tick(210);
    CHECK(queues[5].current_action.throttle.j == 0.0f); /* the real local datum */
    for (i = 0; i < 128; i++) if (i != 5) CHECK(queues[i].current_action.throttle.j > 0.5f);
    network_test.mode=_network_test_host;arena_result=0;last_log[0]=0;
    arena_enabled=1;
    memset(update_server_distributed_inputs,0,sizeof(update_server_distributed_inputs));
    for(i=0;i<128;i++)memset(&queues[i].current_action,0,sizeof(queues[i].current_action));
    network_test_suite_synthetic_input_tick(1);
    CHECK(network_test.mode==_network_test_host&&!last_log[0]);
    /* Pending units must receive ordinary walking through the real host queues,
       freeing occupied stock starts before the all-unit arena readiness gate. */
    for(i=1;i<128;i++) {
        int slot=DATUM_INDEX_TO_ABSOLUTE_INDEX(machine_players[i][0]);
        CHECK(update_server_distributed_inputs[slot].valid);
        CHECK(queues[slot].current_action.throttle.j>0.5f);
        CHECK(!queues[slot].current_action.primary_trigger);
        CHECK(!actual_unit_primary_weapon_flags(queues[slot].current_action.control_flags));
    }
    /* An asynchronous missing queue still waits atomically during setup. */
    queues[DATUM_INDEX_TO_ABSOLUTE_INDEX(machine_players[127][0])].identifier=0;
    for(i=0;i<128;i++)queues[i].current_action.throttle.j=-0.25f;
    network_test_suite_synthetic_input_tick(2);
    CHECK(network_test.mode==_network_test_host);
    for(i=0;i<128;i++)CHECK(queues[i].current_action.throttle.j==-0.25f);
    queues[DATUM_INDEX_TO_ABSOLUTE_INDEX(machine_players[127][0])].identifier=
        (short)(machine_players[127][0]>>16);
    CHECK(network_test.suite_setup_input_ticks==1);
    /* Pending readiness never excuses malformed machine membership. */
    machine_players[127][1]=machine_players[126][0];
    network_test_suite_synthetic_input_tick(3);
    CHECK(network_test.mode==_network_test_off&&strstr(last_log,"second"));
    machine_players[127][1]=NONE;
    network_test.mode=_network_test_host;
    {
        long saved=machine_players[127][0];
        machine_players[127][0]=machine_players[126][0];
        network_test_suite_synthetic_input_tick(4);
        CHECK(network_test.mode==_network_test_off&&strstr(last_log,"duplicate machine 126"));
        machine_players[127][0]=saved;
    }
    network_test.mode=_network_test_host;
    arena_enabled=0;
    arena_result=-1;network_test_suite_synthetic_input_tick(1);
    CHECK(network_test.mode==_network_test_off&&strstr(last_log,"arena preparation failed"));
    network_test.mode=_network_test_host;arena_result=1;
    queues[DATUM_INDEX_TO_ABSOLUTE_INDEX(machine_players[127][0])].identifier=0;
    network_test_suite_synthetic_input_tick(1);
    CHECK(network_test.mode==_network_test_off&&strstr(last_log,"machine 127")&&strstr(last_log,"queue ready 0"));
    /* In a fresh game, the actual local-action copy survives the remote hook. */
    memset(update_server_distributed_inputs,0,sizeof(update_server_distributed_inputs));
    for(i=0;i<128;i++)queues[i].identifier=i+1;
    network_test.mode=_network_test_host;arena_result=1;
    update_client_globals.saved_action_collection.actions[0].throttle.j=0.2945f;
    game_tick=220;update_server_next_update();
    CHECK(observed.actions[5].throttle.j==0.2945f);
    return failures != 0;
}
'''


def test_suite_input_reaches_host_tick_snapshot() -> None:
    compiler = shutil.which("clang") or shutil.which("cc")
    if compiler is None:
        raise SkipTest("A C compiler is required for the synthetic input probe")
    synthetic = ""
    if "void network_test_suite_synthetic_input_tick(" in NETWORK_TEST:
        synthetic = function(NETWORK_TEST, "void network_test_suite_synthetic_input_tick(")
    source = HARNESS
    def control_enum(header, first):
        name = header.index(first)
        start = header.rfind("enum", 0, name)
        end = header.index("};", name) + 2
        return header[start:end]

    for key, value in {
        "__TRACE_TYPE__": NETWORK_TEST[NETWORK_TEST.index("struct network_test_weapon_trace\n{"):NETWORK_TEST.index("};", NETWORK_TEST.index("struct network_test_weapon_trace\n{"))+2],
        "__WEAPON_FIXTURES__": WEAPON_FIXTURES.replace("__ITEM_INVENTORY__", function(ITEMS, "void item_in_unit_inventory(")).replace("__IMMEDIATE_DELETE__", function(OBJECTS, "void object_delete_immediately(")),
        "__BOT_WEAPON__": function(NETWORK_TEST, "static void network_test_suite_clear_bot_weapons(") + function(NETWORK_TEST, "static void network_test_suite_observe_weapon(") + function(NETWORK_TEST, "static short network_test_suite_bot_weapon(") if "static short network_test_suite_bot_weapon(" in NETWORK_TEST else "",
        "__UNIT_CONTROL_ENUM__": control_enum(UNIT_HEADER, "_unit_control_crouch_modifier_bit"),
        "__WEAPON_CONTROL_ENUM__": control_enum(WEAPON_HEADER, "_weapon_control_integrated_light_bit"),
        "__UNIT_DESIRED_WEAPON__": function(function(UNITS, "void unit_control("), "if (control_data->weapon_index!=NONE)"),
        "__UNIT_PRIMARY_GATE__": function(UNITS, "if (TEST_FLAG(unit->unit.control_flags, _unit_control_weapon_primary_trigger_bit))"),
        "__ACTION_VALID__": function(QUEUES, "static boolean distributed_action_valid("),
        "__ADD_TICKS__": function(QUEUES, "static boolean distributed_add_new_ticks("),
        "__QUEUE_READY__": function(QUEUES, "boolean update_server_player_queue_ready("),
        "__HANDLE_INPUT__": function(QUEUES, "void update_server_handle_distributed_input("),
        "__SYNTHETIC_INPUT__": synthetic,
        "__NEXT_UPDATE__": function(QUEUES, "void update_server_next_update("),
        "__TAKE_LOCAL_ACTIONS__": function(QUEUES, "static void update_server_take_local_actions(\n\tvoid)\n{"),
    }.items():
        source = source.replace(key, value)
    with tempfile.TemporaryDirectory(prefix="mp-synthetic-input-") as temporary:
        path = Path(temporary) / "input.c"
        binary = Path(temporary) / "input"
        path.write_text(source)
        subprocess.run([compiler, "-O2", "-std=gnu89", "-fms-extensions", "-Wno-c99-extensions", "-Wall", str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    test_suite_input_reaches_host_tick_snapshot()
