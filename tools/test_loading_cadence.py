"""Exercise production loading polling against async completion and VBlank clocks."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMPILER = shutil.which("clang") or shutil.which("cc")


def test_blocking_cache_poll_keeps_loading_frames_on_consecutive_refreshes():
    cache_source = (ROOT / "source/cache/cache_files_decompress_windows.c").read_text()
    cache_start = cache_source.index("short cache_copy_get_status(")
    cache_end = cache_source.index("\nvoid cache_copy_end(", cache_start)
    cache_poll = cache_source[cache_start:cache_end]

    game_source = (ROOT / "source/game/game.c").read_text()
    map_loader = game_source[game_source.index("void game_precache_new_map("):]
    loop_start = map_loader.index("\t\t\tdo\n")
    loop_end = map_loader.index("\n\t\t\twhile (!map_status);", loop_start) + len(
        "\n\t\t\twhile (!map_status);"
    )
    loading_loop = map_loader[loop_start:loop_end]

    harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef float real;
typedef void *HANDLE;
struct cache_file_header { unsigned long size; };
struct simple_decompressor_definition {
    int blocking;
    unsigned long flags;
    HANDLE copy_thread;
    struct cache_file_header header;
    HANDLE copy_complete_event;
    HANDLE progress_update_event;
    real read_progress;
};
struct game_runtime_globals_prefix { real loading_progress; };
static struct simple_decompressor_definition state;
static struct simple_decompressor_definition *global_self = &state;
static struct game_runtime_globals_prefix game_state;
static struct game_runtime_globals_prefix *game_globals = &game_state;
static const uint64_t VBLANK_US = 16667;
static const uint64_t COPY_FINISH_US = 200000;
static const uint64_t COPY_ERROR_US = 50000;
static uint64_t now_us;
static uint64_t presented[256];
static unsigned int present_count;
static unsigned long poll_sleep_max;
static unsigned int in_progress_polls;
static unsigned int render_cost_ms;
static int vblank_available;
static int fail_copy;
enum { _copy_write_failed_bit, _copy_read_failed_bit, _copy_bad_file_bit };
static unsigned long cache_copy_get_flags(void) { return global_self->flags; }
static unsigned long WaitForSingleObject(HANDLE event, unsigned long timeout) {
    (void)timeout;
    if (event == global_self->copy_complete_event || event == global_self->progress_update_event)
        return now_us >= COPY_FINISH_US ? 0 : 1;
    return 1;
}
static void Sleep(unsigned long milliseconds) {
    if (milliseconds > poll_sleep_max) poll_sleep_max = milliseconds;
    now_us += milliseconds * 1000;
    if (fail_copy && now_us >= COPY_ERROR_US)
        global_self->flags = 1UL << _copy_read_failed_bit;
    if (!fail_copy && now_us >= COPY_FINISH_US) global_self->read_progress = 1.0f;
}
#define match_assert(file, line, expression) assert(expression)
enum cache_copy_status {
    _cache_copy_bad_file_failure,
    _cache_copy_read_failure,
    _cache_copy_write_failure,
    _cache_copy_in_progress,
    _cache_copy_finished
};
#define TEST_FLAG(flags, bit) (((flags) & (1UL << (bit))) != 0)
short cache_copy_get_status(real *progress);
enum cached_map_file_precache_state {
    _cached_map_file_in_progress,
    _cached_map_file_success,
    _cached_map_file_failed
};
static short cache_files_precache_map_status(real *progress) {
    short status = cache_copy_get_status(progress);
    if (status == _cache_copy_in_progress) {
        in_progress_polls++;
        return _cached_map_file_in_progress;
    }
    if (status == _cache_copy_finished) return _cached_map_file_success;
    return _cached_map_file_failed;
}
static void main_pregame_render(void) { now_us += render_cost_ms * 1000; }
static void main_rasterizer_throttle(void) {
    if (vblank_available)
        now_us = ((now_us / VBLANK_US) + 1) * VBLANK_US;
    else
        Sleep(1); /* the production throttle yields while waiting for VBlank */
}
static void main_present_frame(void) {
    assert(present_count < sizeof(presented) / sizeof(presented[0]));
    presented[present_count++] = now_us;
}
'''
    driver = r"""
static void run_trial(unsigned int render_ms, int has_vblank, int error) {
    short map_status;
    state = (struct simple_decompressor_definition){
        1, 0, (HANDLE)3, { 1 }, (HANDLE)1, (HANDLE)2, 0.0f
    };
    now_us = 0;
    present_count = 0;
    in_progress_polls = 0;
    poll_sleep_max = 0;
    render_cost_ms = render_ms;
    vblank_available = has_vblank;
    fail_copy = error;
""" + loading_loop + r"""
    assert(in_progress_polls > 1);
    assert(present_count >= 2);
    if (error) {
        assert(map_status == _cached_map_file_failed);
        assert(state.read_progress == 0.0f);
        assert(now_us < COPY_FINISH_US);
    } else {
        assert(map_status == _cached_map_file_success);
        assert(state.read_progress == 1.0f); /* async copy completion remains authoritative */
    }
    if (has_vblank) {
        uint64_t maximum_interval = 0;
        for (unsigned int index = 1; index < present_count; index++) {
            uint64_t interval = presented[index] - presented[index - 1];
            if (interval > maximum_interval) maximum_interval = interval;
        }
        fprintf(stderr, "render=%u ms, maximum present interval=%llu us\n",
            render_ms, (unsigned long long)maximum_interval);
        assert(maximum_interval <= VBLANK_US + 1);
    } else {
        assert(present_count <= 200); /* no-VBlank fallback remains a yielding poll, not a hot loop */
    }
    assert(poll_sleep_max <= 1); /* pending polls yield instead of keeping the engine thread busy */
}
int main(void) {
    run_trial(1, 1, 0);
    run_trial(2, 1, 0);
    run_trial(6, 1, 0);
    run_trial(1, 0, 0);
    run_trial(2, 1, 1);
    puts("PASS: production map-copy polling preserves refresh cadence, completion, error and yielding paths");
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="loading-cadence-") as directory:
        probe = Path(directory) / "probe.c"
        binary = Path(directory) / "probe"
        probe.write_text(harness + cache_poll + driver)
        subprocess.run([
            COMPILER, "-DHALO_IOS=1", "-O2", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", str(probe), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


def test_non_apple_cache_poll_retains_the_upstream_yield():
    source = (ROOT / "source/cache/cache_files_decompress_windows.c").read_text()
    start = source.index("short cache_copy_get_status(")
    end = source.index("\nvoid cache_copy_end(", start)
    actual = source[start:end]
    harness = r'''
#include <assert.h>
typedef float real;
typedef void *HANDLE;
struct cache_file_header { unsigned long size; };
struct simple_decompressor_definition {
    int blocking;
    unsigned long flags;
    HANDLE copy_thread;
    struct cache_file_header header;
    HANDLE copy_complete_event;
    HANDLE progress_update_event;
    real read_progress;
};
static struct simple_decompressor_definition state = { 1, 0, (HANDLE)3, { 1 }, (HANDLE)1, (HANDLE)2, 0 };
static struct simple_decompressor_definition *global_self = &state;
static unsigned long waited_ms;
static unsigned long cache_copy_get_flags(void) { return global_self->flags; }
static unsigned long WaitForSingleObject(HANDLE event, unsigned long timeout) {
    (void)event; (void)timeout; return 1;
}
static void Sleep(unsigned long milliseconds) { waited_ms = milliseconds; }
#define match_assert(file, line, expression) assert(expression)
enum cache_copy_status {
    _cache_copy_bad_file_failure, _cache_copy_read_failure, _cache_copy_write_failure,
    _cache_copy_in_progress, _cache_copy_finished
};
enum { _copy_write_failed_bit, _copy_read_failed_bit, _copy_bad_file_bit };
#define TEST_FLAG(flags, bit) (((flags) & (1UL << (bit))) != 0)
'''
    driver = r'''
int main(void) {
    real progress = 0;
    assert(cache_copy_get_status(&progress) == _cache_copy_in_progress);
    assert(waited_ms == 16);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="loading-non-apple-") as directory:
        probe = Path(directory) / "probe.c"
        binary = Path(directory) / "probe"
        probe.write_text(harness + actual + driver)
        subprocess.run([
            COMPILER, "-O2", "-Wall", "-Wextra", "-Werror", str(probe),
            "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


@unittest.skipUnless(sys.platform == "darwin", "the Apple Sleep wrapper runs only on Apple hosts")
def test_apple_sleep_wrapper_yields_for_the_requested_millisecond():
    source = (ROOT / "port/linux/src/xbox_kernel.c").read_text()
    start = source.index("DWORD WINAPI SleepEx(")
    end = source.index("\n/* ---------- time", start)
    actual = source[start:end]
    harness = r'''
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
typedef unsigned long DWORD;
typedef int BOOL;
typedef void VOID;
#define WINAPI
#define TRUE 1
#define FALSE 0
#define INFINITE ((DWORD)~0UL)
#define WAIT_IO_COMPLETION 0xC0
static __thread BOOL thread_is_worker;
static BOOL platform_fixed_timestep(void) { return FALSE; }
static BOOL platform_fixed_timestep_launch(void) { return FALSE; }
static BOOL platform_run_apcs(void) { return FALSE; }
static BOOL is_presenting_thread(void) { return FALSE; }
static void platform_quiescence_wait(void) {}
static void workers_add(int delta) { (void)delta; }
'''
    driver = r'''
int main(void) {
    struct timespec before, after;
    assert(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    Sleep(1);
    assert(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    uint64_t elapsed = (uint64_t)(after.tv_sec - before.tv_sec) * 1000000000ULL +
        (uint64_t)(after.tv_nsec - before.tv_nsec);
    assert(elapsed >= 1000000ULL);
    puts("PASS: Apple Sleep(1) yields for at least one millisecond");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="apple-sleep-") as directory:
        probe = Path(directory) / "probe.c"
        binary = Path(directory) / "probe"
        probe.write_text(harness + actual + driver)
        subprocess.run([
            COMPILER, "-O2", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", str(probe), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    test_blocking_cache_poll_keeps_loading_frames_on_consecutive_refreshes()
    test_non_apple_cache_poll_retains_the_upstream_yield()
    if sys.platform == "darwin":
        test_apple_sleep_wrapper_yields_for_the_requested_millisecond()
