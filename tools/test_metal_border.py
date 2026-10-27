"""Exercise production Metal sampler construction/binding without submitting GPU work."""
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "port/ios/host/gpu_metal.m"


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_zero_border_resource_transitions_select_and_bind_the_correct_sampler():
    source = SOURCE.read_text()
    cache_start = source.index("#define FRONT_CACHE_ENTRIES")
    cache_end = source.index("/* every pipeline made", cache_start)
    stages_end = source.index("} stage_samplers[GPU_STAGE_COUNT];") + len("} stage_samplers[GPU_STAGE_COUNT];")
    stages_start = source.rfind("static struct", 0, stages_end)
    actual = source[cache_start:cache_end] + "\n"
    for signature in (
        "static MTLSamplerBorderColor border_color(",
        "static unsigned exact_borders(",
        "static BOOL zero_border_eligible(",
        "static id<MTLSamplerState> sampler_state(",
        "static void *sampler_state(",   # (unretained since the draw path's ARC change)
    ):
        # The initial red run predates the eligibility helper.
        if signature in source:
            actual += function(source, signature) + "\n"
    actual += source[stages_start:stages_end] + "\n"
    actual += function(source, "static void bind_stages(")
    fixture = (ROOT / "port/ios/tests/metal_border_probe.m").read_text()
    with tempfile.TemporaryDirectory(prefix="metal-border-") as directory:
        directory = Path(directory)
        generated = directory / "probe.m"
        generated.write_text(fixture.replace("/* PRODUCTION_SAMPLER_AND_BINDING */", actual))
        binary = directory / "probe"
        subprocess.run([
            "xcrun", "--sdk", "macosx", "clang", "-fobjc-arc", "-O1",
            "-fsanitize=address,undefined", "-Iport/linux/src", "-Iport/ios/host",
            "-framework", "Foundation", "-framework", "Metal", str(generated), "-o", str(binary),
        ], cwd=ROOT, check=True)
        # The capability is immutable in production; each mode needs empty caches.
        for native in ("legacy", "native"):
            subprocess.run([str(binary), native], cwd=ROOT, check=True)


if __name__ == "__main__":
    test_zero_border_resource_transitions_select_and_bind_the_correct_sampler()
