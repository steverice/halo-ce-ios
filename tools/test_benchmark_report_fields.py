"""A run's measurements (port/runtime/benchmark_metrics.c) parse as JSON and carry the CPU split."""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
DRIVER = r"""
#include "benchmark_metrics.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    struct benchmark_metrics metrics = { 0 };
    benchmark_metrics_reset(&metrics, 60.0);
    for (int index = 0; index < 30; index++) {
        if (argc > 1) benchmark_metrics_frame(&metrics, 16.7, 11.0, 8.0);
        else benchmark_metrics_frame_split(&metrics, 16.7, 11.0, 8.0, 4.0 + index % 3, 6.0, 3.0, 1 + index % 2);
    }
    char *members = benchmark_metrics_json(&metrics);
    if (!members) return 1;
    printf("{%s}\n", members);
    free(members);
    benchmark_metrics_free(&metrics);
    return 0;
}
"""


def measurements(tmp_path: Path, *arguments: str) -> dict:
    compiler = shutil.which("cc") or shutil.which("clang")
    if not compiler:
        pytest.skip("no C compiler")
    (tmp_path / "driver.c").write_text(DRIVER)
    program = tmp_path / "driver"
    subprocess.run([compiler, "-std=c99", "-I", str(ROOT / "port/runtime"), str(tmp_path / "driver.c"),
                    str(ROOT / "port/runtime/benchmark_metrics.c"), "-lm", "-o", str(program)], check=True)
    return json.loads(subprocess.run([str(program), *arguments], check=True, capture_output=True, text=True).stdout)


def test_split_fields(tmp_path):
    record = measurements(tmp_path)
    for name in ("cpu_ms", "simulation_cpu_ms", "render_cpu_ms", "translation_cpu_ms"):
        assert set(record[name]) == {"mean", "median", "p95", "p99", "worst"}
    assert record["simulation_cpu_ms"]["mean"] == pytest.approx(5.0)
    assert record["simulation_cpu_ms"]["worst"] == pytest.approx(6.0)
    assert record["render_cpu_ms"]["mean"] == pytest.approx(6.0)
    assert record["cpu_ms"]["mean"] == pytest.approx(11.0)
    assert record["ticks_per_frame"] == pytest.approx(1.5)
    assert record["translation_cpu_ms"]["mean"] == pytest.approx(3.0)


def test_split_unknown_is_null(tmp_path):
    record = measurements(tmp_path, "unsplit")
    assert record["simulation_cpu_ms"] is None
    assert record["render_cpu_ms"] is None
    assert record["ticks_per_frame"] is None
    assert record["translation_cpu_ms"] is None
    assert record["cpu_ms"]["mean"] == pytest.approx(11.0)


def test_split_semantics():
    source = (ROOT / "port/ios/host/host_benchmark_presentation.m").read_text()
    assert '@"simulation_cpu_ms": @"simulation_cpu_duration"' in source
    assert '@"render_cpu_ms": @"render_cpu_duration"' in source
    assert '@"ticks_per_frame": @"simulation_ticks_per_frame"' in source
    assert '@"translation_cpu_ms": @"translation_cpu_duration"' in source


def test_translation_timed_at_device_entry_points():
    device = (ROOT / "port/linux/src/d3d8_device.c").read_text()
    for name in ("D3DDevice_DrawVertices", "D3DDevice_DrawIndexedVertices", "D3DDevice_End", "D3DDevice_Clear",
                 "D3DDevice_SetVertexShaderConstant", "D3DDevice_SetRenderTarget", "halo_draw_quad_ranges"):
        start = device.index(f" {name}(")
        body = device[device.index("{", start):device.index("{", start) + 200]
        assert "TRANSLATION_TIMED();" in body, name
