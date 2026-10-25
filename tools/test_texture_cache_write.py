"""tools/texture_cache_write.py's records against the app's reader checks (port/linux/src/texture_record.h)."""
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import texture_cache_write as writer  # noqa: E402

CHECK = r'''
#include <stdio.h>
#include <stdlib.h>
#include "texture_record.h"
int main(int argc, char **argv)
{
	static unsigned char data[1 << 20];
	struct texture_record_layout layout;
	FILE *file = fopen(argv[1], "rb");
	size_t size = fread(data, 1, sizeof(data), file);
	const char *problem = texture_record_check(data, size, size, strtoull(argv[2], NULL, 16),
		(uint32_t)atoi(argv[3]), (uint32_t)atoi(argv[4]), (uint32_t)atoi(argv[5]), &layout);
	if (!problem)
		problem = texture_record_check_payload(&layout, data + layout.header_size);
	printf("%s %u %u %u %llx\n", problem ? problem : "ok", layout.width, layout.height, layout.levels,
		(unsigned long long)layout.recipe_key);
	return problem != NULL;
}
'''


def levels_of(width, height):
    sizes = []
    while True:
        sizes.append(writer.level_bytes(width, height))
        if width == 1 and height == 1:
            return sizes
        width, height = max(width // 2, 1), max(height // 2, 1)


def checker(tmp_path):
    source = tmp_path / "check.c"
    source.write_text(CHECK)
    binary = tmp_path / "check"
    subprocess.run(["cc", "-Wall", "-Werror", "-I", str(ROOT / "port/linux/src"), str(source), "-o", str(binary)],
                   check=True)
    return binary


def test_a_written_record_passes_the_readers_checks(tmp_path):
    binary = checker(tmp_path)
    levels = [bytes([index + 1]) * size for index, size in enumerate(levels_of(48, 24))]
    data = writer.record_bytes(0x1234, 12, 6, 48, 24, "bc7", 0xfeed, levels)
    path = tmp_path / "0000000000001234-12x6.bin"
    path.write_bytes(data)
    done = subprocess.run([str(binary), str(path), "1234", "12", "6", "1"], capture_output=True, text=True)
    assert done.stdout.split() == ["ok", "48", "24", "6", "feed"]
    # the other format, a stray byte and another texture are refused
    assert "ASTC" in subprocess.run([str(binary), str(path), "1234", "12", "6", "2"], capture_output=True,
                                    text=True).stdout
    path.write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
    assert "damaged" in subprocess.run([str(binary), str(path), "1234", "12", "6", "1"], capture_output=True,
                                       text=True).stdout
    path.write_bytes(data)
    assert "another texture" in subprocess.run([str(binary), str(path), "1235", "12", "6", "1"],
                                               capture_output=True, text=True).stdout


def test_the_checksum_matches_the_c_definition():
    words = struct.pack("<2Q", 1, 2)
    assert writer.checksum(words) == 3 ^ (5 << 32)
    big = struct.pack("<2Q", 2**64 - 1, 2**64 - 1)
    a, b = (2 * (2**64 - 1)) % 2**64, (3 * (2**64 - 1)) % 2**64
    assert writer.checksum(big) == a ^ (((b << 32) | (b >> 32)) & (2**64 - 1))


def test_a_level_of_the_wrong_size_is_refused():
    try:
        writer.record_bytes(1, 4, 4, 8, 8, "astc4x4", 0, [b"\0" * 16])
    except ValueError as error:
        assert "level 0" in str(error)
    else:
        raise AssertionError("expected a refusal")
