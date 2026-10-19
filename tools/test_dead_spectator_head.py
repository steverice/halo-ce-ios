"""Run the headset head path with the production dead-camera predicates."""

import subprocess
import tempfile
from pathlib import Path

from harness import function, read

root = Path(__file__).resolve().parents[1]
director = read("source/camera/director.c")
screen = read("port/linux/game/cinematic_screen.c")
selection = function(director, "director_choose_camera_game")
assert selection.index("coop_spectate_camera_unit(") < selection.index("dead_camera_new(")
assert "(director_camera_update_proc)dead_camera_update" in selection

with tempfile.TemporaryDirectory(prefix="visr-dead-head-") as folder:
    temporary = Path(folder)
    (temporary / "director_dead.inc").write_text(function(director, "director_dead_camera_active"))
    (temporary / "coop_dead.inc").write_text(function(screen, "halo_coop_dead_camera"))
    executable = temporary / "dead-spectator-head-probe"
    subprocess.run(
        ["xcrun", "clang", "-O2", "-fsanitize=address,undefined", "-DHALO_IOS=1",
         "-DHALO_STEREO_SOURCE_DEAD_CAMERA=1", "-Iport/linux/src", "-Iport/ios/host",
         "-I", str(temporary), "port/ios/tests/dead_head_source_probe.c",
         "port/ios/host/host_stereo_head.c", "-o", str(executable)],
        cwd=root, check=True,
    )
    subprocess.run([str(executable)], cwd=root, check=True)
