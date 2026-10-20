"""Compile the production passenger predicate against a small seat state."""

import subprocess
import tempfile
from pathlib import Path

from harness import enum_with, function, read

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="visr-seat-predicate-") as folder:
    temporary = Path(folder)
    (temporary / "config.inc").write_text(
        enum_with(read("source/units/unit_definitions.h"), "_unit_seat_first_person_camera_bit")
    )
    (temporary / "under_test.inc").write_text(
        function(read("port/linux/game/cinematic_screen.c"), "halo_seat_first_person_passenger")
    )
    (temporary / "aiming_unit.inc").write_text(
        function(read("source/units/units.c"), "unit_get_aiming_unit_index")
    )
    (temporary / "zoom_aim.inc").write_text(
        function(read("port/linux/game/cinematic_screen.c"), "halo_zoom_ordinary_aim_ray")
    )
    executable = temporary / "stereo-passenger-predicate-probe"
    subprocess.run(
        ["xcrun", "clang", "-O2", "-fsanitize=address,undefined", "-I", str(temporary),
         str(root / "port/ios/tests/stereo_passenger_predicate_probe.c"), "-o", str(executable)],
        cwd=root, check=True,
    )
    subprocess.run([str(executable)], cwd=root, check=True)
