"""Each experimental setting's index (the SIM_EXPERIMENT_* constants the game and the renderer ask by) is its place in
the names list of sim_experiments.c, and a catalog case's settings are those names in that order (the bits of a scene's
sim_experiments), so adding an experiment cannot silently rebind another's index."""
import re
from pathlib import Path

import benchmark_catalog

ROOT = Path(__file__).resolve().parents[1]


def _names():
    text = (ROOT / "port/linux/game/sim_experiments.c").read_text()
    body = re.search(r"sim_experiment_names\[\]\s*=\s*\{(.*?)\};", text, re.S).group(1)
    return re.findall(r'"([^"]+)"', body)


def _constants():
    found = {}
    for path in list((ROOT / "source").rglob("*.c")) + list((ROOT / "port").rglob("*.c")):
        for name, index in re.findall(r"^#define SIM_EXPERIMENT_([A-Z_]+) (\d+)\s*$", path.read_text(errors="replace"), re.M):
            found.setdefault(name, set()).add((int(index), path.name))
    return found


def test_every_constant_is_the_place_of_its_setting_in_the_names_list():
    names = _names()
    constants = _constants()
    assert constants, "no SIM_EXPERIMENT_* constants found"
    for constant, uses in constants.items():
        setting = "debug." + constant.lower()
        assert setting in names, f"SIM_EXPERIMENT_{constant} names no setting in sim_experiments.c"
        for index, where in uses:
            assert names[index] == setting, f"SIM_EXPERIMENT_{constant} is {index} in {where}, the place of {names[index]}"


def test_every_setting_has_a_constant_and_the_catalog_lists_them_in_the_same_order():
    names = _names()
    assert len(names) == len(set(names))
    assert {"debug." + c.lower() for c in _constants()} == set(names)
    assert list(benchmark_catalog.SIM_SETTINGS) == names


def test_the_host_catalog_reader_lists_the_same_settings_in_the_same_order():
    text = (ROOT / "port/ios/host/host_benchmark_catalog.m").read_text()
    listed = re.search(r"simulation = @\[((?:@\"debug\.[a-z_]+\",?\s*)+)\]", text).group(1)
    assert re.findall(r'@"([^"]+)"', listed) == _names()


def test_defaults_and_explicit_overrides_across_ticks(tmp_path):
    import shutil
    import subprocess
    import pytest
    import settings_schema

    settings = settings_schema.config_settings()
    for name in _names()[:3]:
        assert settings[name]['default'] is True
    assert settings['debug.immediate_merge']['default'] is False
    compiler = shutil.which('clang') or shutil.which('cc')
    if not compiler:
        pytest.skip('A C compiler is required for the simulation probe')
    (tmp_path / 'cseries.h').write_text('typedef int boolean;\n#define FALSE 0\n')
    source = tmp_path / 'probe.c'
    source.write_text(r'''
#include <assert.h>
#include <string.h>
#include "sim_experiments.c"
static long tick;
static unsigned configured = 7;
long game_time_get(void) { return tick; }
int config_boolean(const char *name) {
    for (unsigned i = 0; i < 4; i++)
        if (!strcmp(name, sim_experiment_names[i])) return (configured >> i) & 1;
    assert(0); return 0;
}
int main(void) {
    for (unsigned defaults = 0; defaults < 16; defaults++) {
        configured = defaults;
        for (unsigned mask = 0; mask < 16; mask++) {
            for (unsigned values = 0; values < 16; values++) {
                sim_experiment_override(values, mask);
                unsigned expected = (defaults & ~mask) | (values & mask);
                for (short i = 0; i < 4; i++) {
                    assert(sim_experiment_enabled(i) == ((expected >> i) & 1));
                    assert(sim_experiment_enabled_now(i) == ((expected >> i) & 1));
                }
            }
        }
    }
    configured = 7;
    sim_experiment_override(0, 0);
    assert(sim_experiment_enabled(0));
    configured = 0;
    assert(sim_experiment_enabled(0)); /* cached for this tick */
    assert(!sim_experiment_enabled_now(0));
    tick++;
    assert(!sim_experiment_enabled(0));
    sim_experiment_override(1, 1);
    assert(sim_experiment_enabled(0)); /* override invalidates the cache */
    sim_experiment_override(0, 0);
    assert(!sim_experiment_enabled(0));
    assert(!sim_experiment_enabled(-1) && !sim_experiment_enabled(4));
    assert(!sim_experiment_enabled_now(-1) && !sim_experiment_enabled_now(4));
}
''')
    binary = tmp_path / 'probe'
    subprocess.run([compiler, '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '--include-directory=' + str(tmp_path),
                    '--include-directory=' + str(ROOT / 'port/linux/game'), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_run_records_use_effective_simulation_values(tmp_path):
    import shutil
    import subprocess
    import sys
    import pytest

    if sys.platform != 'darwin' or not shutil.which('xcrun'):
        pytest.skip('Foundation is required for the run-record probe')
    host = (ROOT / 'port/ios/host/host_benchmark.m').read_text()
    function = host[host.index('static void record_applied_settings('):host.index('/* one finished run as a JSON object')]
    source = tmp_path / 'records.m'
    source.write_text(r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#include <string.h>
#include "benchmark_plan.h"
static unsigned configured;
static NSString *configured_alias;
static const char *const names[] = { "debug.collision_type_filter", "debug.sphere_query_type_filter",
    "debug.static_light_relink" };
static NSString *configured_text(const char *name, const char *fallback) {
    if (configured_alias) return configured_alias;
    for (unsigned bit = 0; bit < 3; bit++)
        if (!strcmp(name, names[bit])) return configured & (1u << bit) ? @"true" : @"false";
    return @(fallback);
}
static double host_config_real(const char *name, double fallback) { (void)name; return fallback; }
''' + function + r'''
int main(void) {
    @autoreleasepool {
        for (configured = 0; configured < 8; configured++) {
            for (unsigned mask = 0; mask < 8; mask++) {
                for (unsigned values = 0; values < 8; values++) {
                    struct benchmark_scene scene = { .sim_experiments = values, .sim_experiments_mask = mask };
                    for (const char *stage = "AP"; *stage; stage++) {
                        NSMutableDictionary *record = [NSMutableDictionary dictionary];
                        record_applied_settings(record, &scene, *stage);
                        unsigned expected = (configured & ~mask) | (values & mask);
                        for (unsigned bit = 0; bit < 3; bit++)
                            assert([record[@(names[bit])] isEqual:expected & (1u << bit) ? @"on" : @"off"]);
                    }
                }
            }
        }
        for (NSString *alias in @[@"true", @"1", @"on", @"TRUE", @"false", @"0", @"no", @"OFF"]) {
            configured_alias = alias;
            struct benchmark_scene scene = {0};
            NSMutableDictionary *record = [NSMutableDictionary dictionary];
            record_applied_settings(record, &scene, 'A');
            BOOL enabled = [@[@"true", @"1", @"on", @"TRUE"] containsObject:alias];
            for (unsigned bit = 0; bit < 3; bit++)
                assert([record[@(names[bit])] isEqual:enabled ? @"on" : @"off"]);
        }
    }
}
''')
    binary = tmp_path / 'records'
    subprocess.run(['xcrun', 'clang', '-Wall', '-Wextra', '-Werror', '-fobjc-arc',
                    '-fsanitize=address,undefined', '-framework', 'Foundation',
                    '--include-directory=' + str(ROOT / 'port/runtime'), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_benchmark_plan_signature_includes_simulation_overrides(tmp_path):
    import shutil
    import subprocess
    import pytest

    compiler = shutil.which('clang') or shutil.which('cc')
    if not compiler:
        pytest.skip('A C compiler is required for the benchmark plan probe')
    binary = tmp_path / 'benchmark-plan'
    subprocess.run([compiler, '-O2', '-Wall', '-Werror', '-fsanitize=address,undefined',
                    '--include-directory=' + str(ROOT / 'port/runtime'),
                    str(ROOT / 'port/ios/tests/benchmark_probe.c'),
                    *(str(ROOT / f'port/runtime/benchmark_{part}.c') for part in ('metrics', 'plan', 'journal')),
                    '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
