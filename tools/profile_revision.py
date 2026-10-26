#!/usr/bin/env python3
"""Check or bump the device profile set's revision (profiles/hardware/revision.toml, docs/data-packs.md).

The revision orders a signed profile shard against the profiles an app bundles: the app ignores a downloaded
shard whose revision is lower than its own. The file's sha256 covers every profile input, so a change to a
profile or the hardware catalog without a new revision fails the check.

  python3 tools/profile_revision.py            # check
  python3 tools/profile_revision.py --bump     # after changing profiles: next revision, current hash
"""
import argparse
import hashlib
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
PROFILES = ROOT / "profiles"
NAME = "hardware/revision.toml"
MAXIMUM = 2**53 - 1
HEADER = ("# The device profile set's revision (docs/data-packs.md, \"Profile revisions\"). After changing any profile\n"
          "# or the hardware catalog, run python3 tools/profile_revision.py --bump.\n")
# what the native reader accepts (host_profile.c, host_profile_revision_parse)
LIMIT = 1024


def inputs(folder):
    """The files the revision covers: every profile and the hardware catalog, by name."""
    folder = Path(folder)
    names = sorted(path.name for path in folder.glob("*.toml") if path.name != "TEMPLATE.toml")
    names.append("hardware/catalog.toml")
    return {name: (folder / name).read_bytes() for name in names}


def inputs_sha256(folder):
    digest = hashlib.sha256()
    for name, content in sorted(inputs(folder).items()):
        digest.update(name.encode("ascii") + b"\0" + hashlib.sha256(content).hexdigest().encode("ascii") + b"\n")
    return digest.hexdigest()


FORM = re.compile(r"(?:#[^\n]*\n)*schema = 1\nrevision = ([1-9][0-9]{0,15})\nsha256 = \"([0-9a-f]{64})\"\n")


def parse(content):
    """(revision, sha256) from the file's bytes; ValueError unless it is exactly the form text() writes"""
    if not isinstance(content, bytes) or not 0 < len(content) <= LIMIT:
        raise ValueError(f"{NAME}: missing, empty or larger than {LIMIT} bytes")
    try:
        match = FORM.fullmatch(content.decode("ascii"))
    except UnicodeDecodeError:
        match = None
    if not match or int(match[1]) > MAXIMUM:
        raise ValueError(f"{NAME}: needs comment lines, then exactly schema = 1, revision = N (1 to 2^53 - 1) "
                         "and sha256 = \"64 lowercase hex digits\"")
    return int(match[1]), match[2]


def read(folder):
    return parse((Path(folder) / NAME).read_bytes())


def text(revision, digest):
    return f"{HEADER}schema = 1\nrevision = {revision}\nsha256 = \"{digest}\"\n"


def check(folder=PROFILES):
    """The revision, if the file matches the profiles beside it; ValueError otherwise."""
    revision, digest = read(folder)
    if digest != inputs_sha256(folder):
        raise ValueError(f"{NAME}: the profiles changed since revision {revision}; "
                         "run python3 tools/profile_revision.py --bump")
    return revision


def bump(folder=PROFILES):
    folder = Path(folder)
    path = folder / NAME
    revision = read(folder)[0] if path.exists() else 0
    if revision >= MAXIMUM:
        raise ValueError(f"{NAME}: the revision cannot grow further")
    path.write_text(text(revision + 1, inputs_sha256(folder)), encoding="ascii")
    return revision + 1


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bump", action="store_true", help="write the next revision and the current hash")
    parser.add_argument("--profiles", type=Path, default=PROFILES, help="the profile folder (default: profiles/)")
    args = parser.parse_args(arguments)
    try:
        if args.bump:
            print(f"{NAME}: revision {bump(args.profiles)}")
        else:
            print(f"{NAME}: revision {check(args.profiles)} matches the profiles")
    except (OSError, ValueError) as error:
        print(f"profile_revision: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
