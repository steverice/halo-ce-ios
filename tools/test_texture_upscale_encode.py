#!/usr/bin/env python3
"""Check the device generator's compression gates and cache record publication."""
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/ios/probe/texture-upscale-encode"


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    cc = ["xcrun", "clang"] if os.uname().sysname == "Darwin" else ["cc"]
    cxx = ["xcrun", "clang++"] if os.uname().sysname == "Darwin" else ["c++"]
    flags = ["-O2", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
    includes = [f"--include-directory={ROOT / path}" for path in (
        "port/linux/src", "port/runtime", "port/ios/host", "port/third_party/bc7enc", "port/third_party/bc7enc16", "port/third_party/astcenc")]
    objects = []
    for source in ("port/linux/src/astc_rewrite.c", "port/runtime/texture_cache.c", "port/linux/src/texture_override_cache.c",
                   "port/ios/tests/texture_upscale_reader_probe.c",
                   "port/third_party/bc7enc16/bc7decomp.c"):
        output = BUILD / (Path(source).stem + ".o")
        subprocess.run([*cc, *flags, *includes, "-c", source, "--output", str(output)], cwd=ROOT, check=True)
        objects.append(str(output))
    for source, extra in (("port/ios/tests/texture_upscale_generate_probe.c", ["-Dmain=texture_upscale_generate_probe_main"]),
                          ("port/ios/host/texture_upscale_generate.m", ["-x", "c"])):
        output = BUILD / (Path(source).stem + ".o")
        subprocess.run([*cc, *flags, *includes, *extra, "-c", source, "--output", str(output)], cwd=ROOT, check=True)
        objects.append(str(output))
    vendor = ROOT / "port/third_party/astcenc"
    subprocess.run([*cxx, *flags, "-std=c++14", "-pthread", "-DASTCENC_BLOCK_MAX_TEXELS=16", *includes,
                    "port/ios/tests/texture_upscale_encode_probe.cpp", "port/ios/host/texture_upscale_encode.cpp",
                    "port/linux/src/astc_encode.cpp", "port/third_party/bc7enc/bc7enc.cpp", *map(str, sorted(vendor.glob("*.cpp"))), *objects,
                    "--output", str(BUILD / "probe")], cwd=ROOT, check=True)
    subprocess.run([str(BUILD / "probe")], cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
