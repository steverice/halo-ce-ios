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


def _target_rows(target):
    runs = target.get("runs", [])
    scenarios = sorted(set(target.get('scenarios', ['a10', 'b30'])) | {str(run["scenario"]) for run in runs})
    for scenario in scenarios:
        rounds = {}
        for run in runs:
            if str(run["scenario"]) == scenario and all(key in run.get("metrics", {}) for key in _METRICS):
                rounds.setdefault(run["round"], run)
        valid = list(rounds.values())
        status = target.get("status", "unknown")
        if len(valid) < 3:
            status += f"; incomplete ({len(valid)}/3 rounds)"
        else:
            status += f"; {len(valid)} rounds"
        if target.get("reason"):
            status += "; " + str(target["reason"])
        conditions = (f"{target.get('frame_mode', 'capped')}; +{target.get('native_delay_seconds', 0)} s; "
                      f"{target.get('quality_preset', 'defaults')}; warm-up {target.get('warmup_seconds', 15)} s")
        yield [target.get("id", "unknown"), scenario, status,
               *("—" if target.get('frame_mode') == 'uncapped' and key == 'on_time_pct' else _range(valid, key)
                 for key in _METRICS),
               *(_conditions(valid, key) for key in
                 ("thermal", "resolution", "cap", "display_hz", "display_rate_basis")), conditions]


CAVEAT = ("We try to make these comparisons fair, but our numbers or our setup for other projects may be wrong "
          "or out of date. Corrections are welcome: [open an issue](https://github.com/steverice/visr/issues).")


def render(summary: dict) -> str:
    """Render deterministic Markdown from a public, sanitized summary."""
    lines = ["# Performance benchmarks", "",
             CAVEAT, "",
             f"Date: {_cell(summary.get('date', 'unknown'))}. "
             f"Refresh wall time: {_cell(summary.get('refresh_seconds', 'unknown'))} seconds.", "",
             f"Machine: {_cell(summary.get('machine', 'unknown'))}. "
             f"OS: {_cell(summary.get('os', 'unknown'))}.", "",
             "Ranges show the minimum–maximum across unique rounds for each scenario; at least three rounds are required. "
             "Incomplete measurements remain visible. On-time share uses the recorded display rate with a 5% interval tolerance; "
             "it is omitted for uncapped rows and does not establish a display tier. "
             "The methods measure different events and are not ranked against one another.", "",
             ]
    if summary.get('combined_matrices'):
        lines.extend(['Refresh wall time is the sum of collection time across the retained matrices; '
                      'setup pilots and time waiting between jobs are excluded.', ''])
    if summary.get("cpu_policy") == "baseline":
        lines.extend(["Background CPU is sampled for five seconds immediately before each native sampling window. "
                        "Runs are rejected when average outside-process CPU rises more than half a core above that baseline. "
                        "Steady background work remains active and may affect these estimates. "
                        "Each round allows up to three attempts; rejected attempts do not enter the ranges.", ""])
    if summary.get("native_scenario") == "opening-sequence":
        lines.extend(["Capped opening runs use a10 and b30 with no input: 15 seconds of warm-up "
                        "after player readiness, a five-second background CPU baseline, then 60 seconds of sampling. "
                        "Opening cinematics are included. "
                        "Both apps use 1280×960 drawables on a 60 Hz display, interpolation and a requested 60 fps cap; "
                        "other graphics settings retain each app's defaults. These runs do not establish combat performance "
                        "or identical image quality. Player readiness does not align cinematic clocks, so opening "
                        "sequences can have phase offsets between apps.", ""])
    if any(target.get('frame_mode') == 'uncapped' for target in summary.get('targets', [])):
        lines.extend(['Uncapped runs request `display.frame_rate = "unlimited"` in VISR and '
                      '`display.max_fps = -1` in OpenCE-macOS, with `display.vsync = false` and '
                      'interpolation enabled in both. The Conditions column identifies each mode and extra delay after warm-up '
                      '(+0 s is the opening window). The 60 Hz display still limits visible refreshes; '
                      'presentation intervals do not establish simulation speed or unique displayed frames. '
                      'Above-refresh trace rates have unverified physical meaning and are not verified submission throughput. '
                      'The same elapsed window does not prove identical gameplay. Each launch uses fresh isolated data/save '
                      'folders; OS and driver caches are not reset or equalized.', ''])
    if summary.get('mixed_bundle_storage'):
        lines.extend(['For these uncapped runs, VISR\'s bundle loads from internal storage and OpenCE-macOS\'s '
                      'from external USB storage. Both use the same maps and per-run data storage; bundle I/O '
                      'and caches may still affect stalls.', ''])
    if any(t.get('quality_preset') == 'matched-stock' for t in summary.get('targets', [])):
        lines.extend(['The matched-stock preset disables antialiasing, replacement HUD/text, and first-person legs in both apps; '
                      'both use direct camera and 128-pixel shadow maps. VISR also uses automatic model LOD, the Xbox-size '
                      'effect buffer, 1× anisotropic filtering, no replacement textures, and no first-person body. '
                      'OpenCE-macOS uses native resolution scaling, stock world/weapon FOV, and disables optional per-pixel '
                      'lighting, weapon shields, and shield glow. Other defaults remain unchanged. '
                      'VISR uses native Metal and compressed GPU textures; OpenCE-macOS uses ANGLE over Metal. '
                      'These runs could not verify OpenCE-macOS\'s internal filtering, LOD, effect-buffer, or texture-storage policy, '
                      'so identical image quality remains unverified. Weapon framing and texture filtering still differ visibly. '
                      'Earlier capped rows retain each app\'s graphics defaults and are not quality-matched to the new rows.', ''])
        lines.extend(['Configuration references: [VISR settings](../port/ios/README.md), '
                      '[OpenCE-macOS v1.0.2 settings](https://github.com/channelramble/OpenCE-macOS/blob/v1.0.2/port/linux/README.md#settings).', ''])
    if any(t.get('native_delay_seconds') == 90 for t in summary.get('targets', [])):
        lines.extend(['The later b30 window adds 90 seconds after warm-up, reaching the beach encounter at about '
                      '110 seconds after player readiness once the CPU baseline is included. It samples 60 seconds '
                      'with a stationary player and no input. Start/end screenshots qualify the camera location, '
                      'but AI and combat state can vary between apps and rounds; this is not a deterministic combat replay.', ''])
    lines.extend(["## Builds and availability", ""])
    targets = sorted(summary.get("targets", []), key=lambda target: str(target.get("id", "")))
    if targets:
        _table(lines, ["Target", "Version", "SHA-256", "Method", "Status", "Reason", "Mode", "Delay (s)", "Quality"],
               [[*[target.get(key, "unknown") for key in ("id", "version", "sha256", "method", "status", "reason")],
                 target.get('frame_mode', 'capped'), target.get('native_delay_seconds', 0), target.get('quality_preset', 'defaults')]
                for target in targets])
    else:
        lines.extend(["No targets recorded.", ""])
    groups = [("Mac native Metal performance trace", "trace"), ("Mac native Metal HUD", "hud"),
              ("Safari requestAnimationFrame", "raf"), ("Other methods", "other")]
    headings = ["Target", "Scenario", "Status", "Avg FPS", "p50 (ms)", "p95 (ms)", "p99 (ms)",
                "On time (%)", "Worst (ms)", "Frames", "Duration (s)", "Thermal", "Resolution", "Cap (fps)",
                "Display (Hz)", "Display rate basis", "Conditions"]

    def method_group(target):
        method = str(target.get("method", "")).lower()
        if "raf" in method or "safari" in method:
            return "raf"
        if "metalperftrace" in method:
            return "trace"
        return "hud" if "hud" in method else "other"

    for title, group in groups:
        selected = [target for target in targets if method_group(target) == group]
        if group in {"other", "trace"} and not selected:
            continue
        lines.extend([f"## {title}", ""])
        if group == "trace":
            lines.extend(["Apple's metalperftrace records per-frame on-glass drawable intervals. "
                          "CPU/GPU durations and aggregate FPS are excluded. Repeated image content is not detected.", ""])
        elif group == "hud":
            lines.extend(["Metal HUD per-frame intervals describe native presentation pacing. GPU duration and aggregate HUD FPS are excluded.", ""])
        elif group == "raf":
            lines.extend(["Safari callbacks measure browser scheduling and may include repeated game images; they do not prove unique game frames.", ""])
        if selected:
            _table(lines, headings, [row for target in selected for row in _target_rows(target)])
        else:
            lines.extend(["No measurements recorded.", ""])
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
