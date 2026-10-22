"""Release pins cover target profile shards and additive shader updates."""

import json

import pytest

from tools import release_validation as validation
from tools.test_release_validation import policy


def asset_policy():
    value = policy()
    value.pop("profile_feed_sha256")
    value.update(
        profile_feed_url="https://example.invalid/tester/profiles",
        profile_feed_sha256s={"ios/iPhone13,3/_": "d" * 64},
        shader_feed_url="https://example.invalid/tester/shaders.json",
        shader_feed_sha256="f" * 64,
    )
    return value


def test_new_policy_pins_device_shards_and_shader_feed():
    value = asset_policy()
    assert validation.load_policy(json.dumps(value)) == value
    validation.require_asset_policy(value)
    with pytest.raises(ValueError, match="asset"):
        validation.require_asset_policy(policy())


@pytest.mark.parametrize(
    "targets",
    [
        {},
        {"ios/iPhone13,3/../../x": "d" * 64},
        {"ios/iPhone13,3/Apple-M4": "d" * 64},
        {"ios/iPhone13,3/_": "bad"},
        {"ios/iPhone13,3/_": "D" * 64},
    ],
)
def test_policy_rejects_invalid_profile_target_pins(targets):
    with pytest.raises(ValueError):
        validation.load_policy(
            json.dumps({**asset_policy(), "profile_feed_sha256s": targets})
        )


def test_shader_feed_url_uses_existing_archive_url_restrictions():
    with pytest.raises(ValueError):
        validation.load_policy(
            json.dumps(
                {**asset_policy(), "shader_feed_url": "http://example.invalid/a"}
            )
        )


def test_profile_feed_base_cannot_contain_query_or_traversal():
    for url in (
        "https://example.invalid/profiles?x=1",
        "https://example.invalid/a/../profiles",
    ):
        with pytest.raises(ValueError):
            validation.load_policy(
                json.dumps({**asset_policy(), "profile_feed_url": url})
            )


def signed_envelope(monkeypatch, kind, compatibility, files=None):
    import base64
    import hashlib
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

    key = Ed25519PrivateKey.generate()
    monkeypatch.setattr(
        validation,
        "DATA_PACK_PUBLIC_KEY",
        key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw).hex(),
    )
    payload = json.dumps(
        {
            "files": {
                name: base64.b64encode(value).decode()
                for name, value in (files or {"fixture": b"abc"}).items()
            }
        }
    ).encode()
    manifest = json.dumps(
        {
            "schema": 1,
            "sequence": 1,
            "notes": "",
            "kind": kind,
            "channel": "tester",
            "minimum_engine": 1,
            "maximum_engine": 1,
            "compatibility": compatibility,
            "sha256": hashlib.sha256(payload).hexdigest(),
        }
    ).encode()
    raw = json.dumps(
        {
            name: base64.b64encode(value).decode()
            for name, value in {
                "manifest": manifest,
                "signature": key.sign(manifest),
                "payload": payload,
            }.items()
        }
    ).encode()
    return raw, hashlib.sha256(raw).hexdigest()


def test_signed_profile_shard_is_bound_to_the_exact_target(monkeypatch):
    target = "ios/iPhone13,3/_"
    raw, sha = signed_envelope(monkeypatch, "profiles", "device-profiles-1:" + target)
    pins = {
        **asset_policy(),
        "profile_feed_sha256s": {target: sha, "ios/iPhone18,1/_": sha},
    }
    calls = []
    monkeypatch.setattr(
        validation.packs,
        "validate_payload",
        lambda raw, kind, compatibility: calls.append((kind, compatibility)) or {},
    )
    validation.verify_feed(raw, "profile", pins, profile_target=target)
    assert calls == [("profiles", "device-profiles-1:" + target)]
    with pytest.raises(ValueError, match="compatibility"):
        validation.verify_feed(raw, "profile", pins, profile_target="ios/iPhone18,1/_")


def test_signed_shader_feed_uses_the_existing_signature_and_payload_contract(
    monkeypatch,
):
    raw, sha = signed_envelope(monkeypatch, "shaders", "shader-lists-1")
    pins = {**asset_policy(), "shader_feed_sha256": sha}
    calls = []
    monkeypatch.setitem(validation.ENVELOPE_LIMITS, "shaders", 16 * 1024 * 1024)
    monkeypatch.setattr(
        validation.packs,
        "validate_payload",
        lambda raw, kind, compatibility: calls.append((kind, compatibility)) or {},
    )
    validation.verify_feed(raw, "shader", pins)
    assert calls == [("shaders", "shader-lists-1")]
    with pytest.raises(ValueError, match="hash"):
        validation.verify_feed(raw + b" ", "shader", pins)


def test_new_policy_fetches_each_target_and_shader_without_monolithic_fallback(
    monkeypatch,
):
    from io import BytesIO

    pins = asset_policy()
    pins["profile_feed_sha256s"]["tvos/AppleTV5,3/_"] = "a" * 64
    fetched = []
    verified = []

    class Opener:
        def open(self, url, timeout):
            fetched.append(url)
            assert timeout == 45
            return BytesIO(b"envelope")

    monkeypatch.setattr(
        validation.urllib.request, "build_opener", lambda *args: Opener()
    )
    monkeypatch.setitem(validation.ENVELOPE_LIMITS, "shaders", 16 * 1024 * 1024)

    def verify(data, kind, policy, **kwargs):
        verified.append((kind, kwargs.get("profile_target")))
        return {"fixture": "catalog"} if kind == "recording" else None

    monkeypatch.setattr(validation, "verify_feed", verify)
    assert validation.check_feeds(pins) == {"fixture": "catalog"}
    assert fetched == [
        pins["profile_feed_url"] + "/ios/iPhone13,3/_.json",
        pins["profile_feed_url"] + "/tvos/AppleTV5,3/_.json",
        pins["shader_feed_url"],
        pins["recording_feed_url"],
    ]
    assert verified == [
        ("profile", "ios/iPhone13,3/_"),
        ("profile", "tvos/AppleTV5,3/_"),
        ("shader", None),
        ("recording", None),
    ]


def test_archive_rejects_changed_encryption_or_shader_feed(tmp_path, monkeypatch):
    import hashlib
    import plistlib
    from tools import release

    pins = asset_policy()
    guest = b"guest fixture"
    pins["guest_sha256"] = hashlib.sha256(guest).hexdigest()
    root = tmp_path / "source"
    (root / "build/ios").mkdir(parents=True)
    (root / "build/ios/halo_guest.elf").write_bytes(guest)
    archive = tmp_path / "VISR.xcarchive"
    app = archive / "Products/Applications/VISR.app"
    app.mkdir(parents=True)
    (app / "VISR").write_bytes(b"executable fixture")
    info = {
        "ITSAppUsesNonExemptEncryption": False,
        "CFBundleShortVersionString": "0.2.2",
        "CFBundleVersion": "20261019.1",
        "DTPlatformName": "iphoneos",
        "DTXcodeBuild": "27B5038j",
        "DTSDKName": "iphoneos27.2",
        "CFBundleIdentifier": "example.app",
        "CFBundleExecutable": "VISR",
        "HaloDataPackPublicKey": validation.DATA_PACK_PUBLIC_KEY,
        "HaloProfileFeedURL": pins["profile_feed_url"],
        "HaloRecordingFeedURL": pins["recording_feed_url"],
        "HaloShaderFeedURL": pins["shader_feed_url"],
        "HaloDataPackChannel": "tester",
    }
    (app / "Info.plist").write_bytes(plistlib.dumps(info))
    monkeypatch.setattr(release, "verify_guest", lambda *args: None)
    monkeypatch.setenv("APPLE_BUNDLE_ID", "example.app")
    manifest = {"version": "0.2.2", "build_number": "20261019.1", "build_policy": pins}
    assert (
        release.archive_identity(archive, manifest, "ios", source_root=root)[
            "guest_sha256"
        ]
        == pins["guest_sha256"]
    )
    for declaration in [None, True, 0, "NO"]:
        invalid = dict(info)
        if declaration is None:
            invalid.pop("ITSAppUsesNonExemptEncryption")
        else:
            invalid["ITSAppUsesNonExemptEncryption"] = declaration
        (app / "Info.plist").write_bytes(plistlib.dumps(invalid))
        with pytest.raises(ValueError, match="encryption"):
            release.archive_identity(archive, manifest, "ios", source_root=root)
    info["HaloShaderFeedURL"] = "https://example.invalid/unreviewed.json"
    (app / "Info.plist").write_bytes(plistlib.dumps(info))
    with pytest.raises(ValueError, match="feed"):
        release.archive_identity(archive, manifest, "ios", source_root=root)


@pytest.mark.parametrize("kind", ["profile", "shader"])
def test_signed_asset_payload_passes_the_publisher_and_native_contract(
    tmp_path, monkeypatch, kind
):
    target = "ios/iPhone18,1/_"
    if kind == "profile":
        files = validation.packs.data_pack.device_profile_snapshot(
            validation.packs.data_pack.ROOT / "profiles", "ios", "iPhone18,1"
        )
        compatibility = "device-profiles-1:" + target
        raw, sha = signed_envelope(monkeypatch, "profiles", compatibility, files)
        pins = {**asset_policy(), "profile_feed_sha256s": {target: sha}}
        validation.verify_feed(raw, kind, pins, profile_target=target)
    else:
        text = (
            "vs 0000000000000001 0 0\nps 2fba1332b14beb55 "
            + "00" * 252
            + "\n"
            + "pipeline 0000000000000001.0.0 2fba1332b14beb55 0 0 0 0 15 15 1 "
            + "00" * 16
            + " 0 0\n"
        )
        raw, sha = signed_envelope(
            monkeypatch, "shaders", "shader-lists-1", {"a10.txt": text.encode()}
        )
        pins = {**asset_policy(), "shader_feed_sha256": sha}
        validation.verify_feed(raw, kind, pins)
        bad, sha = signed_envelope(
            monkeypatch,
            "shaders",
            "shader-lists-1",
            {"a10.txt": text.replace("15 15 1", "15 16 1").encode()},
        )
        with pytest.raises(ValueError):
            validation.verify_feed(bad, kind, {**pins, "shader_feed_sha256": sha})


def test_immutable_source_rechecks_the_exact_target_argument(tmp_path):
    source = tmp_path / "application"
    package = source / "tools"
    package.mkdir(parents=True)
    (package / "__init__.py").write_text("")
    (package / "release_validation.py").write_text(
        "def verify_feed(data, kind, policy, *, profile_target):\n"
        ' assert data == b"signed shard" and kind == "profile"\n'
        ' assert profile_target == "ios/iPhone18,1/_"\n'
        ' assert policy == {"fixture": "policy"}\n'
    )
    validation.verify_feed(
        b"signed shard",
        "profile",
        {"fixture": "policy"},
        root=source,
        profile_target="ios/iPhone18,1/_",
    )


@pytest.mark.parametrize("value", [None, True, 0, "NO", "false"])
def test_release_rejects_missing_or_nonboolean_exemption(value):
    from tools import release
    info = {} if value is None else {"ITSAppUsesNonExemptEncryption": value}
    with pytest.raises(ValueError, match="encryption"):
        release.verify_encryption_declaration(info)


def test_release_accepts_boolean_exemption_without_an_approval_code():
    from tools import release
    release.verify_encryption_declaration({"ITSAppUsesNonExemptEncryption": False})


def test_release_rejects_an_unexpected_encryption_approval_code():
    from tools import release
    with pytest.raises(ValueError, match="encryption"):
        release.verify_encryption_declaration({
            "ITSAppUsesNonExemptEncryption": False,
            "ITSEncryptionExportComplianceCode": "unreviewed-code",
        })
