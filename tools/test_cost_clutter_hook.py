"""A cost run's clutter settings reach the renderer through the benchmark's getters, ahead of the configuration."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_clutter_reads_prefer_the_benchmark_run():
    source = (ROOT / "source/render/render_objects.c").read_text()
    assert "double halo_benchmark_clutter_distance(void);" in source
    assert "int halo_benchmark_clutter_lowest_lod(void);" in source
    lod = source.index("clutter_lowest_lod = halo_benchmark_clutter_lowest_lod() >= 0")
    assert source.index('config_boolean("debug.clutter_lowest_lod")', lod) - lod < 200
    distance = source.index("halo_benchmark_clutter_distance() >= 0.0 ?")
    assert source.index('config_real("debug.clutter_distance")', distance) - distance < 200
    # the configured distance is still the fallback
    assert source.index('config_real("display.clutter_distance")', distance) > distance
