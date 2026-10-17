#!/usr/bin/env python3
"""Probe an unsigned release guest without publishing; validate reviewed distribution inputs."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import platform as host_platform
import plistlib
import re
import shlex
import subprocess
import sys
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

if __package__:
    from . import release
    from .ios_build import DATA_PACK_PUBLIC_KEY
else:
    import release
    from ios_build import DATA_PACK_PUBLIC_KEY

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
    if not isinstance(policy, dict) or set(policy) != fields:
        raise ValueError(
            "RELEASE_BUILD_POLICY must contain the exact documented build and feed pins"
        )
    for name in fields:
        value = policy[name]
        if not isinstance(value, str) or not value.strip():
            raise ValueError(f"Missing build policy pin: {name}")
        if name.endswith("sha256") and not re.fullmatch(r"[0-9a-f]{64}", value):
            raise ValueError(f"Invalid SHA-256 build policy pin: {name}")
        if name.endswith("_url"):
            url = urllib.parse.urlsplit(value)
            if (
                url.scheme != "https"
                or not url.hostname
                or url.username
                or url.password
                or url.fragment
            ):
                raise ValueError(f"Explicit HTTPS feed required: {name}")
    return policy


def preflight_identity(source: str, selected: list[str], policy: dict[str, str]) -> str:
    value = {"source_sha": source, "platforms": selected, "build_policy": policy}
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


def toolchain(selected: list[str]) -> dict[str, str]:
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
        release.ROOT / "pgo/visr_host.profdata",
        capture=True,
    )
    return result


def build_command(target: str) -> list[str]:
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
        "--jobs",
        "3",
        *([f"--{target}"] if target != "ios" else []),
    ]


def inspect_build(
    root: Path, target: str, folder: str = "app-unsigned"
) -> dict[str, str]:
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
    run(compiler.parent / "llvm-profdata", "show", profile, capture=True)
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
    app = build / f"Release-{sdk}/VISR.app"
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
    if any(
        name + "=1" in definitions
        for name in (
            "HALO_CLOUD_SAVES",
            "HALO_BENCHMARK_UPLOAD",
            "HALO_RENDER_VALIDATION",
        )
    ):
        raise ValueError(
            "Release build unexpectedly enables cloud capabilities or render validation"
        )
    return {
        "guest_sha256": hashlib.sha256(
            (root / "build/ios/halo_guest.elf").read_bytes()
        ).hexdigest(),
        "guest_compiler_version": run(compiler, "--version", capture=True).splitlines()[
            0
        ],
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
    }


def probe(root: Path, source: str, selected: list[str]) -> dict[str, Any]:
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
        run(*build_command(target))
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


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(
        self, req: Any, fp: Any, code: Any, msg: Any, headers: Any, newurl: Any
    ) -> None:
        raise ValueError("Release feeds must not redirect")


def verify_feed(data: bytes, kind: str, policy: dict[str, str]) -> None:
    if hashlib.sha256(data).hexdigest() != policy[f"{kind}_feed_sha256"]:
        raise ValueError(f"{kind} feed hash differs from the reviewed envelope")
    envelope = json.loads(data)
    raw = base64.b64decode(envelope["manifest"], validate=True)
    manifest = json.loads(raw)
    compatibility = (
        "profiles-2"
        if kind == "profile"
        else "benchmark-replay-1:" + policy["guest_sha256"]
    )
    if (
        manifest.get("compatibility") != compatibility
        or manifest.get("channel") != "tester"
    ):
        raise ValueError(
            f"{kind} feed compatibility/channel differs from this tester guest"
        )
    if (
        manifest.get("schema") != 1
        or manifest.get("minimum_engine") != 1
        or manifest.get("maximum_engine") != 1
        or manifest.get("kind")
        != {"profile": "profiles", "recording": "recordings"}[kind]
    ):
        raise ValueError(f"{kind} feed has an unsupported manifest")
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

    Ed25519PublicKey.from_public_bytes(bytes.fromhex(DATA_PACK_PUBLIC_KEY)).verify(
        base64.b64decode(envelope["signature"], validate=True), raw
    )
    payload = base64.b64decode(envelope["payload"], validate=True)
    if hashlib.sha256(payload).hexdigest() != manifest["sha256"]:
        raise ValueError(f"{kind} feed payload hash differs from signed manifest")


def check_feeds(policy: dict[str, str]) -> None:
    for kind, limit in [("profile", 4 * 1024**2), ("recording", 24 * 1024**2)]:
        url = policy[f"{kind}_feed_url"]
        with urllib.request.build_opener(NoRedirect()).open(
            url, timeout=45
        ) as response:
            data = response.read(limit + 1)
        if len(data) > limit:
            raise ValueError(f"{kind} feed exceeds the app download limit")
        verify_feed(data, kind, policy)


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
        report = probe(release.ROOT, args.source_sha, selected)
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
