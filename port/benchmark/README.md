# The device benchmark's recordings

`recordings/NAME.input` and `recordings/NAME.actions` are routes through levels, played by a person and recorded with `debug.input_record` (`port/linux/src/input_replay.c`). The app bundles them, and the benchmark (`debug.benchmark_suite`) replays them so that a device with nobody at the controller still plays through a fight.

A recording contains controller input and replay determinism data:

- `.input`: one line each time controller 1's state changed: the game time in ticks, the buttons, the triggers and the stick positions.
- `.actions`: one line per game tick: the segment, the tick, and the player's own action for that tick (which way they faced, how they moved, what they pressed).
- `.seeds` (a recording made after 2026-10-14 has one): one line per game tick: the segment, the tick and the game's two random seeds as the tick began, in hex.
- `.route`: the scene's name, map, difficulty and window as `key = value` lines; `.checks`: where player 1 stood every 300 ticks, in a run that followed the recording.

It does not contain game assets, map data or saved state. It does nothing without the player's own copy of the game, and each file is checked to be lines of numbers in exactly these two formats before it is committed.

| Recording | Level | Difficulty | Length | Recorded |
|---|---|---|---|---|
| `a10-bench` | The Pillar of Autumn, from the cryo bay to the bridge | heroic | 108 s | 2026-10-03, opening cutscene skipped |
| `c40-bench` | Two Betrayals, across the bridge | easy | 66 s | 2026-10-03 |
| `b30-beach` (scene `b30-fight`) | Halo, the landing and the fight on the beach | heroic | 85 s | 2026-10-14, on an Apple TV |

A recording fits the disc build it was made on. A replay that leaves its route is detected and its run is not used.

Signed recording snapshots can reach installed builds without a new binary. Complete downloaded sets also require `.seeds` for deterministic replay. See [Updating profiles and benchmark recordings](../../docs/data-packs.md).

## Shader evidence in tester reports

Each finished run includes `shader_capture`: a map name, capture status, SHA-256, and complete descriptor records appended between the run's setup and completion. The interval includes map loading and the measured scene. Missing, disabled, unreadable, oversized, replaced, or incomplete capture evidence is explicit; it never implies zero shader misses. Capture files are limited to 4 MiB each and are read between runs, outside the measured frame window.

The report also includes `shader_dependency_candidates`, capped at 32 MiB across maps. These cumulative records can close dependencies after validation against known source records. They can include earlier sessions or builds and must never be merged wholesale or treated as this run's new misses. Existing UI captures may contain old multiplayer records.

Both Apple TV downloads and Send Report preserve these fields in the same compressed JSON. During an external display mode sweep, `shader_output_intervals` also preserves earlier modes' attributed records with their report code, app identity, and output mode. The consent preview and searchable summary show payload sizes instead of descriptor bodies. Before harvesting, validate the report build, map attribution, descriptor hashes and ranges, and pipeline dependencies; deduplicate against the bundled lists. Older reports without these fields still require separate capture files.

Extract a JSON or compressed JSON report into a new evidence folder:

```sh
python3 tools/benchmark_shaders.py visr-benchmark-REPORT.json.gz --out benchmark-evidence
```

The tool checks payload SHA-256, byte counts, map/run identity, and prior display-mode app identity. It separates attributed interval files from historical dependency candidates. Descriptor validation and dependency closure are still required before merging; extraction does not modify bundled shader lists.
