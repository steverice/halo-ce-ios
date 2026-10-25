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
    acquire = body.index("drawable = frame_held || gave_up ? nil :")
    guard = body.rindex("if (offscreen)", 0, acquire)
    assert "offscreen_target(" in body[guard:acquire]
    assert re.search(r"if \(!offscreen\)\s*\n\s*host_frame_rate_limit\(\);", body)
    assert re.search(r"if \(!offscreen\)\s*\n\s*frame_context_wait\(\);", body)
    # the frame's GPU time is still counted: the same command buffer commits
    assert "commit(YES);" in body[acquire:]


def kernel_function(name):
    kernel = (ROOT / "port/linux/src/xbox_kernel.c").read_text()
    start = kernel.index(name)
    return kernel[start:kernel.index("\n}\n", start)]


def test_offscreen_forces_fixed_timestep():
    assert "host_benchmark_offscreen_request(" in kernel_function("int platform_fixed_timestep_launch(void)")
    # a presented run takes the launch onto real time for a while
    assert "clock_realtime" in kernel_function("int platform_fixed_timestep(void)")


def test_clock_switch_keeps_worker_accounting():
    kernel = (ROOT / "port/linux/src/xbox_kernel.c").read_text()
    # the load barrier counts workers by the launch's decision, so a switch never leaves one counted busy
    for line in kernel.splitlines():
        if "workers_add(" in line or "BOOL tracked" in line or "BOOL park" in line:
            continue
        if "platform_fixed_timestep()" in line and ("suspended" in line or "thread_is_worker" in line):
            raise AssertionError(line)
    switch = kernel_function("void platform_clock_realtime(int on)")
    assert "halo_main_time_restart();" in switch
    assert "clock_realtime_offset =" in switch and "clock_frames_offset" in switch


def test_unpresented_pass_moves_the_clock():
    main = (ROOT / "source/main/main.c").read_text()
    start = main.index("void main_loop(")
    body = main[start:main.index("\n}\n", start)]
    # (a profiling build's scope may wrap the present: profile_scope(section, main_present_frame();))
    assert re.search(r"main_present_frame\(\);\)?\s*\}\s*else\s*\{[^}]*platform_clock_unpresented_frame\(\);", body)


def test_presented_runs_follow_the_host():
    suite = (ROOT / "port/linux/src/benchmark_suite.c").read_text()
    assert "if (host_benchmark_presented(on))\n\t\tplatform_clock_realtime(on);" in suite
    host = (ROOT / "port/ios/host/host_benchmark.m").read_text()
    assert "finished->stage != 'P'" in host and 'record[@"reality_check"]' in host
    assert "plan.reality_checks = plan.mode == BENCHMARK_UNIFIED && !cost_suite && host_benchmark_offscreen_requested();" in host
    imports = (ROOT / "port/runtime/host_imports.list").read_text().split()
    assert "host_benchmark_presented" in imports


def test_offscreen_timing_basis_in_report():
    source = (ROOT / "port/ios/host/host_benchmark_presentation.m").read_text()
    assert 'result[@"timing_basis"]' in source


def test_offscreen_suite_quits_at_its_end():
    source = (ROOT / "port/ios/host/host_benchmark.m").read_text()
    # a scripted launch quits; a tester build goes back to real time and its menu
    assert "#ifndef HALO_TESTER_BUILD\n        if (host_benchmark_offscreen_requested()) completion_screen_requested = 0;" in source
    assert "if(plan.mode==BENCHMARK_UNIFIED && completion_screen_requested) {" in source


def test_offscreen_window_has_a_wall_time_cap():
    host = (ROOT / "port/ios/host/host_benchmark.m").read_text()
    assert "#define OFFSCREEN_WINDOW_WALL_SECONDS 60.0" in host
    # only offscreen frames are cut; the report says so and how much was measured
    assert "host_benchmark_offscreen_active() &&\n            CFAbsoluteTimeGetCurrent() - window_started_at >= window_wall_cap()" in host
    assert 'record[@"capped"]' in host and 'record[@"measured_ticks"]' in host
    # the cap is configurable (default 60 s, bounded), a signed case may set it, and the report names it
    assert 'host_config_real("debug.offscreen_window_wall_seconds", OFFSCREEN_WINDOW_WALL_SECONDS)' in host
    assert "fmin(OFFSCREEN_WINDOW_WALL_MAX, fmax(OFFSCREEN_WINDOW_WALL_MIN, seconds))" in host
    assert 'record[@"window_wall_cap_seconds"]' in host
    catalog = (ROOT / "port/ios/host/host_benchmark_catalog.m").read_text()
    assert '[name isEqual:@"debug.offscreen_window_wall_seconds"]' in catalog
    assert "real_number(value, 30.0, 1800.0, &real)" in catalog
    suite = (ROOT / "port/linux/src/benchmark_suite.c").read_text()
    assert "control == 6 && suite.state == suite_measuring" in suite
    assert "if (suite.capped || state.tick - (long)suite.opened_ticks >= (long)(suite.seconds * 30.0))" in suite


def test_offscreen_lead_in_skips_drawing_but_presents():
    main = (ROOT / "source/main/main.c").read_text()
    # the world goes undrawn, but the frame is still presented, so the suite, the replay and the clock go on
    assert re.search(r"if \(!benchmark_suite_skips_drawing\(\)\)\s*\n\s*main_game_render\(", main)
    suite = (ROOT / "port/linux/src/benchmark_suite.c").read_text()
    assert "#define DRAWN_LEAD_TICKS 300.0" in suite
    assert "return suite.state == suite_lead && suite.skip_drawing;" in suite
    host = (ROOT / "port/ios/host/host_benchmark.m").read_text()
    assert "presented=%d offscreen=%d" in host


@pytest.mark.parametrize("name", ["progress_format", "progress_visibility"])
def test_native_progress(probe, name):
    run_case(probe, name)


def test_progress_for_all_offscreen_suites():
    host = (ROOT / "port/ios/host/host_benchmark.m").read_text()
    assert "plan.mode==BENCHMARK_UNIFIED || host_benchmark_offscreen_requested()" in host
    # The UI hides before the renderer can submit a presented frame, without a timer delay.
    switch = host[host.index("int host_benchmark_presented(int on)"):]
    assert switch.index("host_benchmark_overlay_presented(on)") < switch.index("gpu_metal_benchmark_realtime(on)")
    ui = (ROOT / "port/ios/host/host_benchmark_page.m").read_text()
    assert "self.view.hidden=progress.presented!=0;" in ui
    assert "dispatch_sync(dispatch_get_main_queue(),update)" in ui
    assert "host_benchmark_progress_visible(&progress)" in ui
    assert "scheduledTimerWithTimeInterval:0.25" in ui
    assert "scroll.contentLayoutGuide" in ui and "screen.safeAreaLayoutGuide" in ui
    assert "Benchmark running — please don’t close the app" in ui


def test_progress_phase_bridge():
    guest = (ROOT / "port/linux/src/benchmark_suite.c").read_text()
    for state, phase in [("loading", 0), ("lead", 1), ("measuring", 2)]:
        assert f"state == suite_{state}) host_benchmark_phase({phase})" in guest
    assert "host_benchmark_phase" in (ROOT / "port/runtime/host_imports.list").read_text().split()


def test_overlay_window_lifecycle(tmp_path):
    """Execute the production window code with an inactive launch scene on macOS."""
    import sys

    if sys.platform != "darwin":
        pytest.skip("Objective-C Foundation probe requires macOS")
    compiler = shutil.which("clang")
    if not compiler:
        pytest.skip("no Objective-C compiler")
    ui = (ROOT / "port/ios/host/host_benchmark_page.m").read_text()
    production = ui[ui.index("static HaloBenchmarkOverlayWindow *benchmark_overlay;"):]
    template = (ROOT / "port/ios/tests/benchmark_overlay_probe.m").read_text()
    source = tmp_path / "overlay.m"
    source.write_text(template.replace("// PRODUCTION_OVERLAY", production))
    program = tmp_path / "overlay"
    subprocess.run([compiler, "-fobjc-arc", "-fblocks", "-Wall", "-Werror",
                    "-framework", "Foundation", str(source), "-o", str(program)], check=True)
    subprocess.run([str(program)], check=True, timeout=10)
