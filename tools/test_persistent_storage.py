"""Run the real save opener and Save and Quit call chain against temporary files."""
import os
from pathlib import Path
import re
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
SIZE = 65536


def function(source, name):
    match = re.search(r"^(?:static )?[\w *]+\b" + name + r"\(\s*[^;{]*\)\s*\{", source, re.M)
    assert match, name
    start = source.index("{", match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


@pytest.fixture(scope="module")
def probe(tmp_path_factory):
    folder = tmp_path_factory.mktemp("persistent-storage-probe")
    source_root = Path(os.environ.get("PERSISTENT_STORAGE_SOURCE_ROOT", ROOT))
    storage = (source_root / "source/saved games/game_state_xbox.c").read_text()
    state = (source_root / "source/saved games/game_state.c").read_text()
    ui = (source_root / "source/interface/ui_widget_event_handler_functions.c").read_text()
    functions = "\n".join([
        function(storage, "game_state_open_persistent_storage"),
        function(storage, "game_state_write_to_persistent_storage"),
        function(state, "game_state_save_to_persistent_storage"),
        function(ui, "pause_game_quit_to_main_menu"),
    ])
    harness = (ROOT / "tools/harness/tests/persistent_storage.c").read_text()
    source = folder / "probe.c"
    source.write_text(harness.replace("/* PRODUCTION FUNCTIONS */", functions))
    binary = folder / ("probe.exe" if os.name == "nt" else "probe")
    compiler = shutil.which(os.environ.get("CC", "clang"))
    assert compiler, "clang is required for the persistent-storage regression"
    baseline_flags = ["-Wno-error=format"] if source_root != ROOT else []
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", *baseline_flags, "-Wno-unused-function",
                    "-Wno-unused-parameter", "-Wno-sign-compare", str(source), "-o", str(binary)], check=True)
    return binary


def run(probe, folder, mode="open", fault="none"):
    result = subprocess.run([str(probe), str(folder) + os.sep, mode, fault],
                            capture_output=True, text=True, check=True)
    return dict(item.split("=") for item in result.stdout.strip().split()), result.stderr


@pytest.mark.parametrize("size", [0, 31, SIZE - 1, SIZE + 1])
def test_existing_wrong_size_preserved(probe, tmp_path, size, mode="open"):
    path = tmp_path / "savegame.bin"
    original = bytes((i % 251) + 1 for i in range(size))
    path.write_bytes(original)
    result, message = run(probe, tmp_path, mode=mode)
    assert path.read_bytes() == original
    assert result["ok"] == "0"
    assert result["writes"] == "0"
    assert result["live"] == "0"
    assert message


@pytest.mark.parametrize("fault", ["size", "high", "denied"])
def test_existing_file_io_errors_preserve_bytes(probe, tmp_path, fault):
    path = tmp_path / "savegame.bin"
    original = b"p" * SIZE
    path.write_bytes(original)
    result, message = run(probe, tmp_path, fault=fault)
    assert result["ok"] == "0"
    assert path.read_bytes() == original
    assert result["writes"] == result["live"] == "0"
    assert message


def test_existing_valid_file_not_initialized(probe, tmp_path):
    path = tmp_path / "savegame.bin"
    original = b"v" * SIZE
    path.write_bytes(original)
    result, _ = run(probe, tmp_path)
    assert result["ok"] == "1"
    assert result["writes"] == result["live"] == "0"
    assert path.read_bytes() == original


def test_missing_file_created(probe, tmp_path):
    result, _ = run(probe, tmp_path)
    assert result["ok"] == "1"
    assert result["live"] == "0"
    assert (tmp_path / "savegame.bin").read_bytes() == bytes(SIZE)


def test_file_appearing_during_create_is_not_replaced(probe, tmp_path):
    result, message = run(probe, tmp_path, fault="race")
    assert result["ok"] == "0"
    assert result["writes"] == result["live"] == "0"
    assert (tmp_path / "savegame.bin").read_bytes() == b"racing save"
    assert message


@pytest.mark.parametrize("fault", ["write", "short", "seek", "truncate"])
def test_new_file_initialization_failure_closes_without_deletion(probe, tmp_path, fault):
    result, message = run(probe, tmp_path, fault=fault)
    assert result["ok"] == "0"
    assert result["live"] == result["deletes"] == "0"
    assert message


@pytest.mark.parametrize("fault", ["none", "size", "denied"])
def test_save_quit_preflight_failure_keeps_session(probe, tmp_path, fault):
    path = tmp_path / "savegame.bin"
    original = b"old incompatible save"
    path.write_bytes(original)
    result, _ = run(probe, tmp_path, mode="quit", fault=fault)
    assert result["ok"] == "0"
    assert result["notes"] == result["reverts"] == result["menus"] == result["writes"] == "0"
    assert result["dialogs"] == result["paused"] == "1"
    assert result["live"] == "0"
    assert path.read_bytes() == original


@pytest.mark.parametrize("exists", [False, True])
def test_successful_save_quit(probe, tmp_path, exists):
    path = tmp_path / "savegame.bin"
    if exists:
        path.write_bytes(b"v" * SIZE)
    result, _ = run(probe, tmp_path, mode="quit")
    assert result["ok"] == result["notes"] == result["reverts"] == result["menus"] == "1"
    assert result["dialogs"] == result["live"] == "0"
    assert path.read_bytes()[64:] == b"c" * (SIZE - 64)


@pytest.mark.parametrize("fault", ["write", "short", "seek", "header_write"])
def test_write_failure_does_not_quit(probe, tmp_path, fault):
    (tmp_path / "savegame.bin").write_bytes(b"v" * SIZE)
    result, message = run(probe, tmp_path, mode="quit", fault=fault)
    assert result["ok"] == result["menus"] == result["live"] == "0"
    assert result["dialogs"] == result["reverts"] == result["paused"] == "1"
    assert message


def test_save_reuses_preflight_handle(probe, tmp_path):
    (tmp_path / "savegame.bin").write_bytes(b"v" * SIZE)
    result, _ = run(probe, tmp_path, mode="quit", fault="replace")
    assert result["ok"] == "1"
    assert (tmp_path / "savegame.bin").read_bytes() == b"replacement"
    assert (tmp_path / "original.bin").read_bytes()[64:] == b"c" * (SIZE - 64)
    assert result["live"] == "0"


def test_explicit_directory_rejected(probe, tmp_path):
    test_existing_wrong_size_preserved(probe, tmp_path, 31, mode="directory")
