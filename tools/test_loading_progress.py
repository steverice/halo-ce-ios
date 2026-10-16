"""Exercise the real loading controller with timed compiler completions."""
from pathlib import Path
import subprocess
import tempfile
import sys


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import ios_bridges


def test_compile_progress_bridge_writes_the_guest_snapshot():
    declarations = ios_bridges.declarations(
        ROOT / "port/runtime/guest/runtime/guest_host.h", r"host_\w+")
    ret, name, params = next(entry for entry in declarations if entry[1] == "host_gpu_compile_progress")
    _, bridge = ios_bridges.bridge(ret, name, params)
    boundaries = r'''
#include "ios_host.h"
#include "gpu.h"
#include <assert.h>
#include <stdint.h>
uintptr_t host_arena;
static uint32_t last_action;
void gpu_compile_progress(uint32_t action, struct gpu_compile_progress *progress) {
    last_action = action;
    *progress = (struct gpu_compile_progress){7, 3, 1, 1};
}
'''
    checks = r'''
int main(void) {
    struct gpu_compile_progress progress = {0};
    host_arena = (uintptr_t)&progress & ~((uintptr_t)UINT32_MAX);
    ios_bridge_host_gpu_compile_progress(GPU_COMPILE_PROGRESS_SEAL, (uint32_t)(uintptr_t)&progress);
    assert(last_action == GPU_COMPILE_PROGRESS_SEAL);
    assert(progress.total == 7 && progress.completed == 3 && progress.failed == 1 && progress.sealed == 1);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="loading-bridge-") as directory:
        probe = Path(directory) / "probe.c"
        binary = Path(directory) / "probe"
        probe.write_text(boundaries + bridge + checks)
        subprocess.run([
            "xcrun", "clang", "-O2", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", f"--include-directory={ROOT / 'port/linux/src'}",
            f"--include-directory={ROOT / 'port/ios/host'}", f"--include-directory={ROOT / 'port/runtime/include'}",
            str(probe), f"--output={binary}",
        ], check=True)
        subprocess.run([str(binary)], check=True)
    print("PASS: generated compile-progress bridge writes the guest snapshot")


def test_loading_controller_uses_measured_work_and_preserves_ready_tail():
    source = (ROOT / "port/linux/game/loading_screen.c").read_text()
    # Compile the controller itself, with engine/clock boundaries replaced;
    # its constants, state and all functions remain production code.
    actual = source[source.index("enum\n{", source.index("void __stdcall Sleep")):]
    boundaries = r'''
#include "metal_compile_progress.h"
#include "halo_loading_progress.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef int boolean;
typedef float real;
#define TRUE 1
#define FALSE 0
#define NONE -1
#define HALO_STEREO_OFF 0
#define _scenario_type_solo 1
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define PIN(x,a,b) ((x) < (a) ? (a) : (x) > (b) ? (b) : (x))
static struct metal_compile_progress batch = { .lock = PTHREAD_MUTEX_INITIALIZER };
static unsigned long now = 1;
static real last_t;
static int global_scenario_index = 0, bar_active, frames, ticks;
static int complete_later, fail_later;
static unsigned long complete_at;
static uint64_t later_token;
static struct scenario { int type; } scenario = { _scenario_type_solo };
static struct halo_stereo_frame stereo;
static double minimum;
static real copy_fraction, maximum_drawn;
static int copying;
static struct scenario *global_scenario_get(void) { return &scenario; }
static int game_in_progress(void) { return 1; }
static int game_time_get(void) { return ticks; }
const struct halo_stereo_frame *halo_stereo_frame(void) { return &stereo; }
static unsigned long system_milliseconds(void) { return now; }
static int platform_fixed_timestep(void) { return 0; }
static int config_boolean(const char *name) { (void)name; return 0; }
static long config_integer(const char *name) { (void)name; return 0; }
static double config_real(const char *name) {
    return !strcmp(name, "game.loading_screen_seconds") ? minimum : 0;
}
static const char *config_string(const char *name) {
    return !strcmp(name, "game.loading_screen") ? "on" : "";
}
static void platform_log(const char *format, ...) { (void)format; }
static short main_get_solo_level_from_name(const char *name) { (void)name; return 0; }
static void progress_bar_begin(int loaded) { (void)loaded; bar_active = 1; }
static void progress_bar_end(void) { bar_active = 0; }
static int progress_bar_is_active(void) { return bar_active; }
static int game_map_loading_in_progress(real *progress) {
    *progress = copy_fraction; return copying;
}
static void halo_main_time_restart(void) {}
static void main_rasterizer_throttle(void) {}
static void main_present_frame(void) {}
static void main_pregame_render(void);
static void Sleep(unsigned long milliseconds) {
    now += milliseconds;
    if (complete_later && now >= complete_at) {
        metal_compile_progress_finish(&batch, later_token, !fail_later);
        complete_later = 0;
    }
}
void gpu_compile_progress(uint32_t action, struct gpu_compile_progress *progress) {
    metal_compile_progress_update(&batch, action, progress);
}
uint32_t gpu_compiles_outstanding(void) {
    struct gpu_compile_progress progress;
    gpu_compile_progress(GPU_COMPILE_PROGRESS_SNAPSHOT, &progress);
    return progress.total - progress.completed;
}
'''
    checks = r'''
static void main_pregame_render(void) {
    real progress;
    assert(halo_loading_screen_progress(&progress));
    assert(progress >= last_t && progress <= 1);
    last_t = progress;
    if (progress > maximum_drawn) maximum_drawn = progress;
    frames++;
}
static void finish_tail(void) {
    unsigned count = 0;
    assert(halo_loading_screen_tail());
    ticks = 6;
    while (halo_loading_screen_tail()) {
        main_pregame_render();
        now += 16;
        halo_loading_screen_tail_frame();
        assert(++count < 200);
    }
    assert(!bar_active);
}
int main(void) {
    uint64_t early;
    real progress;
    /* A clock advancing without any copy or compiler completions must not
       move the picture. The original invented time/creep fails this. */
    minimum = 8;
    halo_loading_screen_begin("a10");
    now += 8000;
    halo_loading_screen_progress(&progress);
    assert(progress == 0.001f);
    /* Half the planned jobs finish before finish() first polls. The
       remaining-work peak would lose this completion and start from zero. */
    early = metal_compile_progress_schedule(&batch);
    metal_compile_progress_finish(&batch, early, 1);
    later_token = metal_compile_progress_schedule(&batch);
    complete_at = now + 1000;
    complete_later = 1;
    gpu_compile_progress(GPU_COMPILE_PROGRESS_SEAL, &(struct gpu_compile_progress){0});
    loading_screen.holding = 1;
    loading_screen.work = halo_loading_compile_fraction(&batch.progress);
    for (unsigned index = 0; index < 30; index++) {
        now += 16;
        halo_loading_screen_progress(&progress);
    }
    assert(progress > 0.49f && progress <= 0.525f);
    halo_loading_screen_finish();
    assert(now >= 8000 && !complete_later);
    assert(maximum_drawn <= 0.9f);
    finish_tail();
    /* A cached load has no new jobs, but the screen's existing minimum
       and ready-level tail still apply. */
    last_t = maximum_drawn = 0;
    minimum = 1.5;
    unsigned long began = now;
    halo_loading_screen_begin("a10");
    halo_loading_screen_finish();
    assert(now - began >= 1500 && maximum_drawn <= 0.9f);
    finish_tail();
    /* Failed terminal work ends the wait without pretending the level's
       preparation reached the successful final fade. */
    last_t = maximum_drawn = 0;
    minimum = 0;
    halo_loading_screen_begin("a10");
    later_token = metal_compile_progress_schedule(&batch);
    complete_at = now;
    fail_later = complete_later = 1;
    halo_loading_screen_finish();
    assert(!complete_later && !halo_loading_screen_tail() && !bar_active);
    assert(maximum_drawn < 0.9f);
    puts("PASS: actual loading controller: measured progress, early completions, cache, failure and readiness tail");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="loading-progress-") as directory:
        probe = Path(directory) / "probe.c"
        binary = Path(directory) / "probe"
        probe.write_text(boundaries + actual + checks)
        subprocess.run([
            "xcrun", "clang", "-O2", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", f"--include-directory={ROOT / 'port/linux/src'}",
            f"--include-directory={ROOT / 'port/ios/host'}", str(probe), f"--output={binary}",
        ], check=True)
        subprocess.run([str(binary)], check=True)


def test_synchronous_warmup_counts_actual_backend_jobs():
    source = (ROOT / "port/ios/host/gpu_metal.m").read_text()
    start = source.index("/* the descriptors, then the pipelines in parallel */")
    end = source.index("\n\t\twarm_list = nil;", start)
    template = (ROOT / "port/ios/tests/loading_warmup_probe.m").read_text()
    with tempfile.TemporaryDirectory(prefix="loading-warmup-") as directory:
        probe = Path(directory) / "probe.m"
        binary = Path(directory) / "probe"
        probe.write_text(template.replace("/* PRODUCTION_WARMUP */", source[start:end]))
        subprocess.run([
            "xcrun", "clang", "-O2", "-fobjc-arc", "-fblocks", "-fsanitize=address,undefined",
            "-framework", "Foundation", f"--include-directory={ROOT / 'port/linux/src'}",
            f"--include-directory={ROOT / 'port/ios/host'}", str(probe), f"--output={binary}",
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    test_loading_controller_uses_measured_work_and_preserves_ready_tail()
    test_compile_progress_bridge_writes_the_guest_snapshot()
    test_synchronous_warmup_counts_actual_backend_jobs()
