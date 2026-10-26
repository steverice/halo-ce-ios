"""The latest-release record: validation, merging, signing and the Pages allow-list."""
import base64
import hashlib
import json

import pytest

import data_pack
import latest_release


def folder(tmp_path, record):
    directory = tmp_path / "input"
    directory.mkdir()
    (directory / data_pack.LATEST_NAME).write_bytes(data_pack.canonical(record))
    return directory


GOOD = {"schema": 1, "platforms": {"ios": "0.6.0", "tvos": "0.5.2"}}


def test_record_accepts_a_platform_subset():
    assert data_pack.validate_latest_record(data_pack.canonical(GOOD)) == GOOD


@pytest.mark.parametrize("record", [
    {"schema": 2, "platforms": {"ios": "0.6.0"}},
    {"schema": 1, "platforms": {}},
    {"schema": 1, "platforms": {"watchos": "0.6.0"}},
    {"schema": 1, "platforms": {"ios": "0.6"}},
    {"schema": 1, "platforms": {"ios": "0.6.0-beta"}},
    {"schema": 1, "platforms": {"ios": "01.6.0"}},
    {"schema": 1, "platforms": {"ios": {"version": "0.6.0"}}},
    {"schema": 1, "platforms": {"ios": "0.6.0"}, "extra": 1},
])
def test_record_rejects_bad_shapes(record):
    with pytest.raises(ValueError):
        data_pack.validate_latest_record(data_pack.canonical(record))


def test_record_rejects_duplicate_keys_and_junk():
    with pytest.raises(ValueError):
        data_pack.validate_latest_record(b'{"schema":1,"platforms":{"ios":"0.6.0","ios":"0.7.0"}}')
    with pytest.raises(ValueError):
        data_pack.validate_latest_record(b"not json")
    with pytest.raises(ValueError):
        data_pack.validate_latest_record(b"x" * 5000)


def test_merge_keeps_other_platforms_and_never_lowers():
    merged = latest_release.merge(GOOD, "0.5.2", ["ios", "macos"])
    assert merged["platforms"] == {"ios": "0.6.0", "tvos": "0.5.2", "macos": "0.5.2"}
    assert latest_release.merge(GOOD, "0.10.0", ["tvos"])["platforms"]["tvos"] == "0.10.0"
    assert latest_release.merge(None, "0.6.0", ["ios"])["platforms"] == {"ios": "0.6.0"}


def test_merge_rejects_bad_input():
    for version, platforms in (("0.6", ["ios"]), ("0.6.0", []), ("0.6.0", ["watchos"])):
        with pytest.raises(ValueError):
            latest_release.merge(GOOD, version, platforms)


def test_cli_writes_the_input_folder(tmp_path, capsys):
    previous = tmp_path / "previous.json"
    key = tmp_path / "seed"
    key.write_bytes(bytes(range(32)))
    key.chmod(0o600)
    data_pack.main(["--kind", "latest", "--channel", "tester", "--sequence", "5", "--compatibility", "latest-release-1",
        "--input", str(folder(tmp_path, GOOD)), "--output", str(previous), "--signing-key", str(key)])
    output = tmp_path / "next"
    latest_release.main(["--version", "0.6.0", "--platforms", "tvos,visionos", "--output", str(output),
        "--previous-envelope", str(previous), "--sequence", "9"])
    assert json.loads((output / data_pack.LATEST_NAME).read_bytes()) == {
        "schema": 1, "platforms": {"ios": "0.6.0", "tvos": "0.6.0", "visionos": "0.6.0"}}
    assert "--sequence 9" in capsys.readouterr().out


def test_signed_envelope_uses_the_latest_kind(tmp_path):
    key = tmp_path / "seed"
    key.write_bytes(bytes(range(32)))
    key.chmod(0o600)
    out = tmp_path / "latest.json"
    data_pack.main(["--kind", "latest", "--channel", "tester", "--sequence", "1", "--compatibility", "latest-release-1",
        "--input", str(folder(tmp_path, GOOD)), "--output", str(out), "--signing-key", str(key)])
    envelope = json.loads(out.read_bytes())
    manifest = json.loads(base64.b64decode(envelope["manifest"]))
    assert (manifest["kind"], manifest["compatibility"]) == ("latest", "latest-release-1")
    assert manifest["sha256"] == hashlib.sha256(base64.b64decode(envelope["payload"])).hexdigest()
    assert len(out.read_bytes()) < data_pack.ENVELOPE_LIMITS["latest"]


def test_publisher_rejects_other_files_and_compatibility(tmp_path):
    bad = folder(tmp_path, GOOD)
    (bad / "extra.json").write_text("{}")
    with pytest.raises(ValueError):
        data_pack.snapshot(bad, "latest", "latest-release-1")
    key = tmp_path / "seed"
    key.write_bytes(bytes(range(32)))
    key.chmod(0o600)
    with pytest.raises(ValueError):
        data_pack.main(["--kind", "latest", "--channel", "tester", "--sequence", "1", "--compatibility", "latest-release-2",
            "--input", str(bad), "--output", str(tmp_path / "x.json"), "--signing-key", str(key)])


def test_pages_allows_only_the_exact_latest_path():
    from tools import pages_asset_overlay as overlay
    assert overlay.allowed_asset("docs/site/packs/tester/latest.json") == ("latest", None)
    assert overlay.limit_for("docs/site/packs/tester/latest.json") == data_pack.ENVELOPE_LIMITS["latest"]
    for path in ("docs/site/packs/tester/latest-2.json", "docs/site/packs/stable/latest.json",
                 "docs/site/packs/tester/other/latest.json"):
        with pytest.raises(ValueError):
            overlay.allowed_asset(path)


def test_pages_rejects_an_unsigned_latest_envelope(tmp_path):
    from tools import pages_asset_overlay as overlay
    key = tmp_path / "seed"
    key.write_bytes(bytes(range(32)))
    key.chmod(0o600)
    out = tmp_path / "latest.json"
    data_pack.main(["--kind", "latest", "--channel", "tester", "--sequence", "1", "--compatibility", "latest-release-1",
        "--input", str(folder(tmp_path, GOOD)), "--output", str(out), "--signing-key", str(key)])
    # signed with a seed that is not the pinned project key
    with pytest.raises(Exception):
        overlay.verify_asset("docs/site/packs/tester/latest.json", out.read_bytes())
