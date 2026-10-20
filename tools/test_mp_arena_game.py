"""Exercise the production scenario geometry, staging, respawn, and movement adapter."""
from pathlib import Path
import shutil
import subprocess
from unittest import SkipTest

import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "port/linux/game/benchmark_mp_arena.c"
HARNESS = r'''
#include <assert.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define BENCHMARK_MP_ARENA_PROBE 1
#define HALO_PORT_MAXIMUM_NETWORK_PLAYERS 128
#define NONE -1
#define FLAG(i) (1UL<<(i))
#define _collision_test_front_facing_surfaces_bit 0
#define _collision_test_structure_bit 5
#define _collision_test_for_line_of_sight_flags 0xc2ad
#define TAG_BLOCK_GET_ELEMENT(block,index,type) (&((type *)(block)->address)[index])
typedef float real;
typedef struct { float x,y,z; } real_point3d;
typedef struct { float i,j,k; } real_vector3d;
struct location { int cluster_index; };
struct player_starting_location { real_point3d position; float facing; };
struct scenario { struct { int count; void *address; } players; };
struct collision_result { real_point3d point; struct { struct { float i,j,k; } n; } plane; };
struct player_datum { long unit_index; int local_player_index; struct { int machine_index; } network_player_data; };
struct player_action { unsigned long control_flags; struct {float yaw,pitch;} desired_facing;
    struct {float i,j;} throttle; float primary_trigger; short desired_weapon_index,desired_grenade_index,desired_zoom_level; };
struct game_input_preferences { int joystick_controls,invert_look; };
static int target=128,positions_set,no_ground,blocked,profile_inverted,steep_floor,invalid_cluster,blocked_clearance;
static struct player_datum players[128];
static real_point3d positions[129];
static struct player_starting_location starts[2];
static struct scenario scenario={ {2,starts} };
static int network_test_suite_network_players(void) { return target; }
static int game_time_initialized(void) { return 1; }
static int game_in_progress(void) { return 1; }
static int mp_benchmark_probe_active(void) { return 1; }
static struct scenario *global_scenario_try_and_get(void) { return &scenario; }
static long *machine_get_player_list(long machine) { static long lists[128][4];
    if(machine<0||machine>=target)return NULL;lists[machine][0]=machine;
    lists[machine][1]=lists[machine][2]=lists[machine][3]=NONE;return lists[machine]; }
static struct player_datum *player_try_and_get(long i) { return i>=0&&i<target?&players[i]:NULL; }
static long local_player_get_player_index(int i) { return i==0?0:NONE; }
static void object_get_origin(long i,real_point3d *p) { *p=positions[i]; }
static void object_set_position(long i,const real_point3d *p,void*a,void*b) { (void)a;(void)b;positions[i]=*p;positions_set++; }
static void scenario_location_from_point(struct location*l,const real_point3d*p) { (void)p;l->cluster_index=invalid_cluster?NONE:0; }
static int collision_test_vector(unsigned long flags,const real_point3d*p,const real_vector3d*v,long ignore,struct collision_result*c) {
    (void)flags;(void)ignore;
    if(v->k < -1) { if(no_ground)return 0;c->point=*p;c->point.z=0;c->plane.n.k=steep_floor?0.5f:1;return 1; }
    return blocked;
}
static int collision_test_pill(unsigned long flags,const real_point3d*p,const real_vector3d*v,float r,long ignore,struct collision_result*c) {
    (void)flags;(void)p;(void)v;(void)r;(void)ignore;(void)c;return blocked_clearance; }
static real_vector3d *player_control_get_facing_direction(short i,real_vector3d *v) { (void)i;v->i=1;v->j=v->k=0;return v; }
static void input_abstraction_get_local_player_preferences(short i,struct game_input_preferences*p) {
    (void)i;p->joystick_controls=0;p->invert_look=profile_inverted; }
static char last_log[512];
static void platform_log(const char *format,...) { va_list args;va_start(args,format);vsnprintf(last_log,sizeof(last_log),format,args);va_end(args); }
#include "__SOURCE__"
int main(void) {
    int i,j;
    const int smaller_rosters[]={32,64,90};
    struct benchmark_mp_arena_config cfg={6,1.5f,3.5f,0.4f};
    for(i=0;i<128;i++) {players[i].unit_index=i;players[i].local_player_index=i?NONE:0;players[i].network_player_data.machine_index=i;}
    assert(benchmark_mp_arena_configure(&cfg));
    assert(!benchmark_mp_arena_configure(&cfg));
    assert(benchmark_mp_arena_tick(0)==1&&benchmark_mp_arena_ready());
    assert(positions_set==128);
    for(i=0;i<128;i++) assert(benchmark_mp_arena_repositions(i)==1);
    assert(benchmark_mp_arena_tick(1)==1&&positions_set==128);
    struct player_action action={0};
    for(i=1;i<128;i++) {
        assert(benchmark_mp_arena_action(i,10,&action));
        assert(isfinite(action.desired_facing.yaw)&&isfinite(action.desired_facing.pitch));
        assert(hypotf(action.throttle.i,action.throttle.j)>0&&hypotf(action.throttle.i,action.throttle.j)<=0.76f);
        assert(action.primary_trigger==0.0f); /* host startup still owns combat */
    }
    /* Following a nearby moving waypoint must still request active walking. */
    real_point3d station=positions[1];
    positions[1].x += cfg.movement_radius*cosf(1.4f)*0.9f;
    positions[1].y += cfg.movement_radius*sinf(1.4f)*0.9f;
    assert(benchmark_mp_arena_action(1,10,&action));
    assert(hypotf(action.throttle.i,action.throttle.j)>0.2f);
    positions[1]=station;
    players[127].unit_index=40;
    assert(benchmark_mp_arena_tick(2)==1&&positions_set==129&&benchmark_mp_arena_repositions(127)==2);
    struct mp_benchmark_probe_pad pad;
    assert(benchmark_mp_arena_gamepad(0,10,&pad)&&pad.left_x);
    assert(!pad.fire);
    assert(benchmark_mp_arena_begin_combat()&&benchmark_mp_arena_combat_ready());
    assert(benchmark_mp_arena_action(1,10,&action)&&action.primary_trigger>0.0f);
    assert(benchmark_mp_arena_gamepad(0,10,&pad)&&pad.fire);
    assert(!benchmark_mp_arena_gamepad(1,10,&pad));
    benchmark_mp_arena_reset();assert(!benchmark_mp_arena_repositions(127));
    assert(benchmark_mp_arena_tick(10)==1&&!benchmark_mp_arena_gamepad(0,10,&pad));
    no_ground=1;assert(benchmark_mp_arena_configure(&cfg));assert(benchmark_mp_arena_tick(1)==-1);
    assert(!benchmark_mp_arena_ready());assert(strstr(last_log,"arena geometry exhausted")&&strstr(last_log,"floor misses 8")&&strstr(last_log,"first rejected actor 0"));benchmark_mp_arena_reset();
    no_ground=0;blocked=1;assert(benchmark_mp_arena_configure(&cfg));assert(benchmark_mp_arena_tick(1)==-1);benchmark_mp_arena_reset();
    blocked=0;steep_floor=1;assert(benchmark_mp_arena_configure(&cfg));assert(benchmark_mp_arena_tick(1)==-1);
    assert(strstr(last_log,"steep floors 8"));benchmark_mp_arena_reset();
    steep_floor=0;blocked_clearance=1;assert(benchmark_mp_arena_configure(&cfg));assert(benchmark_mp_arena_tick(1)==-1);
    assert(strstr(last_log,"blocked clearance 8"));benchmark_mp_arena_reset();
    blocked_clearance=0;invalid_cluster=1;assert(benchmark_mp_arena_configure(&cfg));assert(benchmark_mp_arena_tick(1)==-1);
    assert(strstr(last_log,"invalid feet clusters 8")&&strstr(last_log,"invalid head clusters 8"));benchmark_mp_arena_reset();
    invalid_cluster=0;players[127].unit_index=NONE;assert(benchmark_mp_arena_configure(&cfg));
    assert(benchmark_mp_arena_tick(1)==0&&!benchmark_mp_arena_ready());
    assert(!benchmark_mp_arena_begin_combat()&&!benchmark_mp_arena_combat_ready());
    assert(benchmark_mp_arena_gamepad(0,10,&pad)&&pad.left_x&&!pad.fire);
    assert(benchmark_mp_arena_tick(100)==0&&benchmark_mp_arena_tick(450)==0&&benchmark_mp_arena_tick(639)==0);
    players[127].unit_index=127;
    assert(benchmark_mp_arena_tick(640)==1&&benchmark_mp_arena_ready()&&positions_set==257);
    benchmark_mp_arena_reset();players[127].unit_index=NONE;
    assert(benchmark_mp_arena_configure(&cfg));assert(benchmark_mp_arena_tick(640)==0);
    assert(benchmark_mp_arena_tick(641)==-1);assert(strstr(last_log,"arena readiness timeout")&&strstr(last_log,"actor 127"));benchmark_mp_arena_reset();
    players[127].unit_index=127;
    for(j=0;j<3;j++) {
        target=smaller_rosters[j];
        players[target-1].unit_index=NONE;
        assert(benchmark_mp_arena_configure(&cfg));assert(benchmark_mp_arena_tick(450)==0);
        assert(benchmark_mp_arena_tick(451)==-1&&strstr(last_log,"arena readiness timeout"));
        benchmark_mp_arena_reset();players[target-1].unit_index=target-1;
    }
    target=128;assert(benchmark_mp_arena_configure(&cfg));assert(benchmark_mp_arena_tick(20)==1);
    assert(benchmark_mp_arena_tick(1)==-1);assert(strstr(last_log,"arena clock regression")&&strstr(last_log,"previous 20 current 1"));
    benchmark_mp_arena_reset();
    {
        const float nonfinite[]={__builtin_nanf(""),__builtin_inff()};
        for(j=0;j<2;j++) {
            starts[0].position.x=starts[1].position.x=nonfinite[j];
            assert(benchmark_mp_arena_configure(&cfg));
            assert(benchmark_mp_arena_tick(1)==-1&&strstr(last_log,"arena geometry exhausted"));
            benchmark_mp_arena_reset();
            starts[0].position.x=starts[1].position.x=0;
            assert(benchmark_mp_arena_configure(&cfg));
            assert(benchmark_mp_arena_tick(1)==1);
            positions[1].x=nonfinite[j];
            assert(benchmark_mp_arena_tick(2)==-1&&strstr(last_log,"arena invalid position"));
            positions[1].x=0;
            benchmark_mp_arena_reset();
        }
    }
    return 0;
}
'''


@pytest.mark.parametrize("strict_math", [False, True])
def test_actual_geometry_staging_respawn_and_input_adapter(tmp_path, strict_math):
    assert SOURCE.is_file(), "The generic game arena adapter is not implemented"
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SkipTest("A C compiler is required for the game arena probe")
    path,binary=tmp_path / "arena-game.c",tmp_path / "arena-game"
    path.write_text(HARNESS.replace("__SOURCE__",str(SOURCE)))
    flags = ["-D__STRICT_ANSI__"] if strict_math else []
    subprocess.run([compiler,"-O2","-std=gnu89",*flags,"-fms-extensions","-Wno-c99-extensions","-fsanitize=address,undefined","-iquote",str(ROOT / "port/linux/include"),
                    str(path),"-lm","-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)


def _actual_function(path, name):
    text = path.read_text()
    start = text.index("\n" + name + "(") if "\n" + name + "(" in text else -1
    if start < 0:
        import re
        match = re.search(r"^(?:static )?(?:real|long) " + name + r"\(", text, re.M)
        assert match, name
        start = match.start()
    else:
        start = text.rfind("\n", 0, start) + 1
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def test_actual_spawn_occupancy_releases_before_full_arena_readiness(tmp_path):
    """Use stock distance rating and start selection with the actual arena adapter."""
    prefix = HARNESS.split("int main(void)")[0]
    prefix = prefix.replace("long unit_index; int local_player_index;", "long unit_index; int team_index; int local_player_index;")
    prefix = prefix.replace("} players; };", "} players; struct { int count; void *address; } ai_encounters; };")
    engine = ROOT / "source/game/game_engine.c"
    players_source = ROOT / "source/game/players.c"
    stock = r"""
#define TRUE 1
#define FALSE 0
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(x) (x)
#define PIN(x,lo,hi) ((x)<(lo)?(lo):(x)>(hi)?(hi):(x))
typedef int boolean;
static struct { struct { int teams; } universal_variant; } global_variant;
static int game_engine=1, player_data;
struct data_iterator { int index; };
struct encounter_definition { struct { int count; } player_starting_locations; };
static struct { int selected_squad_index; } ai_debug={NONE};
static struct player_datum *player_get(long i) { return &players[i]; }
static void data_iterator_new(struct data_iterator *it,int data) { (void)data; it->index=0; }
static void *data_iterator_next(struct data_iterator *it) { return it->index<target?&players[it->index++]:NULL; }
static float distance3d(const real_point3d *a,const real_point3d *b) { return sqrtf((a->x-b->x)*(a->x-b->x)+(a->y-b->y)*(a->y-b->y)+(a->z-b->z)*(a->z-b->z)); }
static struct scenario *global_scenario_get(void) { return &scenario; }
static struct player_starting_location *player_get_starting_location(int i) { return &starts[i]; }
static int game_engine_running(void) { return 1; }
static float real_random_range(float lo,float hi) { return (lo+hi)*0.5f; }
__RATING__
static real game_engine_get_starting_location_rating(long p,const struct player_starting_location *s) { return game_engine_get_distance_rating_for_spawn(p,&s->position); }
__SELECTION__
int main(void) {
    int i;
    struct benchmark_mp_arena_config cfg={6,1.5f,3.5f,0.4f};
    struct player_action action={0};
    struct mp_benchmark_probe_pad pad;
    starts[1].position.x=100.0f;
    for(i=0;i<128;i++) {
        players[i].unit_index=NONE; players[i].team_index=i;
        players[i].local_player_index=i?NONE:0;players[i].network_player_data.machine_index=i;
    }
    players[0].unit_index=0; positions[0]=starts[0].position;
    players[1].unit_index=1; positions[1]=starts[1].position;
    assert(find_best_starting_location_index(2)==NONE); /* both stock starts occupied */
    assert(benchmark_mp_arena_configure(&cfg));
    assert(benchmark_mp_arena_tick(0)==0);
    assert(positions_set==2); /* move local and first remote before all units exist */
    assert(find_best_starting_location_index(2)==1); /* actual stock selector can spawn again */
    assert(!benchmark_mp_arena_ready()&&!benchmark_mp_arena_begin_combat());
    assert(!benchmark_mp_arena_action(1,0,&action));
    assert(benchmark_mp_arena_gamepad(0,0,&pad)&&!pad.fire);
    /* Missing/replaced units during setup must be staged again. */
    players[1].unit_index=NONE;
    no_ground=1; /* validated geometry is retained while units are still arriving */
    assert(benchmark_mp_arena_tick(1)==0&&!benchmark_mp_arena_ready());
    players[1].unit_index=1;positions[1]=starts[1].position;
    assert(find_best_starting_location_index(2)==NONE);
    assert(benchmark_mp_arena_tick(2)==0&&benchmark_mp_arena_repositions(1)==2);
    no_ground=0;
    assert(find_best_starting_location_index(2)==1);
    players[1].unit_index=NONE;
    assert(benchmark_mp_arena_tick(3)==0);
    players[1].unit_index=128;positions[128]=starts[1].position;
    assert(benchmark_mp_arena_tick(4)==0&&benchmark_mp_arena_repositions(1)==3);
    for(i=2;i<128;i++) {
        assert(find_best_starting_location_index(i)==1);
        players[i].unit_index=i;positions[i]=starts[1].position;
        assert(benchmark_mp_arena_tick(i+3)==(i==127));
        assert(benchmark_mp_arena_ready()==(i==127));
        assert(benchmark_mp_arena_repositions(i)==1);
        assert(!benchmark_mp_arena_combat_ready());
    }
    assert(benchmark_mp_arena_ready()&&benchmark_mp_arena_begin_combat());
    /* Invalid full geometry fails before touching even an available unit. */
    benchmark_mp_arena_reset();no_ground=1;positions_set=0;players[127].unit_index=NONE;
    assert(benchmark_mp_arena_configure(&cfg));
    assert(benchmark_mp_arena_tick(0)==-1&&positions_set==0&&!benchmark_mp_arena_ready());
    return 0;
}
"""
    stock = stock.replace("__RATING__", _actual_function(engine, "game_engine_get_distance_rating_for_spawn"))
    stock = stock.replace("__SELECTION__", _actual_function(players_source, "find_best_starting_location_index"))
    path,binary=tmp_path / "arena-stock-spawn.c",tmp_path / "arena-stock-spawn"
    path.write_text((prefix + stock).replace("__SOURCE__",str(SOURCE)))
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SkipTest("A C compiler is required for the game arena probe")
    subprocess.run([compiler,"-O2","-std=gnu89","-fms-extensions","-Wno-c99-extensions","-fsanitize=address,undefined","-iquote",str(ROOT / "port/linux/include"),
                    str(path),"-lm","-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
