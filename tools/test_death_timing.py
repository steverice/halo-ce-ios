"""Compile actual death handlers, co-op respawn and camera code against a fake world."""
import argparse
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def function(source, name):
    match = re.search(r"^(?:static )?[\w *]+\b" + name + r"\(\s*[^;{]*\)\s*\{", source, re.M)
    if not match:
        raise ValueError(f"Function not found: {name}")
    start = source.index('{', match.start())
    depth, end = 1, start + 1
    # The extracted functions have no braces inside strings/comments.
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


PRELUDE = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include <wchar.h>
typedef int boolean;
typedef float real;
typedef unsigned char byte;
typedef struct {real x,y,z;} real_point3d;
typedef struct {real i,j,k;} real_vector3d;
typedef struct {real yaw,pitch;} real_euler_angles2d;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define NUMBEROF(a) (sizeof(a)/sizeof((a)[0]))
#define FLAG(b) (1u<<(b))
#define TEST_FLAG(v,b) ((v)&FLAG(b))
#define _pi 3.14159265358979323846f
#define DEGREES_TO_RADIANS(d) ((d)*(_pi/180.0f))
#define match_vassert(...) ((void)0)
#define MAXIMUM_LOCAL_PLAYERS 4
#define MAXIMUM_GAMEPADS 8
#define FIRST_GAMEPAD_ANALOG_BUTTON 0
#define COOP_CORPSE_VIEW_TICKS 92
#define _object_mask_unit 2
enum {_gamepad_analog_button_a=0};
#define match_assert(...) ((void)0)
static struct {short loss_timer,respawn_timer;boolean lost_map,respawn,saving_map,reset_map,revert_map;long switch_to_structure_bsp_index;} main_globals;
static long main_loss_last_tick,main_respawn_last_tick;
static long tick;
static int paused,cinematic,reverts,attempts,spawns,teleports,player_count=2;
static long global_scenario_index=1;
static int projectiles,enemies,airborne,vehicle_moving,spawn_failed,teleport_failed,multiplayer;
static int drawn_count;
static wchar_t drawn_text[128];
static boolean waiting_to_start;
static struct {int respawn_failure,respawn_failed;} players_state,*players_globals=&players_state;
struct player_datum {
    long unit_index,team_index,dead_unit_index;
    struct {short deaths;} statistics;
    struct {short controller_index;} network_player_data;
    wchar_t name[16];
};
static struct player_datum players[4];
struct object_datum {struct {real_point3d bounding_sphere_center;long parent_object_index;} object;};
static struct object_datum objects[4];
static boolean object_exists[4]={TRUE,TRUE,TRUE,TRUE};
struct unit_datum {struct {long driver_object_index,player_index;} unit;};
static struct unit_datum units[104];
struct gamepad_state {short buttons[32];};
static struct gamepad_state gamepads[MAXIMUM_GAMEPADS];
struct observer_result {real_point3d position;};
static struct observer_result observer;
struct biped_datum {struct {unsigned flags;} biped;};
static struct biped_datum biped;
struct players_vehicle_datum {struct {int unknown_state;} vehicle;};
static struct players_vehicle_datum vehicle;
struct data_iterator {long datum_index;int next;};
static int player_data;
/* (the network co-op paths, off: this is a local game) */
enum {_game_connection_local,_game_connection_network_client,_game_connection_network_server};
static short game_connection(void) {return _game_connection_local;}
static boolean network_coop_active(void) {return FALSE;}
static boolean players_respawn_network_coop(boolean first_spawn) {(void)first_spawn;return FALSE;}
static void players_respawn_at_checkpoint(void) {}
static boolean players_are_all_dead(void) {return FALSE;}
static boolean main_coop_host_revert(void) {return FALSE;}
enum {_biped_airborne_bit=0,_object_mask_vehicle=1};
static void data_iterator_new(struct data_iterator *i,int d) {(void)d;i->next=0;}
static void *data_iterator_next(struct data_iterator *i) {
    if(i->next>=player_count) return NULL;
    i->datum_index=i->next++;return &players[i->datum_index];
}
static struct player_datum *player_get(long i) {assert(i>=0&&i<player_count);return &players[i];}
static struct player_datum *player_try_and_get(long i) {
    return i>=0&&i<player_count ? &players[i] : NULL;
}
static long local_player_get_player_index(short i) {return i>=0&&i<player_count?i:NONE;}
static struct object_datum *object_try_and_get(long i) {
    if(i==NONE) return NULL;
    if(i<100||i>=104||!object_exists[i-100]) return NULL;
    return &objects[i-100];
}
static struct object_datum *object_get(long i) {return object_try_and_get(i);}
static long object_get_ultimate_parent(long i) {return vehicle_moving ? 999 : i;}
static struct biped_datum *biped_try_and_get(long i) {(void)i;biped.biped.flags=airborne;return &biped;}
static void *object_try_and_get_and_verify_type(long i,int mask) {
    if(mask==_object_mask_unit) return object_try_and_get(i);
    (void)i;(void)mask;vehicle.vehicle.unknown_state=vehicle_moving;return &vehicle;
}
static struct unit_datum *unit_get(long i) {assert(i>=0&&i<104);return &units[i];}
static int dangerous_projectiles_near_player(void) {return projectiles;}
static int any_unit_is_dangerous(void) {return 0;}
static int ai_enemies_attacking_player(void) {return enemies;}
static void player_spawn(long i) {spawns++;if(!spawn_failed) players[i].unit_index=100+i;}
static int player_teleport(long i,long target,const real_point3d *p) {
    (void)i;assert(target==100);assert(p->x==100);teleports++;return !teleport_failed;
}
static long game_time_get(void) {return tick;}
static int game_time_get_paused(void) {return paused;}
static int cinematic_in_progress(void) {return cinematic;}
static int game_engine_running(void) {return multiplayer;}
static struct gamepad_state *input_get_gamepad_state(short i) {
    return i>=0&&i<MAXIMUM_GAMEPADS ? &gamepads[i] : NULL;
}
static byte input_abstraction_port_accept(short i) {(void)i;return 0;}
static struct observer_result const *observer_get_camera(short i) {(void)i;return &observer;}
static real real_local_random_range(real minimum,real maximum) {(void)maximum;return minimum;}
static boolean players_coop_waiting_to_start(long i) {(void)i;return waiting_to_start;}
static int usnprintf(wchar_t *buffer,size_t size,wchar_t const *format,...) {
    va_list args;wchar_t const *name,*hint;int result;(void)format;
    va_start(args,format);name=va_arg(args,wchar_t const *);hint=va_arg(args,wchar_t const *);va_end(args);
    result=swprintf(buffer,size,L"SPECTATING %.12ls   (A: NEXT)\r\n%ls",name,hint);return result;
}
static void draw_bottom_text(wchar_t const *text) {
    drawn_count++;wcsncpy(drawn_text,text,NUMBEROF(drawn_text)-1);drawn_text[NUMBEROF(drawn_text)-1]=0;
}
static void game_state_revert(void) {reverts++;}
static real_vector3d zero_vector,*global_zero_vector3d=&zero_vector;
static void vector3d_from_euler_angles2d(real_vector3d *v,const real_euler_angles2d *a) {
    (void)a;v->i=1;v->j=v->k=0;
}
static void observer_up_from_forward(const real_vector3d *f,real_vector3d *u) {
    (void)f;u->k=1;u->i=u->j=0;
}
static struct {real dead_timer,multiplayer_switch_timer,singleplayer_switch_timer;} dead_camera_constants={3,15,3};
'''
TESTS = r'''
static void reset(int count) {
    int i;
    memset(&main_globals,0,sizeof(main_globals));memset(&players_state,0,sizeof(players_state));
    memset(players,0,sizeof(players));memset(gamepads,0,sizeof(gamepads));
    memset(object_exists,TRUE,sizeof(object_exists));
    for(i=0;i<MAXIMUM_LOCAL_PLAYERS;i++) coop_spectate_watched[i]=NONE;
    tick=main_loss_last_tick=main_respawn_last_tick=0;
    global_scenario_index=1;
    drawn_count=0;drawn_text[0]=0;waiting_to_start=FALSE;
    paused=cinematic=reverts=attempts=spawns=teleports=0;
    projectiles=enemies=airborne=vehicle_moving=spawn_failed=teleport_failed=multiplayer=0;
    player_count=count;
    for(i=0;i<count;i++) {players[i].unit_index=i?NONE:100;players[i].team_index=0;objects[i].object.bounding_sphere_center.x=100+i;}
    for(i=0;i<MIN(count,MAXIMUM_LOCAL_PLAYERS);i++)
        coop_spectate_camera_unit((short)i,FALSE,FALSE,FALSE);
}
static void rates(void) {
    int rates[]={15,30,60,120,144,240,1000};
    int r,frame,count;
    for(r=0;r<7;r++) {
        int hz=rates[r];
        reset(2);main_lost_map();
        for(frame=1;!reverts;frame++) {
            assert(frame<hz*4);tick=(long)frame*30/hz;
            main_lost_map();main_lost_map_private();
            if(tick<92) assert(!reverts);
        }
        assert(reverts==1&&!main_globals.lost_map);
        printf("%d FPS: reload at %.3fs\n",hz,(frame-1)/(double)hz);
        for(count=2;count<=4;count++) {
            reset(count);main_respawn(FALSE);
            for(frame=1;main_globals.respawn;frame++) {
                assert(frame<hz*4);tick=(long)frame*30/hz;
                main_respawn(FALSE);main_respawn_private();
                if(tick<92) assert(!spawns&&!attempts);
            }
            assert(spawns==count-1&&teleports==count-1&&!main_globals.lost_map);
        }
    }
}
static void clock_and_lifecycle(void) {
    int i;
    reset(2);main_lost_map();main_respawn(FALSE);
    for(i=0;i<10000;i++) {main_lost_map_private();main_respawn_private();}
    assert(!reverts&&!attempts&&!main_globals.loss_timer&&!main_globals.respawn_timer);
    tick=40;main_lost_map_private();main_respawn_private();
    paused=1;tick=400;main_lost_map_private();main_respawn_private();
    assert(main_globals.loss_timer==40&&main_globals.respawn_timer==40);
    paused=0;main_lost_map_private();main_respawn_private();assert(!reverts&&!attempts);
    tick=451;main_lost_map_private();main_respawn_private();assert(!reverts&&!attempts);
    tick=452;main_lost_map_private();main_respawn_private();assert(reverts==1&&spawns==1);
    main_lost_map();players[1].unit_index=NONE;main_respawn(FALSE);
    assert(!main_globals.loss_timer&&!main_globals.respawn_timer);
    tick=0;main_lost_map_private();main_respawn_private();
    tick=91;main_lost_map_private();main_respawn_private();assert(reverts==1&&spawns==1);
    tick=92;main_lost_map_private();main_respawn_private();assert(reverts==2&&spawns==2);
    reset(2);main_respawn(FALSE);tick=40;main_respawn_private();
    cinematic=1;tick=400;main_respawn_private();assert(main_globals.respawn_timer==40);
    cinematic=0;main_respawn_private();assert(!attempts);
    tick=452;main_respawn_private();assert(spawns==1);
    reset(2);main_lost_map();cinematic=1;tick=92;main_lost_map_private();assert(reverts==1);
    reset(2);main_respawn(TRUE);main_respawn_private();assert(spawns==1);
    reset(2);main_respawn(TRUE);paused=1;main_respawn_private();assert(!attempts);
    paused=0;main_respawn_private();assert(spawns==1);
    reset(2);main_lost_map();tick=60;main_lost_map_private();
    main_reset_map();assert(!main_globals.lost_map);tick=600;main_lost_map();
    main_lost_map_private();assert(!reverts&&!main_globals.loss_timer);
    tick=692;main_lost_map_private();assert(reverts==1);
    main_lost_map();tick=720;main_lost_map_private();main_revert_map();
    assert(!main_globals.lost_map);tick=800;main_lost_map();main_lost_map_private();
    assert(reverts==1&&!main_globals.loss_timer);
    reset(2);main_lost_map();tick=2147483647L;main_lost_map_private();assert(reverts==1);
    reset(2);tick=2147483600L;main_lost_map();tick=2147483647L;main_lost_map_private();
    assert(main_globals.loss_timer==47);tick=-2147483647L-1;main_lost_map_private();
    assert(!main_globals.loss_timer);tick=-2147483556L;main_lost_map_private();assert(reverts==1);
}
static void coop_blocked(void) {
    int cause,i;
    for(cause=0;cause<5;cause++) {
        reset(2);
        if(cause==0) projectiles=1;
        if(cause==1) enemies=1;
        if(cause==2) airborne=1;
        if(cause==3) vehicle_moving=1;
        if(cause==4) spawn_failed=1;
        main_respawn(FALSE);tick=92;main_respawn_private();assert(main_globals.respawn);
        for(i=0;i<40000;i++) {tick++;main_respawn_private();}
        assert(main_globals.respawn_timer==92&&main_globals.respawn);
        projectiles=enemies=airborne=vehicle_moving=spawn_failed=0;
        main_respawn_private();assert(!main_globals.respawn&&players[1].unit_index!=NONE);
    }
    reset(2);main_respawn(FALSE);players[0].unit_index=NONE;main_lost_map();
    tick=91;main_lost_map_private();main_respawn_private();assert(!reverts&&!spawns);
    tick=92;main_lost_map_private();main_respawn_private();assert(reverts==1&&!spawns);
}
static void cameras(void) {
    struct dead_camera c;
    struct dead_camera_command command;
    struct camera_control control={0,.1f};
    reset(2);memset(&c,0,sizeof(c));memset(&command,0,sizeof(command));
    c.player_index=1;c.current_player_index=NONE;c.unit_index=101;c.switch_timer=0;c.timer=3;
    players[0].unit_index=NONE;
    dead_camera_update(&c,&control,&command);
    assert(c.unit_index==101&&command.position.x==101&&c.switch_timer==3);
    c.switch_timer=0;dead_camera_update(&c,&control,&command);assert(c.unit_index==101);
    players[0].unit_index=100;c.switch_timer=0;
    dead_camera_update(&c,&control,&command);assert(c.unit_index==100&&c.current_player_index==0);
    c.unit_index=NONE;c.position.x=77;c.switch_timer=1;
    dead_camera_update(&c,&control,&command);assert(command.position.x==77);
    c.unit_index=101;c.current_player_index=NONE;c.switch_timer=0;paused=1;
    dead_camera_update(&c,&control,&command);assert(c.unit_index==101);
    paused=0;multiplayer=1;c.switch_timer=0;
    dead_camera_update(&c,&control,&command);assert(c.unit_index==100&&c.switch_timer==15);
}
static void setup_coop_death(void) {
    reset(2);
    players[0].unit_index=NONE;players[0].dead_unit_index=100;players[0].statistics.deaths=1;
    players[1].unit_index=101;players[1].dead_unit_index=101;
}
static void online_coop_death_camera_grace(void) {
    struct dead_camera camera;
    long selected;
    int i;

    setup_coop_death();
    selected=coop_spectate_camera_unit(0,TRUE,TRUE,TRUE);
    assert(selected==100);
    dead_camera_new(&camera,0,selected);
    assert(camera.unit_index==100&&camera.switch_timer==FLT_MAX);
    for(i=0;i<40;i++) assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    tick=91;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    tick=92;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==101);

    setup_coop_death();
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    tick=40;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    paused=1;tick=200;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    paused=0;tick=251;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    tick=252;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==101);

    setup_coop_death();
    assert(coop_spectate_camera_unit(0,TRUE,FALSE,TRUE)==NONE);
    tick=100;assert(coop_spectate_camera_unit(0,TRUE,FALSE,TRUE)==NONE);
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    tick=191;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    tick=192;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==101);

    setup_coop_death();
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    tick=40;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    assert(coop_spectate_camera_unit(0,TRUE,FALSE,TRUE)==NONE);
    tick=400;assert(coop_spectate_camera_unit(0,TRUE,FALSE,TRUE)==NONE);
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    tick=451;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    tick=452;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==101);

    setup_coop_death();
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    tick=50;global_scenario_index=2;
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    tick=141;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    tick=142;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==101);

    setup_coop_death();
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    tick=40;players[0].unit_index=100;
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==NONE);
    players[0].unit_index=NONE;players[0].dead_unit_index=102;players[0].statistics.deaths=2;
    tick=41;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==102);
    tick=132;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==102);
    tick=133;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==101);

    setup_coop_death();
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    object_exists[0]=FALSE;tick=20;
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==101);
    object_exists[0]=TRUE;

    setup_coop_death();players[1].unit_index=NONE;
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    tick=92;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    object_exists[0]=FALSE;
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==NONE);

    setup_coop_death();
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,FALSE)==101);
    tick=200;assert(coop_spectate_camera_unit(0,TRUE,FALSE,FALSE)==NONE);
    assert(coop_spectate_camera_unit(0,FALSE,TRUE,TRUE)==NONE);
    tick=200;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    tick=291;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    tick=292;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==101);
}
static void online_coop_second_death_hud_rider(void) {
    int stale_hud=0,stale_rider=0,script_rider=0;

    setup_coop_death();
    wcscpy(players[1].name,L"Rider");
    objects[1].object.parent_object_index=102;
    units[102].unit.driver_object_index=NONE;
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    tick=92;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==101);
    assert(coop_spectate_watched[0]==1);
    assert(coop_spectate_watching_rider(0));

    players[0].unit_index=100;
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==NONE);
    players[0].unit_index=NONE;players[0].dead_unit_index=103;players[0].statistics.deaths=2;
    tick=93;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==103);
    assert(coop_spectate_watched[0]==1);
    drawn_count=0;coop_spectate_draw(0);
    stale_hud=drawn_count != 0;
    stale_rider=coop_spectate_watching_rider(0);

    tick=184;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==103);
    stale_rider=stale_rider||coop_spectate_watching_rider(0);
    tick=185;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==101);
    assert(coop_spectate_watching_rider(0));
    drawn_count=0;coop_spectate_draw(0);
    assert(drawn_count==1&&wcsstr(drawn_text,L"SPECTATING Rider")!=NULL);

    script_rider=0;
    assert(coop_spectate_camera_unit(0,TRUE,FALSE,TRUE)==NONE);
    script_rider=coop_spectate_watching_rider(0);
    if (stale_hud) puts("FAIL: second death HUD still names the previous teammate during corpse view");
    if (stale_rider) puts("FAIL: prior rider target still requests the borrowed-camera exception during corpse view");
    if (!script_rider) puts("FAIL: scripted camera lost the existing remembered-rider exception");
    assert(!stale_hud&&!stale_rider&&script_rider);
}
static void online_coop_body_borrow_target(void) {
    setup_coop_death();
    players[1].unit_index=NONE;
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    assert(!coop_spectate_nothing_to_watch(0));
    tick=92;assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==100);
    assert(!coop_spectate_nothing_to_watch(0));
    object_exists[0]=FALSE;
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,TRUE)==NONE);
    assert(coop_spectate_nothing_to_watch(0));

    reset(2);players[0].unit_index=NONE;players[1].unit_index=NONE;
    waiting_to_start=TRUE;
    assert(coop_spectate_camera_unit(0,TRUE,TRUE,FALSE)==NONE);
    coop_spectate_draw(0);
    assert(drawn_count==1&&wcsstr(drawn_text,L"JOINING THE GAME")!=NULL);
}
int main(void) {
#ifdef COOP_GRACE_ONLY
    online_coop_death_camera_grace();
    online_coop_second_death_hud_rider();
    online_coop_body_borrow_target();
    puts("PASS: network co-op corpse-view grace and lifecycle boundaries");
#else
    rates();clock_and_lifecycle();coop_blocked();cameras();online_coop_death_camera_grace();
    online_coop_second_death_hud_rider();
    online_coop_body_borrow_target();
    puts("PASS: fixed-tick delays, render-only/catch-up frames, pause/cinematics, clock reset,");
    puts("      2-4 player co-op, blocked retry/overflow, immediate retry and dead-camera fallback");
#endif
    return 0;
}
'''


def structure(source, name):
    start = source.index('struct ' + name + '\n{')
    return source[start:source.index('\n};', start) + 4] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--main-source', type=Path, default=ROOT / 'source/main/main.c')
    parser.add_argument('--coop-grace-only', action='store_true')
    args = parser.parse_args()
    main_source = args.main_source.read_text()
    camera_source = (ROOT / 'source/camera/dead_camera.c').read_text()
    camera_header = (ROOT / 'source/camera/dead_camera.h').read_text()
    players_source = (ROOT / 'source/game/players.c').read_text()
    coop_source = (ROOT / 'port/linux/game/coop_spectate.c').read_text()
    unit = PRELUDE
    for name in ('camera_control', 'dead_camera_command'):
        unit += structure(camera_source, name)
    unit += structure(camera_header, 'dead_camera')
    unit += '#define players_respawn_coop players_respawn_coop_real\n'
    unit += function(players_source, 'players_respawn_coop')
    unit += '\n#undef players_respawn_coop\nstatic boolean players_respawn_coop(void) {attempts++;return players_respawn_coop_real();}\n'
    for name in ('main_lost_map', 'main_respawn', 'main_reset_map', 'main_revert_map', 'main_death_timer_expired',
                 'main_lost_map_private', 'main_respawn_private'):
        unit += function(main_source, name)
    for name in ('dead_camera_new', 'player_has_allies', 'player_get_next_player_with_a_unit', 'dead_camera_update'):
        unit += function(camera_source, name)
    coop_globals_start = coop_source.index('static long coop_spectate_watched')
    coop_globals_end = coop_source.index('/* ---------- private code */', coop_globals_start)
    unit += coop_source[coop_globals_start:coop_globals_end]
    for name in ('next_living_player', 'next_pressed', 'unit_is_passenger', 'coop_spectate_unit'):
        unit += function(coop_source, name)
    if 'long coop_spectate_camera_unit(' in coop_source:
        unit += function(coop_source, 'coop_spectate_camera_unit')
    else:
        unit += r'''
static long coop_spectate_camera_unit(short local_player_index,boolean network_coop,
    boolean camera_available,boolean has_died) {
    (void)has_died;
    return network_coop && camera_available ? coop_spectate_unit(local_player_index) : NONE;
}
'''
    for name in ('coop_spectate_watching_rider', 'coop_spectate_nothing_to_watch', 'coop_spectate_draw'):
        unit += function(coop_source, name)
    if args.coop_grace_only:
        unit += '\n#define COOP_GRACE_ONLY 1\n'
    unit += TESTS
    cc = shutil.which(args.cc) or args.cc
    with tempfile.TemporaryDirectory(prefix='halo-death-timing-') as temp:
        c = Path(temp) / 'death.c'
        exe = Path(temp) / ('death.exe' if os.name == 'nt' else 'death')
        c.write_text(unit)
        flags = ['--target=i686-pc-windows-msvc', '-fuse-ld=lld'] if os.name == 'nt' else []
        subprocess.run([cc, *flags, '-std=c11', '-Wno-parentheses', '-O2', str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
