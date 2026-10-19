"""Exercise real producer attribution, weapon transitions, and respawn guards."""
from pathlib import Path
import shutil
import subprocess
from unittest import SkipTest
from test_mp_synthetic_input import function
ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'port/linux/game/network_test.c').read_text()
HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#define NONE -1
#define TRUE 1
#define FLAG(n) (1UL<<(n))
#define TEST_FLAG(f,n) (((f)&FLAG(n))!=0)
#define SET_FLAG(f,n,b) do {if(b)(f)|=FLAG(n);else(f)&=~FLAG(n);}while(0)
#define MAXIMUM_WEAPONS_PER_UNIT 4
#define BENCHMARK_MP_MAXIMUM_PLAYERS 128
#define _object_dead_bit 0
#define _item_attached_to_unit_bit 0
#define _object_type_projectile 5
#define _network_test_host 1
#define _game_connection_network_server 2
__TRACE_TYPE__
static struct {int suite_owned,mode,suite_network_players;struct network_test_weapon_trace suite_weapon_trace[128];} network_test={.suite_owned=1,.mode=1,.suite_network_players=2};
struct player_datum {long unit_index;int local_player_index;struct {int machine_index;} network_player_data;};
struct unit_datum {struct {unsigned long damage_flags;} object;struct {int current_weapon_index;long weapon_object_indices[4],player_index;} unit;};
struct weapon_datum {long definition_index;struct {long owner_player_index,parent_object_index;} object;struct {unsigned long flags;} item;};
struct object_datum {struct {int type;long owner_object_index;} object;};
static struct player_datum player={1001,NONE,{1}};
static struct unit_datum unit={.object={.damage_flags=0},.unit={.current_weapon_index=0,.weapon_object_indices={11,12,NONE,NONE},.player_index=65537}};
static struct weapon_datum weapons[64];static struct object_datum projectiles[64];
static long players[4]={65537,NONE,NONE,NONE},tick=10;
static int connection=2,logs;static char last_log[2048];
static int game_connection(void){return connection;}
static long game_time_get(void){return tick;}
static long player_index_from_unit_index(long index){return index==player.unit_index?65537:NONE;}
static struct player_datum *player_try_and_get(long index){return index==65537?&player:NULL;}
static struct unit_datum *unit_try_and_get(long index){return index==player.unit_index?&unit:NULL;}
static struct weapon_datum *weapon_try_and_get(long index){return index==11||index==12||index==13?&weapons[index]:NULL;}
static struct object_datum *object_try_and_get(long index){return index==31||index==32?&projectiles[index]:NULL;}
static long *machine_get_player_list(int actor){return actor==1?players:NULL;}
static void platform_log(const char *fmt,...){va_list a;va_start(a,fmt);vsnprintf(last_log,sizeof(last_log),fmt,a);va_end(a);logs++;}
__OBSERVE__
__PRODUCER__
int main(void){
 struct network_test_weapon_trace *trace=&network_test.suite_weapon_trace[1];
 weapons[11]=(struct weapon_datum){101,{65537,1001},{1}};weapons[12]=(struct weapon_datum){202,{65537,NONE},{1}};weapons[13]=(struct weapon_datum){303,{65537,NONE},{1}};
 projectiles[31]=(struct object_datum){{5,1001}};projectiles[32]=projectiles[31];
 network_test_suite_observe_weapon(1,1001);assert(trace->ready&&trace->generation==0&&trace->weapon_index==11);
 network_test_suite_weapon_projectile_created(1001,11,31);assert(trace->projectiles==1&&!logs);
 unit.unit.current_weapon_index=1;tick=20;network_test_suite_observe_weapon(1,1001);
 assert(trace->generation==1&&strstr(last_log,"from_weapon b")&&strstr(last_log,"to_weapon c"));
 /* An old charged projectile cannot be attributed to the newly held object. */
 network_test_suite_weapon_projectile_created(1001,11,31);assert(strstr(last_log,"firing_slot 0 weapon b definition 65")&&strstr(last_log,"matched 0"));
 network_test_suite_weapon_projectile_created(1001,12,32);assert(strstr(last_log,"firing_slot 1 weapon c definition ca")&&strstr(last_log,"projectile 20")&&strstr(last_log,"matched 1"));
 int before=logs;network_test_suite_weapon_projectile_created(1001,12,32);assert(logs==before&&trace->projectiles==4);
 uint32_t counted=trace->projectiles;
 network_test_suite_weapon_projectile_created(NONE,12,32);network_test_suite_weapon_projectile_created(1001,NONE,32);network_test_suite_weapon_projectile_created(1001,12,NONE);
 network_test_suite_weapon_projectile_created(1001,13,32);projectiles[32].object.owner_object_index=1002;network_test_suite_weapon_projectile_created(1001,12,32);projectiles[32].object.owner_object_index=1001;
 projectiles[32].object.type=1;network_test_suite_weapon_projectile_created(1001,12,32);projectiles[32].object.type=5;
 weapons[12].object.owner_player_index=65538;network_test_suite_weapon_projectile_created(1001,12,32);weapons[12].object.owner_player_index=65537;
 weapons[12].object.parent_object_index=999;network_test_suite_weapon_projectile_created(1001,12,32);weapons[12].object.parent_object_index=NONE;
 unit.object.damage_flags=1;network_test_suite_weapon_projectile_created(1001,12,32);unit.object.damage_flags=0;
 player.local_player_index=0;network_test_suite_weapon_projectile_created(1001,12,32);player.local_player_index=NONE;
 players[1]=123;network_test_suite_weapon_projectile_created(1001,12,32);players[1]=NONE;
 connection=0;network_test_suite_weapon_projectile_created(1001,12,32);connection=2;
 network_test.suite_owned=0;network_test_suite_weapon_projectile_created(1001,12,32);network_test.suite_owned=1;
 assert(trace->projectiles==counted&&logs==before);
 /* A salted respawn is a fresh baseline, even with the same slot/definitions. */
 player.unit_index=1002;projectiles[32].object.owner_object_index=1002;network_test_suite_weapon_projectile_created(1002,12,32);assert(trace->unit_index==1002&&!trace->generation&&trace->projectiles==1&&logs==before);
 unit.unit.current_weapon_index=NONE;network_test_suite_observe_weapon(1,1002);assert(!trace->generation&&trace->weapon_index==12);
 unit.unit.current_weapon_index=0;weapons[11].object.parent_object_index=1002;tick=30;network_test_suite_observe_weapon(1,1002);assert(trace->generation==1&&trace->weapon_index==11);
 return 0;
}
'''
def test_actual_firing_objects_and_salted_switch_generations(tmp_path):
    cc=shutil.which('clang') or shutil.which('cc')
    if not cc:
        raise SkipTest('A C compiler is required for the actual weapon trace probe')
    start=SOURCE.index('struct network_test_weapon_trace\n{')
    trace=SOURCE[start:SOURCE.index('};',start)+2]
    code=HARNESS.replace('__TRACE_TYPE__',trace).replace('__OBSERVE__',function(SOURCE,'static void network_test_suite_observe_weapon(')).replace('__PRODUCER__',function(SOURCE,'void network_test_suite_weapon_projectile_created('))
    source,binary=tmp_path/'trace.c',tmp_path/'trace'
    source.write_text(code)
    subprocess.run([cc,'-std=gnu11','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
