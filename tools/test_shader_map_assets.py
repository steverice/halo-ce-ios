"""Per-map downloads bind bounded raw bytes and compressed content identities."""
import hashlib
import json
import zlib

import pytest

import data_pack
import data_pack_validation


def test_per_map_shader_publisher_is_deterministic_and_incremental(tmp_path):
    source = tmp_path / "source"
    source.mkdir()
    for name in ("a10.txt", "b30.txt"):
        (source / name).write_bytes(b"# shader list\nvs 0000000000000001 1 0\n")
    seed = tmp_path / "seed"
    seed.write_bytes(bytes(range(32)))
    seed.chmod(0o600)
    output = tmp_path / "shaders.json"
    args = ["--kind", "shaders", "--channel", "tester", "--sequence", "1",
            "--compatibility", "shader-lists-2", "--input", str(source),
            "--output", str(output), "--signing-key", str(seed)]
    data_pack.main(args)
    first = output.read_bytes()
    envelope = json.loads(first)
    import base64
    payload = base64.b64decode(envelope["payload"])
    index = data_pack_validation.shader_index(payload)
    assert set(index) == {"a10.txt", "b30.txt"}
    assets = {path.name: path.read_bytes() for path in (tmp_path / "shader-maps").iterdir()}
    files = data_pack_validation.validate_payload(payload, "shaders", "shader-lists-2", shader_assets=assets)
    assert files == {name: (source / name).read_bytes() for name in index}
    assert len(first) < 4096
    data_pack.main(args)
    assert output.read_bytes() == first
    # The unchanged map retains exactly its URL and bytes when another map changes.
    (source / "a10.txt").write_bytes(b"# changed shader list\nvs 0000000000000001 1 0\n")
    data_pack.main(args)
    changed = data_pack_validation.shader_index(base64.b64decode(json.loads(output.read_bytes())["payload"]))
    assert changed["b30.txt"] == index["b30.txt"]
    assert changed["a10.txt"] != index["a10.txt"]
    assert assets.items() <= {path.name: path.read_bytes() for path in (tmp_path / "shader-maps").iterdir()}.items()


def descriptor(raw, compressed=None):
    compressed = zlib.compress(raw, 9) if compressed is None else compressed
    return ":".join((hashlib.sha256(raw).hexdigest(), str(len(raw)),
                     hashlib.sha256(compressed).hexdigest(), str(len(compressed))))


@pytest.mark.parametrize("content", [b"x" * 8192, zlib.compress(b"x") + b"trailing", zlib.compress(b"x")[:-1]], ids=["corrupt", "trailing", "incomplete"])
def test_shader_assets_reject_corruption_trailing_and_incomplete_streams(content):
    raw = b"# shader list\nvs 0000000000000001 1 0\n"
    item = descriptor(raw, content)
    payload = data_pack.canonical({"files": {"a10.txt": item}})
    name = item.split(":")[2] + ".zlib"
    with pytest.raises(ValueError):
        data_pack_validation.validate_payload(payload, "shaders", "shader-lists-2", shader_assets={name: content})


def test_shader_index_rejects_aggregate_size_before_loading_assets():
    item = descriptor(b"x").split(":")
    item[1] = str(4 * 1024 * 1024)
    payload = data_pack.canonical({"files": {f"a{i}.txt": ":".join(item) for i in range(3)}})
    with pytest.raises(ValueError, match="total"):
        data_pack_validation.shader_index(payload)


@pytest.mark.parametrize("name", ["../a10.txt", "A10.txt", "a10.txt/other", "a10.txt\n"])
def test_shader_index_rejects_unsafe_names(name):
    with pytest.raises(ValueError):
        data_pack_validation.shader_index(data_pack.canonical({"files": {name: descriptor(b"x")}}))


def test_pages_asset_path_is_content_addressed_and_bounded():
    from tools import pages_asset_overlay as pages
    path = pages.PREFIX + "shader-maps/" + "a" * 64 + ".zlib"
    assert pages.allowed_asset(path) == ("shader-map", None)
    assert pages.limit_for(path) == 4 * 1024 * 1024
    for invalid in [path.replace("a" * 64, "../map"), path.replace(".zlib", ".txt"), path.replace("a" * 64, "A" * 64)]:
        with pytest.raises(ValueError):
            pages.allowed_asset(invalid)


def test_pages_requires_complete_authenticated_shader_asset_set(tmp_path, monkeypatch):
    import base64
    import subprocess
    from tools import pages_asset_overlay as pages
    source = tmp_path / "source"
    source.mkdir()
    (source / "a10.txt").write_bytes(b"vs 0000000000000001 1 0\n")
    seed = tmp_path / "seed"
    seed.write_bytes(bytes(range(32)))
    seed.chmod(0o600)
    data_pack.main(["--kind", "shaders", "--channel", "tester", "--sequence", "1",
        "--compatibility", "shader-lists-2", "--input", str(source), "--output", str(tmp_path / "shaders.json"),
        "--signing-key", str(seed)])
    public = subprocess.check_output([str(data_pack.sign_helper()), "--public-key", str(seed)]).decode().strip()
    monkeypatch.setattr(pages.validation, "DATA_PACK_PUBLIC_KEY", public)
    assets = {pages.SHADER: (tmp_path / "shaders.json").read_bytes(),
        **{pages.PREFIX + "shader-maps/" + path.name: path.read_bytes() for path in (tmp_path / "shader-maps").iterdir()}}
    pages.verify_shader_bundle(assets)
    with pytest.raises(ValueError, match="differ"):
        pages.verify_shader_bundle({pages.SHADER: assets[pages.SHADER]})
    with pytest.raises(ValueError, match="differ"):
        pages.verify_shader_bundle({**assets, pages.PREFIX + "shader-maps/" + "a" * 64 + ".zlib": b"bad"})
    map_path = next(path for path in assets if path != pages.SHADER)
    with pytest.raises(ValueError, match="hash|size"):
        pages.verify_shader_bundle({**assets, map_path: b"bad"})
    envelope = json.loads(assets[pages.SHADER])
    signature = bytearray(base64.b64decode(envelope["signature"]))
    signature[0] ^= 1
    envelope["signature"] = base64.b64encode(signature).decode()
    from cryptography.exceptions import InvalidSignature
    with pytest.raises(InvalidSignature):
        pages.verify_shader_bundle({**assets, pages.SHADER: json.dumps(envelope).encode()})

    # A whole-site deployment contains exactly the newly signed asset set, even
    # when the source tree copied into staging still contains the previous map.
    previous = {**assets, pages.RECORDING: b"recording-fixture"}
    for path, content in previous.items():
        destination = tmp_path / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(content)
    (source / "a10.txt").write_bytes(b"vs 0000000000000002 1 0\n")
    data_pack.main(["--kind", "shaders", "--channel", "tester", "--sequence", "2",
        "--compatibility", "shader-lists-2", "--input", str(source), "--output", str(tmp_path / "shaders.json"),
        "--signing-key", str(seed)])
    next_index = data_pack_validation.shader_index(base64.b64decode(json.loads((tmp_path / "shaders.json").read_bytes())["payload"]))
    selected_name = next_index["a10.txt"][2] + ".zlib"
    following = {pages.SHADER: (tmp_path / "shaders.json").read_bytes(),
        pages.PREFIX + "shader-maps/" + selected_name: (tmp_path / "shader-maps" / selected_name).read_bytes()}
    verify_asset = pages.verify_asset
    def recording_fixture(path, content):
        return ({"compatibility": "recording-fixture", "sequence": 1}
            if path == pages.RECORDING else verify_asset(path, content))
    monkeypatch.setattr(pages, "ROOT", tmp_path)
    monkeypatch.setattr(pages, "trusted_source", lambda: "a" * 40)
    monkeypatch.setattr(pages, "source_assets", lambda: previous)
    monkeypatch.setattr(pages, "live_assets", lambda: previous)
    monkeypatch.setattr(pages, "candidate_assets", lambda commit: following)
    monkeypatch.setattr(pages, "verify_asset", recording_fixture)
    staged = pages.stage_site(asset_commit="b" * 40, destination=tmp_path / "site-output")
    assert {path.name for path in (staged / "packs/tester/shader-maps").iterdir()} == {selected_name}
    assert (staged / "packs/tester/shaders.json").read_bytes() == following[pages.SHADER]
