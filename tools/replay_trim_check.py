#!/usr/bin/env python3
"""Do a catalog's trimmed recordings replay like the full ones? An end-to-end check on the Mac runner.

    replay_trim_check.py --full FOLDER --trimmed FOLDER --out FOLDER CASE [CASE...]

Each case is replayed through the device benchmark's suite from the full catalog and again from the trimmed one (the
folder is installed as the runner's benchmark/local-routes, an unsigned catalog a build that carries routes runs).
The replays' routes (player 1's position every 300 ticks), end ticks and run status are compared. Exit status 0
when every case agrees, 1 when one parts or ends early, 2 when a step failed. Needs tools/ios_build.py --mac first.
"""
import argparse
import json
import plistlib
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import benchmark_routes  # noqa: E402
import mac_run  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def replay(local, folder, case, out):
    shutil.rmtree(local, ignore_errors=True)
    shutil.copytree(folder, local)
    command = [sys.executable, str(ROOT / "tools/mac_run.py"), "run", "--out", str(out), "--set", 'display.renderer="metal"',
               "--set", 'display.frame_rate="unlimited"', "--benchmark-suite", f"quick:{case}", "--time-limit", "1500"]
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=ROOT)
    report = out / "benchmark" / "report.json"
    replays = benchmark_routes.by_recording([report]).get(case, []) if report.is_file() else []
    return replays[0] if replays else None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--full", type=Path, required=True)
    parser.add_argument("--trimmed", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("cases", nargs="+")
    args = parser.parse_args()
    app = mac_run.native_app_default()
    with (app / "Contents/Info.plist").open("rb") as file:
        data = mac_run.native_data_folder(plistlib.load(file)["CFBundleIdentifier"])
    local = data / "benchmark" / "local-routes"
    exit_status, summary = 0, {}
    try:
        for case in args.cases:
            full = replay(local, args.full, case, args.out / f"{case}-full")
            trimmed = replay(local, args.trimmed, case, args.out / f"{case}-trimmed")
            if not full or not trimmed:
                print(f"replay_trim_check: {case}: a replay logged no route (full {bool(full)}, trimmed {bool(trimmed)})")
                exit_status = max(exit_status, 2)
                continue
            compared, worst, parted = benchmark_routes.compare_routes(full[2], trimmed[2])
            ends = (full[2][-1][0], trimmed[2][-1][0])
            ok = parted is None and len(trimmed[2]) >= len(full[2]) - 1 and full[1] == trimmed[1]
            print(f"replay_trim_check: {case}: {compared} points compared, worst {worst:.2f} world units, "
                  f"{'PART at tick %.0f' % parted if parted is not None else 'agree'}; routes end at ticks {ends[0]:.0f} and "
                  f"{ends[1]:.0f}; runs ended {full[1]} and {trimmed[1]}: {'OK' if ok else 'DIFFERS'}", flush=True)
            summary[case] = {"points": compared, "worst": worst, "parted": parted, "ends": ends, "status": [full[1], trimmed[1]], "ok": ok}
            exit_status = max(exit_status, 0 if ok else 1)
    finally:
        shutil.rmtree(local, ignore_errors=True)
    (args.out / "summary.json").write_text(json.dumps(summary, indent=1))
    return exit_status


if __name__ == "__main__":
    sys.exit(main())
