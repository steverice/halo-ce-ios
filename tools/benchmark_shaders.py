#!/usr/bin/env python3
"""Extract hash-checked benchmark shader evidence for subsequent descriptor validation.

This never merges lists. Interval files carry attribution; cumulative dependency
candidates remain separate and must never be merged wholesale.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re

MAXIMUM = 128 * 1024 * 1024


def payload(capture: dict) -> str:
    text = capture.get("text")
    if not isinstance(text, str):
        raise ValueError("Capture text is missing")
    data = text.encode("utf-8")
    if len(data) > 4 * 1024 * 1024 or capture.get("bytes") != len(data):
        raise ValueError("Capture byte count differs or exceeds the limit")
    if hashlib.sha256(data).hexdigest() != capture.get("sha256"):
        raise ValueError("Capture checksum differs")
    if text and not text.endswith("\n"):
        raise ValueError("Capture ends in an incomplete record")
    return text


def map_name(value: object) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[a-z0-9_]{1,48}", value):
        raise ValueError("Invalid capture map")
    return value


def extract(report: dict, destination: Path) -> dict:
    app = report.get("app", {})
    if not re.fullmatch(r"[0-9a-f]{64}", app.get("image", "")):
        raise ValueError("Report has no exact guest image identity")
    groups = [(report, report.get("runs", []))]
    for previous in report.get("shader_output_intervals", []):
        if previous.get("app") != app:
            raise ValueError("Prior output has a different app identity")
        groups.append((previous, previous.get("runs", [])))
    files: dict[str, str] = {}
    outcomes = []
    for source, runs in groups:
        code = source.get("code", "")
        if not re.fullmatch(r"[A-Z2-9]{8}", code):
            raise ValueError("Invalid report code")
        for run in runs:
            index = run.get("index")
            if type(index) is not int or not 0 <= index < 512:
                raise ValueError("Invalid run index")
            capture = run.get("shader_capture")
            outcome = {"code": code, "index": index, "output": source.get("output", {}),
                       "status": capture.get("status") if isinstance(capture, dict) else "unavailable"}
            outcomes.append(outcome)
            if outcome["status"] not in ("captured", "no_new_records"):
                continue
            name = map_name(capture.get("map"))
            expected = run.get("map") or "ui"
            if name != expected or capture.get("format") != 1 or capture.get("scope") != "run_append_interval":
                raise ValueError("Capture attribution differs")
            text = payload(capture)
            if bool(text) != (outcome["status"] == "captured"):
                raise ValueError("Capture status differs from its payload")
            path = f"intervals/{code}-{index:02d}/{name}.txt"
            if path in files:
                raise ValueError("Duplicate run identity")
            files[path] = text
            outcome.update(map=name, path=path, sha256=capture["sha256"])
    for key, capture in report.get("shader_dependency_candidates", {}).items():
        name = map_name(key)
        if capture.get("status") != "captured":
            continue
        if capture.get("map") != name or capture.get("scope") != "cumulative_unattributed_dependency_candidates":
            raise ValueError("Dependency candidate attribution differs")
        files[f"dependency-candidates/{name}.txt"] = payload(capture)
    summary = {"format": 1, "code": report["code"], "app": app, "runs": outcomes,
               "descriptor_validation": "pending; validate hashes, ranges, map attribution, and pipeline dependencies before merging",
               "dependency_candidates": "cumulative and unattributed; never merge wholesale",
               "files": {name: hashlib.sha256(text.encode()).hexdigest() for name, text in files.items()}}
    destination.mkdir(parents=True, exist_ok=False)
    for name, text in files.items():
        path = destination / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
    (destination / "evidence.json").write_text(json.dumps(summary, indent=2) + "\n")
    return summary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    opener = gzip.open if args.report.suffix == ".gz" else open
    with opener(args.report, "rb") as stream:
        data = stream.read(MAXIMUM + 1)
    if len(data) > MAXIMUM:
        parser.error("Report exceeds 128 MiB")
    summary = extract(json.loads(data), args.out)
    print(f"Extracted {len(summary['files'])} evidence files; descriptor validation remains required.")


if __name__ == "__main__":
    main()
