#!/usr/bin/env python3
"""Name the guest frames of an Apple crash report, or of ios-runtime.log's CRASH lines.

The game's code runs from an alias of the app's __TEXT,__halocode in the guest arena
(port/ios/host/host_loader.c), so a crash report shows its frames as addresses outside every image.
An address minus the arena's start is its guest address; the build's guest-symbols.txt
(tools/ios_embed_guest.py writes it to build/ios/embedded, and release builds keep it with their
artifacts) names them.

    python3 tools/guest_symbolicate.py --symbols build/ios/embedded/guest-symbols.txt VISR-2026-10-25-120000.ips
    python3 tools/guest_symbolicate.py --symbols guest-symbols.txt ios-runtime.log
    python3 tools/guest_symbolicate.py --symbols guest-symbols.txt --arena 0x320000000 VISR.ips

ios-runtime.log's "CRASH: arena" line gives the arena's start. An .ips report does not carry it: pass it
with --arena (ios-runtime.log's "guest arena at" line) when the arena is not at a multiple of 4 GB; without
it the address's low 32 bits are taken, which is right for an arena at a multiple of 4 GB.

--symbols also takes the app itself (a VISR.app folder, its executable, or an .ipa), which carries the same
table in its __TEXT,__halosyms section.

An .ips report (JSON) prints its exception, its reason and every thread's frames with the guest ones named,
the crashed thread and the last exception backtrace first; any other text (a .crash report,
ios-runtime.log) is printed with a guest name after each address that is one. A crash the app made itself
(visr_guest_halt, visr_guest_abort, visr_host_error: port/ios/host/host_crash.c) carries its reason packed
into the crashed thread's registers, "VISRCRSH" in x0, then the guest image's short hash, "|" and the text
in x1-x15 and x19-x28; the tool prints it, and warns when that guest image is not the symbol file's.
"""

import argparse
import bisect
import json
import plistlib
import re
import struct
import sys
import zipfile
from pathlib import Path

GUEST_BASE = 0x88000000
HEADER = re.compile(r"guest image sha256 ([0-9a-f]{64})")
RANGE = re.compile(r"code from 0x([0-9a-f]+) to 0x([0-9a-f]+)")
IMAGE_NAMED = re.compile(r"guest image ([0-9a-f]{12})")
REASON_MAGIC = int.from_bytes(b"VISRCRSH", "little")
REASON_REGISTERS = list(range(1, 16)) + list(range(19, 29))
REGISTER = re.compile(r"\bx(\d{1,2}):\s*0x([0-9a-fA-F]+)")
ADDRESS = re.compile(r"0x([0-9a-fA-F]{9,16})\b")
ARENA = re.compile(r"CRASH: arena 0x([0-9a-fA-F]+)")


class Symbols:
    """a guest-symbols.txt: sorted guest addresses and their names, the image's sha256 and code range"""

    def __init__(self, text):
        self.sha256 = None
        self.start, self.end = GUEST_BASE, None
        self.addresses, self.names = [], []
        for line in text.splitlines():
            if line.startswith("#"):
                if match := HEADER.search(line):
                    self.sha256 = match.group(1)
                if match := RANGE.search(line):
                    self.start, self.end = int(match.group(1), 16), int(match.group(2), 16)
                continue
            address, _, name = line.partition(" ")
            if name:
                self.addresses.append(int(address, 16))
                self.names.append(name.strip())
        if self.end is None:
            self.end = (self.addresses[-1] + 0x10000) if self.addresses else self.start

    @classmethod
    def load(cls, path):
        """from a guest-symbols.txt, or from the table an app carries (an .app folder, its executable, an .ipa)"""
        path = Path(path)
        if path.is_dir():
            return cls.from_table(halosyms(app_executable(path)))
        data = path.read_bytes()
        if data[:4] == b"PK\x03\x04":
            return cls.from_table(halosyms(ipa_executable(path)))
        if data[:4] in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe"):
            return cls.from_table(halosyms(data))
        return cls(data.decode("utf-8", "replace"))

    @classmethod
    def from_table(cls, table):
        """from tools/ios_embed_guest.py's binary table (symbol_table)"""
        if table[:8] != b"VGSYM1\0\0":
            raise ValueError("not a guest symbol table")
        count, size, start, end = struct.unpack_from("<IIII", table, 8)
        digest = table[24:88].decode("ascii")
        names = table[88 + count * 8:88 + count * 8 + size]
        lines = [f"# guest image sha256 {digest}", f"# code from 0x{start:x} to 0x{end:x}"]
        for index in range(count):
            address, offset = struct.unpack_from("<II", table, 88 + index * 8)
            name = names[offset:names.index(b"\0", offset)].decode()
            lines.append(f"{address:08x} {name}")
        return cls("\n".join(lines))

    def contains(self, guest):
        return self.start <= guest < self.end

    def name(self, guest):
        """"name+0x1c" for a guest address, or None outside the code"""
        if not self.contains(guest):
            return None
        index = bisect.bisect_right(self.addresses, guest) - 1
        if index < 0:
            return None
        return f"{self.names[index]}+0x{guest - self.addresses[index]:x}"


def app_executable(app):
    """the executable of an .app folder (an iOS app's at its top, a Mac app's in Contents/MacOS)"""
    for info, folder in ((app / "Info.plist", app), (app / "Contents/Info.plist", app / "Contents/MacOS")):
        if info.is_file():
            return (folder / plistlib.loads(info.read_bytes())["CFBundleExecutable"]).read_bytes()
    raise ValueError(f"{app} has no Info.plist")


def ipa_executable(ipa):
    with zipfile.ZipFile(ipa) as archive:
        names = archive.namelist()
        info = next(name for name in names if re.fullmatch(r"Payload/[^/]+\.app/Info\.plist", name))
        executable = plistlib.loads(archive.read(info))["CFBundleExecutable"]
        return archive.read(info.rsplit("/", 1)[0] + "/" + executable)


def halosyms(data):
    """the bytes of the __TEXT,__halosyms section of a Mach-O executable (its arm64 slice, if universal)"""
    if data[:4] == b"\xca\xfe\xba\xbe":
        count = struct.unpack_from(">I", data, 4)[0]
        for index in range(count):
            cputype, _, offset, size, _ = struct.unpack_from(">IIIII", data, 8 + index * 20)
            if cputype == 0x0100000C:
                return halosyms(data[offset:offset + size])
        raise ValueError("no arm64 slice")
    if data[:4] != b"\xcf\xfa\xed\xfe":
        raise ValueError("not a 64-bit Mach-O executable")
    commands = struct.unpack_from("<I", data, 16)[0]
    offset = 32
    for _ in range(commands):
        command, size = struct.unpack_from("<II", data, offset)
        if command == 0x19:  # LC_SEGMENT_64
            sections = struct.unpack_from("<I", data, offset + 64)[0]
            for index in range(sections):
                base = offset + 72 + index * 80
                name, segment = data[base:base + 16].rstrip(b"\0"), data[base + 16:base + 32].rstrip(b"\0")
                if name == b"__halosyms" and segment == b"__TEXT":
                    length, start = struct.unpack_from("<QI", data, base + 40)
                    return data[start:start + length]
        offset += size
    raise ValueError("the executable has no __TEXT,__halosyms section (a build from before guest crash reports)")


def guest_address(address, symbols, arena=None):
    """the guest address of a native one in the arena's alias of the guest code, else None: a guest
    frame's native address is the arena's start plus its guest address. The arena is above the first
    4 GB; without its start, the low 32 bits are taken (right for an arena at a multiple of 4 GB)"""
    if address < 1 << 32:
        return None
    if arena is None:
        guest = address & 0xFFFFFFFF
    elif arena <= address < arena + (1 << 32):
        guest = address - arena
    else:
        return None
    return guest if symbols.contains(guest) else None


def describe(address, symbols, arena=None):
    guest = guest_address(address, symbols, arena)
    if guest is None:
        return None
    return f"guest 0x{guest:08x} {symbols.name(guest) or '?'}"


def read_ips(text):
    """an .ips report's body: the JSON after its one-line header (or the whole text, for a body alone)"""
    text = text.lstrip()
    first, _, rest = text.partition("\n")
    try:
        header = json.loads(first)
    except json.JSONDecodeError:
        return None
    if rest.strip():
        try:
            return json.loads(rest)
        except json.JSONDecodeError:
            return None
    return header if "threads" in header else None


def register_reason(registers):
    """the reason packed into a crash's registers (a list of x0-x28's values), as "image|text", or None"""
    if len(registers) < 29 or registers[0] != REASON_MAGIC:
        return None
    packed = b"".join(registers[index].to_bytes(8, "little") for index in REASON_REGISTERS)
    return packed.split(b"\0", 1)[0].decode("utf-8", "replace")


def ips_registers(thread):
    state = thread.get("threadState", {})
    return [entry.get("value", 0) for entry in state.get("x", [])]


def text_registers(text):
    """x0-x28 from a .crash report's thread state ("x0: 0x..."), the first thread state it has"""
    found = {}
    for match in REGISTER.finditer(text):
        found.setdefault(int(match.group(1)), int(match.group(2), 16))
    return [found.get(index, 0) for index in range(29)] if 0 in found else []


def reason_lines(reason):
    image, _, message = reason.partition("|")
    return [f"reason: {message}", f"guest image {image}"]


def frame_line(frame, images, symbols, arena=None):
    image = images[frame["imageIndex"]] if 0 <= frame.get("imageIndex", -1) < len(images) else {}
    address = image.get("base", 0) + frame.get("imageOffset", 0)
    named = image.get("name") or image.get("path")
    guest = None if named and image.get("size") else describe(address, symbols, arena)
    if guest:
        return f"0x{address:016x}  {guest}"
    symbol = frame.get("symbol")
    where = f"{named or '???'}" + (f"  {symbol}+{frame.get('symbolLocation', 0)}" if symbol else "")
    return f"0x{address:016x}  {where}"


def threads_of(report):
    return report.get("threads", [])


def ips_report(report, symbols, arena=None):
    """the lines that describe an .ips report, its guest frames named"""
    images = report.get("usedImages", [])
    lines = []
    exception = report.get("exception", {})
    if exception:
        lines.append(f"exception: {exception.get('type', '')} {exception.get('signal', '')} "
                     f"{exception.get('subtype', '')}".rstrip())
    for thread in threads_of(report):
        if thread.get("triggered") and (reason := register_reason(ips_registers(thread))):
            lines += reason_lines(reason)
    asi = report.get("asi") or {}
    for owner, messages in asi.items():
        for message in messages:
            lines.append(f"reason ({owner}): {message}")
    if report.get("lastExceptionBacktrace"):
        lines.append("last exception backtrace:")
        lines += [f"  {index:2d}  {frame_line(frame, images, symbols, arena)}"
                  for index, frame in enumerate(report["lastExceptionBacktrace"])]
    threads = threads_of(report)
    order = sorted(range(len(threads)), key=lambda index: not threads[index].get("triggered"))
    for index in order:
        thread = threads[index]
        title = f"thread {index}" + (" (crashed)" if thread.get("triggered") else "")
        if thread.get("name") or thread.get("queue"):
            title += f": {thread.get('name') or thread.get('queue')}"
        lines.append(title)
        lines += [f"  {number:2d}  {frame_line(frame, images, symbols, arena)}"
                  for number, frame in enumerate(thread.get("frames", []))]
    return lines


def text_report(text, symbols, arena=None):
    """text (a .crash report, ios-runtime.log) with a guest name after each address that is one"""
    if arena is None and (match := ARENA.search(text)):
        arena = int(match.group(1), 16)
    reason = register_reason(text_registers(text))
    lines = reason_lines(reason) if reason else []
    for line in text.splitlines():
        names = [describe(int(match.group(1), 16), symbols, arena) for match in ADDRESS.finditer(line)]
        names = [name for name in names if name and name.split()[1] not in line]
        lines.append(line + (f"    [{'; '.join(names)}]" if names else ""))
    return lines


def image_warning(text, symbols, lines):
    """a warning when the report names a guest image other than the symbols' own, else None"""
    named = set(IMAGE_NAMED.findall(text + "\n" + "\n".join(lines)))
    if symbols.sha256 and named and not any(symbols.sha256.startswith(name) for name in named):
        return (f"warning: the report names guest image {', '.join(sorted(named))}, but the symbols are for "
                f"{symbols.sha256[:12]}: use that build's guest-symbols.txt")
    return None


def symbolicate(text, symbols, arena=None):
    report = read_ips(text)
    lines = ips_report(report, symbols, arena) if report else text_report(text, symbols, arena)
    warning = image_warning(text, symbols, lines)
    return ([warning] if warning else []) + lines


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--symbols", type=Path, required=True,
                        help="the build's guest-symbols.txt (build/ios/embedded/guest-symbols.txt)")
    parser.add_argument("--arena", type=lambda text: int(text, 16),
                        help="the guest arena's start in hex (ios-runtime.log's \"guest arena at\" line); "
                             "ios-runtime.log's own CRASH lines give it")
    parser.add_argument("reports", type=Path, nargs="+", help=".ips or .crash reports, or ios-runtime.log")
    args = parser.parse_args(arguments)
    symbols = Symbols.load(args.symbols)
    for path in args.reports:
        if len(args.reports) > 1:
            print(f"== {path}")
        for line in symbolicate(path.read_text(errors="replace"), symbols, args.arena):
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
