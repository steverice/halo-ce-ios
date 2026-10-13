#!/usr/bin/env python3
"""Writes the loading screen's picture, loading.tga, into a maps folder from your own disc.

    python3 tools/loading_picture.py DISC_IMAGE_OR_DEFAULT_XBE MAPS_FOLDER

The game's loading screen lights a 320 by 240 gray picture. A PAL disc carries it as
maps/loading.tga; an NTSC disc's executable (default.xbe) holds it as runs of gray levels
(port/runtime/loading_picture.h, which the importers use). Game data imported before the
importers took the picture has no loading.tga: this writes it, from the disc image the maps
came from or from its default.xbe. The file an existing maps folder has is left alone unless
--force is given.
"""
import argparse
import struct
import sys
from pathlib import Path

SECTOR = 2048
WIDTH, HEIGHT = 320, 240
PIXELS = WIDTH * HEIGHT
TGA_BYTES = 18 + 3 * PIXELS
PARTITIONS = (0, 0x0FD90000, 0x02080000, 0x18300000)
MAGIC = b"MICROSOFT*XBOX*MEDIA"


def find_picture(executable):
    """the picture's gray levels, rows from the top, or None (loading_picture_find)"""
    dimensions = struct.pack("<II", WIDTH, HEIGHT)
    offset = executable.find(dimensions)
    while offset >= 0:
        pixels = bytearray()
        changes = 0
        previous = None
        for run in executable[offset + 8:offset + 8 + PIXELS]:
            count = run >> 4
            if not count or len(pixels) + count > PIXELS:
                break
            changes += previous is not None and (run & 15) != previous
            previous = run & 15
            pixels += bytes([(run & 15) << 4]) * count
            if len(pixels) == PIXELS:
                break
        if len(pixels) == PIXELS and changes >= 256:
            return bytes(pixels)
        offset = executable.find(dimensions, offset + 1)
    return None


def tga(pixels):
    """the file the game reads: uncompressed, 24-bit, rows from the bottom"""
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, WIDTH, HEIGHT, 24, 0)
    rows = [pixels[y * WIDTH:(y + 1) * WIDTH] for y in range(HEIGHT - 1, -1, -1)]
    return header + b"".join(bytes(value for gray in row for value in (gray, gray, gray)) for row in rows)


def disc_files(image, partition, sector, size):
    """the entries of one directory of an Xbox disc image: {lower-case name: (sector, size, is_directory)}"""
    image.seek(partition + sector * SECTOR)
    table = image.read(size)
    found, seen, pending = {}, set(), [0]
    while pending:
        offset = pending.pop()
        if offset in seen or offset + 14 > len(table):
            continue
        seen.add(offset)
        left, right, start, length, attributes, count = struct.unpack_from("<HHIIBB", table, offset)
        if left == 0xFFFF:
            continue
        name = table[offset + 14:offset + 14 + count].decode("latin-1").lower()
        found[name] = (start, length, bool(attributes & 16))
        pending += [link * 4 for link in (left, right) if link]
    return found


def from_disc(path):
    """(the disc's own maps/loading.tga or None, its default.xbe or None)"""
    with open(path, "rb") as image:
        for partition in PARTITIONS:
            image.seek(partition + 32 * SECTOR)
            header = image.read(28)
            if header[:20] == MAGIC:
                break
        else:
            return None, None
        root = disc_files(image, partition, *struct.unpack_from("<II", header, 20))

        def read(entry, most):
            if not entry or entry[2] or entry[1] > most:
                return None
            image.seek(partition + entry[0] * SECTOR)
            return image.read(entry[1])

        maps = root.get("maps")
        picture = read(disc_files(image, partition, maps[0], maps[1]).get("loading.tga"), TGA_BYTES) if maps else None
        return picture, read(root.get("default.xbe"), 16 << 20)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=Path, help="your disc image (.iso, .xiso) or its default.xbe")
    parser.add_argument("maps", type=Path, help="the maps folder to write loading.tga into")
    parser.add_argument("--force", action="store_true", help="replace a loading.tga that is already there")
    args = parser.parse_args()
    target = args.maps / "loading.tga"
    if not args.maps.is_dir():
        sys.exit(f"{args.maps} is not a folder")
    if target.exists() and not args.force:
        sys.exit(f"{target} is already there (--force replaces it)")
    picture, executable = from_disc(args.source)
    if picture is None and executable is None:
        executable = args.source.read_bytes()
    if picture is not None and len(picture) == TGA_BYTES and picture[2] == 2 and picture[16] == 24:
        target.write_bytes(picture)
        print(f"copied the disc's maps/loading.tga to {target}")
        return
    pixels = find_picture(executable) if executable else None
    if pixels is None:
        sys.exit("no loading picture found: neither maps/loading.tga on the disc nor a picture in default.xbe")
    target.write_bytes(tga(pixels))
    print(f"wrote {target} from the picture in default.xbe")


if __name__ == "__main__":
    main()
