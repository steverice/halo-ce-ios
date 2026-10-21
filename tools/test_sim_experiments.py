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
    assert list(benchmark_catalog.CASE_SETTINGS) == names


def test_the_host_catalog_reader_lists_the_same_settings_in_the_same_order():
    text = (ROOT / "port/ios/host/host_benchmark_catalog.m").read_text()
    listed = re.search(r"names = @\[((?:@\"debug\.[a-z_]+\",?\s*)+)\]", text).group(1)
    assert re.findall(r'@"([^"]+)"', listed) == _names()
