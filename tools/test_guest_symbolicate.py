"""Tests for tools/guest_symbolicate.py and the symbol table tools/ios_embed_guest.py embeds."""

import json
import struct

from tools import guest_symbolicate, ios_embed_guest
from tools.test_ios_embed_guest import guest_elf

ARENA = 0x3_0000_0000
SHA = "ab" * 32


def elf_with_symbols(symbols):
    """guest_elf's image with a .text section over its code and a symbol table naming symbols:
    (name, address, info) triples"""
    image = bytearray(guest_elf(code=b"\x1f\x20\x03\xd5" * 64))
    strings = b"\0" + b"".join(name.encode() + b"\0" for name, _, _ in symbols)
    table = b"\0" * 24
    offset = 1
    for name, address, info in symbols:
        table += struct.pack("<IBBHQQ", offset, info, 0, 1, address, 0)
        offset += len(name) + 1
    shstrings = b"\0.text\0.symtab\0.strtab\0"
    blobs = {}
    for name, blob in (("symtab", table), ("strtab", strings), ("shstrtab", shstrings)):
        blobs[name] = len(image)
        image += blob
    sections_at = len(image)
    headers = [struct.pack("<IIQQQQIIQQ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
               struct.pack("<IIQQQQIIQQ", 1, 1, 6, 0x88000000, 0x1000, 256, 0, 0, 4, 0),
               struct.pack("<IIQQQQIIQQ", 7, 2, 0, 0, blobs["symtab"], len(table), 3, 1, 8, 24),
               struct.pack("<IIQQQQIIQQ", 15, 3, 0, 0, blobs["strtab"], len(strings), 0, 0, 1, 0),
               struct.pack("<IIQQQQIIQQ", 0, 3, 0, 0, blobs["shstrtab"], len(shstrings), 0, 0, 1, 0)]
    image += b"".join(headers)
    struct.pack_into("<Q", image, 40, sections_at)
    struct.pack_into("<HHH", image, 58, 64, len(headers), 4)
    return bytes(image)


def symbols():
    return guest_symbolicate.Symbols(
        f"# VISR guest symbols: guest image sha256 {SHA}\n"
        "# address name (guest addresses; code from 0x88000030 to 0x88100000)\n"
        "88000030 tolower\n88001000 display_assert\n88002000 halt_and_catch_fire\n")


def test_embedding_writes_the_functions_in_both_forms(tmp_path):
    image = elf_with_symbols([("first", 0x88000000, 0x12), ("$x", 0x88000000, 0),
                              ("second", 0x88000040, 0x02), ("second_alias", 0x88000040, 0x12),
                              (".Ltmp", 0x88000080, 0), ("data", 0x88000090, 0x11)])
    (tmp_path / "guest.elf").write_bytes(image)
    ios_embed_guest.embed(tmp_path / "guest.elf", tmp_path / "out")
    text = guest_symbolicate.Symbols.load(tmp_path / "out/guest-symbols.txt")
    table = guest_symbolicate.Symbols.from_table((tmp_path / "out/guest-symbols.bin").read_bytes())
    assert text.names == table.names == ["first", "second_alias"]
    assert text.addresses == table.addresses == [0x88000000, 0x88000040]
    assert (text.start, text.end) == (table.start, table.end) == (0x88000000, 0x88000100)
    assert text.sha256 == table.sha256
    assert "_halo_guest_symbols" in (tmp_path / "out/guest_image.S").read_text()


def test_guest_addresses_are_the_arena_alias_s_low_32_bits():
    table = symbols()
    assert table.name(0x88002010) == "halt_and_catch_fire+0x10"
    assert guest_symbolicate.describe(ARENA + 0x88001004, table) == "guest 0x88001004 display_assert+0x4"
    # a host address, and one below the first 4 GB, are not guest frames
    assert guest_symbolicate.describe(ARENA + 0x00001004, table) is None
    assert guest_symbolicate.describe(0x88001004, table) is None


def test_an_ips_report_names_its_guest_frames_and_shows_its_reason():
    body = {
        "exception": {"type": "EXC_CRASH", "signal": "SIGABRT"},
        "asi": {"CoreFoundation": ["*** Terminating app due to uncaught exception 'VISRGuestHalt', reason: "
                                   "'EXCEPTION halt in units.c,#12: x [guest: display_assert+0x4] "
                                   f"(guest image {SHA[:12]})'"]},
        "usedImages": [{"name": "VISR", "base": 0x1_0000_0000, "size": 0x100000}, {"size": 0, "base": 0}],
        "lastExceptionBacktrace": [{"imageIndex": 1, "imageOffset": ARENA + 0x88002008}],
        "threads": [{"frames": [{"imageIndex": 0, "imageOffset": 0x10, "symbol": "main", "symbolLocation": 4}]},
                    {"triggered": True, "frames": [{"imageIndex": 1, "imageOffset": ARENA + 0x88001010},
                                                   {"imageIndex": 0, "imageOffset": 0x20}]}],
    }
    report = json.dumps({"app_name": "VISR"}) + "\n" + json.dumps(body)
    lines = guest_symbolicate.symbolicate(report, symbols())
    assert lines[0] == "exception: EXC_CRASH SIGABRT"
    assert lines[1].startswith("reason (CoreFoundation): ")
    assert "guest 0x88002008 halt_and_catch_fire+0x8" in lines[3]
    crashed = lines.index("thread 1 (crashed)")
    assert crashed < lines.index("thread 0")
    assert "guest 0x88001010 display_assert+0x10" in lines[crashed + 1]
    assert "VISR" in lines[crashed + 2]
    assert not any(line.startswith("warning") for line in lines)


def test_text_gets_names_after_its_guest_addresses_and_a_wrong_build_is_named():
    log = (f"CRASH: frame 3: 0x{ARENA + 0x88001020:016x}\n"
           f"CRASH: frame 4: 0x{ARENA + 0x88002000:016x} guest 0x88002000 halt_and_catch_fire+0x0\n"
           "the reason (guest image 0123456789ab)\n")
    lines = guest_symbolicate.symbolicate(log, symbols())
    assert lines[0].startswith("warning: the report names guest image 0123456789ab")
    assert lines[1].endswith("[guest 0x88001020 display_assert+0x20]")
    assert lines[2].endswith("halt_and_catch_fire+0x0")
