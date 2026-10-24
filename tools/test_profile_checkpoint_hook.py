"""A successful checkpoint load offers a profile break only when reviving a local death."""
from pathlib import Path
import subprocess

from test_profile_game_hooks import compile_probe, function

ROOT = Path(__file__).resolve().parents[1]


def test_checkpoint_revival_boundary(tmp_path):
    actual = function((ROOT / "source/saved games/game_state.c").read_text(), "void game_state_revert(")
    binary = compile_probe(tmp_path, r'''
#include <assert.h>
typedef int boolean;
#define NONE (-1)
#define FALSE 0
#define TEST_FLAG(value, bit) ((value) & (1U << (bit)))
enum { _object_dead_bit=0, CONFIG_PROFILE_LOCAL_RESPAWN=5 };
static struct { int saved_game_valid; } game_state_globals;
static int recover_saved_games_hack, loaded, read_ok=1, calls, resets, before, after;
static int old_player=1, new_player=1, old_unit=NONE, new_unit=1, old_dead, new_dead;
static struct { int unit_index; } player;
static struct { struct { unsigned damage_flags; } object; } object;
static int local_player_get_player_index(int slot) { assert(slot==0); return loaded ? new_player : old_player; }
static void *player_get(int index);
/* Keep production field access typed while checking which side of the load it reads. */
#define player_get(index) (assert((index)==1), player.unit_index=loaded ? new_unit : old_unit, &player)
#define object_get(index) (assert((index)==1), object.object.damage_flags=loaded ? new_dead : old_dead, &object)
static void main_reset_map(void) { resets++; }
static void game_state_note_event(const char *name) { (void)name; }
static void game_state_call_before_load_procs(void) { before++; }
static void game_state_call_after_load_procs(void) { after++; }
static int game_state_read_from_file(void) { loaded=1; return read_ok; }
static void config_profile_game_safe_point(int point) { assert(point==5 && loaded && !after); calls++; }
''' .replace('static void *player_get(int index);\n', '') + actual + r'''
static void reset(void) {
    loaded=calls=resets=before=after=0; read_ok=1;
    game_state_globals.saved_game_valid=1;
    old_player=new_player=1; old_unit=NONE; new_unit=1; old_dead=new_dead=0;
}
int main(void) {
    reset(); game_state_revert(); assert(calls==1 && before==1 && after==1);
    reset(); old_unit=1; old_dead=1; game_state_revert(); assert(calls==1);
    reset(); old_unit=1; game_state_revert(); assert(!calls);
    reset(); old_player=NONE; game_state_revert(); assert(!calls);
    reset(); new_player=NONE; game_state_revert(); assert(!calls);
    reset(); new_unit=NONE; game_state_revert(); assert(!calls);
    reset(); new_dead=1; game_state_revert(); assert(!calls);
    reset(); read_ok=0; game_state_revert(); assert(!calls && resets==1 && after==1);
    reset(); game_state_globals.saved_game_valid=0; game_state_revert(); assert(!calls && resets==1 && !before);
}
''')
    subprocess.run([str(binary)], check=True)
