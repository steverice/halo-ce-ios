"""Exercise the production scenario geometry, staging, respawn, and movement adapter."""
from pathlib import Path
import shutil
import subprocess
from unittest import SkipTest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "port/linux/game/benchmark_mp_arena.c"
HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#define BENCHMARK_MP_ARENA_PROBE 1
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
static int target=32,positions_set,no_ground,blocked,profile_inverted;
static struct player_datum players[32];
static real_point3d positions[64];
static struct player_starting_location starts[2];
static struct scenario scenario={ {2,starts} };
static int network_test_suite_network_players(void) { return target; }
static int game_time_initialized(void) { return 1; }
static int game_in_progress(void) { return 1; }
static int mp_benchmark_probe_active(void) { return 1; }
static struct scenario *global_scenario_try_and_get(void) { return &scenario; }
static long *machine_get_player_list(long machine) { static long lists[32][4];
    if(machine<0||machine>=target)return NULL;lists[machine][0]=machine;
    lists[machine][1]=lists[machine][2]=lists[machine][3]=NONE;return lists[machine]; }
static struct player_datum *player_try_and_get(long i) { return i>=0&&i<target?&players[i]:NULL; }
static long local_player_get_player_index(int i) { return i==0?0:NONE; }
static void object_get_origin(long i,real_point3d *p) { *p=positions[i]; }
static void object_set_position(long i,const real_point3d *p,void*a,void*b) { (void)a;(void)b;positions[i]=*p;positions_set++; }
static void scenario_location_from_point(struct location*l,const real_point3d*p) { (void)p;l->cluster_index=blocked?NONE:0; }
static int collision_test_vector(unsigned long flags,const real_point3d*p,const real_vector3d*v,long ignore,struct collision_result*c) {
    (void)flags;(void)ignore;
    if(v->k < -1) { if(no_ground)return 0;c->point=*p;c->point.z=0;c->plane.n.k=1;return 1; }
    return blocked;
}
static int collision_test_pill(unsigned long flags,const real_point3d*p,const real_vector3d*v,float r,long ignore,struct collision_result*c) {
    (void)flags;(void)p;(void)v;(void)r;(void)ignore;(void)c;return blocked; }
static real_vector3d *player_control_get_facing_direction(short i,real_vector3d *v) { (void)i;v->i=1;v->j=v->k=0;return v; }
static void input_abstraction_get_local_player_preferences(short i,struct game_input_preferences*p) {
    (void)i;p->joystick_controls=0;p->invert_look=profile_inverted; }
static void platform_log(const char *format,...) { (void)format; }
#include "__SOURCE__"
int main(void) {
    int i;
    struct benchmark_mp_arena_config cfg={6,1.5f,3.5f,0.4f};
    for(i=0;i<32;i++) {players[i].unit_index=i;players[i].local_player_index=i?NONE:0;players[i].network_player_data.machine_index=i;}
    assert(benchmark_mp_arena_configure(&cfg));
    assert(!benchmark_mp_arena_configure(&cfg));
    assert(benchmark_mp_arena_tick(0)==1&&benchmark_mp_arena_ready());
    assert(positions_set==32);
    for(i=0;i<32;i++) assert(benchmark_mp_arena_repositions(i)==1);
    assert(benchmark_mp_arena_tick(1)==1&&positions_set==32);
    struct player_action action={0};
    for(i=1;i<32;i++) {
        assert(benchmark_mp_arena_action(i,10,&action));
        assert(isfinite(action.desired_facing.yaw)&&isfinite(action.desired_facing.pitch));
        assert(hypotf(action.throttle.i,action.throttle.j)>0&&hypotf(action.throttle.i,action.throttle.j)<=0.76f);
    }
    /* Following a nearby moving waypoint must still request active walking. */
    real_point3d station=positions[1];
    positions[1].x += cfg.movement_radius*cosf(1.4f)*0.9f;
    positions[1].y += cfg.movement_radius*sinf(1.4f)*0.9f;
    assert(benchmark_mp_arena_action(1,10,&action));
    assert(hypotf(action.throttle.i,action.throttle.j)>0.2f);
    positions[1]=station;
    players[31].unit_index=40;
    assert(benchmark_mp_arena_tick(2)==1&&positions_set==33&&benchmark_mp_arena_repositions(31)==2);
    struct mp_benchmark_probe_pad pad;
    assert(benchmark_mp_arena_gamepad(0,10,&pad)&&pad.left_x);
    assert(!benchmark_mp_arena_gamepad(1,10,&pad));
    benchmark_mp_arena_reset();assert(!benchmark_mp_arena_repositions(31));
    assert(benchmark_mp_arena_tick(10)==1&&!benchmark_mp_arena_gamepad(0,10,&pad));
    no_ground=1;assert(benchmark_mp_arena_configure(&cfg));assert(benchmark_mp_arena_tick(1)==-1);
    assert(!benchmark_mp_arena_ready());benchmark_mp_arena_reset();
    no_ground=0;blocked=1;assert(benchmark_mp_arena_configure(&cfg));assert(benchmark_mp_arena_tick(1)==-1);benchmark_mp_arena_reset();
    blocked=0;players[31].unit_index=NONE;assert(benchmark_mp_arena_configure(&cfg));
    assert(benchmark_mp_arena_tick(1)==0&&!benchmark_mp_arena_ready());
    assert(benchmark_mp_arena_tick(451)==-1);benchmark_mp_arena_reset();
    return 0;
}
'''


def test_actual_geometry_staging_respawn_and_input_adapter(tmp_path):
    assert SOURCE.is_file(), "The generic game arena adapter is not implemented"
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SkipTest("A C compiler is required for the game arena probe")
    path,binary=tmp_path / "arena-game.c",tmp_path / "arena-game"
    path.write_text(HARNESS.replace("__SOURCE__",str(SOURCE)))
    subprocess.run([compiler,"-O2","-std=gnu89","-fms-extensions","-Wno-c99-extensions","-fsanitize=address,undefined","-iquote",str(ROOT / "port/linux/include"),
                    str(path),"-lm","-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
