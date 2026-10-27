"""The device-profile feed generation and the profile set's revision (docs/data-packs.md)."""
import base64
import hashlib
import json
from pathlib import Path
import re
import shutil

import pytest

import data_pack
import ios_build
import profile_revision

ROOT = Path(__file__).resolve().parents[1]
KEYS = ROOT / "port/linux/src/port_profile_keys.h"
# The profile key table generation 2 validates. When port_profile_keys.h changes, apps of this generation would
# refuse shards signed with the new table: bump HOST_PROFILE_FEED_GENERATION (host_profile.h), PROFILE_FEED_GENERATION
# (data_pack.py) and the directory names in ios_build.py, profile_pr.py and pages_asset_overlay.py, then record the
# new table here.
# Generation 2 had not shipped when PROFILE_VARIABLE was added (no release read it, no shard was signed), so its table
# was re-recorded rather than a generation 3 opened, and again for display.shadow_minimum_size (still unreleased).
GENERATION_KEYS = {2: "6a4fb7669346d05409e78d6bf38c08123257bb66555ea9dfa340a7970f655888"}


def key_table_sha256(text):
    lines = [" ".join(line.split()) for line in text.splitlines() if re.fullmatch(r"PROFILE_[A-Z]+\(.*\)\s*", line)]
    return hashlib.sha256(("\n".join(lines) + "\n").encode()).hexdigest()


def test_profile_keys_match_the_feed_generation():
    assert key_table_sha256(KEYS.read_text()) == GENERATION_KEYS[data_pack.PROFILE_FEED_GENERATION], (
        "port_profile_keys.h changed: bump the profile feed generation (see GENERATION_KEYS)")


def test_generation_constants_agree():
    header = (ROOT / "port/ios/host/host_profile.h").read_text()
    generation = int(re.search(r"HOST_PROFILE_FEED_GENERATION = (\d+)", header)[1])
    domain = re.search(r'#define HOST_PROFILE_FEED_DOMAIN "([^"]+)"', header)[1]
    assert generation == data_pack.PROFILE_FEED_GENERATION
    profile_pr = re.search(r'^PROFILE_DOMAIN = "([^"]+)"', (ROOT / "tools/profile_pr.py").read_text(), re.M)[1]
    pages = (ROOT / "tools/pages_asset_overlay.py").read_text()
    assert domain == data_pack.PROFILE_FEED_DOMAIN == ios_build.PROFILE_FEED_DIRECTORY == profile_pr
    assert f'"{domain}": "{domain}"' in pages
    assert data_pack.DEVICE_PROFILE_DOMAINS[-1] == domain


def test_repository_revision_matches_the_profiles():
    assert profile_revision.check() >= 1


def test_a_changed_profile_needs_a_new_revision(tmp_path):
    folder = tmp_path / "profiles"
    shutil.copytree(ROOT / "profiles", folder)
    first = profile_revision.check(folder)
    (folder / "iphone-17-pro.toml").write_text((folder / "iphone-17-pro.toml").read_text() + "\n")
    with pytest.raises(ValueError, match="--bump"):
        profile_revision.check(folder)
    assert profile_revision.bump(folder) == first + 1
    assert profile_revision.check(folder) == first + 1
    # documentation is not an input
    (folder / "README.md").write_text("changed")
    (folder / "TEMPLATE.toml").write_text("changed")
    assert profile_revision.check(folder) == first + 1


@pytest.mark.parametrize("text", [
    b"schema = 1\nrevision = 0\nsha256 = \"" + b"a" * 64 + b"\"\n",
    b"schema = 2\nrevision = 1\nsha256 = \"" + b"a" * 64 + b"\"\n",
    b"schema = 1\nrevision = 1\nsha256 = \"" + b"A" * 64 + b"\"\n",
    b"schema = 1\nrevision = 1\n",
    b"schema = 1\nrevision = 9007199254740992\nsha256 = \"" + b"a" * 64 + b"\"\n",
    b"",
])
def test_malformed_revisions_are_refused(text):
    with pytest.raises(ValueError):
        profile_revision.parse(text)


def payload_files(envelope):
    payload = json.loads(base64.b64decode(json.loads(envelope)["payload"]))
    return {name: base64.b64decode(value) for name, value in payload["files"].items()}


def manifest(envelope):
    return json.loads(base64.b64decode(json.loads(envelope)["manifest"]))


def signing_key(tmp_path):
    seed = tmp_path / "seed"
    seed.write_bytes(bytes(range(32)))
    seed.chmod(0o600)
    return seed


def test_generation_two_shards_carry_the_revision_and_every_bundled_key(tmp_path):
    output = tmp_path / "shard.json"
    assert data_pack.main(["--kind", "profiles", "--channel", "tester", "--sequence", "1",
        "--compatibility", "device-profiles-2:ios/iPhone18,1/_", "--input", str(ROOT / "profiles"),
        "--signing-key", str(signing_key(tmp_path)), "--output", str(output)]) == 0
    envelope = output.read_bytes()
    assert manifest(envelope)["compatibility"] == "device-profiles-2:ios/iPhone18,1/_"
    files = payload_files(envelope)
    assert files["hardware/revision.toml"] == (ROOT / "profiles/hardware/revision.toml").read_bytes()
    assert b"player_capacity = 32" in files["iphone-17-pro.toml"]
    assert b"player_capacity = 8" in files["iphone-ipad.toml"]
    # the Apple TV 4K (3rd generation) shard keeps upscaled textures on, which its device-profiles-1 shard does not
    tv = data_pack.device_profile_snapshot(ROOT / "profiles", "tvos", "AppleTV14,1", generation=2)
    assert b"upscaled_textures = true" in tv["apple-tv-4k-3rd.toml"]
    live = payload_files((ROOT / "docs/site/packs/tester/profiles/tvos/AppleTV14,1/_.json").read_bytes())
    assert b"upscaled_textures = true" not in live["apple-tv-4k-3rd.toml"]
    # the same bytes pass the release and Pages verification of a generation-2 feed
    folder = tmp_path / "decoded"
    for name, content in files.items():
        (folder / name).parent.mkdir(parents=True, exist_ok=True)
        (folder / name).write_bytes(content)
    assert data_pack.snapshot(folder, "profiles", "device-profiles-2:ios/iPhone18,1/_") == files


def test_generation_one_shards_stay_as_they_were(tmp_path):
    files = data_pack.device_profile_snapshot(ROOT / "profiles", "ios", "iPhone18,1")
    assert "hardware/revision.toml" not in files
    folder = tmp_path / "v1"
    for name, content in {**files, "hardware/revision.toml": b"x"}.items():
        (folder / name).parent.mkdir(parents=True, exist_ok=True)
        (folder / name).write_bytes(content)
    # 0.4.2 to 0.5.1 refuse a shard holding it, so neither the publisher nor the verifier admits one
    with pytest.raises(ValueError):
        data_pack.snapshot(folder, "profiles", "device-profiles-1:ios/iPhone18,1/_")


def test_a_generation_two_shard_needs_its_revision(tmp_path):
    files = data_pack.device_profile_snapshot(ROOT / "profiles", "ios", "iPhone18,1", generation=2)
    del files["hardware/revision.toml"]
    folder = tmp_path / "v2"
    for name, content in files.items():
        (folder / name).parent.mkdir(parents=True, exist_ok=True)
        (folder / name).write_bytes(content)
    with pytest.raises(ValueError):
        data_pack.snapshot(folder, "profiles", "device-profiles-2:ios/iPhone18,1/_")


def test_signing_refuses_sources_newer_than_their_revision(tmp_path):
    folder = tmp_path / "profiles"
    shutil.copytree(ROOT / "profiles", folder)
    (folder / "iphone-17-pro.toml").write_text((folder / "iphone-17-pro.toml").read_text() + "\n")
    with pytest.raises(ValueError, match="--bump"):
        data_pack.device_profile_snapshot(folder, "ios", "iPhone18,1", generation=2)
    # generation 1 keeps its old semantics
    assert data_pack.device_profile_snapshot(folder, "ios", "iPhone18,1")


def test_the_app_bundles_the_revision():
    assert "profiles/hardware/revision.toml" in (ROOT / "port/ios/CMakeLists.txt").read_text()


def tags():
    import subprocess
    return subprocess.run(["git", "-C", str(ROOT), "tag", "--list", "visr-v*"], capture_output=True, text=True).stdout.split()


@pytest.mark.skipif("visr-v0.5.1" not in tags(), reason="needs the release tags")
def test_released_generations():
    assert data_pack.tag_generation("visr-v0.2.0") is None
    assert data_pack.tag_generation("visr-v0.5.1") == 1
    assert "visr-v0.5.1" in [tag for tag, _ in data_pack.released_validators(1)]


@pytest.mark.skipif("visr-v0.5.1" not in tags(), reason="needs the release tags")
def test_released_apps_check_every_device_shard():
    target = "ios/iPhone18,1/_"
    live = ROOT / "docs/site/packs/tester/profiles" / f"{target}.json"
    # the shard 0.4.2 to 0.5.1 already have passes every one of their validators
    assert "visr-v0.4.2" in data_pack.validate_with_released_apps(payload_files(live.read_bytes()), target, 1)
    # a device-profiles-1 shard from today's sources would not: 0.5.0 and earlier refuse network.player_capacity
    files = data_pack.device_profile_snapshot(ROOT / "profiles", "ios", "iPhone18,1")
    with pytest.raises(ValueError, match="would refuse"):
        data_pack.validate_with_released_apps(files, target, 1)


def test_native_validators_hold_each_generation_to_its_revision_rule(tmp_path):
    import subprocess
    probe = str(data_pack.profile_delivery_helper())
    with_revision = data_pack.device_profile_snapshot(ROOT / "profiles", "ios", "iPhone18,1", generation=2)
    without = {name: content for name, content in with_revision.items() if name != "hardware/revision.toml"}
    for name, files in (("with", with_revision), ("without", without)):
        for path, content in files.items():
            (tmp_path / name / path).parent.mkdir(parents=True, exist_ok=True)
            (tmp_path / name / path).write_bytes(content)

    def admits(command, name):
        return subprocess.run([probe, command, str(tmp_path / name), "ios", "iPhone18,1", ""], capture_output=True).returncode == 0
    assert admits("validate", "without") and not admits("validate", "with")
    assert admits("validate-2", "with") and not admits("validate-2", "without")
    (tmp_path / "with/hardware/revision.toml").write_text("schema = 1\nrevision = 1\n")
    assert not admits("validate-2", "with")
