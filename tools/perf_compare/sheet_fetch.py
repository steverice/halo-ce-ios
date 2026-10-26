#!/usr/bin/env python3
"""Download the results sheet's public CSV into the committed snapshot the page is built from."""
from __future__ import annotations

import argparse
import csv
import io
from pathlib import Path
import sys
import urllib.parse
import urllib.request

try:
    from .sheet_export import COLUMNS
except ImportError:
    from sheet_export import COLUMNS

ROOT = Path(__file__).resolve().parents[2]
SNAPSHOT = ROOT / "docs/benchmarks/results.csv"
SHEET_ID = "1tuTJpOxUTbqgzOSDSW-nSvWKL8HkZMVucvNRUIee0t4"


def export_url(sheet_id: str, gid: str) -> str:
    query = urllib.parse.urlencode({"format": "csv", "gid": gid})
    return f"https://docs.google.com/spreadsheets/d/{sheet_id}/export?{query}"


def validate(text: str) -> str:
    """Return the CSV with LF line ends, or raise when it is not the results tab."""
    text = text.lstrip("﻿")
    if text.lstrip().lower().startswith("<"):
        raise ValueError("the response is a web page, not CSV: is the sheet public and the gid the results tab?")
    rows = list(csv.reader(io.StringIO(text)))
    if not rows or rows[0] != list(COLUMNS):
        raise ValueError("the first row does not match the results columns: " + ",".join(rows[0] if rows else []))
    if len(rows) < 2:
        raise ValueError("the results tab has no rows")
    out = io.StringIO()
    csv.writer(out, lineterminator="\n").writerows(row for row in rows if any(cell.strip() for cell in row))
    return out.getvalue()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gid", required=True, help="the results tab's gid (the number after gid= in its URL)")
    parser.add_argument("--sheet-id", default=SHEET_ID)
    parser.add_argument("--output", type=Path, default=SNAPSHOT)
    parser.add_argument("--from-file", type=Path, help="read this CSV instead of downloading (for tests)")
    args = parser.parse_args()
    if args.from_file:
        text = args.from_file.read_text(encoding="utf-8")
    else:
        with urllib.request.urlopen(export_url(args.sheet_id, args.gid), timeout=60) as response:
            text = response.read().decode("utf-8")
    args.output.write_text(validate(text), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
