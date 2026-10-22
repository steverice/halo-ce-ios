#!/usr/bin/env python3
"""Exercise production overlay layout in UIKit, without game assets.

Run on a simulator job host. Creates and deletes only its own simulator;
screenshots, app logs, and state receipts remain in --output.
"""
import argparse
import json
import os
from pathlib import Path
import plistlib
import shlex
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
BUNDLE = "org.visr.tests.benchmark-overlay"
DEVICE_SET = []


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, timeout=180, **kwargs)


def sim(*args):
    return run("xcrun", "simctl", *DEVICE_SET, *args, capture_output=True).stdout.strip()


def wait_for_state(container, state, log):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        path = container / "Documents/state.txt"
        if path.exists() and path.read_text() == state:
            return
        if log.exists() and ("uncaught exception" in log.read_text() or "Assertion failed" in log.read_text()):
            break
        time.sleep(0.2)
    raise RuntimeError(f"Missing {state} receipt; see {log}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source", type=Path, default=ROOT / "port/ios/host/host_benchmark_page.m",
                        help="Overlay source, including a saved regression control")
    parser.add_argument("--device-set", type=Path, help="Store temporary simulator data on another volume")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    if args.device_set:
        args.device_set.mkdir(parents=True, exist_ok=True)
        DEVICE_SET[:] = ["--set", str(args.device_set.resolve())]
    inventory = json.loads(sim("list", "--json"))
    (output / "inventory.json").write_text(json.dumps(inventory, indent=2))
    runtime = next(r for r in inventory["runtimes"] if r["isAvailable"] and r.get("platform") == "iOS")
    supported = runtime["supportedDeviceTypes"]
    phone = next(d for d in supported if d["name"].startswith("iPhone SE")) if any(
        d["name"].startswith("iPhone SE") for d in supported) else next(d for d in supported if d["name"].startswith("iPhone"))
    tablet = next((d for d in supported if d["name"].startswith("iPad")), None)  # some runtimes ship phones only
    app = output / "OverlayProbe.app"
    app.mkdir()
    production = args.source.read_text()
    production = production[production.index("/* Pass through game input"):]
    template = (ROOT / "port/ios/tests/benchmark_overlay_uikit_probe.m").read_text()
    source = output / "probe.m"
    source.write_text(template.replace("// PRODUCTION_OVERLAY", production))
    with (app / "Info.plist").open("wb") as stream:
        plistlib.dump({"CFBundleIdentifier": BUNDLE, "CFBundleExecutable": "OverlayProbe",
                      "CFBundleName": "OverlayProbe", "CFBundlePackageType": "APPL",
                      "CFBundleVersion": "1", "CFBundleShortVersionString": "1.0",
                      "UIDeviceFamily": [1, 2], "UILaunchScreen": {},
                      "UIApplicationSceneManifest": {"UIApplicationSupportsMultipleScenes": False,
                          "UISceneConfigurations": {}},
                      "CFBundleURLTypes": [{"CFBundleURLSchemes": ["overlayprobe"]}]}, stream)
    sdk = run("xcrun", "--sdk", "iphonesimulator", "--show-sdk-path", capture_output=True).stdout.strip()
    run("xcrun", "--sdk", "iphonesimulator", "clang", "-isysroot", sdk,
        "-target", "arm64-apple-ios17.0-simulator", "-fobjc-arc", "-fblocks",
        "-I", str(ROOT / "port/ios/host"), "-framework", "UIKit", "-framework", "Foundation",
        "-framework", "CoreGraphics",
        str(source), "-o", str(app / "OverlayProbe"))
    run("codesign", "--sign", "-", str(app))
    for kind, device in (("phone", phone), ("ipad", tablet)):
        if device is None:
            print(f"{kind}: no supported device type in {runtime['name']}, skipped", flush=True)
            continue
        folder = output / kind
        folder.mkdir()
        udid = sim("create", "VISR overlay layout probe", device["identifier"], runtime["identifier"])
        print(f"{kind}: {device['name']}, {runtime['name']}, {udid}", flush=True)
        # A preempted job can shut down this simulator without touching others.
        if cleanup := os.environ.get("HEAVY_JOB_CLEANUP_FILE"):
            with open(cleanup, "a") as stream:
                for command in ("shutdown", "delete"):
                    stream.write(shlex.join(["xcrun", "simctl", *DEVICE_SET, command, udid]) + "\n")
        try:
            sim("boot", udid)
            sim("bootstatus", udid, "-b")
            sim("install", udid, str(app))
            log = folder / "stderr.log"
            sim("launch", "--stdout=" + str(folder / "stdout.log"), "--stderr=" + str(log), udid, BUNDLE)
            container = Path(sim("get_app_container", udid, BUNDLE, "data"))
            for state in ("loading", "measuring", "presented", "restored", "finish", "hidden"):
                if state != "loading":
                    (container / "Documents/command.txt").write_text(state)
                wait_for_state(container, state, log)
                time.sleep(0.5)  # Let the compositor display the checked layout.
                sim("io", udid, "screenshot", str(folder / (state + ".png")))
            sim("terminate", udid, BUNDLE)
            text = log.read_text()
            for failure in ("uncaught exception", "Assertion failed", "Unable to simultaneously satisfy constraints"):
                assert failure not in text, text
            assert "created native progress window" in text
            print(text, flush=True)
        finally:
            try:
                subprocess.run(["xcrun", "simctl", *DEVICE_SET, "shutdown", udid], check=False,
                               capture_output=True, timeout=30)
            finally:
                sim("delete", udid)


if __name__ == "__main__":
    main()
