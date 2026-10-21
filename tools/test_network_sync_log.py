"""Probe the production network log without game assets or an Apple build."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_network_sync_windows(tmp_path):
    source = (ROOT / "port/linux/game/network_distributed.c").read_text()
    start = source.index("void platform_log(char const *format")
    end = source.index("/* ---------- shared (network_distributed.h) */", start)
    actual = source[start:end]
    code = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define TICKS_PER_SECOND 30
#define csmemset memset
static long tick, damage_rejected;
static int lines;
static char line[512];
static struct { long corrections; } distributed_statistics;
static long game_time_get(void) { return tick; }
''' + actual + r'''
void platform_log(char const *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    lines++;
}
void network_damage_statistics(long *sent, long *dealt, long *rejected, long *replayed) {
    *sent = *dealt = *replayed = 0;
    *rejected = damage_rejected;
}
int main(void) {
    distributed_statistics.corrections = 28;
    damage_rejected = 9;
    distributed_sync_log_reset();
    distributed_statistics.corrections += 12;
    distributed_count_prediction_rejected();
    distributed_count_resync();
    damage_rejected += 2;
    for (tick = 0; tick < 900; tick++) distributed_sync_log_tick();
    assert(lines == 0);
    distributed_sync_log_tick();
    assert(lines == 1);
    assert(!strcmp(line, "net sync: 30 s window, corrections 12 (total 12), rejected 1 (total 1), resyncs 1 (total 1), damage rejected 2 (total 2)"));
    distributed_sync_log_tick(); /* paused or repeated tick */
    assert(lines == 1);
    distributed_statistics.corrections += 3;
    distributed_count_prediction_rejected();
    tick = 1799; distributed_sync_log_tick(); assert(lines == 1);
    tick = 1800; distributed_sync_log_tick(); assert(lines == 2);
    assert(!strcmp(line, "net sync: 30 s window, corrections 3 (total 15), rejected 1 (total 2), resyncs 0 (total 1), damage rejected 0 (total 2)"));
    tick = 2700; distributed_sync_log_tick(); assert(lines == 3);
    assert(!strcmp(line, "net sync: 30 s window, corrections 0 (total 15), rejected 0 (total 2), resyncs 0 (total 1), damage rejected 0 (total 2)"));
    distributed_sync_log_reset(); /* next map */
    tick = 899; distributed_sync_log_tick(); assert(lines == 3);
    tick = 900; distributed_sync_log_tick(); assert(lines == 4);
    assert(!strcmp(line, "net sync: 30 s window, corrections 0 (total 0), rejected 0 (total 0), resyncs 0 (total 0), damage rejected 0 (total 0)"));
    tick = 0; distributed_sync_log_tick(); assert(lines == 4); /* clock rewind */
    tick = 900; distributed_sync_log_tick(); assert(lines == 5);
    assert(!strcmp(line, "net sync: 30 s window, corrections 0 (total 0), rejected 0 (total 0), resyncs 0 (total 0), damage rejected 0 (total 0)"));
}
'''
    path = tmp_path / "network_sync.c"
    binary = tmp_path / "network_sync"
    path.write_text(code)
    subprocess.run(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_logging_runs_for_host_and_client_without_network_test():
    source = (ROOT / "port/linux/game/network_distributed.c").read_text()
    start = source.index("void network_distributed_tick(")
    end = source.index("\n/*", start)
    tick = source[start:end]
    assert "if (connection == _game_connection_network_server || connection == _game_connection_network_client)\n\t\tdistributed_sync_log_tick();" in tick
    assert tick.index("distributed_batches_flush();") < tick.index("distributed_sync_log_tick();")
    start = source.index("void network_distributed_new_game(")
    assert "distributed_sync_log_reset();" in source[start:source.index("\n/*", start)]
    assert "network_test" not in tick
