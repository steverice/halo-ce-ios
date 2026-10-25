"""The scale rule of port/assets/texture-policy.json against the shipping set's scales
(port/assets/texture-selection-lock.tsv, from tools/texture_selection_lock.py)."""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / "port/assets/texture-selection-lock.tsv"


def test_the_policy_gives_every_texture_the_shipping_sets_scale(tmp_path):
    table = tmp_path / "texture_policy_table.c"
    subprocess.run([sys.executable, str(ROOT / "tools/embed_texture_policy.py"),
                    str(ROOT / "port/assets/texture-policy.json"), str(table)], check=True)
    probe = tmp_path / "texture-selection-probe"
    subprocess.run(["cc", "-O1", "-Wall", "-Werror", "-I", str(ROOT / "port/runtime"), "-I", str(ROOT / "port/linux/src"),
                    str(ROOT / "port/ios/tests/texture_selection_probe.c"), str(ROOT / "port/runtime/texture_policy.c"),
                    str(table), "-lz", "-o", str(probe)], check=True)
    result = subprocess.run([str(probe), str(LOCK)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "861 textures: 2x 706, 4x 129, 8x 26; 0 differ from the set"


def test_the_lock_matches_its_header():
    lines = LOCK.read_text().splitlines()
    assert lines[0].startswith("# 861 textures of policy-game-selective-round3, by scale: 2x 706, 4x 129, 8x 26")
    assert lines[1].split("\t") == ["hash", "width", "height", "kind", "treatment", "scale", "tags"]
    assert len(lines) == 2 + 861
