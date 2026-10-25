"""The guest lifter adds guest pointers to x27 and turns sp and x29 values into guest pointers."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from guest_asm_convert import ConvertError  # noqa: E402
from ios_asm_convert import lift  # noqa: E402


def lifted(*lines):
    return [line.strip() for line in lift("\n".join(f"\t{line}" for line in lines)).splitlines()]


def test_guest_access_adds_the_pointer_to_the_arena():
    assert lifted("ldr w0, [x8, #4]") == ["add x15, x27, w8, uxtw", "ldr w0, [x15, #4]"]
    assert lifted("str w1, [x2], #8") == ["add x15, x27, w2, uxtw", "str w1, [x15], #8", "add x2, x2, #8"]
    assert lifted("blr x9") == ["add x15, x27, w9, uxtw", "blr x15"]


def test_pc_relative_and_native_values_become_guest_pointers():
    assert lifted("adrp x8, table") == ["adrp x8, table", "sub x8, x8, x27"]
    assert lifted("add x0, sp, #16") == ["sub x15, sp, x27", "add x0, x15, #16"]
    assert lifted("mov x1, sp") == ["sub x15, sp, x27", "mov x1, x15"]
    assert lifted("sub x2, x29, #8") == ["sub x15, x29, x27", "sub x2, x15, #8"]
    assert lifted("tst w29, #3") == ["sub x15, x29, x27", "tst w15, #3"]


def test_stack_and_frame_stay_native():
    for line in ("ldr w0, [sp, #8]", "stur w1, [x29, #-4]", "stp x29, x30, [sp, #-16]!", "add x29, sp, #16",
                 "sub sp, sp, #32", "sub sp, x29, #16", "mov x29, sp"):
        assert lifted(line) == [line]
    assert lifted("mov sp, x9") == ["add x15, x27, w9, uxtw", "mov sp, x15"]
    assert lifted("str w29, [x9, #8]") == ["sub x29, x29, x27", "add x15, x27, w9, uxtw", "str w29, [x15, #8]",
                                           "add x29, x29, x27"]


@pytest.mark.parametrize("line", ["mov x29, x0", "and sp, x9, #-16", "ldr w29, [x8]", "str x29, [x8]",
                                  "ldr w0, [x8, x29]", "mov w15, w0", "sub x0, x29, sp"])
def test_unknown_native_uses_are_rejected(line):
    with pytest.raises(ConvertError):
        lift(f"\t{line}")
