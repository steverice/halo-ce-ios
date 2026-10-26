"""Parse interval evidence and render the sanitized benchmark summary."""

import html
import json
import math


def parse_hud(text: str) -> list[float]:
    """Read millisecond intervals from Apple's per-frame Metal HUD CSV logs."""
    intervals = []
    seen = set()
    for line in text.splitlines():
        if line.lstrip().startswith("{"):
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue
            line = record.get("eventMessage", record.get("message", ""))
            if not isinstance(line, str):
                continue
        if "metal-HUD:" not in line:
            continue
        fields = line.split("metal-HUD:", 1)[1].strip().split(",")
        if len(fields) < 5 or (len(fields) - 3) % 2:
            continue
        try:
            first_frame = int(fields[0])
            memory = [float(field) for field in fields[1:3]]
        except ValueError:
            continue
        if first_frame < 0 or any(not math.isfinite(value) or value < 0 for value in memory):
            continue
        # Invalid pairs still occupy their documented frame number. Shifting
        # later pairs would silently defeat overlap deduplication.
        for index in range(3, len(fields), 2):
            frame = first_frame + (index - 3) // 2
            try:
                interval, gpu = float(fields[index]), float(fields[index + 1])
            except ValueError:
                continue
            if not math.isfinite(interval) or interval <= 0 or not math.isfinite(gpu) or gpu < 0 or frame in seen:
                continue
            seen.add(frame)
            intervals.append(interval)
    return intervals


def parse_metal_trace(record: dict, resolution=None) -> list[float]:
    """Read on-glass intervals, never CPU/GPU durations or aggregate FPS."""
    frames = {}
    layers = set()
    for trace in record['traces']:
        for process in trace:
            if process['PID'] != record['pid']:
                continue
            for layer in process['Layers']:
                timeline = layer.get('On-Glass Drawable Timeline', [])
                if timeline:
                    layers.add(layer['Layer ID'])
                    sizes = {str(item['Width (pixels)']) + '×' + str(item['Height (pixels)'])
                             for item in layer.get('Configuration Timeline', [])}
                    if resolution is not None and sizes != {resolution}:
                        raise ValueError('resolution-mismatch')
                for frame in timeline:
                    start = frame['Start Mach Continuous Time']
                    seconds = frame['Duration MCT Seconds']
                    if not math.isfinite(seconds) or seconds <= 0:
                        raise ValueError('invalid on-glass interval')
                    frames[start] = seconds * 1000
    if len(layers) > 1:
        raise ValueError('multiple presentation layers')
    return [frames[start] for start in sorted(frames)]


def parse_raf(timestamps: list[float]) -> list[float]:
    """Convert strictly increasing browser millisecond timestamps to intervals."""
    if not all(math.isfinite(value) for value in timestamps):
        raise ValueError("RAF timestamps must be finite")
    intervals = [current - previous for previous, current in zip(timestamps, timestamps[1:])]
    if any(value <= 0 or not math.isfinite(value) for value in intervals):
        raise ValueError("RAF timestamps must be strictly increasing")
    return intervals


def metrics(intervals: list[float], display_hz: float) -> dict:
    """Summarize positive millisecond intervals at the recorded display rate."""
    if not math.isfinite(display_hz) or display_hz <= 0:
        raise ValueError("Display rate must be finite and positive")
    if not intervals or any(not math.isfinite(value) or value <= 0 for value in intervals):
        raise ValueError("Intervals must be nonempty, finite, and positive")
    ordered = sorted(intervals)
    duration_ms = math.fsum(intervals)
    if not math.isfinite(duration_ms):
        raise ValueError("Interval duration must be finite")

    def percentile(fraction):
        index = (len(ordered) - 1) * fraction
        lower = math.floor(index)
        upper = math.ceil(index)
        return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)

    return {
        "fps": len(intervals) * 1000 / duration_ms,
        "p50_ms": percentile(0.5), "p95_ms": percentile(0.95),
        "p99_ms": percentile(0.99),
        "on_time_pct": sum(value <= 1000 / display_hz * 1.05 for value in intervals) * 100 / len(intervals),
        "worst_ms": ordered[-1], "frames": len(intervals),
        "duration_s": duration_ms / 1000,
    }


_METRICS = ("fps", "p50_ms", "p95_ms", "p99_ms", "on_time_pct", "worst_ms", "frames", "duration_s")


def _cell(value):
    value = html.escape(str(value), quote=False).replace("\r", " ").replace("\n", " ")
    return "".join("\\" + character if character in "\\|`*_[]" else character for character in value)


def _table(lines, headings, rows):
    lines.append("| " + " | ".join(_cell(value) for value in headings) + " |")
    lines.append("| " + " | ".join("---" for _ in headings) + " |")
    lines.extend("| " + " | ".join(_cell(value) for value in row) + " |" for row in rows)
    lines.append("")


def _range(runs, key):
    values = [run["metrics"][key] for run in runs]
    if not values:
        return "—"
    precision = 0 if key == "frames" else 2
    return f"{min(values):.{precision}f}–{max(values):.{precision}f}"


def _conditions(runs, key):
    return ", ".join(sorted({str(run.get(key, "unknown")) for run in runs})) or "unknown"


# Public names, in page order. Unknown ports follow in identifier order.
PORTS = {"visr": "VISR", "opence-macos": "OpenCE-macOS", "opence-web": "OpenCE web",
         "chupathingy": "Chupathingy", "halo-og": "Halo OG"}
SCHEMA = 2
# Everything that distinguishes one measured set from another. Two records with the same key are the
# same measurement, so a refresh adds records and never replaces one.
KEY_FIELDS = ("port", "version", "build", "source", "machine", "os", "browser", "method",
              "frame_mode", "native_delay_seconds", "quality_preset", "warmup_seconds", "date")


def record_key(record):
    return tuple(str(record.get(field, "")) for field in KEY_FIELDS)


def merge(existing: dict, new: dict) -> dict:
    """Add a refresh's records to the retained summary; an identical key never overwrites."""
    if existing.get("schema", SCHEMA) != SCHEMA or new.get("schema", SCHEMA) != SCHEMA:
        raise ValueError("summary schema mismatch")
    merged = {**existing, "schema": SCHEMA, "records": list(existing.get("records", []))}
    keys = {record_key(record): record for record in merged["records"]}
    for record in new.get("records", []):
        key = record_key(record)
        if key in keys:
            if keys[key] == record:
                continue
            if keys[key].get("runs"):
                raise ValueError("a measured record with this identity already exists: " + " / ".join(key))
            # A record without measurements holds no samples; a later attempt on the same day supersedes it.
            merged["records"].remove(keys[key])
        keys[key] = record
        merged["records"].append(record)
    for field in ("sweep", "sweep_status"):
        if field in new:
            merged[field] = new[field]
    merged["records"].sort(key=_record_order)
    return merged


def _record_order(record):
    port = str(record.get("port", ""))
    ports = list(PORTS)
    return (ports.index(port) if port in ports else len(ports), port, str(record.get("machine", "")),
            str(record.get("date", "")), str(record.get("version", "")), str(record.get("build", "")),
            str(record.get("frame_mode", "capped")), record.get("native_delay_seconds", 0))


def identity(record):
    """The exact product version, build and source of one measured set."""
    name = PORTS.get(str(record.get("port", "")), str(record.get("port", "unknown")))
    details = [f"build {record['build']}" if record.get("build") else "build unknown"]
    if record.get("release_kind") == "pre-release main build":
        details.insert(0, "pre-release")
    if record.get("source"):
        details.append(f"source {record['source']}")
    if record.get("network_version"):
        details.append(f"network version {record['network_version']}")
    return f"{name} {record.get('version') or 'unknown version'} ({', '.join(details)})"


def _mode(record):
    return (f"{record.get('frame_mode', 'capped')}; +{record.get('native_delay_seconds', 0)} s; "
            f"{record.get('quality_preset', 'defaults')}; warm-up {record.get('warmup_seconds', 15)} s")


def _valid_rounds(record, scenario):
    rounds = {}
    for run in record.get("runs", []):
        if str(run["scenario"]) == scenario and all(key in run.get("metrics", {}) for key in _METRICS):
            rounds.setdefault(run["round"], run)
    return list(rounds.values())


def _scenarios(record):
    return sorted(set(record.get("scenarios", ["a10", "b30"])) | {str(run["scenario"]) for run in record.get("runs", [])})


def _status(record, valid):
    status = record.get("status", "unknown")
    status += f"; incomplete ({len(valid)}/3 rounds)" if len(valid) < 3 else f"; {len(valid)} rounds"
    if record.get("reason"):
        status += "; " + str(record["reason"])
    return status


def _fps_cell(record, valid, key):
    return "—" if record.get("frame_mode") == "uncapped" and key == "on_time_pct" else _range(valid, key)


def _port_rows(record):
    for scenario in _scenarios(record):
        valid = _valid_rounds(record, scenario)
        yield [identity(record), record.get("machine", "unknown"), scenario, _mode(record), _status(record, valid),
               *(_fps_cell(record, valid, key) for key in _METRICS),
               *(_conditions(valid, key) for key in ("thermal", "resolution", "cap", "display_hz", "display_rate_basis"))]


def method_group(record):
    method = str(record.get("method", "")).lower()
    if "raf" in method or "safari" in method:
        return "raf"
    if "metalperftrace" in method:
        return "trace"
    return "hud" if "hud" in method else "other"


CAVEAT = ("We try to make these comparisons fair, but our numbers or our setup for other projects may be wrong "
          "or out of date. Corrections are welcome: [open an issue](https://github.com/steverice/visr/issues).")

METHOD_NOTES = {
    "trace": "Apple's metalperftrace records per-frame on-glass drawable intervals. "
             "CPU/GPU durations and aggregate FPS are excluded. Repeated image content is not detected.",
    "hud": "Metal HUD per-frame intervals describe native presentation pacing. GPU duration and aggregate HUD FPS are excluded.",
    "raf": "Safari callbacks measure browser scheduling and may include repeated game images; they do not prove unique game frames.",
    "other": "These records use another method; their relationship to displayed pacing is unverified.",
}


def _notes(records):
    lines = []
    if any(r.get("cpu_policy") == "baseline" for r in records):
        lines.extend(["Background CPU is sampled for five seconds immediately before each native sampling window. "
                      "Runs are rejected when average outside-process CPU rises more than half a core above that baseline. "
                      "Steady background work remains active and may affect these estimates. "
                      "Each round allows up to three attempts; rejected attempts do not enter the ranges.", ""])
    if any(r.get("native_scenario") == "opening-sequence" for r in records):
        lines.extend(["Opening runs use a10 and b30 with no input: 15 seconds of warm-up "
                      "after player readiness, a five-second background CPU baseline, then 60 seconds of sampling. "
                      "Opening cinematics are included. "
                      "Native apps use 1280×960 drawables on a 60 Hz display with interpolation. Capped rows request a 60 fps cap "
                      "and keep each app's other graphics defaults. These runs do not establish combat performance "
                      "or identical image quality. Player readiness does not align cinematic clocks, so opening "
                      "sequences can have phase offsets between apps.", ""])
    if any(r.get("frame_mode") == "uncapped" for r in records):
        lines.extend(['Uncapped runs request `display.frame_rate = "unlimited"` in VISR and '
                      '`display.max_fps = -1` in OpenCE-macOS, with `display.vsync = false` and '
                      'interpolation enabled in both. The Conditions column identifies each mode and extra delay after warm-up '
                      '(+0 s is the opening window). The 60 Hz display still limits visible refreshes; '
                      'presentation intervals do not establish simulation speed or unique displayed frames. '
                      'Above-refresh trace rates have unverified physical meaning and are not verified submission throughput. '
                      'The same elapsed window does not prove identical gameplay. Each launch uses fresh isolated data/save '
                      'folders; OS and driver caches are not reset or equalized.', ""])
    if any(r.get("quality_preset") == "matched-stock" for r in records):
        lines.extend(['The matched-stock preset disables antialiasing, replacement HUD/text, and first-person legs in both apps; '
                      'both use direct camera and 128-pixel shadow maps. VISR also uses automatic model LOD, the Xbox-size '
                      'effect buffer, 1× anisotropic filtering, no replacement textures, and no first-person body. '
                      'OpenCE-macOS uses native resolution scaling, stock world/weapon FOV, and disables optional per-pixel '
                      'lighting, weapon shields, and shield glow. Other defaults remain unchanged. '
                      'VISR uses native Metal and compressed GPU textures; OpenCE-macOS uses ANGLE over Metal. '
                      'These runs could not verify OpenCE-macOS\'s internal filtering, LOD, effect-buffer, or texture-storage policy, '
                      'so identical image quality remains unverified. Weapon framing and texture filtering still differ visibly. '
                      'Capped rows keep each app\'s graphics defaults and are not quality-matched to the matched-stock rows.', ""])
        lines.extend(['Configuration references: [VISR settings](../port/ios/README.md), '
                      '[OpenCE-macOS v1.0.2 settings](https://github.com/channelramble/OpenCE-macOS/blob/v1.0.2/port/linux/README.md#settings).', ""])
    if any(r.get("native_delay_seconds") == 90 for r in records):
        lines.extend(['The later b30 window adds 90 seconds after warm-up, reaching the beach encounter at about '
                      '110 seconds after player readiness once the CPU baseline is included. It samples 60 seconds '
                      'with a stationary player and no input. Start/end screenshots qualify the camera location, '
                      'but AI and combat state can vary between apps and rounds; this is not a deterministic combat replay.', ""])
    if any(r.get("release_kind") == "pre-release main build" for r in records):
        lines.extend(["Pre-release main builds are VISR sources measured before a release was tagged. "
                      "They are kept for history and are not the released product.", ""])
    return lines


def render(summary: dict) -> str:
    """Render deterministic Markdown from a public, sanitized summary."""
    if summary and summary.get("schema") != SCHEMA:
        raise ValueError("summary schema mismatch")
    records = sorted(summary.get("records", []), key=_record_order)
    dates = sorted(str(r["date"]) for r in records if r.get("date"))
    lines = ["# Performance benchmarks", "", CAVEAT, "",
             f"Last measured: {_cell(dates[-1] if dates else 'unknown')}.", "",
             "Ranges show the minimum–maximum across unique rounds for each scenario; at least three rounds are required. "
             "Incomplete measurements remain visible. On-time share uses the recorded display rate with a 5% interval tolerance; "
             "it is omitted for uncapped rows and does not establish a display tier. "
             "The methods measure different events and are not ranked against one another.", ""]
    lines.extend(_notes(records))
    lines.extend(["## Measured builds", ""])
    if records:
        _table(lines, ["Port", "Version", "Build", "Source", "Kind", "Artifact", "Artifact SHA-256", "Bundle storage",
                       "Machine", "OS", "Browser", "Date", "Method", "Conditions", "Status", "Collection (s)"],
               [[PORTS.get(str(r.get("port", "")), r.get("port", "unknown")), r.get("version") or "unknown",
                 r.get("build") or "unknown", r.get("source") or "—", r.get("release_kind") or "—",
                 r.get("artifact") or "—", r.get("sha256") or "—", r.get("bundle_storage") or "—",
                 r.get("machine", "unknown"), r.get("os", "unknown"), r.get("browser") or "—",
                 r.get("date", "unknown"), r.get("method", "unknown"), _mode(r),
                 r.get("status", "unknown") + ("; " + str(r["reason"]) if r.get("reason") else ""),
                 r.get("matrix_seconds", "—")] for r in records])
    else:
        lines.extend(["No targets recorded.", ""])
    measured = [r for r in records if r.get("runs")]
    if measured:
        lines.extend(["## Side by side", "",
                      "Rows with the same machine, method, scenario and conditions are directly comparable.", ""])
        rows = []
        for r in measured:
            for scenario in _scenarios(r):
                valid = _valid_rounds(r, scenario)
                if valid:
                    rows.append((r.get("machine", ""), r.get("method", ""), _mode(r), scenario, _record_order(r),
                                 [r.get("machine", "unknown"), r.get("method", "unknown"), scenario, _mode(r), identity(r),
                                  len(valid), _range(valid, "fps"), _range(valid, "p50_ms"), _range(valid, "p99_ms"),
                                  _range(valid, "worst_ms")]))
        rows.sort(key=lambda row: [str(value) for value in row[:5]])
        _table(lines, ["Machine", "Method", "Scenario", "Conditions", "Build", "Rounds", "Avg FPS", "p50 (ms)",
                       "p99 (ms)", "Worst (ms)"], [row[-1] for row in rows])
    headings = ["Build", "Machine", "Scenario", "Conditions", "Status", "Avg FPS", "p50 (ms)", "p95 (ms)", "p99 (ms)",
                "On time (%)", "Worst (ms)", "Frames", "Duration (s)", "Thermal", "Resolution", "Cap (fps)",
                "Display (Hz)", "Display rate basis"]
    ports = sorted({str(r.get("port", "")) for r in records}, key=lambda port: _record_order({"port": port}))
    for port in ports:
        selected = [r for r in records if str(r.get("port", "")) == port]
        lines.extend([f"## {PORTS.get(port, port)}", ""])
        versions = sorted({identity(r) for r in selected})
        lines.extend(["Measured versions: " + "; ".join(_cell(v) for v in versions) + ".", ""])
        for group in ("trace", "hud", "raf", "other"):
            chosen = [r for r in selected if method_group(r) == group]
            if chosen:
                lines.extend([METHOD_NOTES[group], ""])
                _table(lines, headings, [row for r in chosen for row in _port_rows(r)])
    sweep = summary.get("sweep", [])
    sweep_groups = [
        ("VISR offscreen sweep", "offscreen_uncapped",
         "Offscreen throughput uses the report's timing basis and is separate from displayed native intervals and Safari callbacks."),
        ("VISR submission timing", "submission_timing",
         "These self-reported submission timings measure submission throughput, not displayed pacing. They are separate from offscreen throughput, native presentation intervals, and Safari callbacks."),
        ("VISR other timing bases", "other",
         "These reports have unrecognized timing bases; their relationship to displayed pacing is unverified."),
    ]
    for title, basis, caveat in sweep_groups:
        if basis == "other":
            selected = [row for row in sweep if row.get("timing_basis") not in ("offscreen_uncapped", "submission_timing")]
        else:
            selected = [row for row in sweep if row.get("timing_basis") == basis]
        if basis != "offscreen_uncapped" and not selected:
            continue
        lines.extend([f"## {title}", "", caveat, ""])
        if basis == "offscreen_uncapped" and summary.get("sweep_status"):
            lines.extend([f"Sweep status: {_cell(summary['sweep_status'])}.", ""])
        if selected:
            keys = ("device", "build", "workload", "resolution", "configuration", "settings_hash", "trials",
                    "fps", "timing_basis", "thermal", "report")
            _table(lines, ["Device", "Build", "Workload", "Resolution", "Configuration", "Settings hash", "Trials",
                           "FPS", "Timing basis", "Thermal", "Report"],
                   [[row.get(key, "unknown") for key in keys]
                    for row in sorted(selected, key=lambda row: tuple(str(row.get(key, "")) for key in keys))])
        else:
            lines.extend(["No sweep results recorded.", ""])
    return "\n".join(lines).rstrip() + "\n"
