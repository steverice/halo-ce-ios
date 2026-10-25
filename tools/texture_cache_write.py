#!/usr/bin/env python3
"""Fill the app's upscaled-texture cache (texture-cache/ under the data root) from a set made on a Mac, for testing
the cache reader before the app makes the records itself.

  uv run --no-project --with numpy --with etcpak --with astc-encoder-py --with pillow \\
      tools/texture_cache_write.py SET_DIR CLASSIFIED_MANIFEST OUTPUT_DIR --format bc7 [--map a10] [--psnr]

SET_DIR holds <hash>.rgba files (port/linux/src/texture_override.h: each a complete RGBA8 mip chain, the format
debug.texture_override_directory reads). CLASSIFIED_MANIFEST is the manifest.json beside the set it was composed
from, which gives each texture's original size, result, treatment and maps. Each texture becomes one record,
OUTPUT_DIR/<hash>-<width>x<height>.bin (port/linux/src/texture_record.h): every level of the set's file encoded
as BC7 (the GPUs with BC formats) or ASTC 4x4 LDR at astc-encoder's medium preset (the others), both linear. Then
OUTPUT_DIR/manifest.tsv lists them in port/runtime/texture_cache.c's format. Files go through a temporary name and
a rename. The encoders are MIT-licensed Python packages (etcpak's BC7 is bc7enc; astc-encoder-py binds Arm's
astc-encoder) and are not part of the app.

The recipe key a record carries says it came from this tool and which source file: FNV-1a 64 of
"mac-set;source=<the .rgba file's SHA-256>;encoder=<bc7-v1 or astc4x4-medium-v1>". The app's own generator will key
its records with texture_recipe_key instead. --psnr decodes every level 0 again and prints the RGB PSNR over the
texels whose alpha is not 0, the compression gate of the design (38 dB)."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import time
from pathlib import Path

MAGIC = b"VUTR"
VERSION = 1
FIXED = 64
FORMATS = {"bc7": 1, "astc4x4": 2}
ENCODERS = {"bc7": "bc7-v1", "astc4x4": "astc4x4-medium-v1"}
RESULTS = {"s4g": 0, "bpf": 1, "original": 2}                                        # enum texture_policy_result
TREATMENTS = ["skip", "color", "first-person", "emissive", "multipurpose", "bump"]    # enum texture_policy_treatment


def fnv1a64(text: str) -> int:
    value = 0xcbf29ce484222325
    for byte in text.encode():
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return value


def level_bytes(width: int, height: int) -> int:
    return ((width + 3) // 4) * ((height + 3) // 4) * 16


def checksum(payload: bytes) -> int:
    """texture_record_checksum: a = sum of the little-endian 64-bit words, b = sum of (index + 1) * word, mod 2^64"""
    mask = 0xffffffffffffffff
    try:
        import numpy as np
    except ImportError:   # (the test without numpy: small payloads)
        a = b = 0
        for index, (word,) in enumerate(struct.iter_unpack("<Q", payload), 1):
            a, b = (a + word) & mask, (b + index * word) & mask
    else:
        words = np.frombuffer(payload, dtype="<u8")
        with np.errstate(over="ignore"):
            a = int(words.sum(dtype=np.uint64))
            b = int((words * np.arange(1, len(words) + 1, dtype=np.uint64)).sum(dtype=np.uint64))
    return (a ^ (((b << 32) | (b >> 32)) & mask)) & mask


def record_bytes(hash_: int, source_width: int, source_height: int, width: int, height: int, fmt: str,
                 recipe: int, levels: list[bytes]) -> bytes:
    """a record (texture_record.h) of these encoded levels, largest first"""
    sizes, w, h = [], width, height
    for index, level in enumerate(levels):
        if len(level) != level_bytes(w, h):
            raise ValueError(f"level {index} is {len(level)} bytes, not {level_bytes(w, h)} for {w}x{h}")
        sizes.append(len(level))
        w, h = max(w // 2, 1), max(h // 2, 1)
    payload = b"".join(levels)
    header_size = FIXED + 16 * len(levels)
    header = MAGIC + struct.pack("<7I3Q2I", VERSION, FORMATS[fmt], source_width, source_height, width, height,
                                 len(levels), hash_, recipe, checksum(payload), header_size, 0)
    offset, table = header_size, b""
    for size in sizes:
        table += struct.pack("<2Q", offset, size)
        offset += size
    return header + table + payload


def read_rgba(path: Path):
    """the levels of a <hash>.rgba file (texture_override.h) as (height, width, 4) arrays"""
    import numpy as np
    data = path.read_bytes()
    width, height, count = struct.unpack_from("<3I", data)
    levels, offset, w, h = [], 12, width, height
    for _ in range(count):
        size = w * h * 4
        levels.append(np.frombuffer(data, dtype=np.uint8, count=size, offset=offset).reshape(h, w, 4))
        offset += size
        w, h = max(w // 2, 1), max(h // 2, 1)
    if offset != len(data):
        raise ValueError(f"{path}: {len(data)} bytes, not the {offset} its levels add up to")
    return width, height, levels


class Encoder:
    def __init__(self, fmt: str, threads: int):
        self.fmt = fmt
        if fmt == "bc7":
            import etcpak
            self.etcpak = etcpak
        else:
            import astc_encoder as astc
            self.astc = astc
            config = astc.ASTCConfig(astc.ASTCProfile.LDR, 4, 4, 1, astc.ASTCQualityPreset.MEDIUM)
            self.context = astc.ASTCContext(config, threads=threads)
            self.swizzle = astc.ASTCSwizzle()

    def encode(self, level) -> bytes:
        import numpy as np
        height, width = level.shape[:2]
        if self.fmt == "astc4x4":
            image = self.astc.ASTCImage(self.astc.ASTCType.U8, width, height, 1, np.ascontiguousarray(level).tobytes())
            return self.context.compress(image, self.swizzle)
        # bc7enc wants whole blocks: a level under 4 texels or not a multiple of 4 repeats its edge
        padded = np.pad(level, ((0, -height % 4), (0, -width % 4), (0, 0)), mode="edge")
        return self.etcpak.compress_bc7(np.ascontiguousarray(padded).tobytes(), padded.shape[1], padded.shape[0], None)

    def decode(self, blocks: bytes, width: int, height: int):
        """level 0 again, RGBA, for --psnr"""
        import numpy as np
        if self.fmt == "astc4x4":
            image = self.context.decompress(blocks, self.astc.ASTCImage(self.astc.ASTCType.U8, width, height, 1),
                                            self.swizzle)
            return np.frombuffer(image.data, dtype=np.uint8).reshape(height, width, 4)
        import io
        from PIL import Image
        w4, h4 = width + (-width % 4), height + (-height % 4)
        dds = struct.pack("<4s7I44s8I5I", b"DDS ", 124, 0x81007, h4, w4, len(blocks), 0, 1, b"\0" * 44,
                          32, 0x4, int.from_bytes(b"DX10", "little"), 0, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0)
        dds += struct.pack("<5I", 98, 3, 0, 1, 0)   # DXGI_FORMAT_BC7_UNORM, a 2D texture
        image = Image.open(io.BytesIO(dds + blocks))
        return np.array(image.convert("RGBA"))[:height, :width]


def psnr(original, decoded) -> float:
    import numpy as np
    mask = original[..., 3] != 0
    if not mask.any():
        return float("inf")
    error = (original[..., :3].astype(np.float64) - decoded[..., :3].astype(np.float64))[mask]
    mse = float((error ** 2).mean())
    return float("inf") if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)


def write_atomically(path: Path, data: bytes) -> None:
    temporary = path.with_name(path.name + ".tmp")
    with open(temporary, "wb") as file:
        file.write(data)
        file.flush()
        os.fsync(file.fileno())
    temporary.replace(path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("set_dir", type=Path)
    parser.add_argument("classified", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--format", choices=sorted(FORMATS), required=True)
    parser.add_argument("--map", action="append", help="only the textures these maps use (repeat for more)")
    parser.add_argument("--psnr", action="store_true", help="decode level 0 again and print its RGB PSNR")
    parser.add_argument("--threads", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()

    classified = json.loads(args.classified.read_text())["textures"]
    keys = sorted(path.stem for path in args.set_dir.glob("*.rgba"))
    if args.map:
        keys = [key for key in keys if set(classified[key].get("maps", ())) & set(args.map)]
    if not keys:
        raise SystemExit("no textures to write")
    args.output.mkdir(parents=True, exist_ok=True)
    encoder = Encoder(args.format, args.threads)
    global_key = fnv1a64(f"mac-set;encoder={ENCODERS[args.format]}")
    rows, scores, raw_bytes, record_total, began = [], [], 0, 0, time.time()
    for number, key in enumerate(keys, 1):
        row = classified[key]
        path = args.set_dir / f"{key}.rgba"
        width, height, levels = read_rgba(path)
        encoded = [encoder.encode(level) for level in levels]
        recipe = fnv1a64(f"mac-set;source={hashlib.sha256(path.read_bytes()).hexdigest()};"
                         f"encoder={ENCODERS[args.format]}")
        data = record_bytes(int(key, 16), row["width"], row["height"], width, height, args.format, recipe, encoded)
        write_atomically(args.output / f"{key}-{row['width']}x{row['height']}.bin", data)
        rows.append(f"{key}\t{row['width']}\t{row['height']}\t{RESULTS[row.get('final', row['result'])]}\t"
                    f"{TREATMENTS.index(row['treatment'])}\t{recipe:016x}\t{global_key:016x}\t{len(data)}")
        raw_bytes += path.stat().st_size
        record_total += len(data)
        if args.psnr:
            scores.append((psnr(levels[0], encoder.decode(encoded[0], width, height)), key))
        if number % 50 == 0 or number == len(keys):
            print(f"{number}/{len(keys)} textures, {record_total / 2**20:.1f} MiB of records from "
                  f"{raw_bytes / 2**20:.1f} MiB of RGBA8, {time.time() - began:.0f} s", flush=True)
    write_atomically(args.output / "manifest.tsv", ("\n".join(rows) + "\n").encode())
    print(f"wrote {len(rows)} {args.format} records and manifest.tsv to {args.output}: "
          f"{record_total / 2**20:.1f} MiB, {record_total / raw_bytes:.3f} of the set's {raw_bytes / 2**20:.1f} MiB")
    if scores:
        scores.sort()
        finite = [score for score, _ in scores if score != float("inf")]
        median = finite[len(finite) // 2] if finite else float("inf")
        below = [f"{key} {score:.1f}" for score, key in scores if score < 38]
        print(f"level 0 RGB PSNR over alpha > 0: lowest {scores[0][0]:.1f} dB ({scores[0][1]}), median {median:.1f} dB; "
              f"{len(below)} under 38 dB" + (f": {', '.join(below[:20])}" if below else ""))


if __name__ == "__main__":
    sys.exit(main())
