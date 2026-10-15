# The device benchmark's recordings

`recordings/NAME.input` and `recordings/NAME.actions` are routes through levels, played by a person and recorded with `debug.input_record` (`port/linux/src/input_replay.c`). The app bundles them, and the benchmark (`debug.benchmark_suite`) replays them so that a device with nobody at the controller still plays through a fight.

A recording contains controller input and replay determinism data:

- `.input`: one line each time controller 1's state changed: the game time in ticks, the buttons, the triggers and the stick positions.
- `.actions`: one line per game tick: the segment, the tick, and the player's own action for that tick (which way they faced, how they moved, what they pressed).

It does not contain game assets, map data or saved state. It does nothing without the player's own copy of the game, and each file is checked to be lines of numbers in exactly these two formats before it is committed.

| Recording | Level | Difficulty | Length | Recorded |
|---|---|---|---|---|
| `a10-bench` | The Pillar of Autumn, from the cryo bay to the bridge | heroic | 108 s | 2026-10-03, opening cutscene skipped |
| `c40-bench` | Two Betrayals, across the bridge | easy | 66 s | 2026-10-03 |

A recording fits the disc build it was made on. A replay that leaves its route is detected and its run is not used.

Signed recording snapshots can reach installed builds without a new binary. Complete downloaded sets also require `.seeds` for deterministic replay. See [Updating profiles and benchmark recordings](../../docs/data-packs.md).
