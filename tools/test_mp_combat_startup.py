"""Execute the real scripted-action dispatch guards during private match setup."""
from pathlib import Path
import shutil
import subprocess
from unittest import SkipTest

from test_mp_synthetic_input import function

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "port/linux/game/network_test.c").read_text()


def condition_before(position):
    start = SOURCE.rfind("if (", 0, position) + 3
    depth = 0
    for index in range(start, len(SOURCE)):
        depth += (SOURCE[index] == "(") - (SOURCE[index] == ")")
        if not depth:
            return SOURCE[start:index + 1]
    raise ValueError("Missing scripted dispatch condition")


def test_setup_withholds_scripted_damage_kills_and_gather(tmp_path):
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SkipTest("A C compiler is required for the combat startup probe")
    helper = (function(SOURCE, "static int network_test_suite_combat_ready(")
              if "static int network_test_suite_combat_ready(" in SOURCE
              else "static int network_test_suite_combat_ready(void) {return 1;}")
    shoot = condition_before(SOURCE.index("network_test_shoot();", SOURCE.index("void network_test_update(")))
    gather = condition_before(SOURCE.index("network_test_gather(network_test.vehicle_time", SOURCE.index("void network_test_update(")))
    kill = condition_before(SOURCE.index("network_test.kill_interval >", SOURCE.index("void network_test_update(")))
    source = r'''
#include <assert.h>
#define TICKS_PER_SECOND 30
#define MAX(a,b) ((a)>(b)?(a):(b))
enum {_network_test_host=1};
static struct {int suite_owned,mode;float shoot_interval,kill_interval;} network_test;
static int enabled=1,combat,tick;
static int benchmark_mp_arena_enabled(void) {return enabled;}
static int benchmark_mp_arena_combat_ready(void) {return combat;}
static int game_time_get(void) {return tick;}
__HELPER__
static int shoot(void) {return __SHOOT__;}
static int gather(void) {return __GATHER__;}
static int kill_player(void) {return __KILL__;}
int main(void) {
    network_test.suite_owned=1;network_test.mode=_network_test_host;
    network_test.shoot_interval=5;network_test.kill_interval=25;
    tick=150;assert(!shoot());tick=120;assert(!gather());tick=750;assert(!kill_player());
    combat=1;
    tick=150;assert(shoot());tick=120;assert(gather());tick=750;assert(kill_player());
    combat=0; /* a new arena must return to setup ownership */
    tick=150;assert(!shoot());
    network_test.suite_owned=0; /* ordinary raw network tests keep their schedule */
    tick=150;assert(shoot());tick=120;assert(gather());tick=750;assert(kill_player());
    network_test.suite_owned=1;enabled=0; /* existing local-controller suite */
    tick=150;assert(shoot());
    return 0;
}
'''.replace("__HELPER__", helper).replace("__SHOOT__", shoot).replace("__GATHER__", gather).replace("__KILL__", kill)
    path, binary = tmp_path / "startup.c", tmp_path / "startup"
    path.write_text(source)
    subprocess.run([compiler, "-O2", "-std=gnu89", "-fms-extensions", "-Wno-c99-extensions",
                    "-fsanitize=address,undefined", str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
