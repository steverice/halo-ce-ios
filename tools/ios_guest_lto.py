#!/usr/bin/env python3
"""Link-time optimization for the iOS guest (configure.py --ios --lto=full).

The guest cannot leave link-time optimization to the linker: its code is
compiled to Darwin assembly and rewritten (tools/ios_asm_convert.py) before
it is assembled, so the optimized code has to come out as assembly. This
does the linker's part by hand:

1. llvm-link merges the units' bitcode (clang -flto=full -c) into one module;
2. opt makes every symbol the rest of the guest does not refer to internal
   (the names in --keep: the undefined symbols of the units that stay
   outside, which are the C library, the guest runtime and the import
   table) and runs LLVM's link-time pipeline over the module;
3. clang generates the module's code with the units' own code generation
   flags (--cflags), without optimizing it again.

The Darwin assembly it writes is then converted and assembled as any unit's.

Usage: ios_guest_lto.py --llvm-bin DIR --keep FILE --cflags FLAGS --work DIR
                        --out merged.darwin.s @bitcode.rsp
"""
import argparse
import shlex
import subprocess
import sys
import time
from pathlib import Path


def run(*command: object) -> None:
    start = time.time()
    result = subprocess.run([str(word) for word in command])
    print(f"ios_guest_lto: {Path(str(command[0])).name} took {time.time() - start:.0f} s", file=sys.stderr, flush=True)
    if result.returncode:
        sys.exit(result.returncode)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", type=Path, required=True)
    parser.add_argument("--keep", type=Path, required=True, help="the symbols to leave visible, one per line")
    parser.add_argument("--cflags", required=True, help="the units' code generation flags")
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("bitcode", nargs="+", help="bitcode files, or @FILE listing them (shell quoting)")
    args = parser.parse_args()
    bitcode = []
    for name in args.bitcode:
        bitcode += shlex.split(Path(name[1:]).read_text()) if name.startswith("@") else [name]
    args.work.mkdir(parents=True, exist_ok=True)
    merged = args.work / "merged.bc"
    optimized = args.work / "optimized.bc"
    inputs = args.work / "inputs.rsp"
    inputs.write_text("\n".join(shlex.quote(name) for name in bitcode) + "\n")
    run(args.llvm_bin / "llvm-link", "-o", merged, f"@{inputs}")
    run(args.llvm_bin / "opt", "-passes=internalize,lto<O2>", f"-internalize-public-api-file={args.keep}",
        merged, "-o", optimized)
    run(args.llvm_bin / "clang", *shlex.split(args.cflags), "-Xclang", "-disable-llvm-passes",
        "-Wno-override-module", "-S", "-x", "ir", optimized, "-o", args.out)


if __name__ == "__main__":
    main()
