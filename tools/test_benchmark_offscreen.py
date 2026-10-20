"""debug.benchmark_offscreen: honored only in a suite named at launch, and never waits on the display."""
from __future__ import annotations

from pathlib import Path
import re
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def probe(tmp_path_factory):
    compiler = shutil.which("cc") or shutil.which("clang")
    if not compiler:
        pytest.skip("no C compiler")
    program = tmp_path_factory.mktemp("offscreen") / "probe"
    subprocess.run([compiler, "-std=c99", "-Wall", "-Werror", "-I", str(ROOT / "port/ios/host"),
                    str(ROOT / "port/ios/tests/benchmark_offscreen_probe.c"), "-o", str(program)], check=True)
    return program


def run_case(probe, name):
    result = subprocess.run([str(probe), name], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def test_offscreen_ignored_outside_suite(probe):
    run_case(probe, "ignored_outside_suite")


def test_offscreen_active_in_suite(probe):
    run_case(probe, "active_in_suite")


def test_offscreen_ignored_on_stereo(probe):
    run_case(probe, "ignored_on_stereo")


def present_function():
    source = (ROOT / "port/ios/host/gpu_metal.m").read_text()
    start = source.index("static uint32_t gpu_metal_present(gpu_texture back_buffer)")
    return source[start:source.index("\n}\n", start)]


def test_offscreen_frame_skips_the_display():
    body = present_function()
    # every drawable, display wait and Present sits behind the offscreen check
    acquire = body.index("drawable = frame_held ? nil :")
    guard = body.rindex("if (offscreen)", 0, acquire)
    assert "offscreen_target(" in body[guard:acquire]
    assert re.search(r"if \(!offscreen\)\s*\n\s*host_frame_rate_limit\(\);", body)
    assert re.search(r"if \(!offscreen\)\s*\n\s*frame_context_wait\(\);", body)
    # the frame's GPU time is still counted: the same command buffer commits
    assert "commit(YES);" in body[acquire:]


def test_offscreen_forces_fixed_timestep():
    kernel = (ROOT / "port/linux/src/xbox_kernel.c").read_text()
    start = kernel.index("int platform_fixed_timestep(void)")
    body = kernel[start:kernel.index("\n}\n", start)]
    assert "host_benchmark_offscreen_request(" in body


def test_offscreen_timing_basis_in_report():
    source = (ROOT / "port/ios/host/host_benchmark_presentation.m").read_text()
    assert 'result[@"timing_basis"]' in source


def test_offscreen_suite_quits_at_its_end():
    source = (ROOT / "port/ios/host/host_benchmark.m").read_text()
    assert "if (host_benchmark_offscreen_requested()) completion_screen_requested = 0;" in source
    assert "if(plan.mode==BENCHMARK_UNIFIED && completion_screen_requested) {" in source
