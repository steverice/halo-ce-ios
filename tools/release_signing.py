#!/usr/bin/env python3
"""Install preexisting release credentials in an ephemeral runner keychain; never provision an account."""

from __future__ import annotations

import base64
import datetime
import hashlib
import json
import os
import plistlib
import secrets
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Any

if __package__:
    from .release import platforms, require_ci
else:
    from release import platforms, require_ci


def security(*args: str, capture: bool = False) -> str:
    result = subprocess.run(["security", *args], check=True, capture_output=True)
    return result.stdout.decode().strip() if capture else ""


class XcodeManagedProfileError(ValueError):
    """A safe signing-preflight failure that can be reported without profile data."""


class ProfileCapabilityError(ValueError):
    """A selected profile cannot sign the reviewed capability set."""


def reject_xcode_managed_profile(profile: dict[str, Any], platform: str) -> None:
    """Reject profiles that cannot be used by the release's manual-signing build."""
    if profile.get("IsXcodeManaged") is True:
        raise XcodeManagedProfileError(
            f"{platform} provisioning profile is Xcode-managed, but release signing requires a manually managed profile"
        )


def write_secret(path: Path, value: bytes) -> None:
    path.write_bytes(value)
    path.chmod(0o600)


def validate_profile_entitlements(
    profile: dict[str, Any],
    platform: str,
    *,
    benchmark_upload: bool = False,
    container_identifier: str = "iCloud.org.steverice.visr",
) -> None:
    """Require the capabilities in the exact entitlement file used by release archives."""
    target = {"ios": "iOS", "tvos": "tvOS", "visionos": "visionOS"}[platform]
    required = plistlib.loads(
        (
            Path(__file__).resolve().parents[1]
            / "port/ios"
            / f"VISR-{target}-extended.entitlements"
        ).read_bytes()
    )
    entitlements = profile.get("Entitlements", {})
    for name, value in required.items():
        if value is True and entitlements.get(name) is not True:
            raise ValueError(f"{platform} distribution profile must allow {name}")
    if benchmark_upload:
        services = entitlements.get("com.apple.developer.icloud-services", [])
        containers = entitlements.get("com.apple.developer.icloud-container-identifiers", [])
        service_allowed = services == "*" or (
            isinstance(services, list) and ("CloudKit" in services or "*" in services)
        )
        if not service_allowed:
            raise ProfileCapabilityError(
                f"{platform} distribution profile must allow the CloudKit service for benchmark uploads"
            )
        if container_identifier not in containers:
            raise ProfileCapabilityError(
                f"{platform} distribution profile must allow the configured CloudKit container"
            )
        if entitlements.get("aps-environment") != "production":
            raise ProfileCapabilityError(
                f"{platform} distribution profile must allow production APS for benchmark uploads"
            )


def installed_profile(profile_uuid: str) -> dict[str, Any]:
    """Read a locally installed profile by UUID without invoking provisioning."""
    roots = (
        Path.home() / "Library/Developer/Xcode/UserData/Provisioning Profiles",
        Path.home() / "Library/MobileDevice/Provisioning Profiles",
    )
    paths = [root / f"{profile_uuid}.mobileprovision" for root in roots]
    profile_path = next((path for path in paths if path.is_file()), None)
    if profile_path is None:
        raise ValueError("The selected manual signing profile is not installed")
    try:
        decoded = security("cms", "-D", "-i", str(profile_path), capture=True)
        return plistlib.loads(decoded.encode())
    except (OSError, ValueError, subprocess.CalledProcessError):
        raise ValueError("The selected manual signing profile could not be read") from None


def install(folder: Path) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    state_path = folder / "cleanup.json"
    if state_path.exists():
        raise ValueError("Credential folder already initialized")
    old_search = shlex.split(security("list-keychains", "-d", "user", capture=True))
    keychain = folder / "release.keychain-db"
    state: dict[str, Any] = {
        "keychain": str(keychain),
        "search": old_search,
        "profiles": [],
        "files": [],
    }
    state_path.write_text(json.dumps(state))
    if __package__:
        from . import release_validation as validation
        from .ios_build import cloud_definitions
    else:
        import release_validation as validation
        from ios_build import cloud_definitions
    policy_value = os.environ.get("RELEASE_BUILD_POLICY", "")
    upload_enabled = bool(policy_value) and validation.benchmark_upload_enabled(
        validation.load_policy(policy_value)
    )
    cloud = cloud_definitions(
        os.environ["APPLE_BUNDLE_ID"], False, None, upload_enabled
    )
    container_identifier = next(
        (
            item.split("=", 1)[1]
            for item in cloud
            if item.startswith("-DHALO_CLOUD_CONTAINER=")
        ),
        "iCloud.org.steverice.visr",
    )
    password = secrets.token_urlsafe(32)
    security("create-keychain", "-p", password, str(keychain))
    security("set-keychain-settings", "-lut", "21600", str(keychain))
    security("unlock-keychain", "-p", password, str(keychain))
    certificate = folder / "distribution.p12"
    state["files"].append(str(certificate))
    state_path.write_text(json.dumps(state))
    write_secret(
        certificate,
        base64.b64decode(os.environ["APPLE_DISTRIBUTION_P12_BASE64"], validate=True),
    )
    security(
        "import",
        str(certificate),
        "-P",
        os.environ["APPLE_DISTRIBUTION_P12_PASSWORD"],
        "-t",
        "cert",
        "-f",
        "pkcs12",
        "-k",
        str(keychain),
        "-T",
        "/usr/bin/codesign",
        "-T",
        "/usr/bin/security",
    )
    security(
        "set-key-partition-list",
        "-S",
        "apple-tool:,apple:,codesign:",
        "-k",
        password,
        str(keychain),
    )
    security("list-keychains", "-d", "user", "-s", str(keychain), *old_search)
    identities = security(
        "find-identity", "-v", "-p", "codesigning", str(keychain), capture=True
    )
    env_file = Path(os.environ["GITHUB_ENV"])
    with env_file.open("a") as output:
        for platform in platforms(os.environ["RELEASE_PLATFORMS"]):
            raw = folder / f"{platform}.mobileprovision"
            state["files"].append(str(raw))
            state_path.write_text(json.dumps(state))
            write_secret(
                raw,
                base64.b64decode(
                    os.environ[f"APPLE_PROFILE_{platform.upper()}_BASE64"],
                    validate=True,
                ),
            )
            decoded = security("cms", "-D", "-i", str(raw), capture=True)
            profile = plistlib.loads(decoded.encode())
            reject_xcode_managed_profile(profile, platform)
            entitlements = profile["Entitlements"]
            if (
                profile["TeamIdentifier"] != [os.environ["APPLE_TEAM_ID"]]
                or entitlements["application-identifier"]
                != f"{os.environ['APPLE_TEAM_ID']}.{os.environ['APPLE_BUNDLE_ID']}"
                or profile.get("ProvisionedDevices")
                or profile.get("ProvisionsAllDevices")
                or entitlements.get("get-task-allow", False)
                or profile["ExpirationDate"]
                <= datetime.datetime.now(datetime.UTC).replace(tzinfo=None)
            ):
                raise ValueError(
                    f"{platform} needs an unexpired existing App Store distribution profile for this app and team"
                )
            expected_platform = {
                "ios": {"iOS"},
                "tvos": {"tvOS"},
                "visionos": {"xrOS", "visionOS"},
            }[platform]
            if not expected_platform.intersection(profile.get("Platform", [])):
                raise ValueError(
                    f"{platform} provisioning profile has the wrong platform"
                )
            validate_profile_entitlements(
                profile,
                platform,
                benchmark_upload=upload_enabled,
                container_identifier=container_identifier,
            )
            if not any(
                hashlib.sha1(cert).hexdigest().upper() in identities
                for cert in profile["DeveloperCertificates"]
            ):
                raise ValueError(
                    "Distribution profile does not include the imported signing identity"
                )
            uuid = profile["UUID"]
            if not uuid or any(c not in "0123456789ABCDEFabcdef-" for c in uuid):
                raise ValueError("Invalid provisioning profile UUID")
            destination = (
                Path.home()
                / "Library/Developer/Xcode/UserData/Provisioning Profiles"
                / f"{uuid}.mobileprovision"
            )
            if destination.exists():
                raise ValueError("Refusing to replace an existing provisioning profile")
            destination.parent.mkdir(parents=True, exist_ok=True)
            state["profiles"].append(str(destination))
            state_path.write_text(json.dumps(state))
            write_secret(destination, raw.read_bytes())
            output.write(f"PROFILE_UUID_{platform.upper()}={uuid}\n")
        key = folder / "AuthKey.p8"
        state["files"].append(str(key))
        state_path.write_text(json.dumps(state))
        write_secret(key, os.environ["ASC_PRIVATE_KEY"].encode())
        output.write(f"ASC_KEY_PATH={key}\n")
    certificate.unlink()


def cleanup(folder: Path) -> None:
    state_path = folder / "cleanup.json"
    if not state_path.exists():
        return
    state = json.loads(state_path.read_text())
    failed = False
    try:
        security("list-keychains", "-d", "user", "-s", *state["search"])
    except subprocess.CalledProcessError:
        failed = True
    keychain = Path(state["keychain"])
    if keychain.exists():
        try:
            security("delete-keychain", str(keychain))
        except subprocess.CalledProcessError:
            failed = True
    for path in state["profiles"] + state["files"]:
        try:
            Path(path).unlink(missing_ok=True)
        except OSError:
            failed = True
    if failed:
        raise OSError("Credential cleanup did not complete; keep the runner isolated")
    state_path.unlink()


if __name__ == "__main__":
    require_ci()
    folder = Path(os.environ["RUNNER_TEMP"]) / "visr-release-signing"
    try:
        install(folder) if sys.argv[1] == "install" else cleanup(folder)
    except (XcodeManagedProfileError, ProfileCapabilityError) as error:
        raise SystemExit(str(error)) from None
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError):
        # Subprocess arguments can contain passwords. Do not print the exception or captured output.
        raise SystemExit(
            "Signing setup/cleanup failed; check the named secrets and cleanup step"
        ) from None
