"""Per-second CPU and GPU cost of a replayed take, from a benchmark report's per-frame series.

A replay of a whole take through the benchmark suite writes, for every frame of its measured window, the render's CPU
time, the GPU time and the interval (frame_cpu, frame_gpu, frame_interval), and for the frames that knew their split
the simulation's and the render's CPU time (frame_simulation, frame_render), each as base64 of little-endian
uint16 in tenths of a millisecond. The run also records window_opened_tick, the recorded tick at which the window
opened and every frame's tick count (frame_ticks, ticks plus one, 0 where unknown), so the frames are placed on the
take's own time exactly, through skips and loads that draw no frames.

    python tools/frame_series.py REPORT.json OUT.csv

writes "second,cpu,gpu,render_cpu" lines: per recorded second, the mean over its frames of the simulation plus
render CPU time (cpu, from the frames that knew their split), the GPU time (gpu) and the frame's render CPU
time (render_cpu). Feed the CSV to tools/heavy_window.py (column 2 or 3).
"""
import base64
import json
import struct
import sys

TICKS_PER_SECOND = 30.0


def decode(run, key):
    raw = base64.b64decode(run[key])
    return [value / 10.0 for value in struct.unpack("<%dH" % (len(raw) // 2), raw)]


def series(report):
    """[(recorded second, cpu, gpu, render_cpu)] for the first run of a report (a path or the parsed JSON)"""
    if not isinstance(report, dict):
        with open(report) as file:
            report = json.load(file)
    run = report["runs"][0]
    opened = run.get("window_opened_tick")
    if opened is None or opened < 0 or "frame_ticks" not in run:
        raise ValueError("the run does not record the tick its window opened at and the ticks of its frames")
    render_cpu, gpu = decode(run, "frame_cpu"), decode(run, "frame_gpu")
    raw = base64.b64decode(run["frame_ticks"])
    ticks = struct.unpack("<%dH" % (len(raw) // 2), raw)
    simulation = decode(run, "frame_simulation") if "frame_simulation" in run else []
    render = decode(run, "frame_render") if "frame_render" in run else []
    frames = min(len(render_cpu), len(gpu), len(ticks))
    cells, tick, split = {}, float(opened), 0
    for index in range(frames):
        second = int(tick / TICKS_PER_SECOND)
        cell = cells.setdefault(second, [[], [], []])
        cell[1].append(gpu[index])
        cell[2].append(render_cpu[index])
        if ticks[index] > 0:
            if split < min(len(simulation), len(render)):
                cell[0].append(simulation[split] + render[split])
            split += 1
            tick += ticks[index] - 1
    rows = []
    for second in sorted(cells):
        cpu, gpu_values, render_values = cells[second]
        mean = lambda values: sum(values) / len(values) if values else 0.0
        rows.append((second, mean(cpu), mean(gpu_values), mean(render_values)))
    return rows


def main():
    rows = series(sys.argv[1])
    with open(sys.argv[2], "w") as file:
        file.write("second,cpu,gpu,render_cpu\n")
        for second, cpu, gpu, render in rows:
            file.write(f"{second},{cpu:.3f},{gpu:.3f},{render:.3f}\n")
    print(f"{len(rows)} seconds, from {rows[0][0]} to {rows[-1][0]}")


if __name__ == "__main__":
    main()
