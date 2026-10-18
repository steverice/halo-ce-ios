"""Feed the observer stick through the actual controller and biped consumers."""
from pathlib import Path
import re
import shutil
import subprocess
from unittest import SkipTest

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for position in range(opening, len(source)):
        depth += (source[position] == "{") - (source[position] == "}")
        if depth == 0:
            return source[start:position + 1]
    raise ValueError(signature)


def test_observer_movement_survives_actual_consumers(tmp_path):
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SkipTest("A C compiler is required for the observer consumer probe")
    xbox = (ROOT / "source/input/input_xbox.c").read_text()
    abstraction = (ROOT / "source/input/input_abstraction.c").read_text()
    control = (ROOT / "source/game/player_control.c").read_text()
    bipeds = (ROOT / "source/units/bipeds.c").read_text()
    math = (ROOT / "source/math/real_math.h").read_text()
    xinput = (ROOT / "port/linux/src/xinput_sdl.c").read_text()
    dead_range = xbox.index("GAMEPAD_STICK_DEAD_RANGE")
    dead_enum_start = xbox.rfind("enum", 0, dead_range)
    dead_enum_end = xbox.index("};", dead_range) + 2
    layout_start = abstraction.index("case _joystick_controls_default:", abstraction.index("void input_abstraction_update("))
    layout_end = abstraction.index("case _joystick_controls_southpaw:", layout_start)
    source = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "benchmark_mp_arena.h"
#define SHORT_MAX 32767
#define SHORT_MIN (-32768)
#define MAX(a,b) ((a)>(b)?(a):(b))
#define PIN(v,a,b) ((v)<(a)?(a):(v)>(b)?(b):(v))
#define sine sinf
#define cosine cosf
#define arctangent atan2f
#define _gamepad_stick_left 0
#define _gamepad_stick_right 1
#define _joystick_controls_default 0
#define _gamepad_binary_button_dpad_left 0
#define _gamepad_binary_button_dpad_right 1
#define _gamepad_binary_button_dpad_up 2
#define _gamepad_binary_button_dpad_down 3
#define XINPUT_GAMEPAD_RIGHT_TRIGGER 0
#define XINPUT_GAMEPAD_A 1
#define XINPUT_GAMEPAD_LEFT_TRIGGER 2
typedef float real;
typedef struct {real x,y;} real_point2d;
typedef struct {real i,j,k;} real_vector3d;
typedef struct {short sThumbLX,sThumbLY,sThumbRX,sThumbRY;unsigned char bAnalogButtons[8];} XINPUT_GAMEPAD;
static struct mp_benchmark_probe_latch mp_probe_latches[4];
static int mp_probe_latches_ready=1;
static long tick;
static real requested_yaw=0.1f,requested_pitch=0.05f;static int requested_inversion;
void mp_benchmark_probe_reset_controllers(void) { }
long mp_benchmark_probe_tick(void) { return tick; }
void mp_benchmark_probe_sample(long t,short p,struct mp_benchmark_probe_latch*l,struct mp_benchmark_probe_pad*out) {
    (void)t;(void)p;(void)l;memset(out,0,sizeof(*out));
}
int benchmark_mp_arena_gamepad(short port,long t,struct mp_benchmark_probe_pad*out) {
    assert(port==0);return benchmark_mp_arena_observer_pad(requested_yaw,requested_pitch,requested_inversion,t,out);
}
__DEAD_RANGE__
__XINPUT__
__DEAD_ZONE__
__NORMALIZATION__
__MAGNITUDE__
static real consumed_axes(XINPUT_GAMEPAD *raw,int invert_look,real *before_gate,real *yaw,real *pitch) {
    struct {struct {short x,y;} sticks[2];unsigned char buttons[32];} pad={0},*gamepad=&pad;
    struct {real strafe,forward_movement,yaw,pitch;} value={0},*state=&value;
    struct {struct {real i,j;} throttle;} action={0},*input=&action;
    struct {struct {real_vector3d throttle;} unit;} unit={0},*biped=&unit;
    real_vector3d zero={0},*global_zero_vector3d=&zero;
    real_point2d left_stick,right_stick;real left_angle,right_angle;
    gamepad->sticks[_gamepad_stick_left].x=fix_dead_zone(raw->sThumbLX,GAMEPAD_STICK_DEAD_RANGE);
    gamepad->sticks[1].x=fix_dead_zone(raw->sThumbRX,GAMEPAD_STICK_DEAD_RANGE);gamepad->sticks[1].y=fix_dead_zone(raw->sThumbRY,GAMEPAD_STICK_DEAD_RANGE);
    __LEFT_STICK__
    __RIGHT_STICK__
    switch(0) { __LAYOUT__ }
    *yaw=state->yaw;*pitch=state->pitch;
    __THROTTLE__
    biped->unit.throttle.i=input->throttle.i;biped->unit.throttle.j=input->throttle.j;
    *before_gate=biped->unit.throttle.j;
    __BIPED_GATE__
    return biped->unit.throttle.j;
}
int main(void) {
    int i,inverted;real previous=0;
    for(i=0;i<2;i++) {
        XINPUT_GAMEPAD pad={0};real before,after,yaw,pitch;
        tick=i?40:10;mp_benchmark_probe_gamepad(&pad,0);
        after=consumed_axes(&pad,0,&before,&yaw,&pitch);
        printf("observer raw %d normalized %.6f biped %.6f yaw %.6f pitch %.6f\n",pad.sThumbLX,before,after,yaw,pitch);fflush(stdout);
        assert(fabsf(yaw-requested_yaw)<0.0001f&&fabsf(pitch-requested_pitch)<0.0001f);
        assert(fabsf(after)>=0.1f);
        if(i)assert(previous*after<0);previous=after;
    }
    for(inverted=0;inverted<2;inverted++) {
        XINPUT_GAMEPAD pad={0};real before,yaw,pitch;
        requested_inversion=inverted;requested_yaw=-0.1f;requested_pitch=-0.05f;
        mp_benchmark_probe_gamepad(&pad,0);consumed_axes(&pad,inverted,&before,&yaw,&pitch);
        assert(fabsf(yaw-requested_yaw)<0.0001f&&fabsf(pitch-requested_pitch)<0.0001f);
        requested_yaw=requested_pitch=0;mp_benchmark_probe_gamepad(&pad,0);
        assert(!pad.sThumbRX&&!pad.sThumbRY);consumed_axes(&pad,inverted,&before,&yaw,&pitch);
        assert(yaw==0&&pitch==0);
    }
    {
        XINPUT_GAMEPAD pad={0};real before,yaw,pitch;
        requested_yaw=requested_pitch=1.0f;requested_inversion=0;
        mp_benchmark_probe_gamepad(&pad,0);consumed_axes(&pad,0,&before,&yaw,&pitch);
        assert(fabsf(yaw-0.65f)<0.0001f&&fabsf(pitch-0.65f)<0.0001f);
    }
    return 0;
}
'''
    replacements = {
        "__DEAD_RANGE__": xbox[dead_enum_start:dead_enum_end],
        "__XINPUT__": function(xinput, "static void mp_benchmark_probe_gamepad("),
        "__DEAD_ZONE__": function(xbox, "short fix_dead_zone("),
        "__NORMALIZATION__": re.search(r"static real const gamepad_axis_normalization_scale = [^;]+;", abstraction)[0],
        "__MAGNITUDE__": function(math, "__inline real magnitude_squared3d("),
        "__LEFT_STICK__": function(abstraction, "{\n\t\t\t\treal scale;"),
        "__LAYOUT__": abstraction[layout_start:layout_end],
        "__RIGHT_STICK__": function(abstraction[abstraction.rfind("{", 0, abstraction.index("right_angle =")):], "{"),
        "__THROTTLE__": re.search(r"input->throttle.i = input_state->forward_movement;\s+input->throttle.j = input_state->strafe;", control)[0].replace("input_state", "state"),
        "__BIPED_GATE__": re.search(r"if \(magnitude_squared3d\(&biped->unit.throttle\)<0.1f\*0.1f\)\s+biped->unit.throttle = \*global_zero_vector3d;", bipeds)[0],
    }
    for key, value in replacements.items():
        source = source.replace(key, value)
    path, binary = tmp_path / "observer.c", tmp_path / "observer"
    path.write_text(source)
    subprocess.run([compiler, "-O2", "-std=gnu89", "-fms-extensions", "-fsanitize=address,undefined", "-iquote", str(ROOT / "port/linux/include"), str(path), "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
