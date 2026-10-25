# Desktop network lockstep checks

`tools/net_lockstep.py` starts a native Mac build twice, hosts a private system-link match over loopback, and drives each player with the existing seeded test bot. Each peer has a fresh data folder, configuration, logs, and saves. Only its retail maps directory is shared. The default addresses are `127.0.0.2` and `127.0.0.3`; both must already be available to bind on the Mac. If necessary, have an administrator add loopback aliases, or select two existing local addresses with `--host-address` and `--client-address`. VISR reserves `127.0.0.1` for each copy’s own internal connections. The checker does not change network interfaces. Run this inside your normal exclusive Mac runner job:

```sh
python3 tools/net_lockstep.py \
  --build-a build/macos/app-native/host/Release/VISR.app \
  --maps /path/to/retail/maps --map bloodgulch --map chillout \
  --duration 120 --events --output /path/to/new-results
```

`--build-b` selects another build; omitted, it uses A twice. `--swap` also runs B as host. `--b-hosts` runs only B as host, for a foreign host of an older version that A joins but does not host. `--events` enables scripted hits every seven seconds and a host kill every 23 seconds. The duration counts matched gameplay, excluding startup. A separate startup allowance and each game's exit-after setting bound a stalled run. The output directory must be new. Logs may contain local paths; keep them outside version control.

The result is PASS (exit 0), FAIL (1), or ERROR (2). Any FAIL takes precedence over ERROR across runs. A crash, missing map, malformed state, incomplete roster, unspawned peer, or insufficient matched gameplay is ERROR. Adapter log files must be fresh and separate for each peer; old evidence is rejected. An observed persistent divergence remains FAIL even if a peer later crashes. Interrupted runs report their signal and exit with 128 plus that number. Both owned process groups receive TERM and then KILL on every normal or exceptional exit. A watchdog also stops a peer if the checker is killed outright; wrappers must keep their children in the launched process group.

The checker compares once-per-second `network test: tick` records at equal game ticks. The harness samples after each completed distributed tick at whole game-second boundaries, so a frame that advances several ticks cannot shift the sampling phase. It checks position, health, shields, grenades, weapon tags and ammunition, life state, scores, kills, deaths, friendly fire, and team/machine membership. Prediction and message delivery can cause transient differences: defaults allow one world unit of position error, 0.15 vitality, two rounds, and require three successive divergent observations for the same player and field. These are explicit initial regression thresholds, not calibrated protocol guarantees. `--position-tolerance`, `--vitality-tolerance`, `--rounds-tolerance`, and `--persistence` adjust them. Missing overlapping ticks or gaps over two seconds cannot establish continuous coverage.

This is a sampled state comparison, not a per-tick checksum. Aim, animation, unlogged objects, and changes corrected between samples can escape detection. The existing bot is seeded but follows input poll time; it is not a deterministic replay. Identical-build success establishes only the tested scenario and thresholds.

## Foreign desktop peers

A JSON adapter supplies black-box launch and log hooks for B. The tool does not depend on another project's source, internal structures, or build system. Supply `--build-b /path/to/release-binary --adapter adapter.json`. For example, with a wrapper you provide:

```json
{
  "launch": ["/path/to/launch-wrapper", "{binary}", "{role}", "{map}", "{address}", "{other}", "{data}", "{duration}"],
  "env": {"WRAPPER_DATA": "{data}"},
  "log": "foreign.log",
  "normalize": ["python3", "/path/to/log-normalizer.py", "{log}"]
}
```

Commands are argument arrays, without a shell. Launch/environment placeholders are `binary`, `role`, `map`, `address`, `other`, `data`, and `duration`; normalizers receive `log` and `data`. The log path is relative to the peer's data folder or absolute. The wrapper must arrange the peer's own documented launch settings, matching maps and network version, scripted input, logging, and safety timeout. VISR's guest also reads the generated private `config.toml`, since the Apple host forwards only selected environment settings. Normalizers run with a ten-second timeout and must emit VISR-style records or one normalized JSON record per line:

```text
lockstep: {"tick":30,"players":{"0":{"alive":false,"score":0,"kills":0,"deaths":0,"friendly_fire":0,"team":0,"machine":0},"1":{"alive":false,"score":0,"kills":0,"deaths":0,"friendly_fire":0,"team":1,"machine":1}}}
```

Live players additionally require `position` (three numbers), `health`, `shield`, `grenades` (two counts), and `weapons` (ordered `[numeric_tag, rounds]` pairs). Player identities, ticks, tag identities, and units must be normalized to the same observable meaning on both sides. Never synthesize missing observations. If a foreign binary exposes insufficient state, the result is ERROR; a connection alone cannot produce PASS. No OpenCE or Chupathingy release has been qualified by this tool yet.
