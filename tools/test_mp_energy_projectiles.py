"""Run the actual successful-projectile and misfire branches for suite evidence."""
from pathlib import Path
import shutil
import subprocess
from unittest import SkipTest

import pytest

from test_mp_synthetic_input import function

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "source/items/weapons.c").read_text()

HARNESS = r'''
#include <assert.h>
#include "network_test_suite.h"
#define NONE -1
typedef struct {float x,y,z;} real_point3d;
static struct {int magazine_index,rounds_per_shot,firing_noise;float age_generated_per_round;} trigger_data={0,0,0,0.002f},*trigger_definition=&trigger_data;
static struct {struct {struct {int count;} magazines;} weapon;} definition={{{1}}},*weapon_definition=&definition;
static struct {struct {int alternate_shots_loaded;} weapon;} body,*weapon=&body;
static int next_object=7,evidence,recorded_owner,produced,produced_owner,produced_weapon,produced_projectile;
static long owner_object_index=42,target_object_index=NONE,weapon_index=3;
static short trigger_index;
static int inside_bsp,tracer=1;
static struct { struct {float i,j,k;} forward; } data;
static struct projectile { struct {real_point3d position;} object; } projectile;
static struct projectile *object_get(long index) {(void)index;return &projectile;}
void halo_projectile_spawned(long index,long owner,const float *position,const float *forward)
{(void)index;(void)owner;(void)position;(void)forward;}
static long object_new(void *p) {(void)p;return next_object;}
static void unit_get_camera_position(long i,real_point3d*p) {(void)i;p->x=p->y=p->z=0;}
static void object_force_inside_bsp(long i,const real_point3d*p) {(void)i;(void)p;}
static void projectile_set_target_object_index(long i,long t) {(void)i;(void)t;}
static void projectile_kill_tracer(long i) {(void)i;}
void network_test_suite_projectile_created(long owner,int energy) {if(energy){evidence++;recorded_owner=(int)owner;}}
void network_test_suite_weapon_projectile_created(long owner,long firing_weapon,long projectile) {produced++;produced_owner=(int)owner;produced_weapon=(int)firing_weapon;produced_projectile=(int)projectile;}
static void create(void) {long projectile_object_index=object_new(&data);__CREATED__}
static void trigger_create_projectiles(long w,short t) {(void)w;(void)t;create();}
#define _ai_unit_effect_shooting 1
static void ai_handle_unit_effect(long owner,int effect,int noise) {(void)owner;(void)effect;(void)noise;}
static void fire(int misfired,int loads_alternate_ammunition) {__MISFIRE__}
int main(void) {
    assert(network_test_suite_energy_trigger(0,1,0,0.002f)); /* actual plasma pistol */
    assert(network_test_suite_energy_trigger(NONE,0,0,0.002f));
    assert(!network_test_suite_energy_trigger(0,1,1,0.002f)); /* hybrid */
    assert(!network_test_suite_energy_trigger(0,1,1,0.0f)); /* bullet */
    assert(!network_test_suite_energy_trigger(-2,1,0,0.002f));
    assert(!network_test_suite_energy_trigger(1,1,0,0.002f));
    assert(!network_test_suite_energy_trigger(NONE,3,0,0.002f));
    assert(!network_test_suite_energy_trigger(NONE,0,0,__builtin_nanf("")));
    assert(!network_test_suite_energy_trigger(NONE,0,0,__builtin_inff()));
    fire(1,0);assert(!evidence&&!produced); /* real misfire branch cannot create a projectile */
    next_object=NONE;fire(0,0);assert(!evidence&&!produced);
    next_object=7;fire(0,1);assert(!evidence); /* charging alternate ammunition */
    fire(0,0);assert(evidence==1&&recorded_owner==42);assert(produced==1&&produced_owner==42&&produced_weapon==3&&produced_projectile==7);
    trigger_definition->rounds_per_shot=1;fire(0,0);assert(evidence==1&&produced==2); /* hybrid consumes rounds */
    trigger_definition->rounds_per_shot=0;trigger_definition->magazine_index=1;
    fire(0,0);assert(evidence==1); /* malformed index, even with positive age */
    trigger_definition->magazine_index=NONE;fire(0,0);assert(evidence==2);
    trigger_definition->age_generated_per_round=0;fire(0,0);assert(evidence==2);
    return 0;
}
'''


@pytest.mark.parametrize("strict_math", [False, True])
def test_successful_owned_battery_projectiles_exclude_misfires_and_failed_creation(tmp_path, strict_math):
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SkipTest("A C compiler is required for the energy projectile probe")
    created = function(SOURCE, "if (projectile_object_index!=NONE)")
    fire = function(SOURCE, "static void weapon_trigger_fire(\n\tlong weapon_index,\n\tshort trigger_index)\n{")
    misfire = function(fire, "if (!misfired)")
    path, binary = tmp_path / "energy.c", tmp_path / "energy"
    path.write_text(HARNESS.replace("__CREATED__", created).replace("__MISFIRE__", misfire))
    flags = ["-D__STRICT_ANSI__"] if strict_math else []
    subprocess.run([compiler, "-O2", "-std=gnu89", *flags, "-fms-extensions", "-Wno-c99-extensions",
                    "-fsanitize=address,undefined", "-iquote", str(ROOT / "port/linux/include"),
                    str(path), "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
