#!/usr/bin/env python3
"""Build a validated, signed data snapshot offline; this tool never uploads anything.

The seed is a raw 32-byte Ed25519 seed in an owned file with mode 600 (or 400).
Inspect its public key with build/ios/probe/data-pack-sign --public-key SEED.
Only the canonical manifest bytes are signed; its SHA-256 binds the exact payload bytes.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

if __package__:
    from . import benchmark_catalog
    from . import profile_check
    from .profile_catalog_check import check_catalog
else:
    import benchmark_catalog
    import profile_check
    from profile_catalog_check import check_catalog

ROOT = Path(__file__).resolve().parents[1]
MAXIMUM_SEQUENCE = 2**53 - 1
RECORDING_MAX_FILES = 160
RECORDING_MAX_FILE_BYTES = 4 * 1024 * 1024
# Match the native route grammar: three streams and two short metadata files per
# route, plus one catalog; include per-file padding/names and both base64 layers.
_RECORDING_ENVELOPE_CONTENT_MAX = 16 * (3 * RECORDING_MAX_FILE_BYTES + 2 * 4095) + RECORDING_MAX_FILE_BYTES
_RECORDING_PAYLOAD_MAX = ((_RECORDING_ENVELOPE_CONTENT_MAX + 2) // 3) * 4 + RECORDING_MAX_FILES * 128 + 65536
ENVELOPE_LIMITS = {"profiles": 4 * 1024 * 1024,
                   "recordings": ((_RECORDING_PAYLOAD_MAX + 2) // 3) * 4 + 65536,
                   "shaders": 16 * 1024 * 1024}
LEGACY_BUILTIN_SCENES = {"menu", "a10-opening", "b30-beach"}
LEGACY_MAXIMUM_ROUTES = 13
PROFILE_NAME = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*\.toml")
RECORDING_NAME = re.compile(r"[a-z0-9](?:[a-z0-9-]{0,46}[a-z0-9])?\.(?:input|actions|seeds|route|checks)")


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode("ascii")


def helper(name, sources, includes=(), dependencies=()):
    """Build shared C validation/signing code, atomically, without external crypto dependencies."""
    sources = [ROOT / source for source in sources]
    tracked = sources + [ROOT / path for path in dependencies]
    destination = ROOT / "build/ios/probe" / name
    if destination.exists() and all(source.stat().st_mtime <= destination.stat().st_mtime for source in tracked):
        return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{name}-", dir=destination.parent)
    os.close(descriptor)
    try:
        subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-D_DARWIN_C_SOURCE=1", "-O1", "-Wall", "-Wextra", "-Werror",
                        *[f"-I{ROOT / path}" for path in includes], *map(str, sources), "-o", temporary], check=True)
        os.chmod(temporary, 0o755)
        os.replace(temporary, destination)
    finally:
        Path(temporary).unlink(missing_ok=True)
    return destination


def recording_helper():
    return helper("recording-pack-probe", ["port/ios/tests/recording_pack_probe.c",
        "port/ios/host/host_recording_pack.c", "port/runtime/benchmark_plan.c"], dependencies=[
        "port/ios/host/host_recording_pack.h", "port/runtime/replay_environment.h", "port/runtime/benchmark_plan.h", "port/linux/src/port_profile_keys.h"])


def sign_helper():
    return helper("data-pack-sign", ["tools/data_pack_sign.c", "port/third_party/monocypher/monocypher.c",
        "port/third_party/monocypher/monocypher-ed25519.c"], ["port/third_party/monocypher"], [
        "port/third_party/monocypher/monocypher.h", "port/third_party/monocypher/monocypher-ed25519.h"])


def profile_delivery_helper():
    return helper("profile-delivery-probe", ["port/ios/tests/profile_delivery_probe.c",
        "port/ios/host/host_profile.c", "port/ios/host/host_profile_catalog.c", "port/ios/host/host_profile_delivery.c",
        "port/third_party/tomlc17/tomlc17.c"], ["port/ios/host", "port/third_party/tomlc17"],
        ["port/ios/host/host_profile.h", "port/linux/src/port_profile_keys.h", "port/linux/src/halo_display.h"])


def device_profile_key(platform, model, chip=""):
    if not all(isinstance(value, str) and "\0" not in value for value in (platform, model, chip)):
        raise ValueError("invalid hardware identifier")
    result = subprocess.run([str(profile_delivery_helper()), "key", platform, model, chip], capture_output=True, text=True)
    if result.returncode:
        raise ValueError("invalid or noncanonical hardware identifier")
    return result.stdout.strip()


def profile_target_parts(target):
    if not isinstance(target, str) or len(target) > 160:
        raise ValueError("invalid profile target")
    parts = target.split("/")
    if len(parts) != 3:
        raise ValueError("profile target needs platform/model/chip")
    platform, model, encoded = parts
    chip = encoded.replace("-", " ") if platform == "mac" else ""
    if device_profile_key(platform, model, chip) != target:
        raise ValueError("profile target is not canonical")
    return platform, model, chip


def validate_shaders(folder):
    probe = helper("shader-pack-probe", ["port/ios/tests/shader_pack_probe.c", "port/ios/host/host_shader_pack.c"],
                   dependencies=["port/ios/host/host_shader_pack.h"])
    result = subprocess.run([str(probe), str(folder)], capture_output=True, text=True)
    if result.returncode:
        raise ValueError(result.stderr.strip() or "shader snapshot rejected")


def validate_device_profiles(folder, target):
    platform, model, chip = profile_target_parts(target)
    result = subprocess.run([str(profile_delivery_helper()), "validate", str(folder), platform, model, chip], capture_output=True, text=True)
    if result.returncode:
        raise ValueError(result.stderr.strip() or "device profile snapshot rejected")


def device_profile_snapshot(folder, platform, model, chip=""):
    """Publish one target's class, selected model, and conservative inheritance source closure."""
    target = device_profile_key(platform, model, chip)
    folder = Path(folder)
    source = snapshot(folder, "profiles", "profiles-2")
    tables = {name: profile_check.load_toml(folder / name) for name in source if name.endswith(".toml") and "/" not in name}
    # Ask the actual resolver for exact/and_newer precedence; do not recreate it in Python.
    result = subprocess.run([str(profile_delivery_helper()), "select", str(folder), platform, model, chip],
                            capture_output=True, text=True, check=True)
    selected = result.stdout.splitlines()
    selected_model = selected[1] if len(selected) > 1 else ""
    keep = {name for name, table in tables.items() if table["kind"] == "class" and table["match"]["platform"] == platform}
    if selected_model:
        keep.add(selected_model)
    catalog = profile_check.load_toml(folder / "hardware/catalog.toml")
    hardware, fallbacks = [], []
    if not selected_model:
        candidates = [h for h in catalog["hardware"] if h["platform"] == platform and
                      (not h.get("models") or model in h["models"]) and (not h.get("chips") or chip in h["chips"])]
        if candidates:
            current = candidates[0]
            by_id = {h["id"]: h for h in catalog["hardware"]}
            fallbacks = [f for f in catalog["fallback"] if by_id[f["hardware"]]["platform"] == platform and
                         by_id[f["hardware"]].get("tier", 0) <= current.get("tier", 0)]
            identifiers = {current["id"]} | {f["hardware"] for f in fallbacks}
            hardware = [h for h in catalog["hardware"] if h["id"] in identifiers]
            keep.update(f["profile"] for f in fallbacks)
    lines = ["schema = 1", "version = " + json.dumps(catalog["version"])]
    for name, values in (("hardware", hardware), ("fallback", fallbacks)):
        if not values:
            lines.append(name + " = []")
    for name, values in (("hardware", hardware), ("fallback", fallbacks)):
        for table in values:
            lines.append("[[" + name + "]]")
            lines.extend(key + " = " + json.dumps(value, ensure_ascii=True) for key, value in table.items())
    files = {name: source[name] for name in sorted(keep)}
    files["hardware/catalog.toml"] = ("\n".join(lines) + "\n").encode("ascii")
    with tempfile.TemporaryDirectory(prefix="visr-device-profile-") as temporary:
        root = Path(temporary)
        for name, content in files.items():
            path = root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(content)
        validate_device_profiles(root, target)
    return files


def validate_recordings(folder, compatibility=None):
    result = subprocess.run([str(recording_helper()), str(folder)], capture_output=True, text=True)
    if result.returncode not in (0, 1):
        raise RuntimeError(result.stderr.strip() or "recording validator failed")
    if result.returncode:
        raise ValueError(result.stderr.strip() or "recording snapshot rejected")
    catalog_path = Path(folder) / benchmark_catalog.CATALOG_NAME
    if compatibility and compatibility.startswith("benchmark-catalog-1:"):
        benchmark_catalog.load_catalog(folder)
    elif catalog_path.exists():
        raise ValueError("benchmark-catalog.json requires benchmark-catalog-1 compatibility")
    else:
        # Preserve the exact old-client feed contract in the publisher only. New app code has no built-in case IDs.
        routes = sorted(Path(folder).glob("*.route"))
        if len(routes) > LEGACY_MAXIMUM_ROUTES:
            raise ValueError("benchmark-replay-1 snapshots support at most 13 routes")
        for route in routes:
            scene = route.stem
            for line in route.read_text(encoding="ascii").splitlines():
                if line.startswith("scene = "):
                    scene = line[8:]
                    break
            if scene in LEGACY_BUILTIN_SCENES:
                raise ValueError(f"{route.name}: scene conflicts with a legacy benchmark case")


def validate_profiles(folder):
    tables = {}
    problems = []
    keys = profile_check.profile_keys()
    for path in sorted(folder.glob("*.toml")):
        errors, table = profile_check.check_profile(path, keys)
        if table is not None:
            name = table.get("name")
            if (not isinstance(name, str) or not name or len(name.encode("utf-8")) >= 64
                    or any(ord(character) < 32 or ord(character) == 127 for character in name)):
                errors.append("name must be 1 to 63 UTF-8 bytes without ASCII control characters")
        problems.extend(f"{path.name}: {error}" for error in errors)
        if not errors:
            tables[path.name] = table
    problems.extend(profile_check.check_set(tables))
    problems.extend(check_catalog(folder / "hardware/catalog.toml", tables))
    if problems:
        raise ValueError("profile snapshot rejected: " + "; ".join(problems))


def read_regular(directory, name, maximum):
    """Read through the held parent descriptor; never follow a file symlink or block on a FIFO."""
    descriptor = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= maximum:
            raise ValueError(f"{name}: not a bounded regular file")
        with os.fdopen(descriptor, "rb", closefd=False) as stream:
            data = stream.read(maximum + 1)
        if len(data) != info.st_size:
            raise ValueError(f"{name}: file changed while reading")
        return data
    finally:
        os.close(descriptor)


def snapshot_limits(kind):
    if kind == "shaders":
        return 32, 4 * 1024 * 1024, 8 * 1024 * 1024
    return ((129, 64 * 1024, 2 * 1024 * 1024) if kind == "profiles"
        else (RECORDING_MAX_FILES, RECORDING_MAX_FILE_BYTES, RECORDING_MAX_FILES * RECORDING_MAX_FILE_BYTES))


def snapshot_name_allowed(name, kind, compatibility=None):
    if not isinstance(name, str):
        return False
    if kind == "shaders":
        return re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,62}\.txt", name) is not None
    if kind == "profiles":
        return name == "hardware/catalog.toml" or (compatibility and compatibility.startswith("device-profiles-1:") and name == "capacity.toml") or (len(name) < 64 and PROFILE_NAME.fullmatch(name) and name != "device-profile.toml")
    return ((compatibility and compatibility.startswith("benchmark-catalog-1:") and name == benchmark_catalog.CATALOG_NAME)
        or RECORDING_NAME.fullmatch(name))


def snapshot(folder, kind, compatibility=None):
    """Snapshot only allowed files, then validate those exact bytes in a private temporary folder."""
    maximum_files, maximum_file, maximum_total = snapshot_limits(kind)
    files = {}
    descriptor = os.open(folder, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        for name in sorted(os.listdir(descriptor)):
            if kind == "profiles" and name == "hardware":
                hardware = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=descriptor)
                try:
                    if os.listdir(hardware) != ["catalog.toml"]:
                        raise ValueError("hardware must contain only catalog.toml")
                    files["hardware/catalog.toml"] = read_regular(hardware, "catalog.toml", maximum_file)
                finally:
                    os.close(hardware)
            else:
                if kind == "profiles":
                    # Repository documentation is never part of a published snapshot. Links are still rejected.
                    if name in ("README.md", "TEMPLATE.toml"):
                        read_regular(descriptor, name, maximum_file)
                        continue
                    allowed = snapshot_name_allowed(name, kind, compatibility)
                else:
                    allowed = snapshot_name_allowed(name, kind, compatibility)
                if not allowed:
                    raise ValueError(f"{name}: file is not allowed in a {kind} snapshot")
                files[name] = read_regular(descriptor, name, maximum_file)
            total = sum(map(len, files.values()))
            if len(files) > maximum_files or total > maximum_total:
                raise ValueError("snapshot exceeds its file count or total size limit")
    finally:
        os.close(descriptor)
    if not files:
        raise ValueError("snapshot is empty")
    with tempfile.TemporaryDirectory(prefix="visr-pack-validate-") as temporary:
        validated = Path(temporary)
        for name, content in files.items():
            target = validated / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
        if kind == "shaders":
            validate_shaders(validated)
        elif kind == "profiles":
            if compatibility and compatibility.startswith("device-profiles-1:"):
                validate_device_profiles(validated, compatibility.split(":", 1)[1])
            else:
                validate_profiles(validated)
        else:
            validate_recordings(validated, compatibility)
        if kind == "profiles" and compatibility == "profiles-1":
            for name in files:
                if name == "hardware/catalog.toml":
                    continue
                profile = profile_check.load_toml(validated / name)
                if "external" in profile or "and_newer" in profile.get("match", {}):
                    raise ValueError("profiles-1 snapshots cannot contain external sections or and_newer rules; publish as profiles-2")
    return files


def atomic_write(path, contents):
    path = Path(path)
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", required=True, choices=("profiles", "recordings", "shaders"))
    parser.add_argument("--channel", required=True, choices=("stable", "tester"))
    parser.add_argument("--sequence", required=True, type=int)
    parser.add_argument("--compatibility", required=True)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--signing-key", required=True, type=Path)
    parser.add_argument("--notes", default="")
    parser.add_argument("--device-platform")
    parser.add_argument("--device-model")
    parser.add_argument("--device-chip", default="")
    parser.add_argument("--minimum-engine", type=int, default=1)
    parser.add_argument("--maximum-engine", type=int, default=1)
    args = parser.parse_args(arguments)
    if not 1 <= args.sequence <= MAXIMUM_SEQUENCE:
        raise ValueError("sequence must be a positive integer at most 2^53 - 1")
    if args.minimum_engine != 1 or args.maximum_engine != 1:
        raise ValueError("this publisher supports only minimum_engine = maximum_engine = 1")
    expected = (r"device-profiles-1:.+|profiles-[12]" if args.kind == "profiles" else
                "shader-lists-1" if args.kind == "shaders" else r"benchmark-(?:replay-1|catalog-1):[0-9a-f]{64}")
    if not re.fullmatch(expected, args.compatibility):
        raise ValueError("compatibility must be profiles-1 or profiles-2, or benchmark-replay-1:/benchmark-catalog-1: plus the exact lowercase guest SHA-256")
    if len(args.notes.encode("utf-8")) > 4096:
        raise ValueError("notes must be at most 4096 UTF-8 bytes")
    if args.kind == "profiles" and args.compatibility.startswith("device-profiles-1:"):
        platform, model, chip = profile_target_parts(args.compatibility.split(":", 1)[1])
        if (args.device_platform is not None and args.device_platform != platform) or (args.device_model is not None and args.device_model != model) or (args.device_chip and args.device_chip != chip):
            raise ValueError("device arguments differ from signed compatibility target")
        files = device_profile_snapshot(args.input, platform, model, chip)
    else:
        if args.device_platform or args.device_model or args.device_chip:
            raise ValueError("device arguments require device-profiles-1 compatibility")
        files = snapshot(args.input, args.kind, args.compatibility)
    payload = canonical({"files": {name: base64.b64encode(content).decode("ascii") for name, content in files.items()}})
    manifest = canonical({"schema": 1, "kind": args.kind, "channel": args.channel, "sequence": args.sequence,
        "minimum_engine": args.minimum_engine, "maximum_engine": args.maximum_engine,
        "compatibility": args.compatibility, "sha256": hashlib.sha256(payload).hexdigest(), "notes": args.notes})
    with tempfile.TemporaryDirectory(prefix="visr-pack-sign-") as temporary:
        manifest_path = Path(temporary) / "manifest.json"
        manifest_path.write_bytes(manifest)
        signature = subprocess.check_output([str(sign_helper()), "--sign", str(args.signing_key), str(manifest_path)])
    if len(signature) != 64:
        raise ValueError("signer returned an invalid Ed25519 signature")
    envelope = canonical({name: base64.b64encode(value).decode("ascii")
        for name, value in (("manifest", manifest), ("signature", signature), ("payload", payload))})
    # the app refuses a larger download before it checks the signature (host_data_pack.m, envelopeLimit)
    if len(envelope) + 1 > ENVELOPE_LIMITS[args.kind]:
        raise ValueError(f"envelope exceeds the app's {ENVELOPE_LIMITS[args.kind]} byte download limit")
    atomic_write(args.output, envelope + b"\n")
    print(f"Wrote {args.kind} sequence {args.sequence}: {len(files)} files, {len(envelope) + 1} bytes")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"data_pack: {error}", file=sys.stderr)
        sys.exit(1)
