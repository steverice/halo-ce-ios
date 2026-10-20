"""Exercise the production controller chord without launching the game."""
from pathlib import Path
import shutil
import subprocess

from test_mp_synthetic_input import function

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''
#include <assert.h>
#include <math.h>
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef struct { WORD wButtons; BYTE bAnalogButtons[8]; short sThumbLX,sThumbLY,sThumbRX,sThumbRY; } XINPUT_GAMEPAD;
enum { XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
       XINPUT_GAMEPAD_BLACK, XINPUT_GAMEPAD_WHITE, XINPUT_GAMEPAD_LEFT_TRIGGER, XINPUT_GAMEPAD_RIGHT_TRIGGER };
#define XINPUT_GAMEPAD_DPAD_UP 1
#define XINPUT_GAMEPAD_DPAD_DOWN 2
#define XINPUT_GAMEPAD_BACK 32
#define XINPUT_GAMEPAD_LEFT_THUMB 64
#define XINPUT_GAMEPAD_RIGHT_THUMB 128
#define PITCH_CHORD_PUSH 0.7f
#define PITCH_CHORD_RELEASE 0.3f
#define LASER_CHORD_PULL 128
#define LASER_CHORD_RELEASE 40
static int aiming=1, laser, pitch, steady, follow;
static int halo_controller_hand_aiming(int player){ assert(player==0); return aiming; }
static void halo_controller_laser_cycle(void){ laser++; }
static void halo_controller_reticle_ease_toggle(void){}
static void halo_controller_arms_turn_limit_cycle(void){}
static void halo_controller_arms_follow_cycle(void){ follow++; }
static void halo_controller_origin_raise_step(int direction){ (void)direction; }
static void halo_aim_assist_step(int direction){ (void)direction; }
static void halo_controller_pitch_offset_step(int direction){ pitch+=direction; }
static void halo_aim_steady_cycle(void){ steady++; }
static void halo_controller_weapon_smoothing_toggle(void){}
static void halo_controller_arms_cycle(int direction){ (void)direction; }
static int config_boolean(const char *name){ (void)name; return @ENABLED@; }
'''


def build_and_run(tmp_path, name, enabled, main):
    compiler = shutil.which("clang") or shutil.which("cc")
    assert compiler, "a C compiler is required"
    source = (ROOT / "port/linux/src/xinput_sdl.c").read_text()
    chord = function(source, "static void aim_pitch_offset_chord(")
    source_file = tmp_path / (name + ".c")
    binary = tmp_path / name
    source_file.write_text(FIXTURE.replace("@ENABLED@", str(enabled)) + chord + main)
    compile_result = subprocess.run([compiler, "-std=gnu11", "-O1", "-fsanitize=address,undefined",
                                     str(source_file), "-lm", "-o", str(binary)], capture_output=True, text=True)
    assert compile_result.returncode == 0, compile_result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def test_aim_chord_is_ordinary_input_without_the_debug_setting(tmp_path):
    main = r'''
int main(void){
 XINPUT_GAMEPAD pad={0};
 pad.wButtons=XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_RIGHT_THUMB;
 pad.sThumbRY=30000;
 pad.sThumbLX=30000;
 pad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER]=255;
 pad.bAnalogButtons[XINPUT_GAMEPAD_A]=255;
 aim_pitch_offset_chord(&pad);
 assert(laser==0 && pitch==0 && steady==0 && follow==0);
 assert(pad.sThumbRY==30000 && pad.sThumbLX==30000 && (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB));
 assert(pad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER]==255 && pad.bAnalogButtons[XINPUT_GAMEPAD_A]==255);
 return 0;
}
'''
    build_and_run(tmp_path, "controller_chord_off", 0, main)


def test_aim_chord_consumes_buttons_until_release(tmp_path):
    main = r'''
int main(void){
 XINPUT_GAMEPAD pad={0};
 pad.wButtons=XINPUT_GAMEPAD_BACK;
 pad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER]=255;
 aim_pitch_offset_chord(&pad);
 assert(laser==1 && pad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER]==0);
 pad.wButtons=0;
 pad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER]=255;
 aim_pitch_offset_chord(&pad);
 assert(laser==1 && pad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER]==0);
 pad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER]=0;
 aim_pitch_offset_chord(&pad);
 pad.wButtons=XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_RIGHT_THUMB;
 pad.sThumbRY=30000;
 aim_pitch_offset_chord(&pad);
 assert(pitch==-1 && steady==1 && pad.sThumbRY==0 && !(pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB));
 pad.wButtons=XINPUT_GAMEPAD_BACK;
 pad.bAnalogButtons[XINPUT_GAMEPAD_A]=255;
 aim_pitch_offset_chord(&pad);
 assert(follow==1 && pad.bAnalogButtons[XINPUT_GAMEPAD_A]==0);
 return 0;
}
'''
    build_and_run(tmp_path, "controller_chord", 1, main)


KNOB_FIXTURE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef struct { WORD wButtons; BYTE bAnalogButtons[8]; short sThumbRY; } XINPUT_GAMEPAD;
#define XINPUT_GAMEPAD_DPAD_UP 1
#define XINPUT_GAMEPAD_DPAD_DOWN 2
#define XINPUT_GAMEPAD_DPAD_LEFT 4
#define XINPUT_GAMEPAD_DPAD_RIGHT 8
#define XINPUT_GAMEPAD_BACK 32
#include "knob_cycle.h"
static char applied[64], message[96], logged[64];
static int applications;
static int config_boolean(const char *name){ (void)name; return @ENABLED@; }
static long config_integer(const char *name){ return !strcmp(name, "display.shadow_map_size") ? 512 : 0; }
static const char *config_string(const char *name){ (void)name; return "max"; }
static int halo_knob_apply(const char *name, const char *value){ snprintf(applied, sizeof(applied), "%s=%s", name, value); applications++; return 1; }
static void halo_knob_message(const char *text){ snprintf(message, sizeof(message), "%s", text); }
static void platform_log(const char *format, const char *name, const char *value){ (void)format; snprintf(logged, sizeof(logged), "knob: %s=%s", name, value); }
'''


def run_knob_chord(tmp_path, name, enabled, main):
    compiler = shutil.which("clang") or shutil.which("cc")
    assert compiler, "a C compiler is required"
    source = (ROOT / "port/linux/src/xinput_sdl.c").read_text()
    chord = function(source, "static void knob_current(") + "\n" + function(source, "static void knob_cycle_chord(")
    source_file = tmp_path / (name + ".c")
    source_file.write_text(KNOB_FIXTURE.replace("@ENABLED@", str(enabled)) + chord + main)
    binary = tmp_path / name
    compiled = subprocess.run([compiler, "-std=gnu11", "-O1", "-fsanitize=address,undefined", "-I" + str(ROOT / "port/linux/src"),
                               str(source_file), "-o", str(binary)], capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def test_knob_chord_consumes_nothing_without_the_debug_setting(tmp_path):
    run_knob_chord(tmp_path, "knob_off", 0, r'''
int main(void){
 XINPUT_GAMEPAD pad={0};
 pad.wButtons=XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_DPAD_RIGHT|XINPUT_GAMEPAD_DPAD_DOWN;
 knob_cycle_chord(&pad);
 assert(pad.wButtons==(XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_DPAD_RIGHT|XINPUT_GAMEPAD_DPAD_DOWN));
 assert(!applications && !message[0]);
 return 0;
}
''')


def test_knob_chord_steps_a_value_and_keeps_the_d_pad_from_the_game(tmp_path):
    run_knob_chord(tmp_path, "knob_on", 1, r'''
int main(void){
 XINPUT_GAMEPAD pad={0};
 /* without Back the D-pad is the game's */
 pad.wButtons=XINPUT_GAMEPAD_DPAD_RIGHT;
 knob_cycle_chord(&pad);
 assert(pad.wButtons==XINPUT_GAMEPAD_DPAD_RIGHT && !applications);
 /* Back + down chooses the shadow map (the second knob), changing nothing */
 pad.wButtons=XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_DPAD_DOWN;
 knob_cycle_chord(&pad);
 assert(pad.wButtons==XINPUT_GAMEPAD_BACK && !applications && !strcmp(message, "shadow map 512"));
 /* a press held across frames steps once */
 pad.wButtons=XINPUT_GAMEPAD_BACK|XINPUT_GAMEPAD_DPAD_LEFT;
 knob_cycle_chord(&pad);
 knob_cycle_chord(&pad);
 assert(applications==1 && !strcmp(applied, "display.shadow_map_size=256") && !strcmp(logged, "knob: display.shadow_map_size=256"));
 assert(!strcmp(message, "shadow map 256") && pad.wButtons==XINPUT_GAMEPAD_BACK);
 return 0;
}
''')
