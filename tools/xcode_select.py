#!/usr/bin/env python3
"""Choose the installed Xcode whose build a release accepts and export it as DEVELOPER_DIR.

Lists every /Applications/Xcode*.app with its build first, so the log says what the image has. It
exits with an error when none of the accepted builds is installed.
"""
import glob
import os
import plistlib
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xcode_builds import XCODE_BUILDS  # noqa: E402


def installed() -> dict[str, str]:
    found: dict[str, str] = {}
    for app in sorted(glob.glob("/Applications/Xcode*.app")):
        try:
            with open(f"{app}/Contents/version.plist", "rb") as file:
                build = plistlib.load(file)["ProductBuildVersion"]
        except (OSError, KeyError, plistlib.InvalidFileException):
            build = "unknown"
        print(f"{app}  build {build}")
        found.setdefault(build, os.path.realpath(app))
    return found


def main() -> None:
    found = installed()
    for build in XCODE_BUILDS:
        if build in found:
            developer_dir = f"{found[build]}/Contents/Developer"
            print(f"Selected Xcode build {build}: {developer_dir}")
            print(subprocess.run(["xcodebuild", "-version"], env={**os.environ, "DEVELOPER_DIR": developer_dir},
                                 capture_output=True, text=True, check=False).stdout, end="")
            target = os.environ.get("GITHUB_ENV")
            if target:
                with open(target, "a", encoding="utf-8") as file:
                    file.write(f"DEVELOPER_DIR={developer_dir}\n")
            return
    sys.exit(f"None of the accepted Xcode builds {', '.join(XCODE_BUILDS)} is installed")


if __name__ == "__main__":
    main()
