#!/usr/bin/env python3
"""Build and verify the unsigned iPhone/iPad, Apple TV and Apple Vision Pro apps attached to a VISR release.

`build` runs in a checkout of the exact release tag and builds each app from that tag with the release manifest's
version, build number, reproducibility epochs, feeds and optimization settings, but unsigned and without the iCloud
features, which need a signed app. `verify` checks a folder of finished IPAs. Both refuse anything signed: every
bundle and Mach-O file must report "not signed at all", no provisioning profile or signature folder may be present,
and no file may contain the team ID when one is given. Each app must embed the release's guest, whose SHA-256 the
manifest's build policy pins.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import plistlib
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Any

if __package__:
    from . import release
else:
    import release

PLATFORMS = ("ios", "tvos", "visionos")
IPA_NAMES = {
    "ios": "VISR-iOS-unsigned.ipa",
    "tvos": "VISR-tvOS-unsigned.ipa",
    "visionos": "VISR-visionOS-unsigned.ipa",
}
GUEST_NAME = "halo_guest.elf"
NOT_SIGNED = "code object is not signed at all"
MACHO_MAGICS = {
    b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xfe\xed\xfa\xce",
    b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
}
# code bundles; resource bundles such as Settings.bundle have no code for codesign to judge
BUNDLE_SUFFIXES = (".app", ".appex", ".framework", ".xctest")


def asset_names() -> list[str]:
    """Every file attached to a release, in upload order."""
    return [name for platform in PLATFORMS for name in (IPA_NAMES[platform], IPA_NAMES[platform] + ".sha256")]


def build_command(platform: str, manifest: dict[str, Any], output: Path) -> list[str | Path]:
    """The release build of one platform, unsigned. CloudKit features (iCloud saves, uploads) are left out: they need
    the signed app's iCloud entitlements. None of these options changes the guest."""
    policy = manifest["build_policy"]
    return [
        sys.executable,
        "tools/ios_build.py",
        "--unsigned",
        "--tester",
        "--benchmark-routes",
        "off",
        "--marketing-version",
        manifest["version"],
        "--build-number",
        manifest["build_number"],
        "--pgo",
        "use",
        "--lto",
        "full",
        "--host-pgo",
        "use",
        "--bink",
        "on",
        "--render-validation",
        "off",
        "--data-packs",
        "on",
        "--profile-feed-url",
        policy["profile_feed_url"],
        "--recording-feed-url",
        policy["recording_feed_url"],
        *(["--shader-feed-url", policy["shader_feed_url"]] if "shader_feed_url" in policy else []),
        "--extended-virtual-addressing",
        "--jobs",
        "3",
        "--ipa",
        output / IPA_NAMES[platform],
        *([f"--{platform}"] if platform != "ios" else []),
    ]


def signature_state(path: Path) -> str:
    result = subprocess.run(["codesign", "--display", str(path)], capture_output=True, text=True)
    return (result.stdout + result.stderr).strip()


def require_unsigned(app: Path, team_id: str | None) -> None:
    """Every bundle and Mach-O file in the app must be unsigned, with no signing material or team ID."""
    files = sorted(path for path in app.rglob("*") if path.is_file() or path.is_symlink())
    for path in files:
        if path.is_symlink():
            raise ValueError(f"Unsigned app contains a symbolic link: {path.relative_to(app)}")
        if path.suffix == ".mobileprovision" or path.name in ("CodeResources", "embedded.provisionprofile"):
            raise ValueError(f"Unsigned app contains signing material: {path.relative_to(app)}")
    if any(part == "_CodeSignature" for path in app.rglob("*") for part in path.relative_to(app).parts):
        raise ValueError("Unsigned app contains a _CodeSignature folder")
    bundles = [app, *(path for path in app.rglob("*") if path.is_dir() and path.suffix in BUNDLE_SUFFIXES)]
    machos = [path for path in files if path.read_bytes()[:4] in MACHO_MAGICS]
    if not machos:
        raise ValueError("Unsigned app contains no Mach-O executable")
    for path in [*bundles, *machos]:
        state = signature_state(path)
        if NOT_SIGNED not in state:
            raise ValueError(f"codesign does not report {path.relative_to(app.parent)} as unsigned: {state}")
    if team_id:
        needles = (team_id.encode(), team_id.encode("utf-16-le"))
        for path in files:
            data = path.read_bytes()
            if any(needle in data for needle in needles):
                raise ValueError(f"Unsigned app contains the team ID: {path.relative_to(app)}")


def extract_app(ipa: Path, folder: Path) -> Path:
    with zipfile.ZipFile(ipa) as archive:
        for item in archive.infolist():
            parts = Path(item.filename).parts
            if (
                item.filename.startswith("/")
                or ".." in parts
                or parts[:2] != ("Payload", "VISR.app")
                or stat.S_ISLNK(item.external_attr >> 16)
            ):
                raise ValueError(f"{ipa.name}: unexpected archive entry {item.filename}")
        archive.extractall(folder)
    return folder / "Payload/VISR.app"


def verify_ipa(
    ipa: Path, manifest: dict[str, Any], platform: str, *, guest: bytes | None = None, team_id: str | None = None
) -> dict[str, Any]:
    """Check one finished IPA and its checksum file; return its name, size and SHA-256."""
    policy = manifest["build_policy"]
    data = ipa.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    checksum = ipa.with_name(ipa.name + ".sha256")
    if checksum.read_text() != f"{digest}  {ipa.name}\n":
        raise ValueError(f"{checksum.name} does not match {ipa.name}")
    with tempfile.TemporaryDirectory() as folder:
        app = extract_app(ipa, Path(folder))
        require_unsigned(app, team_id)
        info = plistlib.loads((app / "Info.plist").read_bytes())
        if (
            info.get("CFBundleShortVersionString") != manifest["version"]
            or info.get("CFBundleVersion") != manifest["build_number"]
            or info.get("DTPlatformName") != release.PLATFORMS[platform][1]
            or info.get("CFBundleExecutable") != "VISR"
        ):
            raise ValueError(f"{ipa.name}: version, build number or platform differs from the release manifest")
        executable = (app / "VISR").read_bytes()
        expected = policy["guest_sha256"]
        if expected.encode() not in executable:
            raise ValueError(f"{ipa.name}: the app does not carry the release guest {expected}")
        if guest is not None:
            if hashlib.sha256(guest).hexdigest() != expected:
                raise ValueError("The built guest differs from the release guest")
            release.verify_guest(executable, guest, expected)
    return {"name": ipa.name, "size": len(data), "sha256": digest, "guest_sha256": expected}


def verify_directory(
    folder: Path, manifest: dict[str, Any], *, team_id: str | None = None, require_guest: bool = True
) -> list[dict[str, Any]]:
    """Check every release IPA in a folder. The folder may hold only the release files and the guest ELF."""
    allowed = set(asset_names()) | {GUEST_NAME}
    present = {path.name for path in folder.iterdir()}
    if present - allowed:
        raise ValueError(f"Unexpected files beside the unsigned builds: {', '.join(sorted(present - allowed))}")
    guest_path = folder / GUEST_NAME
    if require_guest and not guest_path.is_file():
        raise ValueError(f"{GUEST_NAME} is missing; the archived guest cannot be compared with it")
    guest = guest_path.read_bytes() if guest_path.is_file() else None
    return [
        verify_ipa(folder / IPA_NAMES[platform], manifest, platform, guest=guest, team_id=team_id)
        for platform in PLATFORMS
    ]


def checked_tag_source(root: Path) -> dict[str, Any]:
    """The manifest of the release tag checked out at root; refuse any other commit."""
    manifest = json.loads((root / release.MANIFEST).read_text())
    head = release.run("git", "rev-parse", "HEAD", capture=True, root=root)
    if head != release.run("git", "rev-parse", f"{manifest['tag']}^{{commit}}", capture=True, root=root):
        raise ValueError("Build checkout is not the exact release tag")
    if release.run("git", "rev-parse", "HEAD^", capture=True, root=root) != release.full_sha(manifest["source_sha"]):
        raise ValueError("Release commit parent differs from the pinned source")
    release.require_clean_checkout(root)
    return manifest


def build(root: Path, output: Path, team_id: str | None) -> list[dict[str, Any]]:
    root = root.resolve()
    manifest = checked_tag_source(root)
    env = {**os.environ, "SOURCE_DATE_EPOCH": manifest["source_date_epoch"]}
    env[release.GUEST_EPOCH_ENV] = release.manifest_guest_epoch(manifest)
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError(f"{output} must start empty")
    for platform in PLATFORMS:
        release.run(*build_command(platform, manifest, output.resolve()), root=root, env=env)
        guest = (root / "build/ios" / GUEST_NAME).read_bytes()
        verify_ipa(output / IPA_NAMES[platform], manifest, platform, guest=guest, team_id=team_id)
    release.require_clean_checkout(root)
    shutil.copy2(root / "build/ios" / GUEST_NAME, output / GUEST_NAME)
    return verify_directory(output, manifest, team_id=team_id)


def report(assets: list[dict[str, Any]]) -> None:
    for item in assets:
        print(f"{item['name']}  {item['size']} bytes  sha256 {item['sha256']}  guest {item['guest_sha256']}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    build_parser = commands.add_parser("build", help="build the unsigned apps in a release tag checkout")
    build_parser.add_argument("--source-root", type=Path, required=True)
    build_parser.add_argument("--output", type=Path, required=True)
    verify_parser = commands.add_parser("verify", help="check a folder of unsigned release apps")
    verify_parser.add_argument("folder", type=Path)
    verify_parser.add_argument("--manifest", type=Path, required=True, help="the release tag's .release/current.json")
    for command in (build_parser, verify_parser):
        command.add_argument("--team-id", default=os.environ.get("APPLE_TEAM_ID") or None,
                             help="refuse any file that contains this team ID (default: $APPLE_TEAM_ID)")
    args = parser.parse_args()
    try:
        if args.command == "build":
            assets = build(args.source_root, args.output, args.team_id)
        else:
            assets = verify_directory(args.folder, json.loads(args.manifest.read_text()), team_id=args.team_id)
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Unsigned release builds stopped: {error}\n")
    report(assets)


if __name__ == "__main__":
    main()
