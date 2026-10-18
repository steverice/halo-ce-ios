"""Exercise the actual checkpoint handlers across render rates and tick batches."""
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def function(source, name):
    match = re.search(r"^(?:static )?[\w *]+\b" + name + r"\(\s*[^;{]*\)\s*\{", source, re.M)
    if not match:
        raise ValueError(f"Function not found: {name}")
    start = source.index("{", match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


PRELUDE = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef int boolean;
#define TRUE 1
#define FALSE 0
#define NONE (-1)
enum { _game_connection_local, _game_connection_network_client, _game_connection_network_server };
static struct {
    boolean saving_map, save_map_safely, save_map_timeout, save_map_completed;
    long ticks_until_next_save_check, ticks_unable_to_save;
    short safe_intervals;
    long connection, switch_to_structure_bsp_index;
    boolean won_map, defer_map_change, revert_map, skip_cinematic;
    boolean reset_map, save_core, load_core, lost_map;
    short loss_timer;
} main_globals;
static long tick, main_loss_last_tick, main_save_last_tick;
static boolean main_save_clock_valid;
static int paused, safe, isolated, initialized = 1, save_count, check_count, debug_game_save;
static int __attribute__((unused)) game_time_initialized(void) { return initialized; }
static long game_time_get(void) { assert(initialized); return tick; }
static int game_time_get_paused(void) { return paused; }
static int game_safe_to_save(void) { check_count++; return safe; }
static void players_note_checkpoint(void) { save_count++; }
static void hud_autosave(int enabled) { (void)enabled; }
static void console_printf(int a, const char *b) { (void)a; (void)b; }
static unsigned char input_replay_sim_isolated(void) { return isolated; }
static boolean main_steps_each_tick(void);
'''

TESTS = r'''
static void reset(void) {
    memset(&main_globals, 0, sizeof(main_globals));
    main_globals.connection = _game_connection_local;
    main_globals.switch_to_structure_bsp_index = NONE;
    tick = main_loss_last_tick = main_save_last_tick = paused = isolated = save_count = check_count = 0;
    main_save_clock_valid = FALSE;
    initialized = 1;
    safe = 1;
}
static long run_rate(int hz, int connection, int replay_isolated) {
    int frame, next_tick = 0;
    reset();
    main_globals.connection = connection;
    isolated = replay_isolated;
    main_save_map_safe();
    for (frame = 1; frame <= hz * 10 && !save_count; frame++) {
        int target = frame * 30 / hz;
        while (next_tick < target) {
            tick = ++next_tick;
            if (main_tick_end()) break;
        }
        if (main_globals.saving_map && !main_steps_each_tick()) main_save_map_private();
        if (main_globals.save_map_completed) {
            assert(save_count == 1);
            return tick;
        }
    }
    assert(!"checkpoint never completed");
    return -1;
}
static void rates(void) {
    int hz[] = {30, 60, 90, 120};
    int i;
    long reference = run_rate(30, _game_connection_local, 0);
    assert(reference == 34); /* checks at ticks 1, 12, 23, 34 */
    for (i = 0; i < 4; i++) {
        assert(run_rate(hz[i], _game_connection_local, 0) == reference);
        assert(run_rate(hz[i], _game_connection_network_server, 0) == reference);
        assert(run_rate(hz[i], _game_connection_network_server, 1) == reference);
        assert(run_rate(hz[i], _game_connection_network_client, 0) == reference);
        assert(run_rate(hz[i], _game_connection_network_client, 1) == reference);
    }
    assert(run_rate(15, _game_connection_local, 0) == 38); /* observed end of each two-tick frame */
}
static void boundaries(void) {
    int i;
    reset(); main_save_map_safe();
    for (i = 1; i <= 33; i++) { tick = i; assert(!main_tick_end()); main_save_map_private(); }
    assert(!save_count && check_count == 3);
    tick = 34; assert(!main_tick_end()); main_save_map_private(); assert(save_count == 1);
    reset(); main_save_map_safe(); safe = 0;
    for (i = 1; i <= 23; i++) { tick = i; main_tick_end(); main_save_map_private(); }
    assert(!save_count && main_globals.safe_intervals == 0);
    safe = 1;
    for (; i <= 67; i++) { tick = i; main_tick_end(); if (main_globals.saving_map) main_save_map_private(); }
    assert(save_count == 1); /* unsafe results never count as safe windows */
    reset(); main_save_map_safe(); safe = 0;
    for (i = 1; i <= 240; i++) { tick = i; main_tick_end(); main_save_map_private(); }
    assert(main_globals.saving_map && !save_count);
    tick = 241; main_tick_end(); main_save_map_private(); assert(!main_globals.saving_map);
    reset(); main_save_map_no_timeout(); safe = 0;
    for (i = 1; i <= 500; i++) { tick = i; main_tick_end(); main_save_map_private(); }
    assert(main_globals.saving_map && !save_count);
    reset(); main_save_map_nonsafe(); main_save_map_private();
    assert(save_count == 1 && main_globals.save_map_completed);
    reset(); main_save_map_safe(); safe = 0; tick = 241; main_save_map_private();
    assert(!main_globals.saving_map && !save_count);
}
static void lifecycle(void) {
    int i;
    reset(); main_save_map_safe();
    for (i = 0; i < 100; i++) { assert(!main_tick_end()); main_save_map_private(); }
    assert(!check_count && !main_globals.ticks_unable_to_save);
    paused = 1; tick = 100; assert(!main_tick_end()); main_save_map_private();
    assert(!check_count && !main_globals.ticks_unable_to_save);
    paused = 0; tick = 101; main_tick_end(); main_save_map_private();
    assert(check_count == 1 && main_globals.ticks_unable_to_save == 1);
    main_save_map_safe(); /* a new safe request restarts its window */
    assert(main_globals.ticks_unable_to_save == 0);
    main_save_map_no_timeout(); assert(!main_globals.save_map_timeout);
    assert(main_globals.ticks_unable_to_save == 0);
    tick = 0; main_tick_end(); main_save_map_private(); /* a reverted clock cannot credit elapsed history */
    assert(main_globals.ticks_unable_to_save <= 1);
    reset(); isolated = 1; main_save_map_safe();
    for (i = 1; i <= 34; i++) { tick = i; main_tick_end(); }
    assert(save_count == 1);
    reset(); isolated = 1; safe = 0; main_save_map_no_timeout();
    for (i = 1; i <= 500; i++) { tick = i; main_tick_end(); }
    assert(main_globals.saving_map && main_globals.ticks_unable_to_save == 500);
    reset(); initialized = 0; main_save_map_safe();
    initialized = 1; tick = 100; main_save_map_private();
    assert(!check_count && !main_globals.ticks_unable_to_save);
}
static void isolation_transition(void) {
    int i;
    reset(); safe = 0; main_save_map_safe();
    for (i = 1; i <= 10; i++) { tick = i; main_save_map_private(); }
    assert(main_globals.ticks_unable_to_save == 10);
    isolated = 1;
    for (i = 11; i <= 20; i++) { tick = i; assert(!main_tick_end()); }
    assert(main_globals.ticks_unable_to_save == 20);
    isolated = 0;
    tick = 21; main_save_map_private();
    assert(main_globals.ticks_unable_to_save == 21);
    reset(); safe = 0; isolated = 1; main_save_map_safe();
    for (i = 1; i <= 10; i++) { tick = i; assert(!main_tick_end()); }
    isolated = 0; tick = 11; main_save_map_private();
    assert(main_globals.ticks_unable_to_save == 11);
}
int main(void) {
    rates(); boundaries(); lifecycle(); isolation_transition();
    puts("PASS: checkpoint timing across render rates and tick batches");
    return 0;
}
'''


def main():
    source = (ROOT / "source/main/main.c").read_text()
    names = ["main_save_map_nonsafe", "main_save_map_safe", "main_save_map_no_timeout", "main_save_map_private", "main_steps_each_tick", "main_tick_end"]
    code = PRELUDE + "\n".join(function(source, name) for name in names) + TESTS
    with tempfile.TemporaryDirectory(prefix="checkpoint-timing-") as tmp:
        path = Path(tmp) / "test.c"
        binary = Path(tmp) / "test"
        path.write_text(code)
        subprocess.run(["xcrun", "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
