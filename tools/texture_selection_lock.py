#!/usr/bin/env python3
"""Write port/assets/texture-selection-lock.tsv: the shipping texture set's scale for every texture, read from the
set itself, for the lock test of port/assets/texture-policy.json's "scales" rule (tools/test_texture_selection.py).

  texture_selection_lock.py SET_DIR CLASSIFIED_MANIFEST [OUTPUT]

SET_DIR is the composed set (step-tools texture-upscale/sets/policy-game-selective-round3): its
manifest-selective.json names each texture, and each <hash>.rgba file's header gives the replacement's size.
CLASSIFIED_MANIFEST is the manifest.json of the set it doubled from (sets/policy-game-double), which carries each
texture's original size, kind, treatment and tags. A row's scale is the replacement's width over the original's.
"""
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "port/assets/texture-selection-lock.tsv"
COLUMNS = ("hash", "width", "height", "kind", "treatment", "scale", "tags")


def rows(set_dir: Path, classified: Path) -> list[tuple]:
    selective = json.loads((set_dir / "manifest-selective.json").read_text())
    textures = json.loads(classified.read_text())["textures"]
    result = []
    for key, row in sorted(selective["textures"].items()):
        source = textures[key]
        with open(set_dir / f"{key}.rgba", "rb") as file:
            width, height, _levels = struct.unpack("<3I", file.read(12))
        if width % source["width"] or height % source["height"] or \
                width // source["width"] != height // source["height"]:
            raise SystemExit(f"{key}: {width}x{height} is not a whole multiple of {source['width']}x{source['height']}")
        tags = sorted({tag for tag, _index in source.get("tags", [])})
        if any("\t" in tag or "|" in tag for tag in tags):
            raise SystemExit(f"{key}: a tag path holds a tab or a bar")
        result.append((key, source["width"], source["height"], row["kind"], row["treatment"],
                       width // source["width"], "|".join(tags)))
    if len(result) != selective["texture_count"]:
        raise SystemExit(f"{len(result)} rows, but the manifest counts {selective['texture_count']}")
    return result


def main() -> None:
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    set_dir, classified = Path(sys.argv[1]), Path(sys.argv[2])
    output = Path(sys.argv[3]) if len(sys.argv) == 4 else OUTPUT
    table = rows(set_dir, classified)
    counts = {}
    for row in table:
        counts[row[5]] = counts.get(row[5], 0) + 1
    lines = [f"# {len(table)} textures of {set_dir.name}, by scale: " +
             ", ".join(f"{scale}x {count}" for scale, count in sorted(counts.items())) +
             "; written by tools/texture_selection_lock.py", "\t".join(COLUMNS)]
    lines += ["\t".join(str(value) for value in row) for row in table]
    output.write_text("\n".join(lines) + "\n")
    print(lines[0])


if __name__ == "__main__":
    main()
