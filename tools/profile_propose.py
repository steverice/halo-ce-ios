#!/usr/bin/env python3
"""Choose the least-visible settings change that lets a device meet its frame-rate target."""
import argparse
import sys
import tomllib
from dataclasses import dataclass, field

if __package__:
    from . import profile_cost_model as cost
    from . import profile_predict as predict
else:
    import profile_cost_model as cost
    import profile_predict as predict

RANKING = cost.COST / "visual-ranking.toml"
FIXED = {"display.high_res_hud", "display.high_res_text", "display.foveation"}
GRADE_COST = {"invisible": 0, "barely": 1, "obvious": 3}
FLOOR_LINES = 540
LOWER = {120: 60, 90: 60, 60: 30}
DEFAULT_ASPECT = 16 / 9


@dataclass
class Option:
    settings: dict
    fps: float
    worst_grade: str
    hz: int = 0
    effects: dict = field(default_factory=dict)


@dataclass
class Proposal:
    kind: str
    settings: dict
    predicted_fps: float
    bound: str
    options: list = field(default_factory=list)
    fps_before: float = 0.0
    target_hz: int = 0
    effects: dict = field(default_factory=dict)


def read_ranking(path=RANKING):
    with open(path, "rb") as handle:
        return tomllib.load(handle)


def _grade(value, order, grades):
    return grades.get(value, "obvious") if order.index(value) else "invisible"


def _size(device, settings):
    height = int(settings.get(cost.RESOLUTION_KEY) or device.native_height or 0)
    aspect = device.native_width / device.native_height if device.native_width and device.native_height else DEFAULT_ASPECT
    return round(height * aspect), height


def _predict(fit, device, settings):
    width, height = _size(device, settings)
    return cost.predict(fit, settings, width, height)


def _times(prediction, hz):
    return prediction.render_cpu_ms + prediction.simulation_tick_ms * 30 / hz, prediction.gpu_ms


def _fps(prediction, hz):
    frame = prediction.frame_ms(hz)
    return min(float(hz), 1000 / frame) if frame > 0 else float(hz)


def _next_value(knob, value, order):
    """The next cheaper value on the ladder, or None. A resolution between rungs steps to the next rung below it."""
    if value in order:
        position = order.index(value) + 1
    elif knob == cost.RESOLUTION_KEY:
        position = next((i for i, rung in enumerate(order) if int(rung) <= int(value)), len(order))
    else:
        return None
    if position >= len(order):
        return None
    candidate = order[position]
    if knob == cost.RESOLUTION_KEY and int(candidate) < FLOOR_LINES:
        return None
    return candidate


def _greedy(fit, device, current, ladders, hz, allowed=("invisible", "barely", "obvious")):
    """Return (settings, ok, worst_grade, bound) after taking steps until frame_ms fits the target."""
    settings = dict(current)
    worst = "invisible"
    bound = None
    while True:
        prediction = _predict(fit, device, settings)
        cpu, gpu = _times(prediction, hz)
        side = "gpu" if gpu >= cpu else "cpu"
        bound = bound or side
        if prediction.frame_ms(hz) <= 1000 / hz:
            return settings, True, worst, bound
        best = None
        for knob, (order, grades) in ladders.items():
            value = settings.get(knob)
            following = _next_value(knob, value, order) if value is not None else None
            if following is None:
                continue
            grade = _grade(following, order, grades)
            if grade not in allowed:
                continue
            trial = dict(settings, **{knob: following})
            after = _times(_predict(fit, device, trial), hz)
            gain = (cpu - after[0]) if side == "cpu" else (gpu - after[1])
            if gain <= 1e-9:
                continue
            score = gain / (GRADE_COST[grade] + 0.01)
            if best is None or score > best[0]:
                best = (score, knob, following, grade)
        if best is None:
            return settings, False, worst, bound
        _, knob, following, grade = best
        settings[knob] = following
        if GRADE_COST[grade] > GRADE_COST[worst]:
            worst = grade


def target_hz(device, workload_kind, fit=None, current=None, ranking=None, output_class="handheld"):
    """90 for Vision Pro, 30 for cutscenes, 120 where the display allows it and only invisible or barely steps reach it."""
    if "Vision" in device.model:
        return 90
    if workload_kind == "cutscene":
        return 30
    if device.max_refresh_hz >= 120 and fit is not None and ranking is not None:
        ladders = _ladders(ranking, output_class)
        settings = _fill(fit, current or {}, ladders)
        if _greedy(fit, device, settings, ladders, 120, ("invisible", "barely"))[1]:
            return 120
    return 60


def _ladders(ranking, output_class):
    return {knob: (table["order"], table.get("grade", {})) for knob, table in ranking.get(output_class, {}).items()
            if isinstance(table, dict) and knob not in FIXED}


def _fill(fit, current, ladders):
    settings = {knob: value for knob, value in current.items()}
    for knob in ladders:
        if knob not in settings and knob in fit.baseline:
            settings[knob] = fit.baseline[knob]
    return settings


def _effects(fit, current, proposed):
    result = {}
    for knob, value in proposed.items():
        if knob == cost.RESOLUTION_KEY:
            continue
        old = fit.knobs.get(knob, {}).get(current.get(knob), (1.0, 1.0)) if current.get(knob) != fit.baseline.get(knob) else (1.0, 1.0)
        new = fit.knobs.get(knob, {}).get(value, (1.0, 1.0)) if value != fit.baseline.get(knob) else (1.0, 1.0)
        result[knob] = (new[0] / old[0] if old[0] else 1.0, new[1] / old[1] if old[1] else 1.0)
    return result


def propose(fit, device, workload_kind, current, ranking, output_class="handheld"):
    ladders = _ladders(ranking, output_class)
    start = _fill(fit, current, ladders)
    hz = target_hz(device, workload_kind, fit, start, ranking, output_class)
    before = _fps(_predict(fit, device, start), hz)

    def option(rate):
        settings, ok, worst, bound = _greedy(fit, device, start, ladders, rate)
        changed = {k: v for k, v in settings.items() if start.get(k) != v}
        return ok, bound, Option(changed, _fps(_predict(fit, device, settings), rate), worst, rate,
                                 _effects(fit, start, changed))

    ok, bound, first = option(hz)
    if ok and not first.settings:
        return Proposal("none", {}, first.fps, bound, fps_before=before, target_hz=hz)
    if ok and first.worst_grade != "obvious":
        return Proposal("change", first.settings, first.fps, bound, fps_before=before, target_hz=hz, effects=first.effects)
    options = [first] if ok else []
    rate = hz
    while rate in LOWER:
        rate = LOWER[rate]
        lower_ok, _, lower = option(rate)
        if lower_ok:
            options.append(lower)
            break
    if not options:
        return Proposal("not_recommended", {}, before, bound, fps_before=before, target_hz=hz)
    if len(options) == 1 and options[0].worst_grade != "obvious":
        only = options[0]
        return Proposal("change", only.settings, only.fps, bound, fps_before=before, target_hz=only.hz, effects=only.effects)
    return Proposal("choice", {}, options[-1].fps, bound, options=options, fps_before=before, target_hz=hz)


def _percent(factor):
    return f"{(factor - 1) * 100:+.0f}%" if abs(factor - 1) > 0.005 else "none"


def _table(settings, current, effects):
    lines = ["| Setting | Current | Proposed | CPU effect | GPU effect |", "| --- | --- | --- | --- | --- |"]
    for knob in sorted(settings):
        cpu, gpu = effects.get(knob, (1.0, 1.0))
        lines.append(f"| `{knob}` | {current.get(knob, '')} | {settings[knob]} | {_percent(cpu)} | {_percent(gpu)} |")
    return lines


def markdown(proposal, device, current):
    head = f"### {device.model}"
    if proposal.kind == "none":
        return f"{head}\n\nNo change. Predicted {proposal.predicted_fps:.0f} FPS at the {proposal.target_hz} FPS target.\n"
    if proposal.kind == "not_recommended":
        return (f"{head}\n\nNot recommended. Predicted {proposal.fps_before:.0f} FPS before any change, and the lowest "
                f"settings at {FLOOR_LINES} render lines still miss 30 FPS.\n")
    if proposal.kind == "change":
        lines = [head, "", *_table(proposal.settings, current, proposal.effects), "",
                 f"Predicted {proposal.fps_before:.0f} FPS before, {proposal.predicted_fps:.0f} FPS after "
                 f"(target {proposal.target_hz} FPS, {proposal.bound}-bound)."]
        return "\n".join(lines) + "\n"
    lines = [head, "", f"Choice needed ({proposal.bound}-bound). Predicted {proposal.fps_before:.0f} FPS before any change."]
    for item in proposal.options:
        lines += ["", f"Option at {item.hz} FPS: predicted {item.fps:.0f} FPS, worst visual cost {item.worst_grade}.", "",
                  *_table(item.settings, current, item.effects)]
    return "\n".join(lines) + "\n"


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", help="device model name from profiles/cost/device-facts.csv")
    parser.add_argument("--workload", default="gameplay")
    parser.add_argument("--output-class", default="handheld", choices=["handheld", "tv"])
    options = parser.parse_args(arguments)
    devices = predict.read_devices()
    device = next(d for d in devices if d.model == options.model)
    fit_ = predict.predicted_fit(device, cost.read_model(cost.MODEL_TOML), devices, workload=options.workload)
    print(markdown(propose(fit_, device, options.workload, dict(fit_.baseline), read_ranking(), options.output_class),
                   device, dict(fit_.baseline)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
