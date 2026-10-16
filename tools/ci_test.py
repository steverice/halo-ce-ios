#!/usr/bin/env python3
"""Run the asset-free Python/tooling checks used by GitHub Actions.

Native Apple probes run separately through ios_test.py. The extracted-source
harness has known baseline compile failures; see docs/ci.md before broadening
this target to recursively collect tools/.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
# These compile through xcrun and are already executed by ios_test.py.
APPLE_PROBES = {
    "test_menu_cancel.py",
    "test_replay_control.py",
    "test_network_light_state.py",
    "test_network_shader_warmup.py",
    "test_shader_list_warmup.py",
}
# These are standalone programs, not pytest modules.
STANDALONE = {"test_death_timing.py", "test_light_storage.py"}


def run(*arguments: str) -> None:
    print("+ " + " ".join(arguments), flush=True)
    subprocess.run(arguments, cwd=ROOT, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--junit", type=Path, help="write pytest's JUnit report")
    parser.add_argument("--linux", action="store_true", help="also run standalone Linux linker/death probes")
    args = parser.parse_args()
    if args.linux and sys.platform != "linux":
        parser.error("--linux requires Linux to execute the ELF linker probe")
    tests = sorted(str(path.relative_to(ROOT)) for path in (ROOT / "tools").glob("test_*.py")
                   if path.name not in APPLE_PROBES | STANDALONE)
    options = ["--quiet", "--strict-config", "--strict-markers", "--import-mode=prepend"]
    if args.junit:
        args.junit.parent.mkdir(parents=True, exist_ok=True)
        options.append(f"--junitxml={args.junit}")
    run(sys.executable, "-m", "pytest", *options, *tests)
    run(sys.executable, "tools/settings_schema.py", "--check")
    run(sys.executable, "tools/profile_check.py")
    run(sys.executable, "tools/profile_catalog_check.py")
    if args.linux:
        for name in sorted(STANDALONE):
            run(sys.executable, f"tools/{name}")


if __name__ == "__main__":
    main()
