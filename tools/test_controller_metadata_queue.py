"""Exercise production action dequeue and network egress with independent metadata."""
from pathlib import Path
import subprocess
import shutil
from unittest import SkipTest

from test_mp_synthetic_input import function

ROOT = Path(__file__).resolve().parents[1]


def test_local_tutorial_metadata_is_consumed_before_simulation_and_never_sent(tmp_path):
    compiler = shutil.which('clang') or shutil.which('cc')
    if not compiler:
        raise SkipTest('A C compiler is required for the action metadata probe')
    queues = (ROOT / 'source/game/player_queues_new.c').read_text()
    network = (ROOT / 'port/linux/game/network_distributed.c').read_text()
    source = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#define _pi 3.14159265358979323846f
#define MIN(a,b) ((a)<(b)?(a):(b))
#define TEST_FLAG(flags,bit) ((flags)&(1u<<(bit)))
#define SET_FLAG(flags,bit,on) do {if(on)(flags)|=1u<<(bit);else(flags)&=~(1u<<(bit));}while(0)
#define TRUE 1
#define FALSE 0
#define NONE -1
#define MAXIMUM_LOCAL_PLAYERS 4
#define MAXIMUM_NUMBER_OF_PLAYERS 128
#define MAXIMUM_WEAPONS_PER_UNIT 4
#define NUMBER_OF_UNIT_GRENADE_TYPES 2
#define DISTRIBUTED_INPUT_HISTORY 4
#define LATCHED_CONTROL_FLAGS 0
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(x) ((x)&0xffff)
#define csmemcpy memcpy
#define csmemmove memmove
#define csmemset memset
#define match_assert(file,line,value) assert(value)
#define FLAG(bit) (1UL<<(bit))
__CONTROL_ENUM__
__ACTION_ONLY_BIT__
_Static_assert(NUMBER_OF_UNIT_CONTROL_FLAGS<=16 && UNIT_CONTROL_PORT_ACTION_ONLY_BIT<16,"tutorial metadata must not overlap controls");
#define MAX(a,b) ((a)>(b)?(a):(b))
#define PIN(v,lo,hi) MIN(hi,MAX(lo,v))
#define MAXIMUM_TRACKED_PLAYERS 128
#define HALO_PORT_MAXIMUM_NETWORK_MACHINES 32
#define MAXIMUM_ROUND_TRIP_TICKS 300
#define DATUM_INDEX_NEW(index,id) (((id)<<16)|(index))
typedef int boolean;
typedef float real;
typedef unsigned char byte;
typedef unsigned short word;
enum {_game_connection_local,_game_connection_network_client,_game_connection_network_server};
struct angles {float yaw,pitch;};
struct throttle {float i,j;};
struct player_action {uint32_t control_flags;struct angles desired_facing;struct throttle throttle;float primary_trigger;short desired_weapon_index,desired_grenade_index,desired_zoom_level,pad;};
_Static_assert(sizeof(struct player_action)==32,"actual action layout");
struct update_client_queue_datum {uint32_t control_flags,latched_control_flags;struct angles desired_facing;struct throttle throttle;float primary_trigger;short desired_weapon_index,desired_grenade_index,desired_zoom_level,action_pad;};
static struct update_client_queue_datum queue_entries[128];
static struct {int count;void *data;} queue_array={2,queue_entries};
static struct {int initialized,current_local_player;long next_update_number_to_dequeue,latest_update_number_received;typeof(queue_array)*queues;struct {struct player_action actions[4];} saved_action_collection;} update_client_globals={1,0,0,0,&queue_array};
struct update {struct {int action_count;struct player_action actions[128];} update;};
static struct update update;
static struct {int valid;long tick;struct player_action action;unsigned short control_flags[4];} update_client_local_inputs[4];
static struct {int valid;struct player_action action;uint32_t pending_control_flags;} update_client_relayed_actions[128];
static unsigned long update_client_pending_control_flags[4];
static float update_client_pending_primary_triggers[4];
static int connection,isolated=1,noted;
static long game_time_get(void){return 42;}
static int game_connection(void){return connection;}
static long local_player_get_player_index(short local){return local==0?1:NONE;}
static struct update *update_client_get_update(long tick){(void)tick;return &update;}
static int input_replay_sim_isolated(void){return isolated;}
static void input_replay_tick_action(void *a){(void)a;}
static void player_control_apply_replay(short local){(void)local;}
static void player_control_action_test_note_tick(unsigned short flags){noted=flags;}
__QUEUE__
__LOCAL__
__DISTRIBUTED_DEQUEUE__
__DEQUEUE__
__EGRESS__
struct distributed_message_header {uint32_t data[4];};
__WIRE_INPUT__
static long distributed_host_time;
static byte distributed_player_to_byte(long index){return (byte)index;}
static int global_structure_bsp_index_get(void){return 0;}
enum {_distributed_message_player_inputs,_distributed_to_host};
static struct distributed_player_input sent;
static void distributed_send(const void *message,int kind,short count,word size,int destination){
 (void)kind;(void)size;(void)destination;assert(count==1);
 memcpy(&sent,(const char *)message+sizeof(struct distributed_message_header),sizeof(sent));
}
__SEND__
__RELAY_STRUCT__
__PUT__
__TAKE__
__ANGLE_PACK__
__RELAY_FROM__
__RELAY_WRITE__
__RELAY_READ__
__ANGLE_UNPACK__
struct player_datum {long unit_index;unsigned short identifier;};
static struct player_datum remote_player={17,2};
static struct {int valid;float average,deviation;} distributed_round_trips[32];
static struct player_action received_host,received_peer;
static long received_player;
static int distributed_machine_has_player(long machine,short player){return machine==5&&player==1;}
static struct player_datum *distributed_player(short player){return player==1?&remote_player:NULL;}
static int network_coop_active(void){return 0;}
static void distributed_note_input_actions(short player,const struct player_action *action){(void)player;(void)action;}
static void network_coop_note_player_structure_bsp(short player,int bsp){(void)player;(void)bsp;}
static void update_server_handle_distributed_input(long player,long tick,const struct player_action *action,
 const unsigned short *flags,short count){(void)tick;(void)flags;(void)count;received_player=player;received_host=*action;}
static void update_client_handle_relayed_action(short player,long tick,const struct player_action *action,
 const unsigned short *flags,short count){(void)tick;(void)flags;(void)count;assert(player==1);received_peer=*action;}
__HOST_RECEIVE__
__PEER_RECEIVE__
int main(void){
 struct player_action actions[128]={0};
 update.update.action_count=2;
 update.update.actions[0]=(struct player_action){.control_flags=0x03428001,.pad=(short)0xc001};
 update.update.actions[1]=(struct player_action){.control_flags=0x00000800,.pad=0};
 assert(update_client_dequeue(actions));
 assert(noted==0x0342 && actions[0].control_flags==0x8001 && (unsigned short)actions[0].pad==0xc001);
 assert(actions[1].control_flags==0x800 && actions[1].pad==0);
 /* Network local player zero is absolute player one. Tutorial recording remains
    requested, but the distributed path must neither sample nor transmit it. */
 connection=_game_connection_network_client;noted=0;queue_entries[0].latched_control_flags=queue_entries[1].latched_control_flags=0;
 struct player_action live={.control_flags=0x8001,.desired_facing={1.0f,0.2f},.throttle={0.5f,-0.25f},.pad=0};
 update_client_queue(&live);
 assert(update_client_dequeue(actions));
 assert(noted==0 && actions[1].control_flags==0x8001 && actions[1].desired_facing.yaw==1.0f);
 distributed_client_send_inputs();
 assert(sent.player_index==1 && sent.action.control_flags==0x8001 && sent.action.pad==0);
 assert(sent.action.throttle.i==0.5f && sent.action.throttle.j==-0.25f);
 assert(sent.control_flags[0]==0x8001);
 distributed_handle_inputs(5,&sent,1);
 assert(received_player==DATUM_INDEX_NEW(1,2));
 assert(received_host.desired_facing.yaw==1.0f && received_host.desired_facing.pitch==.2f);
 assert(received_host.pad==0 && received_host.control_flags==0x8001);
 struct distributed_relayed_action packed,decoded;byte bytes[32],before[32];
 const struct player_action *recent[4]={actions,actions,actions,actions};short counts[4]={2,2,2,2};
 distributed_relayed_action_from(1,recent,counts,&packed);
 word size=distributed_relayed_action_write(&packed,bytes);assert(size==14);
 assert(distributed_relayed_action_read(bytes,bytes+size,&decoded)==size);
 assert(decoded.player_index==1 && decoded.control_flags[0]==0x8001);
 assert(decoded.throttle_i==64 && decoded.throttle_j==-32);
 byte peer_bytes[64];long update_number=42;
 memcpy(peer_bytes,&update_number,sizeof(update_number));memcpy(peer_bytes+sizeof(update_number),bytes,size);
 distributed_handle_actions(peer_bytes,peer_bytes+sizeof(update_number)+size,1);
 assert(fabsf(received_peer.desired_facing.yaw-1.0f)<.0001f && fabsf(received_peer.desired_facing.pitch-.2f)<.0001f);
 assert(received_peer.pad==0 && received_peer.control_flags==0x8001);
 assert(fabsf(received_peer.throttle.i-.5f)<.005f&&fabsf(received_peer.throttle.j+.25f)<.005f);
 memcpy(before,bytes,size);actions[1].pad=(short)0xc001;actions[1].control_flags|=0x12340000u;
 distributed_relayed_action_from(1,recent,counts,&packed);
 assert(distributed_relayed_action_write(&packed,bytes)==size && !memcmp(before,bytes,size));
 return 0;
}
'''
    unit_header = (ROOT / 'source/units/units.h').read_text()
    control_start = unit_header.rfind('enum', 0, unit_header.index('_unit_control_crouch_modifier_bit'))
    control_end = unit_header.index('};', control_start) + 2
    action_only = next(line for line in unit_header.splitlines() if line.startswith('#define UNIT_CONTROL_PORT_ACTION_ONLY_BIT '))
    for marker, body in {
        '__CONTROL_ENUM__': unit_header[control_start:control_end],
        '__ACTION_ONLY_BIT__': action_only,
        '__QUEUE__': function(queues, 'void update_client_queue(\n'),
        '__LOCAL__': function(queues, 'static short update_client_local_player_index('),
        '__DISTRIBUTED_DEQUEUE__': function(queues, 'static boolean update_client_dequeue_distributed('),
        '__DEQUEUE__': function(queues, 'boolean update_client_dequeue(\n'),
        '__EGRESS__': function(queues, 'boolean update_client_distributed_input('),
        '__WIRE_INPUT__': function(network, 'struct distributed_player_input\n') + ';',
        '__SEND__': function(network, 'static void distributed_client_send_inputs(\n'),
        '__RELAY_STRUCT__': function(network, 'struct distributed_relayed_action\n')+';',
        '__PUT__': function(network, 'static byte *distributed_put('),
        '__TAKE__': function(network, 'static boolean distributed_take('),
        '__ANGLE_PACK__': function(network, 'short distributed_angle_pack('),
        '__RELAY_FROM__': function(network, 'static boolean distributed_relayed_action_from('),
        '__RELAY_WRITE__': function(network, 'static word distributed_relayed_action_write('),
        '__RELAY_READ__': function(network, 'static word distributed_relayed_action_read('),
        '__ANGLE_UNPACK__': function(network, 'real distributed_angle_unpack('),
        '__HOST_RECEIVE__': function(network, 'static void distributed_handle_inputs('),
        '__PEER_RECEIVE__': function(network, 'static void distributed_handle_actions('),
    }.items():
        source = source.replace(marker, body)
    path = tmp_path / 'metadata.c'
    path.write_text(source)
    binary = tmp_path / 'metadata'
    subprocess.run([compiler,'-O2','-fsanitize=address,undefined',str(path),'-lm','-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
