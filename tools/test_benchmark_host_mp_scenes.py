"""Exercise the production signed-catalog loader and typed scene selector."""
from pathlib import Path
import shutil
import subprocess
import sys
from unittest import SkipTest

import pytest

from test_benchmark_catalog import _cases, _recording, _write_catalog

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def catalog_probe(tmp_path_factory):
    if sys.platform != "darwin":
        raise SkipTest("The production catalog loader requires Foundation")
    if not shutil.which("xcrun"):
        raise SkipTest("Xcode's Clang is required for the production catalog probe")
    build = tmp_path_factory.mktemp("benchmark-catalog-host-probe")
    binary = build / "benchmark-catalog-probe"
    compiled = subprocess.run([
        "xcrun", "clang", "-O2", "-Wall", "-Wextra", "-Werror", "-fobjc-arc",
        "-fsanitize=address,undefined", "-framework", "Foundation",
        "-I", str(ROOT / "port/ios/host"), "-I", str(ROOT / "port/runtime"),
        str(ROOT / "port/ios/tests/benchmark_catalog_probe.m"),
        str(ROOT / "port/ios/host/host_benchmark_catalog.m"),
        str(ROOT / "port/runtime/benchmark_plan.c"), "-o", str(binary),
    ], cwd=ROOT, capture_output=True, text=True)
    if compiled.returncode:
        raise AssertionError(compiled.stderr or compiled.stdout or "catalog probe compilation failed")
    return binary


def run_probe(binary, folder, mode, *filters):
    return subprocess.run([str(binary), str(folder), mode, *filters], capture_output=True, text=True)


def test_host_loader_selects_signed_catalog_cases_by_type_and_mode(tmp_path, catalog_probe):
    _recording(tmp_path)
    _write_catalog(tmp_path, _cases(players=2))
    full = run_probe(catalog_probe, tmp_path, "full")
    assert full.returncode == 0, full.stderr
    rows = full.stdout.splitlines()[1:]
    assert [row.split("\t")[0] for row in rows] == [
        "menu", "a10-opening", "b30-warthog-fight", "mp-chillout-proof"
    ]
    mp = rows[-1].split("\t")
    assert mp[1:6] == ["3", "network_players", "2", "1", "slayer"]
    assert mp[6:] == ["1000", "0"]

    quick = run_probe(catalog_probe, tmp_path, "quick")
    assert quick.returncode == 0, quick.stderr
    assert "mp-chillout-proof" not in quick.stdout
    filtered = run_probe(catalog_probe, tmp_path, "quick", "a10-opening")
    assert filtered.returncode == 0, filtered.stderr
    assert [row.split("\t")[0] for row in filtered.stdout.splitlines()[1:]] == ["a10-opening"]
    filtered_mp = run_probe(catalog_probe, tmp_path, "quick", "mp-chillout-proof")
    assert filtered_mp.returncode == 0 and filtered_mp.stdout.splitlines()[1:] == []


def test_host_loader_accepts_mp_only_catalog_without_placeholder_replays(tmp_path, catalog_probe):
    _write_catalog(tmp_path, _cases(gameplay=False))
    result = run_probe(catalog_probe, tmp_path, "full")
    assert result.returncode == 0, result.stderr
    rows = result.stdout.splitlines()[1:]
    assert [row.split("\t")[0] for row in rows] == ["menu", "a10-opening", "mp-chillout-proof"]


def test_host_loader_rejects_missing_or_idle_catalog_data(tmp_path, catalog_probe):
    missing = run_probe(catalog_probe, tmp_path, "full")
    assert missing.returncode != 0 and "catalog" in missing.stderr.lower()
    _recording(tmp_path, active=False)
    _write_catalog(tmp_path, _cases())
    idle = run_probe(catalog_probe, tmp_path, "full")
    assert idle.returncode != 0 and "movement or combat" in idle.stderr.lower()


@pytest.mark.parametrize("players", [3, 16, 32])
def test_host_loader_accepts_bounded_network_rosters(tmp_path, catalog_probe, players):
    _write_catalog(tmp_path, _cases(players=players, gameplay=False))
    full = run_probe(catalog_probe, tmp_path, "full")
    assert full.returncode == 0, full.stderr
    mp = full.stdout.splitlines()[-1].split("\t")
    assert mp[2:5] == ["network_players", str(players), "1"]
    quick = run_probe(catalog_probe, tmp_path, "quick")
    assert quick.returncode == 0 and "mp-chillout-proof" not in quick.stdout


def test_host_loader_rejects_network_roster_over_capacity(tmp_path, catalog_probe):
    _write_catalog(tmp_path, _cases(players=33, gameplay=False))
    result = run_probe(catalog_probe, tmp_path, "full")
    assert result.returncode != 0 and "unsupported" in result.stderr
