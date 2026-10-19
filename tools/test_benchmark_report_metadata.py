"""Compile the production multiplayer report metadata and check its expected-state labels."""
from pathlib import Path
import shutil
import subprocess
import sys
from unittest import SkipTest

ROOT = Path(__file__).resolve().parents[1]


def test_multiplayer_report_metadata_uses_c_strings_and_expected_counts(tmp_path):
    if sys.platform != "darwin" or not shutil.which("xcrun"):
        raise SkipTest("The production report metadata requires Foundation")
    production = (ROOT / "port/ios/host/host_benchmark.m").read_text()
    start = production.index("    if (scene->kind == BENCHMARK_SCENE_MULTIPLAYER)\n        record[@\"simulation\"]")
    end = production.index("    NSString *map =", start)
    source = tmp_path / "report.m"
    source.write_text(r'''
#import <Foundation/Foundation.h>
#include <math.h>
#include <string.h>
#include "benchmark_plan.h"
static NSDictionary *metadata(const struct benchmark_scene *scene) {
    NSMutableDictionary *record = [NSMutableDictionary dictionary];
''' + production[start:end] + r'''
    return record[@"simulation"];
}
int main(void) {
    @autoreleasepool {
        struct benchmark_scene scene = { .kind = BENCHMARK_SCENE_MULTIPLAYER,
            .simulation = "network_players", .game_mode = "slayer", .players = 2,
            .views = 1, .score_limit = 37, .time_limit_minutes = 7, .lead = 15, .seconds = 60 };
        NSDictionary *result = metadata(&scene);
        assert([result[@"mode"] isEqual:@"network_players"] && [result[@"game_mode"] isEqual:@"slayer"]);
        assert([result[@"requested_players"] intValue] == 2 && [result[@"requested_views"] intValue] == 1);
        assert([result[@"expected_local_players"] intValue] == 1 && [result[@"expected_synthetic_players"] intValue] == 1);
        assert(result[@"local_players"] == nil && result[@"synthetic_players"] == nil);
        assert([result[@"score_limit"] intValue] == 37 && [result[@"time_limit_minutes"] intValue] == 7);
        scene.simulation = "local_controllers"; scene.players = 4;
        result = metadata(&scene);
        assert([result[@"expected_local_players"] intValue] == 4 && [result[@"expected_synthetic_players"] intValue] == 0);
        scene.kind = BENCHMARK_SCENE_GAMEPLAY;
        assert(metadata(&scene) == nil);
        scene.kind = BENCHMARK_SCENE_MULTIPLAYER; scene.simulation = "network_players"; scene.players = 128;
        result = metadata(&scene);
        assert([result[@"requested_players"] intValue] == 128 && [result[@"expected_synthetic_players"] intValue] == 127);
        NSData *serialized = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
        NSDictionary *decoded = [NSJSONSerialization JSONObjectWithData:serialized options:0 error:nil];
        assert([decoded[@"requested_players"] intValue] == 128 && [decoded[@"expected_synthetic_players"] intValue] == 127);
    }
    return 0;
}
''')
    binary = tmp_path / "report"
    subprocess.run(["xcrun", "clang", "-O1", "-Wall", "-Wextra", "-Werror", "-fobjc-arc",
                    "-fsanitize=address,undefined", "-framework", "Foundation",
                    "-I", str(ROOT / "port/runtime"), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
