"""Compile actual renderer classification and report aggregation, including MetalFX fallback."""
from pathlib import Path
import shutil
import subprocess
import sys
from unittest import SkipTest

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_requested_metalfx_does_not_claim_effective_metalfx_on_fallback(tmp_path):
    if sys.platform != "darwin" or not shutil.which("xcrun"):
        raise SkipTest("Production JSON metadata requires Foundation")
    gpu = (ROOT / "port/ios/host/gpu_metal.m").read_text()
    report = (ROOT / "port/ios/host/host_benchmark.m").read_text()
    actual = "\n".join([function(gpu, "static unsigned scene_upscale_path("),
        function(report, "void host_benchmark_upscaler("),
        function(report, "static NSString *upscaler_name("),
        function(report, "static NSDictionary *upscaler_metadata(")])
    source = tmp_path / "upscaler.m"
    source.write_text(r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include "host_benchmark.h"
static _Atomic int running = 1;
static int window_open = 1, lock;
static unsigned upscaler_requests, upscaler_paths;
static uint64_t upscaler_frames[5];
#define os_unfair_lock_lock(p) ((void)(p))
#define os_unfair_lock_unlock(p) ((void)(p))
''' + actual + r'''
int main(void) { @autoreleasepool {
    unsigned fallback = scene_upscale_path(640,480,1280,960,0);
    assert(fallback == HOST_UPSCALER_BILINEAR);
    host_benchmark_upscaler("metalfx",fallback);
    NSDictionary *result = upscaler_metadata();
    assert([result[@"requested"] isEqual:@"metalfx"]);
    assert([result[@"effective"] isEqual:@"bilinear"]);
    assert([result[@"bilinear_frames"] unsignedLongLongValue] == 1);
    assert([result[@"metalfx_frames"] unsignedLongLongValue] == 0);
    NSData *bytes = [NSJSONSerialization dataWithJSONObject:result options:0 error:NULL];
    NSDictionary *decoded = [NSJSONSerialization JSONObjectWithData:bytes options:0 error:NULL];
    assert([decoded[@"effective"] isEqual:@"bilinear"]);
    host_benchmark_upscaler("metalfx",scene_upscale_path(640,480,1280,960,1));
    assert([upscaler_metadata()[@"effective"] isEqual:@"mixed"]);
    memset(upscaler_frames,0,sizeof(upscaler_frames)); upscaler_requests = upscaler_paths = 0;
    host_benchmark_upscaler("metalfx",scene_upscale_path(1280,960,1280,960,0));
    assert([upscaler_metadata()[@"effective"] isEqual:@"none"]);
    window_open = 0; host_benchmark_upscaler("bilinear",HOST_UPSCALER_BILINEAR);
    assert([upscaler_metadata()[@"requested"] isEqual:@"metalfx"]);
    memset(upscaler_frames,0,sizeof(upscaler_frames)); upscaler_requests = upscaler_paths = 0;
    assert([upscaler_metadata()[@"effective"] isEqual:@"unknown"]);
} return 0; }
''')
    binary = tmp_path / "upscaler"
    subprocess.run(["xcrun", "clang", "-O1", "-Wall", "-Wextra", "-Werror", "-fobjc-arc",
        "-fsanitize=address,undefined", "-framework", "Foundation", "-I", str(ROOT / "port/ios/host"),
        str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_resolved_graphics_first_last_change_and_json_persistence(tmp_path):
    if sys.platform != "darwin" or not shutil.which("xcrun"):
        raise SkipTest("Production report metadata requires Foundation")
    report = (ROOT / "port/ios/host/host_benchmark.m").read_text()
    actual = "\n".join(function(report, signature) for signature in (
        "static BOOL graphics_key_allowed(", "static BOOL graphics_snapshot_valid(",
        "void host_benchmark_graphics(", "static NSDictionary *graphics_metadata("))
    source = tmp_path / "graphics.m"
    source.write_text(r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include "../../runtime/graphics_capture.h"
static _Atomic int running = 1;
static int window_open = 0, lock;
static NSDictionary *graphics_first, *graphics_last;
static BOOL graphics_changed;
static NSUInteger graphics_snapshots, graphics_invalid;
#define os_unfair_lock_lock(p) ((void)(p))
#define os_unfair_lock_unlock(p) ((void)(p))
''' + actual + r'''
static const char *payload = "{\"schema\":1,\"scene\":\"gameplay\",\"configuration_revision\":0,\"external_output\":false,\"values\":{\"display.render_scale\":0.5,\"display.native_hud\":true,\"display.shadow_map_size\":256}}";
int main(void) { @autoreleasepool {
    host_benchmark_graphics(payload);
    assert(![graphics_metadata()[@"available"] boolValue]);
    window_open = 1; host_benchmark_graphics(payload);
    NSDictionary *first = graphics_metadata();
    assert([first[@"available"] boolValue] && ![first[@"changed"] boolValue]);
    assert([first[@"first"][@"values"][@"display.native_hud"] boolValue]);
    NSString *changed = [@(payload) stringByReplacingOccurrencesOfString:@"0.5" withString:@"0.75"];
    host_benchmark_graphics(changed.UTF8String);
    NSDictionary *result = graphics_metadata();
    assert([result[@"changed"] boolValue] && [result[@"snapshots"] unsignedIntegerValue] == 2);
    assert([result[@"first"][@"values"][@"display.render_scale"] doubleValue] == 0.5);
    assert([result[@"last"][@"values"][@"display.render_scale"] doubleValue] == 0.75);
    host_benchmark_graphics(payload); /* A -> B -> A remains changed. */
    result = graphics_metadata();
    assert([result[@"changed"] boolValue] && [result[@"snapshots"] unsignedIntegerValue] == 3);
    assert([result[@"last"][@"values"][@"display.render_scale"] doubleValue] == 0.5);
    NSData *bytes = [NSJSONSerialization dataWithJSONObject:@{@"graphics":result} options:0 error:NULL];
    NSDictionary *decoded = [NSJSONSerialization JSONObjectWithData:bytes options:0 error:NULL];
    assert([decoded[@"graphics"][@"changed"] boolValue]);
    NSString *unsafe = [@(payload) stringByReplacingOccurrencesOfString:@"display.render_scale" withString:@"network.account"];
    host_benchmark_graphics(unsafe.UTF8String);
    assert([graphics_metadata()[@"invalid_snapshots"] unsignedIntegerValue] == 1);
    assert([graphics_metadata()[@"last"][@"values"][@"display.render_scale"] doubleValue] == 0.5);
} return 0; }
''')
    binary = tmp_path / "graphics"
    subprocess.run(["xcrun", "clang", "-O1", "-Wall", "-Wextra", "-Werror", "-fobjc-arc",
        "-fsanitize=address,undefined", "-framework", "Foundation", "-I", str(ROOT / "port/ios/host"),
        str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
