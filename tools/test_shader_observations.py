"""Untrusted shader contributions require bounded decoding and native admission."""
import gzip
import hashlib
import json

import pytest
import shader_observations as observations


def payload(**changes):
    value = dict(schema=1, nonce="a" * 32, mapFingerprint="b" * 64,
                 mapKey="m_" + "b" * 60, mapFormat="cache-7", renderer="metal",
                 model="iPhone17,1", platform="ios", appVersion="0.5.0", appBuild="1",
                 guestSHA256="c" * 64, descriptorABI="shader-lists-2", shaderListID="shader-lists-2:bundled:" + "d" * 64,
                 descriptors=["vs 0123456789abcdef 0 0"], dependencies=[])
    value.update(changes)
    return value


def record(value):
    raw = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    digest = hashlib.sha256(raw).hexdigest()
    metadata = {key: value[key] for key in observations.DUPLICATED}
    metadata.update(payloadSHA256=digest, descriptorCount=len(value["descriptors"]), recordName="shader-" + digest)
    return gzip.compress(raw), metadata


def validate(value, **options):
    asset, metadata = record(value)
    return observations.validate(asset, metadata, guest_sha256="c" * 64,
                                 descriptor_abi="shader-lists-2", **options)


def test_valid_native_admission_and_sanitized_staging(tmp_path):
    clean = validate(payload())
    observations.stage(clean, tmp_path)
    target = tmp_path / "community" / (clean["mapKey"] + ".txt")
    assert target.read_text() == "vs 0123456789abcdef 0 0\n"
    evidence = json.loads(target.with_suffix(".json").read_text())
    assert evidence["misses"] == [hashlib.sha256(clean["descriptors"][0].encode()).hexdigest()]
    assert "nonce" not in evidence and "model" not in evidence


@pytest.mark.parametrize("changes", [
    {"schema": True}, {"renderer": "gl"}, {"nonce": "../bad"},
    {"mapKey": "a10"}, {"mapKey": "../bad"}, {"guestSHA256": "d" * 64},
    {"descriptorABI": "other"}, {"model": "private filename.map"},
    {"extra": "private"}, {"descriptors": []},
    {"descriptors": ["vs 0123456789abcdef 0 0\n"]},
    {"descriptors": ["vs 0123456789abcdef 2 0"]},
    {"descriptors": ["ps 0123456789abcdef " + "00" * 252]},
])
def test_reject_schema_identity_and_invalid_descriptors(changes):
    with pytest.raises(ValueError):
        validate(payload(**changes))


def test_duplicated_fields_and_hashes():
    asset, metadata = record(payload())
    for field, bad in [("model", "Mac1,1"), ("descriptorCount", True),
                       ("payloadSHA256", "0" * 64), ("recordName", "shader-wrong")]:
        with pytest.raises(ValueError):
            observations.validate(asset, dict(metadata, **{field: bad}), guest_sha256="c" * 64,
                                  descriptor_abi="shader-lists-2")


@pytest.mark.parametrize("raw", [b'{"schema":1,"schema":1}', b'[]', b'{"schema":NaN}',
                                    b'[' * 2000 + b']' * 2000, b'\xff',
                                    b' ' * (256 * 1024 + 1)])
def test_hostile_json_and_decoded_limit(raw):
    asset, metadata = record(payload())
    with pytest.raises(ValueError):
        observations.validate(gzip.compress(raw), metadata, guest_sha256="c" * 64,
                              descriptor_abi="shader-lists-2")


def test_gzip_limit_truncation_and_concatenation():
    asset, metadata = record(payload())
    for bad in [b'x' * (128 * 1024 + 1), asset[:-2], asset + asset, asset + b'trailing']:
        with pytest.raises(ValueError):
            observations.validate(bad, metadata, guest_sha256="c" * 64, descriptor_abi="shader-lists-2")


def pipeline_payload():
    pixel = "00" * 252
    hashed = 14695981039346656037
    for byte in bytes.fromhex(pixel):
        hashed = ((hashed ^ byte) * 1099511628211) & ((1 << 64) - 1)
    ps = f"ps {hashed:016x} {pixel}"
    pipeline = f"pipeline 0123456789abcdef.0.0 {hashed:016x} 0 0 0 0 0 0 0 {'00' * 16} 0 0"
    return payload(descriptors=[pipeline], dependencies=[ps, "vs 0123456789abcdef 0 0"])


def test_pipeline_dependencies_are_support_not_misses(tmp_path):
    clean = validate(pipeline_payload())
    observations.stage(clean, tmp_path)
    evidence = json.loads(next((tmp_path / "community").glob("*.json")).read_text())
    assert len(evidence["misses"]) == 1
    assert len(next((tmp_path / "community").glob("*.txt")).read_text().splitlines()) == 3
    with pytest.raises(ValueError):
        validate(dict(pipeline_payload(), dependencies=[]))


def test_community_prefix_collision_is_quarantined(tmp_path):
    observations.stage(validate(payload()), tmp_path)
    other = validate(payload(mapFingerprint="b" * 60 + "cccc"))
    with pytest.raises(ValueError):
        observations.stage(other, tmp_path)
    assert list((tmp_path / "quarantine").glob("*.json"))


def test_verified_stock_is_separate(tmp_path):
    clean = validate(payload(mapKey="a10"), official={"b" * 64: "a10"})
    observations.stage(clean, tmp_path)
    assert (tmp_path / "stock" / "a10.txt").is_file()
    assert not (tmp_path / "community").exists()


def test_unrelated_dependencies_are_rejected():
    value = payload(dependencies=["vs 1123456789abcdef 0 0"])
    with pytest.raises(ValueError):
        validate(value)


def test_noncanonical_dependency_order_and_duplicate_lines():
    for value in [dict(pipeline_payload(), dependencies=list(reversed(pipeline_payload()["dependencies"]))),
                  payload(descriptors=["vs 0123456789abcdef 0 0"] * 2),
                  payload(descriptors=["vs 0123456789abcdef 00 0"])]:
        with pytest.raises(ValueError):
            validate(value)


def test_incomplete_catalog_state_fails_closed(tmp_path):
    clean = validate(payload())
    directory = tmp_path / "community"
    directory.mkdir()
    (directory / (clean["mapKey"] + ".txt")).write_text("vs 0123456789abcdef 0 0\n")
    with pytest.raises(ValueError):
        observations.stage(clean, tmp_path)


def test_cli_quarantines_oversized_metadata_without_retaining_it(tmp_path):
    metadata = tmp_path / "record.json"
    metadata.write_bytes(b'private' * 2000)
    (tmp_path / "asset.gz").write_bytes(record(payload())[0])
    with pytest.raises(SystemExit) as result:
        observations.main(["--metadata", str(metadata), "--asset", str(tmp_path / "asset.gz"),
                           "--guest-sha256", "c" * 64, "--output", str(tmp_path / "catalog")])
    assert result.value.code == 1
    marker = next((tmp_path / "catalog" / "quarantine").glob("*.json"))
    assert "private" not in marker.read_text()


def test_official_table_matches_native_identity():
    official = observations.official_maps()
    assert official["f7461e5b6eaf0314c4f03a1c6e43752b06ae3e4782b70b240a8c15e5f634b3ce"] == "a10"
    assert len(official) >= 24


@pytest.mark.parametrize("changes", [{"nonce": int("1" * 32)}, {"descriptors": [7]},
                                     {"dependencies": [None]}, {"dependencies": {}},
                                     {"mapFormat": None}, {"model": []}])
def test_hostile_field_types_are_rejected(changes):
    with pytest.raises(ValueError):
        validate(payload(**changes))


def test_catalog_detects_crash_between_replacing_list_and_evidence(tmp_path):
    clean = validate(payload())
    target = observations.stage(clean, tmp_path)
    target.write_text(target.read_text() + "vs 1123456789abcdef 0 0\n")
    with pytest.raises(ValueError):
        observations.stage(clean, tmp_path)
