"""Signed device shards and shader additions use the runtime admission contract."""
from pathlib import Path
import shutil
import subprocess

import pytest
import data_pack

ROOT = Path(__file__).resolve().parents[1]


def shader_text(exact=15):
    # Hand-checked FNV-1a of 252 zero bytes.
    return ("vs 0000000000000001 0 0\n"
            "ps 2fba1332b14beb55 " + "00" * 252 + "\n"
            f"pipeline 0000000000000001.0.0 2fba1332b14beb55 0 0 0 0 15 {exact} 1 "
            + "00" * 16 + " 0 0\n")


def test_signed_shader_snapshot_accepts_complete_map(tmp_path):
    (tmp_path / "a10.txt").write_text(shader_text())
    files = data_pack.snapshot(tmp_path, "shaders", "shader-lists-1")
    assert set(files) == {"a10.txt"}


@pytest.mark.parametrize("change", [
    lambda s: s.replace("2fba1332b14beb55", "0000000000000000"),
    lambda s: s.replace("15 15 1", "15 16 1"),
    lambda s: "\n".join(s.splitlines()[1:]) + "\n",
    lambda s: s + "unknown 1\n",
])
def test_shader_snapshot_rejects_corrupt_descriptors(tmp_path, change):
    (tmp_path / "a10.txt").write_text(change(shader_text()))
    with pytest.raises(ValueError):
        data_pack.snapshot(tmp_path, "shaders", "shader-lists-1")


def test_profile_shard_keeps_only_target_and_class(tmp_path):
    files = data_pack.device_profile_snapshot(ROOT / "profiles", "ios", "iPhone18,1", "")
    assert set(files) == {"iphone-ipad.toml", "iphone-17-pro.toml", "hardware/catalog.toml"}
    assert b'"tvos"' not in files["hardware/catalog.toml"]


def test_device_profile_key_is_canonical_and_target_bound():
    assert data_pack.device_profile_key("ios", "iPhone18,1", "") == "ios/iPhone18,1/_"
    assert data_pack.device_profile_key("mac", "Mac14,5", "Apple M2 Max") == "mac/Mac14,5/Apple-M2-Max"
    for model in ["../iPhone18,1", "iPhone18,1?x=1", "iPhone18,1/other", ".", ""]:
        with pytest.raises(ValueError):
            data_pack.device_profile_key("ios", model, "")


def test_real_bundled_shader_corpus_matches_delivery_abi():
    files = data_pack.snapshot(ROOT / "port/shader-lists", "shaders", "shader-lists-2")
    assert sum(map(len, files.values())) <= 16 * 1024 * 1024


def test_shader_snapshot_aggregate_limit(tmp_path):
    maximum_file = 4 * 1024 * 1024
    text = shader_text().encode("ascii")
    padded = text + b"\n" * (maximum_file - len(text))
    for index in range(4):
        (tmp_path / f"map{index}.txt").write_bytes(padded)
    files = data_pack.snapshot(tmp_path, "shaders", "shader-lists-2")
    assert sum(map(len, files.values())) == 16 * 1024 * 1024
    (tmp_path / "overflow.txt").write_bytes(b"\n")
    with pytest.raises(ValueError, match="total size"):
        data_pack.snapshot(tmp_path, "shaders", "shader-lists-2")
    with pytest.raises(ValueError, match="shader"):
        data_pack.validate_shaders(tmp_path)


def test_inherited_device_shard_keeps_its_source_envelope():
    files = data_pack.device_profile_snapshot(ROOT / "profiles", "ios", "iPad13,4")
    # Every source at or below the target's tier ships, so the device can fall back when the nearer one's envelope rejects it.
    assert set(files) == {"iphone-ipad.toml", "ipad-pro-m1.toml", "ipad-pro-12-9-1st.toml", "ipad-pro-12-9-2nd.toml",
                          "iphone-16e.toml", "iphone-17-pro.toml", "hardware/catalog.toml"}
    catalog = files["hardware/catalog.toml"]
    assert b"ipad-pro-m1-11" in catalog and b"ipad-pro-m1-12-9" in catalog


def write_shard(tmp_path, files):
    for name, content in files.items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)


@pytest.mark.parametrize("text", [
    'schema=1\n',
    'schema=1\nvalidated_maximum_players=128\nevidence="Measured active combat on this hardware"\n',
])
def test_optional_capacity_is_validated_without_becoming_a_profile(tmp_path, text):
    files = data_pack.device_profile_snapshot(ROOT / "profiles", "ios", "iPhone18,1")
    write_shard(tmp_path, files)
    (tmp_path / "capacity.toml").write_text(text)
    result = data_pack.snapshot(tmp_path, "profiles", "device-profiles-1:ios/iPhone18,1/_")
    assert result["capacity.toml"] == text.encode()


@pytest.mark.parametrize("text", ['schema=1\nvalidated_maximum_players=129\n',
    'schema=1\nvalidated_maximum_players=32\n', 'schema=1\nwarning=true\n'])
def test_capacity_rejects_unbounded_or_unsupported_metadata(tmp_path, text):
    write_shard(tmp_path, data_pack.device_profile_snapshot(ROOT / "profiles", "ios", "iPhone18,1"))
    (tmp_path / "capacity.toml").write_text(text)
    with pytest.raises(ValueError):
        data_pack.snapshot(tmp_path, "profiles", "device-profiles-1:ios/iPhone18,1/_")


def test_device_shard_rejects_profiles_for_another_device(tmp_path):
    write_shard(tmp_path, data_pack.device_profile_snapshot(ROOT / "profiles", "ios", "iPhone18,1"))
    shutil.copy(ROOT / "profiles/ipad-pro-m1.toml", tmp_path / "ipad-pro-m1.toml")
    with pytest.raises(ValueError):
        data_pack.snapshot(tmp_path, "profiles", "device-profiles-1:ios/iPhone18,1/_")
