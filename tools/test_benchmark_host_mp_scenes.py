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


def function_source(signature: str) -> str:
    start = SOURCE.index(signature)
    opening = SOURCE.index("{", start)
    depth = 0
    for position in range(opening, len(SOURCE)):
        if SOURCE[position] == "{":
            depth += 1
        elif SOURCE[position] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start:position + 1]
    raise ValueError(f"Unclosed host function: {signature}")


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
static int window_width = 1280, window_height = 720;
static NSDictionary *run_output, *suite_output, *run_environment_start, *run_environment_end;
static NSDictionary *run_shader_before, *run_shader_after;
static NSString *run_shader_directory;
static NSString *json_object(id value) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:value ?: @{} options:0 error:nil];
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
}
static NSString *json_string(NSString *value) {
    if (!value) return @"null";
    NSData *data = [NSJSONSerialization dataWithJSONObject:@[value] options:0 error:nil];
    NSString *array = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    return [array substringWithRange:NSMakeRange(1, array.length - 2)];
}
static NSDictionary *output_now(void) { return @{}; }
static NSDictionary *environment_now(void) { return @{}; }
static NSString *run_path(int index) { return [NSString stringWithFormat:@"benchmark/run-%02d.json", index]; }
static NSDictionary *host_benchmark_shader_snapshot(NSString *folder, NSString *map) {
    (void)folder; (void)map; return @{};
}
static NSDictionary *host_benchmark_shader_delta(NSDictionary *before, NSDictionary *after) {
    (void)before; (void)after; return @{};
}
static NSDictionary *host_benchmark_measurement_record(NSDictionary *record, NSDictionary *presentation) {
    (void)presentation; return record;
}
__WRITE_RUN__
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
        install_scenes(@[ @"mp-chillout-2-proof" ]);
        CHECK(plan.scene_count == 1 && !strcmp(plan.scenes[0].id, "mp-chillout-2-proof") &&
            plan.scenes[0].kind == BENCHMARK_SCENE_MULTIPLAYER);
        struct benchmark_run proof = { 0 }; proof.index = 0; proof.scene = 0; proof.stage = 'A';
        write_run(&proof, BENCHMARK_FAILED, @"proof", NULL, nil);
        NSData *bytes = [NSData dataWithContentsOfFile:run_path(0)];
        NSDictionary *record = bytes ? [NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil] : nil;
        NSDictionary *simulation = record[@"simulation"];
        CHECK([simulation[@"local_players"] intValue] == 1 &&
            [simulation[@"synthetic_players"] intValue] == 1 &&
            [simulation[@"remote_clients"] intValue] == 0 &&
            [simulation[@"views"] intValue] == 1);
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
        source.write_text(HARNESS.replace("__INSTALL__", function_source("static void install_scenes(NSArray<NSString *> *only) {"))
                          .replace("__WRITE_RUN__", function_source("static void write_run(const struct benchmark_run *finished,")))
        subprocess.run([compiler, "-O2", "-fobjc-arc", "-fsanitize=address,undefined", "-framework", "Foundation",
                        "-I", str(ROOT / "port/runtime"), str(source), str(ROOT / "port/runtime/benchmark_plan.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], cwd=folder, check=True)


if __name__ == "__main__":
    test_host_scene_selection()
