"""Exercise the production guest reticle handoff to the live scope plane."""

import subprocess
import tempfile
from pathlib import Path

from harness import function, read


root = Path(__file__).resolve().parents[1]
guest = function(read("port/linux/src/d3d8_device.c"), "stereo_reticle_aimed")
with tempfile.TemporaryDirectory(prefix="visr-scope-guest-") as folder:
    temporary = Path(folder)
    (temporary / "stereo_reticle_aimed.inc").write_text(guest)
    binary = temporary / "scope-guest-probe"
    subprocess.run(
        ["xcrun", "clang", "-O2", "-fsanitize=address,undefined", "-I", str(temporary),
         "port/ios/tests/stereo_scope_guest_probe.c", "-o", str(binary)],
        cwd=root, check=True,
    )
    subprocess.run([str(binary)], cwd=root, check=True)
