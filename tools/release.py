#!/usr/bin/env python3
"""CI-only, resumable GitHub and internal TestFlight releases; no game data or release assets."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import plistlib
import re
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = Path(".release/current.json")
PLATFORMS = {
    "ios": ("IOS", "iphoneos"),
    "tvos": ("TV_OS", "appletvos"),
    "visionos": ("VISION_OS", "xros"),
}
MARKER = "\n\n<!-- visr-release-state "


def run(*args: str | Path, capture: bool = False) -> str:
    result = subprocess.run(
        [str(arg) for arg in args],
        cwd=ROOT,
        check=True,
        text=True,
        stdout=subprocess.PIPE if capture else None,
    )
    return result.stdout.strip() if capture else ""


def require_ci() -> None:
    if (
        os.environ.get("GITHUB_ACTIONS") != "true"
        or os.environ.get("GITHUB_REF") != "refs/heads/main"
        or os.environ.get("GITHUB_EVENT_NAME") != "workflow_dispatch"
    ):
        raise ValueError(
            "Release mutations require workflow_dispatch from main in GitHub Actions"
        )


def platforms(value: str) -> list[str]:
    selected = value.split(",")
    if (
        not selected
        or len(selected) != len(set(selected))
        or any(p not in PLATFORMS for p in selected)
    ):
        raise ValueError(
            "Platforms must be a unique comma-separated selection of ios,tvos,visionos"
        )
    return selected


def full_sha(value: str) -> str:
    if not re.fullmatch(r"[0-9a-f]{40}", value):
        raise ValueError("An exact lowercase 40-character commit SHA is required")
    return value


def canonical_build_number(value: str) -> str:
    """App Store Connect removes leading zeros from each numeric build component."""
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){0,2}", value):
        raise ValueError("A build number must have one to three numeric components")
    return ".".join(str(int(component)) for component in value.split("."))


def release_notes(changelog: str, version: str) -> str:
    entries = list(re.finditer(r"^## (?!#)", changelog, re.MULTILINE))
    if not entries:
        raise ValueError("Commitizen did not generate a release entry")
    notes = changelog[
        entries[0].start() : entries[1].start() if len(entries) > 1 else len(changelog)
    ].strip()
    if not re.match(rf"^## (?:visr-v)?{re.escape(version)}(?:\s|$)", notes):
        raise ValueError("Generated release entry does not match the selected version")
    if len(notes) > 4000:
        raise ValueError(
            f"Generated release entry is {len(notes)} characters; TestFlight permits 4000. No notes were truncated."
        )
    return notes


def output(name: str, value: str) -> None:
    with Path(os.environ["GITHUB_OUTPUT"]).open("a") as stream:
        stream.write(f"{name}={value}\n")


def prepare(args: argparse.Namespace) -> None:
    require_ci()
    source = full_sha(args.source_sha)
    selected = platforms(args.platforms)
    if __package__:
        from .release_validation import load_policy, preflight_identity
    else:
        from release_validation import load_policy, preflight_identity
    policy = load_policy(os.environ.get("RELEASE_BUILD_POLICY", ""))
    if os.environ.get("RELEASE_PREFLIGHT_IDENTITY") != preflight_identity(
        source, selected, policy
    ):
        raise ValueError(
            "Source, platforms, or build policy changed after release preflight"
        )
    run("git", "fetch", "--tags", "origin", "main")
    manifest: dict[str, Any]
    if args.resume_tag:
        if not re.fullmatch(r"visr-v\d+\.\d+\.\d+", args.resume_tag):
            raise ValueError("Resume requires an existing VISR release tag")
        run("git", "checkout", "--detach", f"refs/tags/{args.resume_tag}")
        manifest = json.loads((ROOT / MANIFEST).read_text())
        if (
            manifest["source_sha"] != source
            or manifest["platforms"] != selected
            or manifest["tag"] != args.resume_tag
            or manifest.get("build_policy") != policy
        ):
            raise ValueError("Resume inputs differ from the immutable release manifest")
        run("git", "merge-base", "--is-ancestor", "HEAD", "origin/main")
        if run("git", "rev-parse", "HEAD^", capture=True) != source:
            raise ValueError("Release commit parent differs from the pinned source")
    else:
        if run("git", "rev-parse", "origin/main", capture=True) != source:
            raise ValueError("Main moved; review and dispatch again with its exact SHA")
        run("git", "checkout", "--detach", source)
        tags = run("git", "tag", "--list", "visr-v*", capture=True).splitlines()
        for tag in tags:
            # Ignore inherited upstream version tags entirely. Only this flow owns VISR-prefixed tags.
            prior = json.loads(run("git", "show", f"{tag}:{MANIFEST}", capture=True))
            if prior["tag"] != tag:
                raise ValueError("Unrecognized VISR version tag")
            run("git", "merge-base", "--is-ancestor", tag, source)
        if not tags:
            since = full_sha(args.bootstrap_since)
            if not re.fullmatch(r"\d+\.\d+\.\d+", args.bootstrap_version):
                raise ValueError("Bootstrap version must have three numeric components")
            if tuple(map(int, args.bootstrap_version.split("."))) < (0, 1, 2):
                raise ValueError(
                    "Bootstrap cannot precede the existing 0.1.2 TestFlight version"
                )
            run("git", "merge-base", "--is-ancestor", since, source)
            # This local-only boundary is never pushed. Commitizen still derives versions from SCM.
            run("git", "tag", f"visr-v{args.bootstrap_version}", since)
        elif args.bootstrap_since:
            raise ValueError(
                "Bootstrap boundary is only valid before the first managed release"
            )
        command = ["cz", "bump", "--yes", "--files-only", "--changelog"]
        if args.increment != "auto":
            command += ["--increment", args.increment.upper()]
        version = run(
            "cz", "bump", "--get-next", *command[5:], capture=True
        ).splitlines()[-1]
        if not re.fullmatch(r"\d+\.\d+\.\d+", version):
            raise ValueError(
                "Only stable three-component release versions are supported"
            )
        run(*command)
        notes = release_notes((ROOT / "CHANGELOG.md").read_text(), version)
        # Git commit time is stable across workflow reruns and follows the existing packaging convention.
        stamp = run("git", "show", "--no-patch", "--format=%ct", source, capture=True)
        build = canonical_build_number(
            time.strftime("%Y%m%d.%H%M%S", time.gmtime(int(stamp)))
        )
        manifest = {
            "schema": 1,
            "tag": f"visr-v{version}",
            "version": version,
            "build_number": build,
            "source_sha": source,
            "platforms": selected,
            "notes_sha256": hashlib.sha256(notes.encode()).hexdigest(),
            "bootstrap_since": args.bootstrap_since,
            "source_date_epoch": stamp,
            "internal_only": True,
            "build_policy": policy,
        }
        (ROOT / MANIFEST).parent.mkdir(exist_ok=True)
        (ROOT / MANIFEST).write_text(json.dumps(manifest, indent=2) + "\n")
        run("git", "add", "--", "CHANGELOG.md", MANIFEST)
        run(
            "git",
            "commit",
            "--message",
            f"🔧 chore(release): generate {manifest['tag']} changelog",
        )
        run("git", "tag", "--annotate", manifest["tag"], "--message", manifest["tag"])
        # The remote rejects the whole update if main advanced or the tag already exists.
        run(
            "git",
            "push",
            "--atomic",
            "origin",
            "HEAD:refs/heads/main",
            f"refs/tags/{manifest['tag']}",
        )
    notes = release_notes((ROOT / "CHANGELOG.md").read_text(), manifest["version"])
    if hashlib.sha256(notes.encode()).hexdigest() != manifest["notes_sha256"]:
        raise ValueError("Release notes changed after generation")
    output("tag", manifest["tag"])


class API:
    def __init__(self, base: str, token: Any) -> None:
        self.base, self.token = base, token

    def request(
        self, method: str, path: str, body: dict[str, Any] | None = None
    ) -> Any:
        url = self.base + path
        payload = json.dumps(body).encode() if body is not None else None
        for attempt in range(4):
            token = self.token() if callable(self.token) else self.token
            request = urllib.request.Request(
                url,
                data=payload,
                method=method,
                headers={
                    "Authorization": f"Bearer {token}",
                    "Accept": "application/json",
                    "Content-Type": "application/json",
                    "User-Agent": "VISR-release",
                },
            )
            try:
                with urllib.request.urlopen(request, timeout=45) as response:
                    return json.load(response)
            except urllib.error.HTTPError as error:
                # Mutations are journaled and reconciled by identity; never blindly repeat them.
                if (
                    method != "GET"
                    or error.code not in (429, 500, 502, 503, 504)
                    or attempt == 3
                ):
                    raise
            except urllib.error.URLError:
                if method != "GET" or attempt == 3:
                    raise
            time.sleep(2**attempt)
        raise RuntimeError("HTTP retry budget exhausted")

    def collection(self, path: str) -> list[dict[str, Any]]:
        items: list[dict[str, Any]] = []
        for _ in range(100):
            page = self.request("GET", path)
            items.extend(page["data"])
            next_url = page.get("links", {}).get("next")
            if not next_url:
                return items
            if not next_url.startswith(self.base + "/"):
                raise ValueError("Unexpected API pagination host")
            path = next_url[len(self.base) :]
        raise ValueError("API pagination limit exceeded")


def exact_app(api: API, bundle_id: str) -> dict[str, Any]:
    # App Store Connect's bundle-ID filter can also return prefix matches.
    # Consume every bounded, same-origin page before establishing uniqueness.
    query = urllib.parse.urlencode({"filter[bundleId]": bundle_id, "limit": "200"})
    matches = [
        item
        for item in api.collection("/v1/apps?" + query)
        if item.get("attributes", {}).get("bundleId") == bundle_id
    ]
    if len(matches) != 1:
        raise ValueError(
            "Actual bundle does not match exactly one existing App Store Connect record"
        )
    return matches[0]


def apple_token() -> str:
    import jwt

    now = int(time.time())
    return jwt.encode(
        {
            "iss": os.environ["ASC_ISSUER_ID"],
            "iat": now - 20,
            "exp": now + 600,
            "aud": "appstoreconnect-v1",
        },
        Path(os.environ["ASC_KEY_PATH"]).read_text(),
        algorithm="ES256",
        headers={"kid": os.environ["ASC_KEY_ID"], "typ": "JWT"},
    )


def exact_build(
    api: API, app_id: str, platform: str, version: str, build: str
) -> dict[str, Any] | None:
    build = canonical_build_number(build)
    query = urllib.parse.urlencode(
        {
            "filter[app]": app_id,
            "filter[version]": build,
            "filter[preReleaseVersion.version]": version,
            "filter[preReleaseVersion.platform]": platform,
            "include": "preReleaseVersion,app",
            "limit": "200",
        }
    )
    page = api.request("GET", f"/v1/builds?{query}")
    if page.get("links", {}).get("next"):
        raise ValueError("Ambiguous exact-build result exceeds one page")
    related = {(item["type"], item["id"]): item for item in page.get("included", [])}
    matches = []
    for item in page["data"]:
        relationship = item["relationships"]
        release_id = relationship["preReleaseVersion"]["data"]["id"]
        release = related[("preReleaseVersions", release_id)]["attributes"]
        if (
            relationship["app"]["data"]["id"] == app_id
            and canonical_build_number(item["attributes"]["version"]) == build
            and release["version"] == version
            and release["platform"] == platform
        ):
            matches.append(item)
    if len(matches) > 1:
        raise ValueError("App Store Connect returned duplicate exact build identities")
    return matches[0] if matches else None


def wait_for_build(
    api: API,
    app_id: str,
    platform: str,
    version: str,
    build: str,
    attempts: int = 90,
    delay: int = 20,
) -> dict[str, Any]:
    for attempt in range(attempts):
        item = exact_build(api, app_id, platform, version, build)
        if item:
            state = item["attributes"]["processingState"]
            if state == "VALID":
                if item["attributes"].get("buildAudienceType") != "INTERNAL_ONLY":
                    raise ValueError(
                        "Uploaded build is not restricted to internal TestFlight testing"
                    )
                return item
            if state in ("FAILED", "INVALID"):
                raise ValueError(
                    f"Exact uploaded build {platform} {version} ({build}) is {state}"
                )
        if attempt + 1 < attempts:
            time.sleep(delay)
    raise TimeoutError(
        f"Exact build {platform} {version} ({build}) did not finish processing; resume this tag"
    )


def set_testflight_notes(
    api: API, build_id: str, notes: str, locale: str = "en-US"
) -> None:
    if not notes or len(notes) > 4000:
        raise ValueError("TestFlight notes must contain 1 to 4000 characters")
    existing = api.collection(f"/v1/builds/{build_id}/betaBuildLocalizations?limit=200")
    selected = [item for item in existing if item["attributes"]["locale"] == locale]
    if len(selected) > 1:
        raise ValueError("Duplicate TestFlight localization")
    if selected:
        current = selected[0]["attributes"].get("whatsNew")
        if current == notes:
            return
        if current:
            raise ValueError(
                "Existing TestFlight notes differ; refusing to overwrite human text"
            )
        api.request(
            "PATCH",
            f"/v1/betaBuildLocalizations/{selected[0]['id']}",
            {
                "data": {
                    "type": "betaBuildLocalizations",
                    "id": selected[0]["id"],
                    "attributes": {"whatsNew": notes},
                }
            },
        )
    else:
        api.request(
            "POST",
            "/v1/betaBuildLocalizations",
            {
                "data": {
                    "type": "betaBuildLocalizations",
                    "attributes": {"locale": locale, "whatsNew": notes},
                    "relationships": {
                        "build": {"data": {"type": "builds", "id": build_id}}
                    },
                }
            },
        )
    verified = api.collection(f"/v1/builds/{build_id}/betaBuildLocalizations?limit=200")
    if not any(
        item["attributes"]["locale"] == locale
        and item["attributes"].get("whatsNew") == notes
        for item in verified
    ):
        raise ValueError("TestFlight did not retain the exact generated notes")


def draft_body(notes: str, journal: dict[str, Any]) -> str:
    return notes + MARKER + json.dumps(journal, sort_keys=True) + " -->"


def read_journal(body: str, notes: str) -> dict[str, Any]:
    if not body.startswith(notes + MARKER) or not body.endswith(" -->"):
        raise ValueError(
            "Draft release notes or recovery journal differ from the release manifest"
        )
    return json.loads(body[len(notes + MARKER) : -4])


def find_release(api: API, path: str, tag: str) -> dict[str, Any] | None:
    # Drafts are included in the authenticated list; the by-tag endpoint only promises published releases.
    for number in range(1, 101):
        page = api.request("GET", f"{path}?per_page=100&page={number}")
        matches = [item for item in page if item["tag_name"] == tag]
        if matches:
            if len(matches) != 1:
                raise ValueError("Duplicate GitHub release tag")
            return matches[0]
        if len(page) < 100:
            return None
    raise ValueError("GitHub release pagination limit exceeded")


def archive_identity(
    archive: Path, manifest: dict[str, Any], platform: str
) -> dict[str, str]:
    app = archive / "Products/Applications/VISR.app"
    info = plistlib.loads((app / "Info.plist").read_bytes())
    if (
        info["CFBundleShortVersionString"] != manifest["version"]
        or info["CFBundleVersion"] != manifest["build_number"]
        or info["DTPlatformName"] != PLATFORMS[platform][1]
    ):
        raise ValueError(
            "Actual archive version, build number, or platform differs from the release manifest"
        )
    guest = (ROOT / "build/ios/halo_guest.elf").read_bytes()
    guest_hash = hashlib.sha256(guest).hexdigest()
    executable = (app / info["CFBundleExecutable"]).read_bytes()
    verify_guest(executable, guest, guest_hash)
    policy = manifest["build_policy"]
    if guest_hash != policy["guest_sha256"]:
        raise ValueError("Actual archived guest differs from the reviewed probe")
    expected_info = {
        "DTXcodeBuild": "27B5028f",
        "DTSDKName": PLATFORMS[platform][1] + "27.2",
        "CFBundleIdentifier": os.environ["APPLE_BUNDLE_ID"],
        "HaloDataPackPublicKey": "b57463a3c1b157fdb49a4df5c2041b88cabf07b4ba733d0c7290b90d85e3b5ea",
        "HaloProfileFeedURL": policy["profile_feed_url"],
        "HaloRecordingFeedURL": policy["recording_feed_url"],
        "HaloDataPackChannel": "tester",
    }
    if any(info.get(name) != value for name, value in expected_info.items()):
        raise ValueError(
            "Actual archive toolchain, bundle, or tester feed differs from release policy"
        )
    return {"bundle_id": info["CFBundleIdentifier"], "guest_sha256": guest_hash}


def verify_guest(executable: bytes, guest: bytes, guest_hash: str) -> None:
    """Compare the actual archived Mach-O guest sections with the shared ELF load segments."""
    if executable[:4] != b"\xcf\xfa\xed\xfe" or guest[:6] != b"\x7fELF\x02\x01":
        raise ValueError("Expected a thin ARM64 Mach-O and little-endian ELF64")
    sections: dict[str, bytes] = {}
    command = 32
    count = struct.unpack_from("<I", executable, 16)[0]
    for _ in range(count):
        kind, size = struct.unpack_from("<II", executable, command)
        if kind == 0x19:
            section_count = struct.unpack_from("<I", executable, command + 64)[0]
            for number in range(section_count):
                section = command + 72 + number * 80
                name = executable[section : section + 16].split(b"\0")[0].decode()
                length, offset = struct.unpack_from("<QI", executable, section + 40)
                if name in ("__halocode", "__halodata"):
                    sections[name] = executable[offset : offset + length]
        command += size
    offset = struct.unpack_from("<Q", guest, 32)[0]
    stride, count = struct.unpack_from("<HH", guest, 54)
    segments = []
    for number in range(count):
        kind, flags, start, address, _, length, memory, _ = struct.unpack_from(
            "<IIQQQQQQ", guest, offset + number * stride
        )
        if kind == 1:
            if start + length > len(guest) or length > memory:
                raise ValueError("Invalid ELF segment bounds")
            segments.append((flags, address, guest[start : start + length]))
    if len(segments) != 2 or [item[0] for item in segments] != [5, 6]:
        raise ValueError("Expected exactly two shared guest load segments")
    code = segments[0][2].ljust(segments[1][1] - segments[0][1], b"\0")
    if (
        sections.get("__halocode") != code
        or sections.get("__halodata") != segments[1][2]
        or guest_hash.encode() not in executable
    ):
        raise ValueError("Actual archived guest differs from the shared guest ELF")


def publish(args: argparse.Namespace) -> None:
    require_ci()
    manifest = json.loads((ROOT / MANIFEST).read_text())
    if run("git", "rev-parse", "HEAD", capture=True) != run(
        "git", "rev-parse", f"{manifest['tag']}^{{commit}}", capture=True
    ):
        raise ValueError("Build checkout is not the exact release tag")
    notes = release_notes((ROOT / "CHANGELOG.md").read_text(), manifest["version"])
    if hashlib.sha256(notes.encode()).hexdigest() != manifest["notes_sha256"]:
        raise ValueError("Generated notes hash differs from release manifest")
    github = API("https://api.github.com", os.environ["GH_TOKEN"])
    apple = API("https://api.appstoreconnect.apple.com", apple_token)
    path = f"/repos/{os.environ['GITHUB_REPOSITORY']}/releases"
    release = find_release(github, path, manifest["tag"])
    if release is None:
        release = github.request(
            "POST",
            path,
            {
                "tag_name": manifest["tag"],
                "name": manifest["tag"],
                "draft": True,
                "prerelease": False,
                "body": draft_body(notes, {}),
            },
        )
    if release["assets"]:
        raise ValueError("VISR GitHub releases must not have assets")
    if not release["draft"]:
        if release["body"] != notes:
            raise ValueError("Published GitHub notes differ from this release")
        print(f"{manifest['tag']} is already published with identical notes")
        return
    if __package__:
        from . import release_validation as validation
    else:
        import release_validation as validation
    policy = validation.load_policy(os.environ.get("RELEASE_BUILD_POLICY", ""))
    if policy != manifest["build_policy"]:
        raise ValueError("Build policy differs from the immutable release manifest")
    validation.toolchain(manifest["platforms"])
    validation.check_feeds(policy)
    journal = read_journal(release["body"], notes)

    def checkpoint() -> None:
        github.request(
            "PATCH", f"{path}/{release['id']}", {"body": draft_body(notes, journal)}
        )

    os.environ["SOURCE_DATE_EPOCH"] = manifest["source_date_epoch"]
    for platform in manifest["platforms"]:
        record = journal.get(platform)
        if not record or record["state"] == "prepared":
            profile = os.environ[f"PROFILE_UUID_{platform.upper()}"]
            package = ROOT / "build/release" / platform
            command: list[str | Path] = [
                sys.executable,
                "tools/ios_build.py",
                "--team",
                os.environ["APPLE_TEAM_ID"],
                "--bundle-id",
                os.environ["APPLE_BUNDLE_ID"],
                "--signing-profile",
                profile,
                "--marketing-version",
                manifest["version"],
                "--build-number",
                manifest["build_number"],
                "--tester",
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
                "--benchmark-routes",
                "off",
                "--internal-only",
                "--extended-virtual-addressing",
                "--jobs",
                "3",
                "--app-store-package",
                package,
            ]
            if platform != "ios":
                command.append(f"--{platform}")
            run(*command)
            validation.compare_evidence(
                validation.inspect_build(ROOT, platform, "app-device"), policy
            )
            identity = archive_identity(package / "VISR.xcarchive", manifest, platform)
            prior_hashes = {item["guest_sha256"] for item in journal.values()}
            if prior_hashes and prior_hashes != {identity["guest_sha256"]}:
                raise ValueError(
                    "Platforms do not share the same archived guest identity; do not publish a recording pack"
                )
            app = exact_app(apple, identity["bundle_id"])
            record = {**identity, "app_id": app["id"], "state": "prepared"}
            journal[platform] = record
            checkpoint()
        item = exact_build(
            apple,
            record["app_id"],
            PLATFORMS[platform][0],
            manifest["version"],
            manifest["build_number"],
        )
        if not item and record["state"] == "prepared":
            # Persist before the upload. After an ambiguous timeout or runner loss, resume only polls;
            # it never creates a second upload or build number automatically.
            record["state"] = "uploading"
            checkpoint()
            package = ROOT / "build/release" / platform
            options = plistlib.loads((package / "ExportOptions.plist").read_bytes())
            options["destination"] = "upload"
            upload_options = package / "UploadOptions.plist"
            upload_options.write_bytes(plistlib.dumps(options))
            run(
                "xcodebuild",
                "-exportArchive",
                "-archivePath",
                package / "VISR.xcarchive",
                "-exportOptionsPlist",
                upload_options,
                "-exportPath",
                package / "upload",
                "-authenticationKeyPath",
                os.environ["ASC_KEY_PATH"],
                "-authenticationKeyID",
                os.environ["ASC_KEY_ID"],
                "-authenticationKeyIssuerID",
                os.environ["ASC_ISSUER_ID"],
            )
        item = wait_for_build(
            apple,
            record["app_id"],
            PLATFORMS[platform][0],
            manifest["version"],
            manifest["build_number"],
        )
        set_testflight_notes(apple, item["id"], notes)
        record.update(state="complete", build_id=item["id"])
        checkpoint()
        print(f"{platform}: exact internal build processed, release notes verified")
    github.request(
        "PATCH",
        f"{path}/{release['id']}",
        {"body": notes, "draft": False, "make_latest": "true"},
    )
    print(f"{manifest['tag']}: GitHub release published without assets")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare_parser = commands.add_parser("prepare")
    prepare_parser.add_argument("--source-sha", required=True)
    prepare_parser.add_argument("--platforms", default="ios,tvos")
    prepare_parser.add_argument(
        "--increment", choices=("auto", "patch", "minor", "major"), default="auto"
    )
    prepare_parser.add_argument("--resume-tag", default="")
    prepare_parser.add_argument("--bootstrap-since", default="")
    prepare_parser.add_argument("--bootstrap-version", default="0.1.2")
    commands.add_parser("publish")
    args = parser.parse_args()
    try:
        prepare(args) if args.command == "prepare" else publish(args)
    except (
        ValueError,
        OSError,
        subprocess.CalledProcessError,
        TimeoutError,
        KeyError,
    ) as error:
        parser.exit(1, f"Release stopped: {error}\n")


if __name__ == "__main__":
    main()
