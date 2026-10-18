"""Characterize the real spawn selection assumption used during suite setup."""
from pathlib import Path
import shutil
import subprocess
from unittest import SkipTest

from test_mp_synthetic_input import function

ROOT = Path(__file__).resolve().parents[1]
ENGINE = (ROOT / "source/game/game_engine.c").read_text()
PLAYERS = (ROOT / "source/game/players.c").read_text()

HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stddef.h>
#define TRUE 1
#define FALSE 0
#define NONE -1
#define PIN(v,lo,hi) ((v)<(lo)?(lo):(v)>(hi)?(hi):(v))
#define TAG_BLOCK_GET_ELEMENT(block,index,type) (&((type *)(block)->address)[index])
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(i) (i)
typedef int boolean;
typedef float real;
typedef struct { float x,y,z; } real_point3d;
struct player_starting_location {real_point3d position; short team_index,game_types[4];};
struct player_datum {long unit_index;short team_index;};
struct data_iterator {int index;};
struct block {int count;void *address;};
struct scenario {struct block players,ai_encounters;};
struct encounter_definition {struct block player_starting_locations;};
static struct {int selected_squad_index;} ai_debug={NONE};
static struct {struct {int teams;} universal_variant;} global_variant;
static struct {float (*starting_location_rating)(long,const struct player_starting_location *);} engine,*game_engine=&engine;
static struct player_starting_location starts[16]= { __STARTS__ };
static struct scenario scenario={{16,starts},{0,NULL}};
static struct player_datum players[32];
static real_point3d origins[16];
static int player_data;
static struct player_datum *player_get(long i) {return &players[i];}
static void data_iterator_new(struct data_iterator *it,int data) {(void)data;it->index=0;}
static void *data_iterator_next(struct data_iterator *it) {return it->index<32?&players[it->index++]:NULL;}
static void object_get_origin(long i,real_point3d *p) {*p=origins[i];}
static float distance3d(const real_point3d*a,const real_point3d*b) {return sqrtf((a->x-b->x)*(a->x-b->x)+(a->y-b->y)*(a->y-b->y)+(a->z-b->z)*(a->z-b->z));}
static int game_engine_running(void) {return 1;}
static int game_engine_test_flag(int bit) {(void)bit;return 0;}
static int game_engine_has_teams(void) {return 0;}
static float game_engine_get_friendly_bonus(long i,const real_point3d*p) {(void)i;(void)p;return 1;}
static int game_engine_get_type(void) {return 2;}
static int nearby_vehicle(long i,const struct player_starting_location*p) {(void)i;(void)p;return 0;}
static struct scenario *global_scenario_get(void) {return &scenario;}
static struct player_starting_location *player_get_starting_location(int i) {return &starts[i];}
static float real_random_range(float lo,float hi) {return (lo+hi)*0.5f;}
enum {game_engine_none=0,game_engine_ctf=1,game_engine_race=5,_game_engine_all=12,_game_engine_all_non_team=13,_game_engine_all_normal=14};
__FUNCTIONS__
int main(void) {
    int i;
    for(i=0;i<32;i++){players[i].team_index=NONE;players[i].unit_index=i<16?i:NONE;}
    for(i=0;i<16;i++)origins[i]=starts[i].position;
    assert(find_best_starting_location_index(16)==NONE);
    /* Normal movement can release a start; no larger roster cap or fallback
       spawn is needed, and reoccupying it must still block that start. */
    origins[0].x+=1000;
    assert(find_best_starting_location_index(16)==0);
    origins[0]=starts[0].position;
    assert(find_best_starting_location_index(16)==NONE);
    return 0;
}
'''


def probe_source(starts):
    functions = [function(ENGINE, signature) for signature in (
        "real game_engine_get_distance_rating_for_spawn(",
        "static real default_starting_location_rate_function(",
        "boolean match_game_type(",
        "real game_engine_get_starting_location_rating(")]
    functions.append(function(PLAYERS, "long find_best_starting_location_index("))
    return HARNESS.replace("__FUNCTIONS__", "\n".join(functions)).replace(
        "__STARTS__", ",\n".join("{{%.9g,%.9g,%.9g},0,{2,0,0,0}}" % tuple(p) for p in starts))


def test_occupied_starts_wait_until_normal_movement_releases_one(tmp_path):
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SkipTest("A C compiler is required for the spawn selection probe")
    source, binary = tmp_path / "spawn.c", tmp_path / "spawn"
    source.write_text(probe_source([(i * 4, 0, 0) for i in range(16)]))
    subprocess.run([compiler, "-O2", "-std=gnu89", "-fms-extensions", "-Wno-c99-extensions",
                    "-fsanitize=address,undefined", str(source), "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
