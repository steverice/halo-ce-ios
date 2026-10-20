"""Exercise the production controller chord without launching the game."""
from pathlib import Path
import shutil
import subprocess

from test_mp_synthetic_input import function

ROOT = Path(__file__).resolve().parents[1]


def test_aim_chord_consumes_buttons_until_release(tmp_path):
    compiler = shutil.which("clang") or shutil.which("cc")
    assert compiler, "a C compiler is required"
    source = (ROOT / "port/linux/src/xinput_sdl.c").read_text()
    chord = function(source, "static void aim_pitch_offset_chord(")
    fixture = r'''
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
'''
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
    source_file = tmp_path / "controller_chord.c"
    binary = tmp_path / "controller_chord"
    source_file.write_text(fixture + chord + main)
    compile_result = subprocess.run([compiler, "-std=gnu11", "-O1", "-fsanitize=address,undefined",
                                     str(source_file), "-lm", "-o", str(binary)], capture_output=True, text=True)
    assert compile_result.returncode == 0, compile_result.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
