#!/usr/bin/env python3
"""Probe an unsigned release guest without publishing; validate reviewed distribution inputs."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform as host_platform
import plistlib
import re
import shlex
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any

if __package__:
    from . import data_pack_validation as packs
    from . import release
    from .data_pack import ENVELOPE_LIMITS
    from .ios_build import DATA_PACK_PUBLIC_KEY, data_pack_definitions
else:
    import data_pack_validation as packs
    import release
    from data_pack import ENVELOPE_LIMITS
    from ios_build import DATA_PACK_PUBLIC_KEY, data_pack_definitions

XCODE_BUILD = "27B5028f"
SDK_VERSION = "27.2"


def run(*args: str | Path, capture: bool = False, root: Path | None = None) -> str:
    result = subprocess.run(
        [str(arg) for arg in args],
        cwd=root or release.ROOT,
        check=True,
        text=True,
        stdout=subprocess.PIPE if capture else None,
    )
    return result.stdout.strip() if capture else ""


def credential_names(selected: list[str]) -> list[str]:
    return [
        "APPLE_TEAM_ID",
        "APPLE_BUNDLE_ID",
        "APPLE_DISTRIBUTION_P12_BASE64",
        "APPLE_DISTRIBUTION_P12_PASSWORD",
        "ASC_KEY_ID",
        "ASC_ISSUER_ID",
        "ASC_PRIVATE_KEY",
        *[f"APPLE_PROFILE_{target.upper()}_BASE64" for target in selected],
    ]


def check_credentials(env: Any, selected: list[str]) -> None:
    missing = [
        name for name in credential_names(selected) if not env.get(name, "").strip()
    ]
    if missing:
        raise ValueError("Missing release configuration: " + ", ".join(missing))


def load_policy(value: str) -> dict[str, str]:
    policy = json.loads(value or "{}")
    fields = {
        "guest_sha256",
        "guest_compiler_version",
        "guest_profile_sha256",
        "host_profile_sha256",
        "profile_feed_url",
        "profile_feed_sha256",
        "recording_feed_url",
        "recording_feed_sha256",
    }
    if not isinstance(policy, dict) or set(policy) not in (fields, fields | {"benchmark_upload"}):
        raise ValueError(
            "RELEASE_BUILD_POLICY must contain the exact documented build and feed pins"
        )
    if "benchmark_upload" in policy and policy["benchmark_upload"] not in {"on", "off"}:
        raise ValueError("RELEASE_BUILD_POLICY benchmark_upload must be on or off")
    for name in fields:
        value = policy[name]
        if not isinstance(value, str) or not value.strip():
            raise ValueError(f"Missing build policy pin: {name}")
        if name.endswith("sha256") and not re.fullmatch(r"[0-9a-f]{64}", value):
            raise ValueError(f"Invalid SHA-256 build policy pin: {name}")
    data_pack_definitions(
        DATA_PACK_PUBLIC_KEY, policy["profile_feed_url"], policy["recording_feed_url"]
    )
    for name in ("profile_feed_url", "recording_feed_url"):
        url = urllib.parse.urlsplit(policy[name])
        path = urllib.parse.unquote(url.path).strip("/").split("/")
        host = (url.hostname or "").rstrip(".").lower()
        reference = ""
        if host in ("raw.githubusercontent.com", "raw.github.com") and len(path) >= 4:
            reference = path[2]
        elif host == "github.com" and len(path) >= 5 and path[2] in ("raw", "blob"):
            reference = path[3]
        if re.fullmatch(r"[0-9a-fA-F]{40}", reference):
            raise ValueError(
                f"{name}: runtime feed URL is commit-pinned; use a mutable HTTPS endpoint "
                "and pin its release-time bytes with the feed SHA-256"
            )
    return policy


def benchmark_upload_enabled(policy: dict[str, str]) -> bool:
    """Legacy release policies omit benchmark_upload and therefore keep it off."""
    return policy.get("benchmark_upload", "off") == "on"


def preflight_identity(source: str, selected: list[str], policy: dict[str, str]) -> str:
    value = {"source_sha": source, "platforms": selected, "build_policy": policy}
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


def toolchain(selected: list[str], *, root: Path | None = None) -> dict[str, str]:
    if host_platform.system() != "Darwin" or host_platform.machine() != "arm64":
        raise ValueError("Release validation requires an Apple silicon Mac")
    version = run("xcodebuild", "-version", capture=True)
    if f"Build version {XCODE_BUILD}" not in version.splitlines():
        raise ValueError(f"Release requires exact Xcode build {XCODE_BUILD}")
    result = {"xcode_build": XCODE_BUILD, "xcode": version}
    for target in selected:
        sdk = release.PLATFORMS[target][1]
        actual = run("xcrun", "--sdk", sdk, "--show-sdk-version", capture=True)
        if actual != SDK_VERSION:
            raise ValueError(f"{target} requires SDK {SDK_VERSION}, found {actual}")
        result[sdk] = actual
    run(
        "xcrun",
        "llvm-profdata",
        "show",
        (root if root is not None else release.ROOT) / "pgo/visr_host.profdata",
        capture=True,
    )
    return result


def build_command(target: str, benchmark_upload: bool = False) -> list[str]:
    # This first-stage artifact establishes the guest identity. It is never signed or distributed.
    return [
        sys.executable,
        "tools/ios_build.py",
        "--unsigned",
        "--tester",
        "--benchmark-routes",
        "off",
        "--pgo",
        "use",
        "--lto",
        "full",
        "--host-pgo",
        "use",
        "--render-validation",
        "off",
        "--data-packs",
        "off",
        "--bink",
        "on",
        "--extended-virtual-addressing",
        *(["--benchmark-upload"] if benchmark_upload else []),
        "--jobs",
        "3",
        *([f"--{target}"] if target != "ios" else []),
    ]


def inspect_build(
    root: Path,
    target: str,
    folder: str = "app-unsigned",
    *,
    app: Path | None = None,
    benchmark_upload: bool = False,
) -> dict[str, str]:
    signed_app = app is not None
    build = root / "build" / target / folder
    commands = run(
        "ninja", "-t", "commands", "ios_guest", capture=True, root=root
    ).splitlines()
    compiled = [
        line
        for line in commands
        if "source/game/player_control.c" in line and " -c " in line
    ]
    if len(compiled) != 1:
        raise ValueError("Cannot identify the effective guest compile command")
    tokens = shlex.split(compiled[0])
    profiles = [
        value.split("=", 1)[1]
        for value in tokens
        if value.startswith(("-fprofile-use=", "-fprofile-instr-use="))
    ]
    if (
        len(profiles) != 1
        or "-flto=full" not in tokens
        or not any("ios_guest_lto.py" in line for line in commands)
    ):
        raise ValueError("Effective guest PGO/full LTO is missing")
    profile = root / profiles[0]
    compiler = next(Path(value) for value in tokens if Path(value).name == "clang")
    run(compiler.parent / "llvm-profdata", "show", profile, capture=True, root=root)
    settings = json.loads(
        run(
            "xcodebuild",
            "-project",
            build / "VISR.xcodeproj",
            "-target",
            "VISR",
            "-configuration",
            "Release",
            "-showBuildSettings",
            "-json",
            capture=True,
            root=root,
        )
    )
    rows = [row["buildSettings"] for row in settings if row.get("target") == "VISR"]
    if len(rows) != 1:
        raise ValueError("Cannot identify resolved VISR build settings")
    flags = rows[0].get("OTHER_CFLAGS", "")
    if isinstance(flags, list):
        flags = " ".join(flags)
    host_profile = root / "pgo/visr_host.profdata"
    if f"-fprofile-instr-use={host_profile}" not in flags:
        raise ValueError(
            "Effective host PGO is missing; fallback is forbidden for release"
        )
    definitions = rows[0].get("GCC_PREPROCESSOR_DEFINITIONS", "")
    if isinstance(definitions, list):
        definitions = " ".join(definitions)
    if (
        "HALO_TESTER_BUILD=1" not in definitions
        or "HALO_BENCHMARK_NO_ROUTES=1" not in definitions
    ):
        raise ValueError("Effective tester/routes-off build definitions are missing")
    guest = (root / "build/ios/halo_guest.elf").read_bytes()
    guest_hash = hashlib.sha256(guest).hexdigest()
    sdk = release.PLATFORMS[target][1]
    app = app if app is not None else build / f"Release-{sdk}/VISR.app"
    info = plistlib.loads((app / "Info.plist").read_bytes())
    if (
        info.get("DTXcodeBuild") != XCODE_BUILD
        or info.get("DTSDKName") != sdk + SDK_VERSION
    ):
        raise ValueError(
            "Actual unsigned/signed app was built with a different toolchain"
        )
    release.verify_guest(
        (app / info["CFBundleExecutable"]).read_bytes(), guest, guest_hash
    )
    cloud_saves = "HALO_CLOUD_SAVES=1" in definitions
    upload_enabled = "HALO_BENCHMARK_UPLOAD=1" in definitions
    if cloud_saves or "HALO_RENDER_VALIDATION=1" in definitions:
        raise ValueError(
            "Release build unexpectedly enables cloud saves or render validation"
        )
    if upload_enabled != benchmark_upload:
        raise ValueError("Release benchmark-upload build differs from the reviewed policy")
    if benchmark_upload and signed_app:
        verify_benchmark_upload_entitlements(
            signed_app_entitlements(app), "iCloud.org.steverice.visr"
        )
    return {
        "guest_sha256": hashlib.sha256(
            (root / "build/ios/halo_guest.elf").read_bytes()
        ).hexdigest(),
        "guest_compiler_version": run(
            compiler, "--version", capture=True, root=root
        ).splitlines()[0],
        "guest_profile_sha256": hashlib.sha256(profile.read_bytes()).hexdigest(),
        "host_profile_sha256": hashlib.sha256(host_profile.read_bytes()).hexdigest(),
        "guest_compile_command_sha256": hashlib.sha256(
            compiled[0].encode()
        ).hexdigest(),
        "guest_pgo": "use",
        "guest_lto": "full",
        "host_pgo": "use",
        "tester": "on",
        "benchmark_routes": "off",
        "benchmark_upload": "on" if benchmark_upload else "off",
    }


def signed_app_entitlements(app: Path) -> dict[str, Any]:
    return plistlib.loads(
        run("codesign", "-d", "--entitlements", ":-", app, capture=True).encode()
    )


def verify_benchmark_upload_entitlements(
    entitlements: dict[str, Any], container_identifier: str
) -> None:
    services = entitlements.get("com.apple.developer.icloud-services", [])
    containers = entitlements.get("com.apple.developer.icloud-container-identifiers", [])
    if not isinstance(services, list) or "CloudKit" not in services:
        raise ValueError("Signed release app is missing the CloudKit service entitlement")
    if container_identifier not in containers:
        raise ValueError("Signed release app is missing the configured CloudKit container entitlement")
    if entitlements.get("com.apple.developer.icloud-container-environment") != "Production":
        raise ValueError("Signed App Store release must use the production CloudKit environment")
    if entitlements.get("aps-environment") != "production":
        raise ValueError("Signed App Store release must use the production APS environment")


def verify_exported_ipa(
    package: Path, container_identifier: str = "iCloud.org.steverice.visr"
) -> None:
    archives = sorted(package.glob("*.ipa"))
    if len(archives) != 1:
        raise ValueError("Release export must contain exactly one IPA")
    with tempfile.TemporaryDirectory(prefix="visr-export-check-") as temporary:
        with zipfile.ZipFile(archives[0]) as exported:
            app_info = [
                name
                for name in exported.namelist()
                if name.startswith("Payload/") and name.endswith(".app/Info.plist")
            ]
            if len(app_info) != 1:
                raise ValueError("Release IPA must contain exactly one app")
            app_root = app_info[0][: -len("/Info.plist")]
            for member in exported.infolist():
                if member.filename == app_root or member.filename.startswith(app_root + "/"):
                    path = PurePosixPath(member.filename)
                    if path.is_absolute() or ".." in path.parts:
                        raise ValueError("Release IPA contains an unsafe app path")
                    exported.extract(member, temporary)
        app = Path(temporary) / app_root
        verify_benchmark_upload_entitlements(
            signed_app_entitlements(app), container_identifier
        )


def probe(
    root: Path, source: str, selected: list[str], benchmark_upload: bool = False
) -> dict[str, Any]:
    source = release.full_sha(source)
    if run("git", "rev-parse", "HEAD", capture=True) != source:
        raise ValueError("Probe checkout differs from the reviewed source SHA")
    stamp = run("git", "show", "--no-patch", "--format=%ct", source, capture=True)
    os.environ["SOURCE_DATE_EPOCH"] = stamp
    report: dict[str, Any] = {
        "source_sha": source,
        "source_date_epoch": stamp,
        "toolchain": toolchain(selected),
        "platforms": {},
    }
    for target in selected:
        run(*build_command(target, benchmark_upload))
        if benchmark_upload:
            report["platforms"][target] = inspect_build(
                root, target, benchmark_upload=True
            )
        else:
            report["platforms"][target] = inspect_build(root, target)
    if len({item["guest_sha256"] for item in report["platforms"].values()}) != 1:
        raise ValueError("All selected platforms must use the same guest")
    return report


def compare_evidence(evidence: dict[str, str], policy: dict[str, str]) -> None:
    for name in (
        "guest_sha256",
        "guest_compiler_version",
        "guest_profile_sha256",
        "host_profile_sha256",
    ):
        if evidence[name] != policy[name]:
            raise ValueError(f"Actual build differs from reviewed policy: {name}")
    if evidence.get("benchmark_upload", "off") != policy.get("benchmark_upload", "off"):
        raise ValueError("Actual benchmark-upload build differs from reviewed policy")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(
        self, req: Any, fp: Any, code: Any, msg: Any, headers: Any, newurl: Any
    ) -> None:
        raise ValueError("Release feeds must not redirect")


def verify_feed(
    data: bytes, kind: str, policy: dict[str, str], *, root: Path | None = None
) -> None:
    if root is not None:
        # The immutable app supplies its public key, envelope contract, profile
        # schema and native recording validator. Isolate its imports from the
        # controller's modules; do not rebind their independent ROOT globals.
        subprocess.run(
            [
                sys.executable,
                "-I",
                "-c",
                "import json, sys; sys.path.insert(0, sys.argv[1]); "
                "from tools.release_validation import verify_feed; "
                "verify_feed(sys.stdin.buffer.read(), sys.argv[2], json.loads(sys.argv[3]))",
                str(root),
                kind,
                json.dumps(policy),
            ],
            input=data,
            cwd=root,
            check=True,
        )
        return
    compatibility = (
        "profiles-2"
        if kind == "profile"
        else "benchmark-catalog-1:" + policy["guest_sha256"]
    )
    _verify_feed(data, kind, policy, compatibility)


def verify_pages_recording_feed(data: bytes, guest_sha256: str) -> None:
    """Accept only the two published recording-feed contracts for this guest."""
    if not re.fullmatch(r"[0-9a-f]{64}", guest_sha256):
        raise ValueError("Recording feed guest must be an exact lowercase SHA-256")
    policy = {
        "guest_sha256": guest_sha256,
        "recording_feed_sha256": hashlib.sha256(data).hexdigest(),
    }
    _verify_feed(
        data,
        "recording",
        policy,
        (
            "benchmark-replay-1:" + guest_sha256,
            "benchmark-catalog-1:" + guest_sha256,
        ),
    )


def _verify_feed(
    data: bytes,
    kind: str,
    policy: dict[str, str],
    compatibility: str | tuple[str, ...],
) -> None:
    if hashlib.sha256(data).hexdigest() != policy[f"{kind}_feed_sha256"]:
        raise ValueError(f"{kind} feed hash differs from the reviewed envelope")
    payload_kind = {"profile": "profiles", "recording": "recordings"}[kind]
    limit = ENVELOPE_LIMITS[payload_kind]
    if len(data) > limit:
        raise ValueError(f"{kind} feed exceeds the app download limit")
    envelope = packs.decode_json(data)
    if set(envelope) != {"manifest", "signature", "payload"}:
        raise ValueError("Pack envelope has invalid fields")
    raw = packs.decode_base64(envelope["manifest"], 65536)
    signature = packs.decode_base64(envelope["signature"], 64)
    if len(signature) != 64:
        raise ValueError("Pack signature must be exactly 64 bytes")
    manifest = packs.decode_json(raw)
    accepted = (compatibility,) if isinstance(compatibility, str) else compatibility
    if manifest.get("compatibility") not in accepted:
        raise ValueError("Pack manifest compatibility, kind, or channel differs from this build")
    selected = manifest["compatibility"]
    packs.validate_manifest(manifest, payload_kind, "tester", selected)
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

    Ed25519PublicKey.from_public_bytes(bytes.fromhex(DATA_PACK_PUBLIC_KEY)).verify(
        signature, raw
    )
    payload = packs.decode_base64(envelope["payload"], limit)
    if hashlib.sha256(payload).hexdigest() != manifest["sha256"]:
        raise ValueError(f"{kind} feed payload hash differs from signed manifest")
    packs.validate_payload(payload, payload_kind, selected)


def check_feeds(policy: dict[str, str], *, root: Path | None = None) -> None:
    for kind, payload_kind in [("profile", "profiles"), ("recording", "recordings")]:
        limit = ENVELOPE_LIMITS[payload_kind]
        url = policy[f"{kind}_feed_url"]
        with urllib.request.build_opener(NoRedirect()).open(
            url, timeout=45
        ) as response:
            data = response.read(limit + 1)
        if len(data) > limit:
            raise ValueError(f"{kind} feed exceeds the app download limit")
        if root is None:
            verify_feed(data, kind, policy)
        else:
            verify_feed(data, kind, policy, root=root)


def check_release(
    report: dict[str, Any], policy: dict[str, str], selected: list[str]
) -> None:
    check_credentials(os.environ, selected)
    if set(report["platforms"]) != set(selected):
        raise ValueError("Probe platforms differ from release selection")
    for item in report["platforms"].values():
        compare_evidence(item, policy)
    check_feeds(policy)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("probe", "check"):
        command = commands.add_parser(name)
        command.add_argument("--source-sha", required=True)
        command.add_argument("--platforms", default="ios,tvos")
        command.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    release.require_ci()
    selected = release.platforms(args.platforms)
    if args.command == "probe":
        policy = load_policy(os.environ.get("RELEASE_BUILD_POLICY", ""))
        report = probe(
            release.ROOT, args.source_sha, selected, benchmark_upload_enabled(policy)
        )
        report["github_run_id"] = os.environ.get("GITHUB_RUN_ID", "")
        report["github_run_attempt"] = os.environ.get("GITHUB_RUN_ATTEMPT", "")
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    else:
        report = json.loads(args.report.read_text())
        if report["source_sha"] != release.full_sha(args.source_sha):
            raise ValueError("Probe report source differs from dispatch")
        check_release(
            report, load_policy(os.environ.get("RELEASE_BUILD_POLICY", "")), selected
        )
        apple = release.API(
            "https://api.appstoreconnect.apple.com", release.apple_token
        )
        release.exact_app(apple, os.environ["APPLE_BUNDLE_ID"])
        release.output(
            "identity",
            preflight_identity(
                args.source_sha,
                selected,
                load_policy(os.environ.get("RELEASE_BUILD_POLICY", "")),
            ),
        )
        print("Build, feeds, signing configuration, and existing app access validated")


if __name__ == "__main__":
    main()
