"""The offline publisher shares runtime validation and uses standard Ed25519."""
import base64
import hashlib
import json
import re
from pathlib import Path
import shutil
import subprocess

import pytest
import data_pack


def recording(folder, name="test-route"):
    folder.mkdir(parents=True, exist_ok=True)
    files = {
        "input": "halo input recording 1\n0 0000 0 0 0 0 0 0 0 0 0 0 0 0\n",
        "actions": "halo action recording 1\n0 0 00000000 00000000 00000000 00000000 80000000 00000000 ffffffff 0000ffff\n",
        "seeds": "halo seed recording 1\n0 0 00000000 12345678\n",
        "route": "map = a10\ndifficulty = normal\nentered = start\nfirst_tick = 0\nplayers = 1\n",
    }
    for extension, content in files.items():
        (folder / f"{name}.{extension}").write_text(content)
    return folder


def test_minimal_recording(tmp_path):
    data_pack.validate_recordings(recording(tmp_path / "recordings"))


def large_recording_pack(folder):
    row = b"0 0000 0 0 0 0 0 0 0 0 0 0 0 0\n"
    rows = (3_600_000 + len(row) - 1) // len(row)
    for index in range(4):
        take = f"large-take-{index}"
        recording(folder, take)
        (folder / f"{take}.input").write_bytes(b"halo input recording 1\n" + row * rows)
    return folder


def test_large_recording_snapshot_and_published_envelope(tmp_path):
    folder = large_recording_pack(tmp_path / "recordings")
    files = data_pack.snapshot(folder, "recordings", "benchmark-replay-1:" + "a" * 64)
    assert sum(map(len, files.values())) > 12 * 1024 * 1024

    args = arguments(tmp_path)
    args[args.index("--input") + 1] = str(folder)
    data_pack.main(args)
    assert (tmp_path / "pack.json").stat().st_size > 24 * 1024 * 1024


@pytest.mark.parametrize("extension,content", [
    ("input", "wrong header\n"), ("actions", "halo action recording 2\n"),
    ("seeds", "halo seed recording 1\n0 1 0 0\n"),
    ("input", "halo input recording 1\nnan 0 0 0 0 0 0 0 0 0 0 0 0 0\n"),
    ("input", "halo input recording 1\n999999999999 0 0 0 0 0 0 0 0 0 0 0 0 0\n"),
    ("input", "halo input recording 1\n0 0 256 0 0 0 0 0 0 0 0 0 0 0\n"),
    ("input", "halo input recording 1\n0 0 0 0 0 0 0 0 0 0 32768 0 0 0\n"),
    ("actions", "halo action recording 1\n0 0 deadbeef\n"),
    ("actions", "halo action recording 1\n0 0 0 7f800000 0 0 0 0 0 0\n"),
    ("actions", "halo action recording 1\n0 999999999999 0 0 0 0 0 0 0 0\n"),
    ("route", "map = ../bad\ndifficulty = normal\n"),
    ("route", "map = a10\ndifficulty = normal\nlead = nan\n"),
    ("route", "map = a10\ndifficulty = normal\nplayers = 2\n"),
    ("route", "map = a10\ndifficulty = normal\nfirst_tick = nope\n"),
    ("checks", "nan:0,0,0"), ("checks", "0:0,0,0;garbage"),
])
def test_reject_records(tmp_path, extension, content):
    folder = recording(tmp_path / "recordings")
    (folder / f"test-route.{extension}").write_text(content)
    with pytest.raises(ValueError):
        data_pack.validate_recordings(folder)


@pytest.mark.parametrize("extension", ["input", "actions", "seeds", "route"])
def test_missing_sidecar(tmp_path, extension):
    folder = recording(tmp_path / "recordings")
    (folder / f"test-route.{extension}").unlink()
    with pytest.raises(ValueError):
        data_pack.validate_recordings(folder)


def test_symlink_and_unknown_file(tmp_path):
    folder = recording(tmp_path / "recordings")
    extra = folder / "bad.cfg"
    extra.write_text("quit")
    with pytest.raises(ValueError):
        data_pack.validate_recordings(folder)
    extra.unlink()
    target = folder / "test-route.input"
    target.rename(tmp_path / "outside")
    target.symlink_to(tmp_path / "outside")
    with pytest.raises(ValueError):
        data_pack.validate_recordings(folder)


def test_nested_directory(tmp_path):
    folder = recording(tmp_path / "recordings")
    (folder / "nested").mkdir()
    with pytest.raises(ValueError):
        data_pack.validate_recordings(folder)


@pytest.mark.parametrize("count", [14, 33])
def test_route_limit(tmp_path, count):
    folder = tmp_path / "recordings"
    for number in range(count):
        recording(folder, f"route-{number}")
    with pytest.raises(ValueError):
        data_pack.validate_recordings(folder)


def test_nonmatching_seed_order(tmp_path):
    folder = recording(tmp_path / "recordings")
    with (folder / "test-route.seeds").open("a") as stream:
        stream.write("0 1 0 0\n")
    with pytest.raises(ValueError):
        data_pack.validate_recordings(folder)


def arguments(tmp_path):
    key = tmp_path / "seed"
    key.write_bytes(bytes(range(32)))
    key.chmod(0o600)
    return ["--kind", "recordings", "--channel", "tester", "--sequence", "1",
        "--compatibility", "benchmark-replay-1:" + "a" * 64,
        "--input", str(recording(tmp_path / "recordings")), "--output", str(tmp_path / "pack.json"),
        "--signing-key", str(key)]


def test_deterministic_envelope_and_signature(tmp_path):
    args = arguments(tmp_path)
    data_pack.main(args)
    first = (tmp_path / "pack.json").read_bytes()
    data_pack.main(args)
    assert (tmp_path / "pack.json").read_bytes() == first
    envelope = json.loads(first)
    assert set(envelope) == {"manifest", "signature", "payload"}
    manifest = base64.b64decode(envelope["manifest"], validate=True)
    payload = base64.b64decode(envelope["payload"], validate=True)
    signature = base64.b64decode(envelope["signature"], validate=True)
    assert json.loads(manifest)["sha256"] == hashlib.sha256(payload).hexdigest()
    assert set(json.loads(payload)) == {"files"}
    public = subprocess.check_output([str(data_pack.sign_helper()), "--public-key", str(tmp_path / "seed")]).decode().strip()
    assert len(public) == 64
    (tmp_path / "manifest").write_bytes(manifest)
    (tmp_path / "signature").write_bytes(signature)
    checker = [str(data_pack.sign_helper()), "--verify", public, str(tmp_path / "manifest"), str(tmp_path / "signature")]
    assert subprocess.run(checker).returncode == 0
    (tmp_path / "manifest").write_bytes(manifest + b" ")
    assert subprocess.run(checker).returncode != 0
    openssl = shutil.which("openssl")
    if openssl:
        (tmp_path / "public.der").write_bytes(bytes.fromhex("302a300506032b6570032100" + public))
        (tmp_path / "manifest").write_bytes(manifest)
        command = [openssl, "pkeyutl", "-verify", "-pubin", "-inkey", str(tmp_path / "public.der"),
            "-keyform", "DER", "-rawin", "-in", str(tmp_path / "manifest"), "-sigfile", str(tmp_path / "signature")]
        result = subprocess.run(command, capture_output=True)
        if b"unsupported" not in result.stderr and b"Invalid operation" not in result.stderr:
            assert result.returncode == 0, result.stderr
            print("OpenSSL independently verified Ed25519 signature")
            (tmp_path / "manifest").write_bytes(manifest + b" ")
            assert subprocess.run(command, capture_output=True).returncode != 0


@pytest.mark.parametrize("option,value", [("--sequence", "0"), ("--sequence", "9007199254740992"),
    ("--compatibility", "benchmark-replay-1:bad"), ("--minimum-engine", "2"), ("--maximum-engine", "2")])
def test_invalid_manifest(tmp_path, option, value):
    args = arguments(tmp_path)
    if option in args:
        args[args.index(option) + 1] = value
    else:
        args += [option, value]
    with pytest.raises((ValueError, SystemExit)):
        data_pack.main(args)
    assert not (tmp_path / "pack.json").exists()


def test_private_key_permissions(tmp_path):
    args = arguments(tmp_path)
    (tmp_path / "seed").chmod(0o644)
    with pytest.raises((ValueError, subprocess.CalledProcessError)):
        data_pack.main(args)
    assert not (tmp_path / "pack.json").exists()


def test_profiles_snapshot():
    files = data_pack.snapshot(data_pack.ROOT / "profiles", "profiles")
    assert "hardware/catalog.toml" in files
    assert "README.md" not in files
    assert "TEMPLATE.toml" not in files
    assert files["apple-tv-hd.toml"] == (data_pack.ROOT / "profiles/apple-tv-hd.toml").read_bytes()


def test_bad_profile_and_catalog(tmp_path):
    source = data_pack.ROOT / "profiles"
    folder = tmp_path / "profiles"
    shutil.copytree(source, folder)
    profile = folder / "apple-tv-hd.toml"
    with profile.open("a") as stream:
        stream.write('\n[debug]\nexit_after = 10\n')
    with pytest.raises(ValueError, match="profile snapshot rejected"):
        data_pack.snapshot(folder, "profiles")
    shutil.copyfile(source / profile.name, profile)
    catalog = folder / "hardware/catalog.toml"
    catalog.write_text(catalog.read_text().replace('profile = "ipad-pro-m1.toml"', 'profile = "missing.toml"'))
    with pytest.raises(ValueError, match="profile snapshot rejected"):
        data_pack.snapshot(folder, "profiles")


@pytest.mark.parametrize("name", ["device-profile.toml", "init.txt", "A.input", "a..input", "a_.input", "a" * 49 + ".input"])
def test_bad_snapshot_name(tmp_path, name):
    folder = recording(tmp_path / "recordings")
    (folder / name).write_text("bad")
    with pytest.raises(ValueError):
        data_pack.snapshot(folder, "recordings")


def test_null_and_suffix_records(tmp_path):
    folder = recording(tmp_path / "recordings")
    path = folder / "test-route.input"
    original = path.read_bytes()
    for suffix in (b"\0hidden", b"garbage\n", b"0 0 0 0 0 0 0 0 0 0 0 0 0 0 EXTRA\n"):
        path.write_bytes(original + suffix)
        with pytest.raises(ValueError):
            data_pack.validate_recordings(folder)


def test_file_size_limit(tmp_path):
    folder = recording(tmp_path / "recordings")
    path = folder / "test-route.input"
    with path.open("wb") as stream:
        stream.truncate(4 * 1024 * 1024 + 1)
    with pytest.raises(ValueError):
        data_pack.validate_recordings(folder)
    with pytest.raises(ValueError):
        data_pack.snapshot(folder, "recordings")


def test_root_symlink(tmp_path):
    folder = recording(tmp_path / "recordings")
    (tmp_path / "link").symlink_to(folder, target_is_directory=True)
    with pytest.raises(ValueError):
        data_pack.validate_recordings(tmp_path / "link")
    with pytest.raises(OSError):
        data_pack.snapshot(tmp_path / "link", "recordings")


def test_key_symlink_and_size(tmp_path):
    args = arguments(tmp_path)
    key = tmp_path / "seed"
    key.rename(tmp_path / "real-seed")
    key.symlink_to(tmp_path / "real-seed")
    with pytest.raises(subprocess.CalledProcessError):
        data_pack.main(args)
    key.unlink()
    key.write_bytes(b"x" * 31)
    key.chmod(0o600)
    with pytest.raises(subprocess.CalledProcessError):
        data_pack.main(args)


def test_duplicate_scene_and_builtin_scene(tmp_path):
    folder = recording(tmp_path / "recordings")
    recording(folder, "another-route")
    first = folder / "test-route.route"
    second = folder / "another-route.route"
    first.write_text(first.read_text() + "scene = duplicate\n")
    second.write_text(second.read_text() + "scene = duplicate\n")
    with pytest.raises(ValueError):
        data_pack.validate_recordings(folder)
    first.write_text(first.read_text().replace("duplicate", "menu"))
    second.write_text(second.read_text().replace("duplicate", "another"))
    with pytest.raises(ValueError):
        data_pack.validate_recordings(folder)


def test_numeric_overflow_and_trailing_route_junk(tmp_path):
    folder = recording(tmp_path / "recordings")
    route = folder / "test-route.route"
    original = route.read_text()
    for line in ("first_tick = 999999999999999999999999999999", "first_tick = 0 garbage",
                 "first_tick = -1", "window = inf", "init = quit", "map = a10\nmap = c40"):
        route.write_text(original + line + "\n")
        with pytest.raises(ValueError):
            data_pack.validate_recordings(folder)


def test_good_optional_metadata_and_checks(tmp_path):
    folder = recording(tmp_path / "recordings")
    route = folder / "test-route.route"
    route.write_text(route.read_text() + 'scene = portable\nlead = 1.5\nwindow = 30\nstereo = screen\n'
                     'segment_seed = 0 0 abcdef00\ndisplay.frame_rate = "display"\n')
    actions = folder / "test-route.actions"
    actions.write_text(actions.read_text().replace("halo action recording 1", "halo action recording 1 stereo=screen"))
    (folder / "test-route.checks").write_text("0:1.2,-3.4,5;30:0,0,0\n")
    data_pack.validate_recordings(folder)


@pytest.mark.parametrize("metadata", ["lip_sync=original", "lip_sync=simulated stereo=head",
                                      "stereo=off lip_sync=simulated", "enhanced_animations=0",
                                      "lip_sync=simulated enhanced_animations=1"])
def test_recording_mouth_metadata(tmp_path, metadata):
    folder = recording(tmp_path / "recordings")
    actions = folder / "test-route.actions"
    actions.write_text(actions.read_text().replace("halo action recording 1", "halo action recording 1 " + metadata))
    data_pack.validate_recordings(folder)


@pytest.mark.parametrize("metadata", ["lip_sync=unknown", "lip_sync=original lip_sync=simulated",
                                      "lip_sync=simulated stereo=off stereo=head", "enhanced_animations=true",
                                      "enhanced_animations=0 enhanced_animations=1",
                                      "enhanced_animations=2"])
def test_recording_refuses_invalid_mouth_metadata(tmp_path, metadata):
    folder = recording(tmp_path / "recordings")
    actions = folder / "test-route.actions"
    actions.write_text(actions.read_text().replace("halo action recording 1", "halo action recording 1 " + metadata))
    with pytest.raises(ValueError):
        data_pack.validate_recordings(folder)


def test_bad_update_preserves_existing_output(tmp_path):
    args = arguments(tmp_path)
    output = tmp_path / "pack.json"
    output.write_bytes(b"previous artifact")
    (tmp_path / "recordings/test-route.seeds").unlink()
    with pytest.raises(ValueError):
        data_pack.main(args)
    assert output.read_bytes() == b"previous artifact"


def test_profile_filename_limit(tmp_path):
    folder = tmp_path / "profiles"
    shutil.copytree(data_pack.ROOT / "profiles", folder)
    (folder / ("x" * 59 + ".toml")).write_text("schema = 1\n")
    with pytest.raises(ValueError, match="file is not allowed"):
        data_pack.snapshot(folder, "profiles")


def test_notes_escaped_boundary(tmp_path):
    args = arguments(tmp_path)
    data_pack.main(args + ["--notes", "\x01" * 4096])
    envelope = json.loads((tmp_path / "pack.json").read_bytes())
    manifest = json.loads(base64.b64decode(envelope["manifest"]))
    assert manifest["notes"] == "\x01" * 4096
    with pytest.raises(ValueError, match="notes must be at most"):
        data_pack.main(args + ["--notes", "\x01" * 4097])


@pytest.mark.parametrize("name", ["bad\nname", "bad\tname", "bad\x7fname", "x" * 64, "é" * 32])
def test_profile_name_cannot_inject_settings(tmp_path, name):
    folder = tmp_path / "profiles"
    shutil.copytree(data_pack.ROOT / "profiles", folder)
    path = folder / "apple-tv-hd.toml"
    literal = json.dumps(name)
    changed, count = re.subn(r"^name =.*$", lambda match: "name = " + literal, path.read_text(), count=1, flags=re.M)
    assert count == 1
    path.write_text(changed)
    with pytest.raises(ValueError, match="profile snapshot rejected"):
        data_pack.snapshot(folder, "profiles")


def test_external_profiles_require_new_compatibility(tmp_path):
    args = arguments(tmp_path)
    args[args.index("--kind") + 1] = "profiles"
    args[args.index("--input") + 1] = str(data_pack.ROOT / "profiles")
    args[args.index("--compatibility") + 1] = "profiles-1"
    with pytest.raises(ValueError, match="external sections"):
        data_pack.main(args)
    args[args.index("--compatibility") + 1] = "profiles-2"
    assert data_pack.main(args) == 0
    envelope = json.loads((tmp_path / "pack.json").read_bytes())
    manifest = json.loads(base64.b64decode(envelope["manifest"]))
    assert manifest["compatibility"] == "profiles-2"
