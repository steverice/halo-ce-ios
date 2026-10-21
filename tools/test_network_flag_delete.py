"""Exercise host-directed objective deletion without game data or an app build."""
from test_network_controller_aim import ROOT, run_probe
from test_mp_synthetic_input import function


def test_host_delete_scope_assertions_and_objective_goals(tmp_path):
    network = (ROOT / "port/linux/game/network_objects.c").read_text()
    objects = (ROOT / "source/objects/objects.c").read_text()
    weapons = (ROOT / "source/items/weapons.c").read_text()
    engine = (ROOT / "source/game/game_engine.c").read_text()
    units = (ROOT / "source/units/units.c").read_text()
    assert "boolean network_objects_deleting_host_object(" in network, "missing host-delete scope"
    program = r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
typedef int boolean;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define MAXIMUM_WEAPONS_PER_UNIT 2
#define MAXIMUM_TRACKED_OBJECTS 8
#define MAXIMUM_ODDBALLS 16
#define NUMBEROF(a) (sizeof(a)/sizeof((a)[0]))
#define VALID_INDEX(i,n) ((i)>=0 && (i)<(n))
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(i) ((i)&65535)
#define TEST_FLAG(f,b) ((f)&(1u<<(b)))
#define SET_FLAG(f,b,v) ((f)=((f)&~(1u<<(b)))|((!!(v))<<(b)))
enum { _object_type_weapon, _object_type_unit, _object_type_equipment };
enum { _object_mask_item=5, _object_mask_unit=2, _object_mask_weapon=1 };
enum { _object_header_being_deleted_bit, _player_action_result_reload,
       _object_change_create, _object_change_delete };
enum { game_engine_ctf=1, game_engine_oddball, game_engine_slayer };
struct object_fields { int type; long first_child_object_index, next_object_index, owner_team_index; };
struct object_datum { struct object_fields object; };
struct weapon_datum { struct object_fields object; };
struct object_header_datum { unsigned flags; };
struct unit_datum { struct object_fields object; struct { long weapon_object_indices[2], player_index; short parent_seat_index, current_weapon_index, desired_weapon_index; } unit; };
struct player_datum { long action_object_index, unit_index; int action_result; };
struct data_iterator { int done; };
struct object_iterator { long index; int done; };
struct distributed_object_change { long object_index; int change; };
static struct weapon_datum weapon;
static struct object_header_datum object_header;
static struct player_datum player;
static struct unit_datum unit;
static int client=1, running=1, flag=1, failures, released, deleted;
static boolean objects_client_deleting, objects_client_host_deleting;
static long objects_client_has[8];
static struct { int deletes; } objects_statistics;
static struct { int game_engine_index; } global_variant;
static void *game_engine=(void*)1, *player_data;
static int goals[32];
static int network_game_distributed_client(void) { return client; }
static int game_engine_running(void) { return running; }
static int weapon_is_flag(long index) { assert(index==1 || index==2); return flag; }
static struct object_datum *object_try_and_get(long index) { return index==1 || index==2 ? (void*)&weapon : NULL; }
static struct object_datum *object_get(long index) { assert(index==1 || index==2); return (void*)&weapon; }
static struct weapon_datum *weapon_get(long index) { assert(index==1 || index==2); return &weapon; }
static struct object_header_datum *object_header_get(long index) { assert(index==1 || index==2); return &object_header; }
#define match_assert(file,line,value) do { if (!(value)) failures++; } while(0)
static void object_set_visibility(long index,int visible) { (void)index; (void)visible; }
static void object_name_list_delete(long index) { (void)index; }
static void game_engine_clear_goal_position(short index) { assert(VALID_INDEX(index,32)); goals[index]=0; }
static struct unit_datum *unit_get(long index) { (void)index; return &unit; }
static int network_objects_client_has(long index) { (void)index; return 0; }
static void unit_network_drop_weapon(long index,short slot) { (void)index; (void)slot; }
static void unit_exit_seat_end(long index) { (void)index; }
static struct player_datum *player_try_and_get(long index) { (void)index; return &player; }
static struct player_datum *player_get(long index) { (void)index; return &player; }
static void network_player_detach_unit(long index) { (void)index; }
static void data_iterator_new(struct data_iterator *i,void *data) { (void)data; i->done=0; }
static void *data_iterator_next(struct data_iterator *i) { return i->done++ ? NULL : &player; }
static short unit_weapon_next_index(long index,long previous,int direction) {
 assert(!objects_client_deleting && !objects_client_host_deleting);
 assert(unit.unit.weapon_object_indices[0]==NONE && unit.unit.weapon_object_indices[1]==7);
 released++; return 1;
}
static void object_iterator_new(struct object_iterator *i,int mask,int flags) { i->done=0; i->index=5; }
static void *object_iterator_next(struct object_iterator *i) { return i->done++ ? NULL : &unit; }
static int distributed_object_index_valid(long index) { return index>=0 && index<8; }
static void distributed_client_create(const struct distributed_object_change *change) { (void)change; }
'''
    program += function(units, "void unit_network_forget_weapon(\n")
    program += function(network, "static void distributed_client_release_item(")
    program += function(network, "boolean network_objects_deleting_host_object(")
    program += function(engine, "void game_engine_objective_weapon_deleted(")
    program += function(objects, "static void object_delete_initial_recursive(")
    program += function(weapons, "void weapon_delete(")
    program += r'''
static void object_delete_immediately(long index) {
 assert(released && player.action_object_index==NONE);
 assert(unit.unit.weapon_object_indices[0]==NONE && unit.unit.current_weapon_index==NONE && unit.unit.desired_weapon_index==1);
 assert(objects_client_deleting);
 object_delete_initial_recursive(index,FALSE);
 weapon_delete(index);
 deleted++;
}
'''
    program += function(network, "static void distributed_client_delete(")
    program += function(network, "void network_objects_handle_changes(")
    program += r'''
static void reset(void) {
 failures=released=deleted=0; flag=running=client=1; game_engine=(void*)1;
 weapon.object=(struct object_fields){_object_type_weapon,NONE,NONE,0};
 player.action_object_index=1;
 unit.unit.weapon_object_indices[0]=1; unit.unit.weapon_object_indices[1]=7;
 unit.unit.current_weapon_index=unit.unit.desired_weapon_index=0;
 for(int i=0;i<32;i++) goals[i]=1;
}
int main(void) {
 struct distributed_object_change change={1,_object_change_delete};
 for(int mode=game_engine_ctf;mode<=game_engine_slayer;mode++) {
  for(int team=0;team<2;team++) {
   reset(); global_variant.game_engine_index=mode; weapon.object.owner_team_index=team;
   objects_client_has[1]=1; network_objects_handle_changes(&change,1);
   assert(deleted==1 && released==1 && failures==0 && objects_client_has[1]==NONE);
   assert(!objects_client_deleting && !network_objects_deleting_host_object());
   for(int i=0;i<32;i++) {
    int cleared=(mode==game_engine_ctf && (i==team || i==team+2)) ||
                (mode==game_engine_oddball && i==team);
    assert(goals[i]==!cleared);
   }
  }
 }
 /* Local cleanup and ordinary deletes retain both assertions, including on a client. */
 for(client=0;client<2;client++) {
  failures=0; object_delete_initial_recursive(1,FALSE); weapon_delete(1); assert(failures==2);
 }
 reset(); distributed_client_delete(1,FALSE); assert(failures==2 && goals[0] && goals[2]);
 assert(!network_objects_deleting_host_object());
 /* A nonclient cannot gain the exception even if this internal caller is misused. */
 reset(); client=0; distributed_client_delete(1,TRUE); assert(failures==2);
 reset(); running=0; distributed_client_delete(1,FALSE); assert(failures==0);
 reset(); flag=0; distributed_client_delete(1,TRUE); assert(failures==0 && goals[0] && goals[2]);
 reset(); distributed_client_delete(NONE,TRUE); assert(!deleted && !network_objects_deleting_host_object());
 reset(); global_variant.game_engine_index=game_engine_oddball; weapon.object.owner_team_index=15;
 distributed_client_delete(1,TRUE); assert(!goals[15] && goals[14] && goals[16]);
 reset(); objects_client_has[1]=2; network_objects_handle_changes(&change,1); assert(!deleted);
 reset(); global_variant.game_engine_index=game_engine_ctf; weapon.object.owner_team_index=NONE;
 distributed_client_delete(1,TRUE); for(int i=0;i<32;i++) assert(goals[i]);
}
'''
    run_probe(tmp_path, "flag-delete", program)


def test_only_host_delete_instruction_enables_scope():
    """Cleanup callers must not opt into the host-instruction exception."""
    import re

    source = (ROOT / "port/linux/game/network_objects.c").read_text()
    calls = re.findall(r"distributed_client_delete\(([^;{}]+)\);", source)
    enabled = [call for call in calls if call.strip().endswith(", TRUE")]
    assert enabled == ["change->object_index, TRUE"]
    scope = function(source, "static void distributed_client_delete(")
    assignments = re.findall(r"objects_client_host_deleting\s*=\s*(\w+);", source)
    assert assignments.count("host_delete") == 1
    assert "objects_client_host_deleting = host_delete;" in scope
    assert all(value in {"host_delete", "FALSE"} for value in assignments)
