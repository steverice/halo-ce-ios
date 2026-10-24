"""Tests for tools/settings_bundle.py."""

import plistlib
import subprocess
import sys
from pathlib import Path

import pytest

from tools import settings_bundle

ROOT = Path(__file__).resolve().parents[1]


def test_the_cloud_rows_follow_the_bundle_s_own(tmp_path):
    subprocess.run([sys.executable, ROOT / "tools/settings_bundle.py", ROOT / "port/ios/Settings.bundle", tmp_path / "Settings.bundle"],
                   check=True)
    source = plistlib.loads((ROOT / "port/ios/Settings.bundle/Root.plist").read_bytes())
    written = plistlib.loads((tmp_path / "Settings.bundle/Root.plist").read_bytes())
    rows = written["PreferenceSpecifiers"]
    assert rows[: len(source["PreferenceSpecifiers"])] == source["PreferenceSpecifiers"]
    assert [row.get("Key") for row in rows[len(source["PreferenceSpecifiers"]):]] == [
        None, "cloud_saves", "cloud_saves_status", "cloud_saves_restore", "cloud_saves_delete"]
    assert written["StringsTable"] == source["StringsTable"]


def test_the_keys_are_the_ones_the_host_reads():
    host = (ROOT / "port/ios/host/host_cloud_saves.m").read_text()
    for row in settings_bundle.CLOUD_ROWS:
        if "Key" in row:
            assert f'@"{row["Key"]}"' in host


def test_a_bundle_that_has_the_rows_is_refused():
    root = settings_bundle.with_cloud_rows({"PreferenceSpecifiers": []})
    with pytest.raises(ValueError, match="already has"):
        settings_bundle.with_cloud_rows(root)


def test_cloud_saves_require_player_opt_in():
    rows = settings_bundle.with_cloud_rows({"PreferenceSpecifiers": []})["PreferenceSpecifiers"]
    switch = next(row for row in rows if row.get("Key") == "cloud_saves")
    assert switch["DefaultValue"] is False


@pytest.mark.parametrize("cloud", ["on", "off"])
@pytest.mark.parametrize("shader", ["on", "off"])
def test_generated_settings_follow_each_build_capability(tmp_path, cloud, shader):
    output = tmp_path / "Settings.bundle"
    subprocess.run([sys.executable, ROOT / "tools/settings_bundle.py",
                    ROOT / "port/ios/Settings.bundle", output,
                    "--cloud-saves", cloud, "--shader-upload", shader], check=True)
    rows = plistlib.loads((output / "Root.plist").read_bytes())["PreferenceSpecifiers"]
    keys = {row.get("Key"): row for row in rows}
    assert ("cloud_saves" in keys) == (cloud == "on")
    assert ("share_shader_improvements" in keys) == (shader == "on")
    if shader == "on":
        assert keys["share_shader_improvements"]["DefaultValue"] is True
        from tools.release import SHADER_UPLOAD_DISCLOSURE
        assert any(row.get("FooterText") == SHADER_UPLOAD_DISCLOSURE for row in rows)


def test_base_settings_do_not_offer_inactive_shader_controls():
    root = plistlib.loads((ROOT / "port/ios/Settings.bundle/Root.plist").read_bytes())
    assert not any(row.get("Key") == "share_shader_improvements" for row in root["PreferenceSpecifiers"])
    with pytest.raises(ValueError, match="already has"):
        settings_bundle.with_shader_rows(settings_bundle.with_shader_rows(root))


@pytest.mark.parametrize("cloud", [False, True])
def test_cmake_settings_selection_resets_after_shader_upload_turns_off(tmp_path, cloud):
    cmake = (ROOT / "port/ios/CMakeLists.txt").read_text()
    start = cmake.index('set(HALO_SETTINGS_BUNDLE "${CMAKE_CURRENT_SOURCE_DIR}/Settings.bundle")')
    block = cmake[start:cmake.index("add_custom_command(", start)]
    script = tmp_path / "settings.cmake"
    selected = tmp_path / "selected.txt"
    for shader in (True, False):
        script.write_text(
            f'set(ROOT "{ROOT}")\nset(Python3_EXECUTABLE "{sys.executable}")\n'
            f'set(CMAKE_CURRENT_SOURCE_DIR "{ROOT / "port/ios"}")\n'
            f'set(CMAKE_CURRENT_BINARY_DIR "{tmp_path}")\n'
            f'set(HALO_CLOUD_SAVES {"ON" if cloud else "OFF"})\n'
            f'set(HALO_SHADER_UPLOAD {"ON" if shader else "OFF"})\n'
            + block + f'file(WRITE "{selected}" "${{HALO_SETTINGS_BUNDLE}}")\n'
        )
        subprocess.run(["cmake", "-P", script], check=True)
        bundle = Path(selected.read_text())
        assert (bundle == ROOT / "port/ios/Settings.bundle") == (not cloud and not shader)
        rows = plistlib.loads((bundle / "Root.plist").read_bytes())["PreferenceSpecifiers"]
        keys = {row.get("Key") for row in rows}
        assert ("cloud_saves" in keys) == cloud
        assert ("share_shader_improvements" in keys) == shader
