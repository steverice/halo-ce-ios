#!/usr/bin/env python3
"""tools/route_check.py's parsing and verdicts, and the benchmark suite's way of setting a scene's difficulty. Run by
tools/ios_test.py (plain asserts: python3 tools/test_route_check.py)."""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import route_check  # noqa: E402

CHECKS = """# a comment
recording = port/benchmark/recordings/a10-bench.input
map = a10
difficulty = hard
suite = a10-crew
exit_after = 110
1 180 -56.41,0.41,0.42 0.5 in the cryo tube
1 480 -52.45,3.31,0.00 2.0 out of the cryo tube
"""

LOG = """halo-linux: debug.input_replay: route segment 0 tick 0 at none (recorded action)
halo-linux: debug.input_replay: route segment 1 tick 180 at -56.41,0.41,0.42 (recorded action)
halo-linux: debug.input_replay: route segment 1 tick 480 at -52.45,3.31,-0.00 (recorded action)
"""


def verdicts(log):
    results = route_check.evaluate(route_check.parse_checks(CHECKS), route_check.parse_route(log))
    return [passed for _, passed, _ in results]


parsed = route_check.parse_checks(CHECKS, base=Path("/repo"))
assert parsed.recording == Path("/repo/port/benchmark/recordings/a10-bench.input")
assert (parsed.map, parsed.difficulty, parsed.suite, parsed.exit_after) == ("a10", "hard", "a10-crew", 110.0)
assert [(check.segment, check.tick) for check in parsed.checks] == [(1, 180), (1, 480)]
assert parsed.checks[0].what == "in the cryo tube"

try:
    route_check.parse_checks(CHECKS + "1 185 0,0,0 1.0\n")
    raise AssertionError("a tick the route never logs was accepted")
except ValueError as problem:
    assert "multiple" in str(problem), problem

route = route_check.parse_route(LOG)
assert route[(0, 0)] is None
assert route[(1, 480)] == (-52.45, 3.31, -0.0)

# a replay that follows the route passes; one that leaves the player in the cryo tube, or never reaches the
# segment, fails
assert verdicts(LOG) == [True, True]
assert verdicts(LOG.replace("-52.45,3.31,-0.00", "-56.41,0.41,0.42")) == [True, False]
assert verdicts(LOG.replace("route segment 1", "route segment 2")) == [False, False]

for path in sorted((route_check.ROOT / "tools/route_checks").glob("*.checks")):
    assert route_check.parse_checks(path.read_text()).recording.is_file(), path

# the suite sets a scene's difficulty for the next map load (main_set_difficulty, as the console's
# game_difficulty_set does); game_difficulty_level_set changes only the game in progress, and the map load then took
# the global difficulty (normal), so every route replayed on normal and a10's player never left the cryo tube
scenario = (route_check.ROOT / "port/linux/game/benchmark_scenario.c").read_text()
assert not re.search(r"\bgame_difficulty_level_set\s*\(", scenario), "the map load overwrites that difficulty"
assert re.search(r"\bmain_set_difficulty\s*\(", scenario), "the next map's difficulty is never set"
print("PASS: route_check.py parses checks, tells a followed route from a stuck player, and the suite sets "
      "difficulty for the next map")
