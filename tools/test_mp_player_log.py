"""Check complete production per-actor telemetry for a full network roster."""
from pathlib import Path
import shutil
import subprocess
from unittest import SkipTest
from test_mp_synthetic_roster import function

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "port/linux/game/network_test.c").read_text()
HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <math.h>
#define NONE -1
#define HALO_PORT_MAXIMUM_NETWORK_PLAYERS 128
#define TICKS_PER_SECOND 30
#define MAXIMUM_WEAPONS_PER_UNIT 4
#define _object_mask_weapon 1
#define _object_mask_equipment 2
#define _object_connected_to_map_bit 0
#define _item_attached_to_unit_bit 0
#define _unit_active_camouflaged_bit 0
#define _unit_control_weapon_primary_trigger_bit 11
#define _get_score_individual 0
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(i) ((i) & 0xffff)
#define csmemset memset
#define csmemcmp memcmp
#define csmemcpy memcpy
#define TEST_FLAG(value, bit) ((value) & (1UL << (bit)))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define PIN(a,b,c) ((a)<(b)?(b):(a)>(c)?(c):(a))
struct point { float x,y,z; };
struct vector { float i,j,k; };
struct object_datum { struct { int parent_object_index; struct point position; float body_vitality,shield_vitality;
    struct vector forward; unsigned long flags; } object; };
struct unit_datum { struct { unsigned long flags,control_flags; float primary_trigger;
    int grenade_counts[2],current_weapon_index; long weapon_object_indices[4];
    struct vector aiming_vector,looking_vector,throttle;
    struct { int aiming_screen_index,looking_screen_index,state; } animation; } unit; };
struct player_datum { wchar_t name[12]; long unit_index,team_index,respawn_timer; int local_player_index;
    struct { int kills[4],deaths,friendly_fire_kills; } statistics;
    struct { int machine_index; } network_player_data; };
struct item_datum { struct { long parent_object_index; unsigned long flags; } object;
    struct { unsigned long flags; } item; };
struct weapon_datum { long definition_index; struct { struct { int rounds_total,rounds_loaded; } magazines[1]; } weapon; };
struct observer_result { struct point position; };
struct data_iterator { long datum_index; int next; };
struct object_iterator { long index; };
static struct player_datum players[32];
static struct object_datum objects[32];
static struct unit_datum units[32];
static struct weapon_datum weapon;
static struct { void *data; } data, *player_data = &data;
static struct { int suite_owned,suite_network_players; } network_test = {1,32};
static int actor_lines,summary_lines,seen[32],bad;
static unsigned long network_test_suite_repositions(int actor) { (void)actor; return 1; }
static unsigned int network_test_suite_energy_projectiles(int actor) { return (unsigned int)actor; }
static void network_test_suite_scripted_counts(unsigned long *hits,unsigned long *kills) { *hits=3;*kills=1; }
static long game_time_get(void) { return 90; }
static void data_iterator_new(struct data_iterator *iterator, void *unused) { (void)unused; iterator->next=0; }
static void *data_iterator_next(struct data_iterator *iterator) {
    if (iterator->next >= 32) return NULL;
    iterator->datum_index=iterator->next; return &players[iterator->next++];
}
static struct object_datum *object_get(long i) { return &objects[i]; }
static struct unit_datum *unit_get(long i) { return &units[i]; }
static struct weapon_datum *weapon_get(long i) { (void)i; return &weapon; }
static void object_iterator_new(struct object_iterator *it,int a,int b) { (void)it;(void)a;(void)b; }
static void *object_iterator_next(struct object_iterator *it) { (void)it;return NULL; }
static struct item_datum *item_get(long i) { (void)i;return NULL; }
static void network_distributed_statistics(long*a,long*b,long*c) { *a=*b=*c=0; }
static void network_distributed_item_statistics(long*a,long*b,long*c,long*d) { *a=*b=*c=*d=0; }
static void network_damage_statistics(long*a,long*b,long*c,long*d) { *a=*b=*c=*d=0; }
static long local_player_get_player_index(int i) { return i==0?0:NONE; }
static const struct observer_result *observer_get_camera(int i) { (void)i;return NULL; }
static struct player_datum *player_get(long i) { return &players[i]; }
static int game_engine_can_score(void) { return 1; }
static int game_engine_running(void) { return 1; }
static struct { struct { long score_to_win; } universal_variant; } variant;
static void *game_engine_get_variant(void);
#define game_engine_get_variant() (&variant)
static struct { long (*get_player_score)(long,int); } *game_engine;
static void platform_log(const char *format,...) {
    char line[8192]; va_list args; va_start(args,format);vsnprintf(line,sizeof(line),format,args);va_end(args);
    if (!strncmp(line,"network test: tick 90 actor player ",35)) {
        int actor=-1; sscanf(line,"network test: tick 90 actor player %d:",&actor);
        if (actor<0||actor>=32||!strstr(line," m")||!strstr(line,"reposition1")) bad=1; else seen[actor]++;
        actor_lines++;
    } else if (strstr(line,"network test: tick 90") && strstr(line,"roster 32 spawned 32 requested 32")&&strstr(line,"scripted_hits 3 scripted_kills 1")) summary_lines++;
}
__APPEND__
__LOG__
int main(void) {
    for(int i=0;i<32;i++) {
        players[i].name[0]=L'B';players[i].unit_index=i;players[i].local_player_index=i?NONE:0;players[i].network_player_data.machine_index=i;
        objects[i].object.parent_object_index=NONE;
        for(int j=0;j<4;j++) units[i].unit.weapon_object_indices[j]=j;
    }
    network_test_log_players();
    if(actor_lines!=32||summary_lines!=1||bad) { fprintf(stderr,"actors=%d summaries=%d bad=%d\n",actor_lines,summary_lines,bad);return 1; }
    for(int i=0;i<32;i++) if(seen[i]!=1) return 2;
    return 0;
}
'''


def test_complete_per_actor_records_do_not_truncate(tmp_path):
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SkipTest("A C compiler is required for player telemetry")
    source = HARNESS.replace("__APPEND__", function(SOURCE, "static void network_test_append("))
    source = source.replace("__LOG__", function(SOURCE, "static void network_test_log_players("))
    path, binary = tmp_path / "log.c", tmp_path / "log"
    path.write_text(source)
    subprocess.run([compiler,"-O2","-fsanitize=address,undefined",str(path),"-lm","-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
