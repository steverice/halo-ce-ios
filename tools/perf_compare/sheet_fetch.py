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
    from .sheet_export import COLUMNS, RELEASE_KINDS
except ImportError:
    from sheet_export import COLUMNS, RELEASE_KINDS

ROOT = Path(__file__).resolve().parents[2]
SNAPSHOT = ROOT / "docs/benchmarks/results.csv"
SHEET_ID = "1tuTJpOxUTbqgzOSDSW-nSvWKL8HkZMVucvNRUIee0t4"


def export_url(sheet_id: str, gid: str) -> str:
    query = urllib.parse.urlencode({"format": "csv", "gid": gid})
    return f"https://docs.google.com/spreadsheets/d/{sheet_id}/export?{query}"


def validate(text: str) -> str:
    """Return the CSV in canonical column order with LF line ends, or raise when it is not the results tab.

    Columns are read by header name, so the sheet may reorder or add columns; a missing column is an error.
    """
    text = text.lstrip("\ufeff")
    if text.lstrip().lower().startswith("<"):
        raise ValueError("the response is a web page, not CSV: is the sheet public and the gid the results tab?")
    table = list(csv.reader(io.StringIO(text)))
    if not table:
        raise ValueError("the response is empty")
    missing = [name for name in COLUMNS if name not in table[0]]
    if missing:
        raise ValueError("the header lacks results columns: " + ", ".join(missing))
    position = {name: table[0].index(name) for name in COLUMNS}
    body = []
    for row in table[1:]:
        if not any(cell.strip() for cell in row):
            continue
        record = {name: row[index] if index < len(row) else "" for name, index in position.items()}
        if record["release_kind"] not in RELEASE_KINDS:
            continue  # the snapshot holds released builds only, whatever the sheet carries
        # Sheets writes booleans as TRUE/FALSE; the snapshot keeps the export's lowercase spelling.
        if record["reference_box"].lower() in ("true", "false"):
            record["reference_box"] = record["reference_box"].lower()
        body.append(record)
    if not body:
        raise ValueError("the results tab has no released-build rows")
    out = io.StringIO()
    writer = csv.DictWriter(out, fieldnames=list(COLUMNS), lineterminator="\n")
    writer.writeheader()
    writer.writerows(body)
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
