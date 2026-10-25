#!/usr/bin/env python3
"""Lift ILP32 guest accesses into the iOS arena, at any 16 KB-aligned base.

The compiler reserves x15 (scratch) and x27 (arena base). Disk pointers and
structure layouts remain 32-bit. A guest pointer is an offset from x27: memory
accesses and indirect branches add it to x27 (`add x15, x27, wN, uxtw`), so the
arena no longer has to start at a multiple of 4 GB. Direct branches and
PC-relative references keep their offsets. The host enters with x27 set, and
preserves it across every host call.

Two registers hold native addresses rather than guest pointers: sp and the
frame pointer x29 (the guest is built with frame pointers). Accesses based on
them stay native. Wherever their value flows into a general register, a
comparison or a 32-bit store, it is first turned into a guest pointer by
subtracting x27; a guest pointer moved into sp gets x27 added back. Any other
use of them is rejected, so a new compiler pattern fails the build instead of
running with a wrong address.
"""

import re
import sys
from pathlib import Path

from guest_asm_convert import Converter, ConvertError

NATIVE = re.compile(r"\b(sp|wsp|x29|w29)\b")
# instructions whose first operand is read, not written
NO_DESTINATION = {"cmp", "cmn", "tst", "ccmp", "ccmn", "cbz", "cbnz", "tbz", "tbnz"}
STORE = re.compile(r"st[a-z0-9]*$")


def guest_value(register: str) -> str:
    """x15 = the guest pointer for a native register's value (sp or x29)."""
    source = "sp" if register in ("sp", "wsp") else "x29"
    return f"\tsub x15, {source}, x27"


def scratch(register: str) -> str:
    """the x15 operand that stands in for a native register, at its width"""
    return "w15" if register in ("wsp", "w29") else "x15"


def lift_native_operands(number: int, line: str, text: str, op: str, args: str) -> list:
    """A non-memory instruction that names sp or x29."""
    operands = [part.strip() for part in args.split(",")]
    first = operands[0] if operands else ""
    if op not in NO_DESTINATION and NATIVE.fullmatch(first):
        sources = operands[1:]
        # Frame arithmetic: native plus or minus a size stays native.
        if op in ("add", "sub", "mov") and sources and NATIVE.fullmatch(sources[0]):
            return [line]
        # A guest pointer becomes the stack (alloca restoring sp).
        if op == "mov" and first == "sp" and len(sources) == 1 and re.fullmatch(r"x\d+", sources[0]):
            return [f"\tadd x15, x27, w{sources[0][1:]}, uxtw", "\tmov sp, x15"]
        raise ConvertError(f"line {number}: unsupported write to a native register: {text}")
    natives = {match.group(1) for match in NATIVE.finditer(args)}
    if len({"sp" if name in ("sp", "wsp") else "x29" for name in natives}) > 1:
        raise ConvertError(f"line {number}: both sp and x29 read: {text}")
    register = next(iter(natives))
    rewritten = NATIVE.sub(lambda match: scratch(match.group(1)), args)
    return [guest_value(register), f"\t{op} {rewritten}"]


def lift(assembly: str) -> str:
    """Rewrite ELF assembly, rejecting unsupported addressing forms."""
    output = []
    for number, line in enumerate(assembly.splitlines(), 1):
        text = line.strip()
        if not text or text.startswith(".") or text.endswith(":"):
            output.append(line)
            continue
        if re.search(r"\b[wx](15|27)\b", text) and not (re.match(r"(?:stp|ldp) .*\[sp", text) and "x15" not in text):
            raise ConvertError(f"line {number}: reserved register used: {text}")
        op, _, args = text.partition(" ")
        if op in ("adr", "adrp"):
            # The ILP32 backend assumes symbol addresses are zero-extended
            # guest pointers. Actual PC-relative addresses are native.
            register = args.split(",", 1)[0].strip()
            output.extend([line, f"\tsub {register}, {register}, x27"])
            continue
        if op in ("br", "blr"):
            if not re.fullmatch(r"x\d+", args.strip()) or NATIVE.search(args):
                raise ConvertError(f"line {number}: unsupported branch: {text}")
            output.extend([f"\tadd x15, x27, w{args.strip()[1:]}, uxtw", f"\t{op} x15"])
            continue
        match = re.search(r"\[(x\d+|sp)([^\]]*)\](!?)(.*)$", args)
        if not match:
            if NATIVE.search(args) and op not in ("ret",):
                output.extend(lift_native_operands(number, line, text, op, args))
            else:
                output.append(line)
            continue
        base, offset, pre, tail = match.groups()
        data = args[:match.start()]
        if NATIVE.search(offset) or NATIVE.search(tail):
            raise ConvertError(f"line {number}: native register as an index: {text}")
        # A 32-bit store of the frame pointer stores it as a guest pointer.
        wrap_frame = False
        if re.search(r"\bw29\b", data):
            if not STORE.match(op):
                raise ConvertError(f"line {number}: load into the frame pointer: {text}")
            if base == "x29":
                raise ConvertError(f"line {number}: frame pointer stored through itself: {text}")
            wrap_frame = True
        elif re.search(r"\bx29\b", data) and base != "sp":
            raise ConvertError(f"line {number}: frame pointer saved outside the stack: {text}")
        if wrap_frame:
            output.append("\tsub x29, x29, x27")
        if base in ("sp", "x29"):
            output.append(line)
        else:
            # Clang emits immediate pre/post increments. Preserve the original
            # register's representation and the instruction's flag behavior.
            update = None
            if pre:
                increment = offset.lstrip(", ")
                if not re.fullmatch(r"#-?\d+", increment):
                    raise ConvertError(f"line {number}: unsupported pre-index: {text}")
                update = increment
            elif tail.strip():
                increment = tail.lstrip(", ")
                if not re.fullmatch(r"#-?\d+", increment):
                    raise ConvertError(f"line {number}: unsupported post-index: {text}")
                update = increment
            output.append(f"\tadd x15, x27, w{base[1:]}, uxtw")
            rewritten = args[:match.start()] + "[x15" + offset + "]" + pre + tail
            output.append(f"\t{op} {rewritten}")
            if update:
                value = int(update[1:])
                output.append(f"\t{'sub' if value < 0 else 'add'} {base}, {base}, #{abs(value)}")
        if wrap_frame:
            output.append("\tadd x29, x29, x27")
    return "\n".join(output) + "\n"


def main():
    """Convert the compiler's Darwin assembly and lift guest addressing."""
    source, destination = map(Path, sys.argv[1:])
    destination.write_text(lift(Converter(source.read_text().splitlines()).run()))


if __name__ == "__main__":
    main()
