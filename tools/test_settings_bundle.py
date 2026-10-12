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
