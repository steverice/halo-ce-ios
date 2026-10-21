"""Compile the engine's help display code against fake profiles and widgets."""
from pathlib import Path
import shutil
import subprocess
import tempfile

from harness import function, read


def test_scenario_help_profiles():
    player_ui = read("source/interface/player_ui.c")
    accessor = function(player_ui, "player_ui_ingame_help_disabled")
    mapping = function(player_ui, "player_ui_get_single_player_local_player_from_controller")
    dialog = function(read("source/interface/ui_widget.c"), "display_scenario_help")
    hud = function(read("source/interface/hud_messaging.c"), "hud_messaging_update")
    start = hud.index("help_active =")
    help_expression = hud[start:hud.index(";", start) + 1]
    fixture = Path(__file__).with_name("harness") / "tests" / "scenario_help.c"
    with tempfile.TemporaryDirectory() as folder:
        work = Path(folder)
        (work / "under_test.inc").write_text(mapping + "\n" + accessor + "\n" + dialog)
        (work / "hud_help.inc").write_text(help_expression)
        compiler = shutil.which("clang") or shutil.which("cc")
        assert compiler, "a C compiler is required"
        executable = work / "scenario_help"
        subprocess.run([compiler, "-std=c99", "-I", str(work), str(fixture), "-o", str(executable)], check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
