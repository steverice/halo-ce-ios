"""Compose docs/benchmarks.md: headline charts from the results snapshot, then the methodology."""
from __future__ import annotations

try:
    from . import charts
    from .results import CAVEAT, METHOD_NOTES, SCHEMA, _notes, method_group, _record_order
    from .sheet_fetch import SHEET_ID
except ImportError:
    import charts
    from results import CAVEAT, METHOD_NOTES, SCHEMA, _notes, method_group, _record_order
    from sheet_fetch import SHEET_ID

SHEET_URL = f"https://docs.google.com/spreadsheets/d/{SHEET_ID}"


PINNED_REF = "main"  # replace with the merged commit that holds the harness the numbers came from
CODE_URL = f"https://github.com/steverice/visr/blob/{PINNED_REF}/tools/perf_compare"
SITE_PAGE_URL = "https://code.steverice.org/visr/benchmarks.html"


PLATFORM_ORDER = ("Mac", "iPhone/iPad", "Apple TV", "Vision Pro")
# How each timing method reads, per platform. The ports and devices that used it come from the rows.
TIMING = {
    "trace": "Metal's per-frame timing: `metalperftrace` on-glass drawable intervals for the app's process.",
    "mst": "Instruments Metal System Trace recorded over the network; each frame's time is a row of the "
           "`displayed-surfaces-interval` table. Against VISR's own frame log over the same window, average rate and "
           "p95/p99 agree within 1%.",
    "app": "The app's own benchmark report: VISR's report, or the telemetry of a port that has no timing tool, "
           "counted over the same warm-up and sampling window.",
    "raf": "Safari `requestAnimationFrame` callbacks, which measure browser scheduling.",
    "hud": "Metal HUD per-frame presentation intervals.",
    "other": "Another method, named in the `method` column.",
}
OG_SETTINGS = "shadow size, model LOD and field of view"


def _ok(rows: list[dict]) -> list[dict]:
    return [row for row in rows if row.get("status") == "ok" and row.get("fps_avg", "") != ""]


def _names(rows: list[dict]) -> str:
    names = []
    for row in rows:
        name = row["port_name"] if row["port"] != "visr" else "VISR"
        if name not in names:
            names.append(name)
    return ", ".join(names)


def _count(number: int, noun: str) -> str:
    return f"{number} {noun}" + ("" if number == 1 else "s")


def timing_section(rows: list[dict]) -> list[str]:
    """How each platform's frame times were taken, from the methods and ports in the rows."""
    ok = _ok(rows)
    lines = ["### Timing by platform", ""]
    for platform in PLATFORM_ORDER:
        mine = [row for row in ok if row["platform"] == platform]
        if not mine:
            continue
        hosts = sorted({row["host"] for row in mine})
        for group in ("trace", "mst", "app", "raf", "hud", "other"):
            chosen = [row for row in mine if method_group(row) == group]
            if not chosen:
                continue
            modes = sorted({"capped" if row["frame_mode"] == "display" else row["frame_mode"] for row in chosen})
            run = ("uncapped, vsync off" if modes == ["uncapped"] else "frame cap on"
                   if modes == ["capped"] else "frame cap on and uncapped")
            lines.append(f"- **{platform}** ({', '.join(hosts)}; {run}): {TIMING[group]} Ports: {_names(chosen)}.")
    lines.append("")
    chef = [row for row in ok if row["port"] == "master-chef" and row["cap_fps"] not in ("", "0")]
    if chef:
        caps = "/".join(sorted({row["cap_fps"] for row in chef}))
        lines.extend([f"master-chef runs at a fixed {caps} fps as shipped, below the {chef[0]['display_hz']} Hz display, "
                      "so it has no uncapped rows.", ""])
    return lines


def device_section(rows: list[dict]) -> list[str]:
    """Device sections are presented runs at the display rate; Mac sections are uncapped."""
    ok = _ok(rows)
    devices = [platform for platform in PLATFORM_ORDER if platform != "Mac" and any(r["platform"] == platform for r in ok)]
    lines = []
    if devices:
        names = " and ".join(devices) if len(devices) < 3 else ", ".join(devices[:-1]) + f", and {devices[-1]}"
        lines.extend([f"{names} charts use runs presented at the device's display rate or, where its profile sets a lower "
                      "frame cap, at that cap, not an uncapped mode. Their on-time share (frames within 5% of the display interval) is in the "
                      "spreadsheet, and a device that holds its display rate shows an average near that rate. The Mac chart "
                      "uses the uncapped, matched-stock runs described below.", ""])
    og = [row for row in ok if row["port"] == "halo-og"]
    if og:
        sizes = sorted({row["resolution"] for row in og if row["resolution"]})
        lines.extend([f"Halo OG always renders at the display's full size ({'/'.join(sizes) or 'unknown'} drawable on the "
                      f"{og[0]['host']}), so it draws more pixels than the 1280×960 native runs. It also lacks several settings "
                      f"the other ports expose, among them {OG_SETTINGS}; its rows use its defaults.", ""])
    return lines


def uncapped_section(rows: list[dict]) -> list[str]:
    """Why uncapped charts show average fps and GPU frame time, shown when the data has uncapped rows."""
    if not any(row["frame_mode"] == "uncapped" for row in _ok(rows)):
        return []
    return ["### Uncapped charts", "",
            "Uncapped charts show average fps and GPU frame time, not 1% low rates. When a game renders faster than the "
            "display refreshes, macOS hands drawables to the window server in bursts, so the on-glass intervals record when "
            "the window server latches drawables rather than the frames a player sees; VISR's intervals alternate between "
            "about 0 ms and about 5 ms. Percentiles of those intervals, the 1% low included, describe that latching. "
            "The average counts frames shown per second and is not affected. GPU frame "
            "time is the GPU duration of each presented frame from the same Metal trace, reported as the median and the 90th "
            "percentile. Capped and device charts keep the 1% low and add the share of frames that arrive on time. "
            "The spreadsheet keeps every column.", ""]


def suite_section(rows: list[dict]) -> list[str]:
    """Per-suite definitions, the overall rule and the rejected-attempt count, with counts from the data."""
    rejected = [row for row in rows if row.get("status") == "rejected"]
    reasons = sorted({row["notes"].split(": ", 1)[-1] for row in rejected})
    ok = _ok(rows)
    measured = {name: [r for r in ok if r["suite"] == name] for name in ("campaign", "multiplayer", "co-op")}

    def state(name: str) -> str:
        mine = measured[name]
        if not mine:
            return "No measurements yet."
        return f"Measured on {_count(len({r['port'] for r in mine}), 'port')}: {_names(mine)}."

    lines = ["### Suites", "",
             "- **Campaign:** every measured window: the opening of a10, the opening of b30, and the b30 beach encounter 90 "
             "seconds later (`b30 +90 s`). One player, no input, default enemies. Each window warms up for 15 seconds after player readiness, takes a five-second background CPU baseline, then samples for "
             "60 seconds. Three rounds per set. A set with fewer than three valid rounds is marked incomplete. "
             f"{state('campaign')}",
             "- **Multiplayer:** a hosted Slayer match on Chill Out and one on Blood Gulch, each a sampling window; the suite "
             "value is the mean over both maps' windows. One local player, driven by the scripted bot input, hosts and starts "
             "the match with no other humans. Bot input is paced from the wall clock, so runs on different ports, and "
             "repeated runs on one port, play equivalent workloads, not identical replays. "
             f"{state('multiplayer')}",
             "- **Co-op:** local split-screen campaign co-op, two players. Enemy scaling is set explicitly to static 1x on every "
             "port that has the option: VISR sets `debug.enemy_multiplier = 1`, and OpenCE-macOS and CoreCE set their "
             "enemies-per-player scaling to static 1x. A port without the option runs its own fixed behavior, and its notes "
             "column in the spreadsheet says so. A port without split screen shows "
             "\"Does not support split screen\" and the status `not_supported`; that differs from \"not measured\", which "
             f"means the suite has not been run for the build. {state('co-op')}",
             "- **Overall:** per build, the geometric mean of the suites' average rates and the geometric mean of their 1% low "
             "rates, each suite weighted equally, so every suite counts the same whatever its frame-rate scale. A build "
             "missing a suite has no overall bar.", "",
             "A round is rejected when the outside-process CPU average rises more than half a core above the baseline taken "
             "before the window. Each round allows three attempts. Rejected attempts appear in the spreadsheet with the status "
             f"`rejected` and their reason. This snapshot has {len(rejected)} rejected attempts"
             + (f" ({', '.join(reasons)})." if reasons else "."), "",
             f"Suite definitions and the harness are in [`tools/perf_compare`]({CODE_URL}) "
             f"([`adapters.py`]({CODE_URL}/adapters.py), [`refresh.py`]({CODE_URL}/refresh.py)).", ""]
    return lines


def methodology(summary: dict, rows: list[dict]) -> list[str]:
    records = sorted(summary.get("records", []), key=_record_order)
    ok = _ok(rows)
    ports = {row["port"] for row in ok}
    platforms = [p for p in PLATFORM_ORDER if any(r["platform"] == p for r in ok)]
    lines = ["## Methodology", "",
             f"The snapshot holds {_count(len(ok), 'measured window')} for {_count(len(ports), 'port')} on "
             f"{_count(len(platforms), 'platform')} ({', '.join(platforms) or 'none'}). "
             "Each window's value is the mean over its valid rounds. A bar is the geometric mean of the windows' average "
             "presentation rates, drawn beside the geometric mean of their 1% low rates (1000 divided by the 99th percentile "
             "interval). When a build was measured on several "
             "dates, the newest set is charted. Sets with fewer than three rounds, and targets that could not run, stay in the "
             "spreadsheet with their status. On-time share uses the recorded display rate with a 5% interval tolerance, is "
             "omitted for uncapped rows and does not establish a display tier. The methods measure different events and are "
             "not ranked against one another. Only released builds are shown. Web builds run in a browser that always syncs to the display, so they have "
             "capped numbers only and do not appear in the uncapped charts. The spreadsheet's README tab describes each "
             "column.", ""]
    lines.extend(timing_section(rows))
    lines.extend(device_section(rows))
    lines.extend(uncapped_section(rows))
    lines.extend(suite_section(rows))
    lines.extend(_notes(records))
    groups = [group for group in ("trace", "mst", "app", "hud", "raf", "other")
              if any(method_group(r) == group for r in ok)]
    lines.extend(line for group in groups for line in (METHOD_NOTES[group], ""))
    return lines


def render_page(summary: dict, rows: list[dict], specs: list[dict]) -> str:
    """Deterministic page text from the public summary, the results snapshot and the chart specs."""
    if summary and summary.get("schema") != SCHEMA:
        raise ValueError("summary schema mismatch")
    records = sorted(summary.get("records", []), key=_record_order)
    dates = sorted(str(r["date"]) for r in records if r.get("date"))
    lines = ["# Performance benchmarks", "", CAVEAT, "",
             f"Last measured: {dates[-1] if dates else 'unknown'}. The [results spreadsheet]({SHEET_URL}) holds every number. "
             f"The [interactive version]({SITE_PAGE_URL}) adds suites and a choice of builds. The methodology is at the bottom "
             "of this page.", "",
             "## Results", "", "Campaign suite, newest build of each port.", "", charts.BEGIN, "",
             charts.render_all(rows, specs).rstrip(), "", charts.END, ""]
    lines.extend(methodology(summary, rows))
    return "\n".join(lines).rstrip() + "\n"
