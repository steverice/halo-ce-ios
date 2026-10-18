#!/usr/bin/env python3
"""Exercise the host's actual scene installation with and without local routes."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from unittest import SkipTest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "port/ios/host/host_benchmark.m").read_text()


def install_source() -> str:
    start = SOURCE.index("static void install_scenes(NSArray<NSString *> *only) {")
    opening = SOURCE.index("{", start)
    depth = 0
    for position in range(opening, len(SOURCE)):
        if SOURCE[position] == "{":
            depth += 1
        elif SOURCE[position] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start:position + 1]
    raise ValueError("Unclosed host scene installer")


HARNESS = r'''
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "benchmark_plan.h"
#define FOLDER "benchmark"
#define HOST_LOG_INFO 0
static struct benchmark_plan plan;
static void host_logf(int priority, const char *format, ...) { (void)priority; (void)format; }
__INSTALL__
static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "host scenes line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)
int main(void) {
    @autoreleasepool {
        mkdir("benchmark", 0755);
        mkdir("benchmark/recordings", 0755);
        plan.mode = BENCHMARK_FULL;
        install_scenes(nil);
        CHECK(plan.scene_count == benchmark_suite_scene_count);
        CHECK(plan.scenes[plan.scene_count - 1].kind == BENCHMARK_SCENE_MULTIPLAYER);
        install_scenes(@[ @"mp-chillout-4" ]);
        CHECK(plan.scene_count == 1 && plan.scenes[0].kind == BENCHMARK_SCENE_MULTIPLAYER);
        plan.mode = BENCHMARK_QUICK;
        install_scenes(nil);
        CHECK(plan.scene_count == benchmark_suite_scene_count - 1);
        for (int index = 0; index < plan.scene_count; index++)
            CHECK(plan.scenes[index].kind != BENCHMARK_SCENE_MULTIPLAYER);
        install_scenes(@[ @"mp-chillout-4" ]);
        CHECK(plan.scene_count == 0);

        for (int index = 0; index < BENCHMARK_SCENES_MAXIMUM + 4; index++) {
            NSString *path = [NSString stringWithFormat:@"benchmark/recordings/route-%02d.route", index];
            NSString *route = [NSString stringWithFormat:
                @"scene = route-%02d\nmap = a10\ndifficulty = normal\nfirst_tick = 0\nplayers = 1\nwindow = 30\n", index];
            CHECK([route writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]);
        }
        plan.mode = BENCHMARK_FULL;
        install_scenes(nil);
        CHECK(plan.scene_count == BENCHMARK_SCENES_MAXIMUM);
        CHECK(plan.scenes[plan.scene_count - 1].kind == BENCHMARK_SCENE_MULTIPLAYER);
        plan.mode = BENCHMARK_QUICK;
        install_scenes(nil);
        CHECK(plan.scene_count == BENCHMARK_SCENES_MAXIMUM);
        for (int index = 0; index < plan.scene_count; index++)
            CHECK(plan.scenes[index].kind != BENCHMARK_SCENE_MULTIPLAYER);
    }
    return failures != 0;
}
'''


def test_host_scene_selection() -> None:
    if sys.platform != "darwin":
        raise SkipTest("Foundation host scene selection requires macOS")
    compiler = shutil.which("clang")
    if compiler is None:
        raise SkipTest("Clang is required for the host scene probe")
    with tempfile.TemporaryDirectory(prefix="benchmark-host-scenes-") as temporary:
        folder = Path(temporary)
        source = folder / "host_scenes.m"
        binary = folder / "host_scenes"
        source.write_text(HARNESS.replace("__INSTALL__", install_source()))
        subprocess.run([compiler, "-O2", "-fobjc-arc", "-fsanitize=address,undefined", "-framework", "Foundation",
                        "-I", str(ROOT / "port/runtime"), str(source), str(ROOT / "port/runtime/benchmark_plan.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], cwd=folder, check=True)


if __name__ == "__main__":
    test_host_scene_selection()
