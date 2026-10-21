#!/usr/bin/env python3
"""Turn a benchmark report into a device profile pull request: the profile change, the check, before and after
Mac-runner screenshots, the signed tester shards and the pull request text."""
import argparse
import base64
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path
from types import SimpleNamespace

if __package__:
    from . import profile_check as check
    from . import profile_cost_model as cost
    from . import profile_predict as predict
    from . import profile_propose as propose
else:
    import profile_check as check
    import profile_cost_model as cost
    import profile_predict as predict
    import profile_propose as propose

ROOT = Path(__file__).resolve().parent.parent
PROFILES = ROOT / "profiles"
FEED = Path("docs/site/packs/tester")
RECEIPT = FEED / "publication.json"
RUN_JOB = Path.home() / "code/halovision/step-tools/run-job.sh"
REPOSITORY = "steverice/visr"
EVIDENCE_DIRECTORY = Path("docs/profile-prs")
# the cost model's name for a knob, where the profile's differs
PROFILE_KEY = {cost.RESOLUTION_KEY: "display.render_height"}
SCREENSHOT_KEYS = {cost.RESOLUTION_KEY: "display.render_height"}


class Failure(Exception):
    pass


def run_command(*args, **options):
    """Run a command and return its result; a nonzero exit is reported in the result, not raised."""
    print("+", " ".join(str(arg) for arg in args), flush=True)
    return subprocess.run([str(arg) for arg in args], check=False, text=True, **options)


def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")


def toml_literal(kind, value):
    if kind == "boolean":
        return "true" if str(value).lower() == "true" else "false"
    if kind == "integer":
        return str(int(float(value)))
    if kind == "real":
        return str(float(value))
    return json.dumps(str(value))


def set_value(text, section, key, literal):
    """text with `key = literal` in the [section] table, replacing a value or adding the line and table."""
    lines = text.splitlines()
    header = f"[{section}]"
    start = next((i for i, line in enumerate(lines) if line.strip() == header), None)
    if start is None:
        evidence = next((i for i, line in enumerate(lines) if line.startswith("[evidence")), len(lines))
        block = [header, f"{key} = {literal}", ""]
        lines[evidence:evidence] = block
        return "\n".join(lines).rstrip("\n") + "\n"
    end = next((i for i in range(start + 1, len(lines)) if lines[i].lstrip().startswith("[")), len(lines))
    for i in range(start + 1, end):
        if re.match(rf"\s*{re.escape(key)}\s*=", lines[i]):
            lines[i] = f"{key} = {literal}"
            return "\n".join(lines).rstrip("\n") + "\n"
    last = max(i for i in range(start, end) if i == start or lines[i].strip())
    lines.insert(last + 1, f"{key} = {literal}")
    return "\n".join(lines).rstrip("\n") + "\n"


def set_evidence(text, name, basis, why, source=None):
    """text with the [evidence."name"] table written afresh at the end."""
    lines = text.splitlines()
    header = f'[evidence."{name}"]'
    start = next((i for i, line in enumerate(lines) if line.strip() == header), None)
    if start is not None:
        end = next((i for i in range(start + 1, len(lines)) if lines[i].lstrip().startswith("[")), len(lines))
        del lines[start:end]
    block = ["", header, f"basis = {json.dumps(basis)}", f"why = {json.dumps(why)}"]
    if source:
        block.append(f"source = {json.dumps(source)}")
    return "\n".join(lines).rstrip("\n") + "\n" + "\n".join(block) + "\n"


def find_profile(folder, identifiers, platform):
    for path in sorted(Path(folder).glob("*.toml")):
        if path.name in ("TEMPLATE.toml", "device-profile.toml"):
            continue
        table = tomllib.loads(path.read_text())
        match = table.get("match", {})
        if table.get("kind") == "model" and match.get("platform") == platform and set(match.get("models", [])) & set(identifiers):
            return path
    return None


def new_profile_text(device, platform):
    models = ", ".join(json.dumps(m) for m in device.identifiers)
    return (f'#:schema ../port/settings/profile.schema.json\nschema = 1\nname = {json.dumps(device.model)}\n'
            f'kind = "model"\nstatus = "provisional"\n\n[match]\nplatform = {json.dumps(platform)}\nmodels = [{models}]\n')


def apply_change(text, proposal, fit, report_name):
    """The profile text with the proposal's settings and one evidence entry for each."""
    keys = check.profile_keys()
    basis = "measured" if fit.confidence == "measured" else "assumed"
    for knob in sorted(proposal.settings):
        name = PROFILE_KEY.get(knob, knob)
        if name not in keys:
            raise Failure(f"{name} is not a profile setting")
        section, key = name.split(".", 1)
        text = set_value(text, section, key, toml_literal(keys[name][0], proposal.settings[knob]))
        cpu, gpu = proposal.effects.get(knob, (1.0, 1.0))
        why = (f"Predicted effect of {propose._percent(cpu)} CPU and {propose._percent(gpu)} GPU time a frame; "
               f"{proposal.fps_before:.0f} FPS before and {proposal.predicted_fps:.0f} FPS after "
               f"at the {proposal.target_hz} FPS target ({proposal.bound}-bound, {fit.confidence} confidence).")
        text = set_evidence(text, name, basis, why, f"cost-model fit from report {report_name}" if basis == "measured" else None)
    return text


def fits_from(report, report_name):
    fits = cost.fit(cost.runs_from_report(report, report_name))
    if cost.MODEL_TOML.exists():
        known = {(f.device, f.workload) for f in fits if f.confidence != "none"}
        fits += [f for f in cost.read_model(cost.MODEL_TOML) if (f.device, f.workload) not in known]
    return [f for f in fits if f.confidence != "none"]


def shot_settings(settings):
    return [item for knob in sorted(settings) for item in ("--set", f"{SCREENSHOT_KEYS.get(knob, PROFILE_KEY.get(knob, knob))}={settings[knob]}")]


def render_screenshots(label, sets, folder, options, run):
    """Render one frame of the scene for each named settings set in one runner job; returns {name: picture}.

    The job builds the Mac app when the worktree has none, then runs the game once per set."""
    import shlex
    remote = f"build/profile-pr/{label}"
    scene = (["--set", f'debug.input_replay="{options.recording}"'] if options.recording
             else ["--init", f"map_name {options.map}"])
    steps = ['test -d "$(python3 -c \'import sys;sys.path.insert(0,"tools");import mac_run;print(mac_run.native_app_default())\')" '
             "|| python3 tools/ios_build.py --mac"]
    for name, settings in sets.items():
        command = ["python3", "tools/mac_run.py", "run", "--runner", "native", "--out", f"{remote}/{name}",
                   "--screenshot-every", options.frames, "--exit-after", options.exit_after,
                   "--set", "debug.fixed_timestep=true", *shot_settings(settings), *scene]
        for flag, value in (("--maps", options.maps), ("--app", options.app)):
            if value:
                command += [flag, value]
        steps.append(" ".join(shlex.quote(str(part)) for part in command))
    folder.mkdir(parents=True, exist_ok=True)
    result = run(RUN_JOB, "--kind", "runner", *(["--host", options.host] if options.host else []), "--fetch", f"{remote}={folder}", label, options.worktree,
                 "bash", "-ec", "\n".join(steps))
    if result.returncode:
        raise Failure(f"the screenshot job {label} failed (exit {result.returncode})")
    pictures = {}
    for name in sets:
        found = sorted(p for p in (folder / name).rglob("*") if p.suffix in (".png", ".bmp") and "benchmark" not in p.parts)
        if not found:
            raise Failure(f"the screenshot job {label} wrote no picture for the {name} settings")
        pictures[name] = found[-1]
    return pictures


def feed_targets(path, platform):
    table = tomllib.loads(Path(path).read_text())
    if platform == "mac":
        raise Failure("Mac profiles match by chip; publish their shards by hand")
    return [f"{platform}/{model}/_" for model in table["match"]["models"]]


def manifest_sequence(shard):
    if not Path(shard).exists():
        return 0
    manifest = json.loads(base64.b64decode(json.loads(Path(shard).read_text())["manifest"]))
    return int(manifest["sequence"])


def build_feed(targets, profiles, out, signing_key, run, repo=ROOT):
    """Sign a tester shard for each target; returns {target: (shard path, sequence)}."""
    shards = {}
    for target in targets:
        sequence = manifest_sequence(repo / FEED / "profiles" / f"{target}.json") + 1
        output = out / "feed" / f"{target}.json"
        output.parent.mkdir(parents=True, exist_ok=True)
        result = run(sys.executable, repo / "tools/data_pack.py", "--kind", "profiles", "--channel", "tester",
                     "--sequence", sequence, "--compatibility", f"device-profiles-1:{target}", "--input", profiles,
                     "--signing-key", signing_key, "--output", output)
        if result.returncode:
            raise Failure(f"data_pack.py failed for {target} (exit {result.returncode})")
        shards[target] = (output, sequence)
    return shards


def evidence_url(branch, name):
    return f"https://raw.githubusercontent.com/{REPOSITORY}/{branch}/{EVIDENCE_DIRECTORY}/{name}"


def body(proposal, device, current, fit, scene, branch, report_name, stem):
    lines = [propose.markdown(proposal, device, current).rstrip("\n"), "",
             f"Confidence: {fit.confidence}" + (" (predicted from another chip's measurements)" if fit.confidence != "measured" else "")
             + f". Scene: {scene}. Report: {report_name}.", "",
             "| Before | After |", "| --- | --- |",
             f"| ![Before]({evidence_url(branch, f'{stem}/before.png')}) | ![After]({evidence_url(branch, f'{stem}/after.png')}) |"]
    return "\n".join(lines) + "\n"


def build(options, run=None):
    run = run or run_command
    out = Path(options.out) if options.out else ROOT / "build/profile-pr" / slug(options.model)
    out.mkdir(parents=True, exist_ok=True)
    report_path = Path(options.report)
    report = json.loads(report_path.read_text())
    report_name = report.get("code") or report_path.parent.name or report_path.stem
    devices = predict.read_devices()
    device = next((d for d in devices if d.model == options.model), None)
    if device is None:
        raise Failure(f"{options.model} is not in cost-model/device-facts.csv")
    fits = fits_from(report, report_name)
    if not fits:
        raise Failure("the report has no usable offscreen_uncapped runs and no fit exists to anchor on")
    fit = predict.predicted_fit(device, fits, devices, workload=options.workload)
    ranking = propose.read_ranking()
    platform = report.get("device", {}).get("platform", "ios")

    work = out / "profiles"
    if work.exists():
        shutil.rmtree(work)
    shutil.copytree(PROFILES, work, ignore=shutil.ignore_patterns("README.md"))
    target = find_profile(work, device.identifiers, platform)
    text = target.read_text() if target else new_profile_text(device, platform)
    target = target or work / f"{slug(device.model)}.toml"
    # a knob the profile leaves alone is at the best value on its ladder; the profile's own values replace those
    current = {knob: table["order"][0] for knob, table in ranking.get(options.output_class, {}).items()
               if isinstance(table, dict) and knob not in propose.FIXED}
    existing = tomllib.loads(text)
    for knob in list(current):
        section, _, key = PROFILE_KEY.get(knob, knob).partition(".")
        if key in existing.get(section, {}):
            value = existing[section][key]
            current[knob] = str(value).lower() if isinstance(value, bool) else str(value)
    proposal = propose.propose(fit, device, options.workload, current, ranking, options.output_class)
    description = propose.markdown(proposal, device, current)
    (out / "proposal.md").write_text(description)
    if proposal.kind != "change" or not proposal.settings:
        print(f"{proposal.kind if proposal.settings or proposal.kind != 'change' else 'frame-rate target only'}: no pull request ({out / 'proposal.md'})")
        return 0

    target.write_text(apply_change(text, proposal, fit, report_name))
    result = run(sys.executable, ROOT / "tools/profile_check.py", target)
    if result.returncode:
        raise Failure("tools/profile_check.py rejected the changed profile; no pull request")

    after_settings = {**current, **proposal.settings}
    shots = out / "screenshots"
    stem = target.stem
    evidence = out / "evidence" / stem
    evidence.mkdir(parents=True, exist_ok=True)
    pictures = render_screenshots(f"profile-{stem}", {"before": current, "after": after_settings}, shots, options, run)
    for name, picture in pictures.items():
        shutil.copyfile(picture, evidence / f"{name}.png")

    shards = {}
    if options.signing_key:
        shards = build_feed(feed_targets(target, platform), work, out, options.signing_key, run)
    elif not options.dry_run:
        raise Failure("a pull request needs --signing-key to regenerate the tester shards")
    else:
        print("no --signing-key: the tester shards were not regenerated")

    branch = f"profile/{stem}"
    text = body(proposal, device, current, fit, options.workload, branch, report_name, stem)
    (out / "body.md").write_text(text)
    if options.dry_run:
        print(f"dry run: profile, screenshots and body are in {out}")
        return 0
    return open_pull_request(options, out, target, evidence, shards, branch, stem, device, run)


def open_pull_request(options, out, target, evidence, shards, branch, stem, device, run):
    repo = Path(options.worktree)
    paths = [Path("profiles") / target.name, EVIDENCE_DIRECTORY / stem / "before.png", EVIDENCE_DIRECTORY / stem / "after.png"]
    shutil.copyfile(target, repo / paths[0])
    (repo / EVIDENCE_DIRECTORY / stem).mkdir(parents=True, exist_ok=True)
    for name in ("before", "after"):
        shutil.copyfile(evidence / f"{name}.png", repo / EVIDENCE_DIRECTORY / stem / f"{name}.png")
    receipt = json.loads((repo / RECEIPT).read_text())
    for shard, (path, _) in shards.items():
        destination = FEED / "profiles" / f"{shard}.json"
        (repo / destination).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, repo / destination)
        receipt["assets"][str(destination)] = hashlib.sha256(path.read_bytes()).hexdigest()
        paths.append(destination)
    (repo / RECEIPT).write_text(json.dumps(receipt, sort_keys=True, separators=(",", ":")))
    paths.append(RECEIPT)
    if run(sys.executable, repo / "tools/profile_check.py").returncode:
        raise Failure("tools/profile_check.py rejected the profile folder; nothing was committed")
    title = f"Update the {device.model} profile"
    steps = [("git", "-C", repo, "switch", "--create", branch),
             ("git", "-C", repo, "add", "--", *paths),
             ("git", "-C", repo, "commit", "--message", f"📦️ build(profiles): {title.lower()} from a benchmark report"),
             ("git", "-C", repo, "push", "--set-upstream", "origin", branch),
             ("gh", "pr", "create", "--repo", REPOSITORY, "--head", branch, "--title", title, "--body-file", out / "body.md")]
    for step in steps:
        if run(*step).returncode:
            raise Failure(f"{step[0]} failed; stopped before the pull request")
    return 0


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", help="device model name from cost-model/device-facts.csv")
    parser.add_argument("--report", required=True, help="a benchmark report.json to fit and propose from")
    parser.add_argument("--dry-run", action="store_true", help="write everything to --out and open no pull request")
    parser.add_argument("--out", help="folder for the changed profile, screenshots, shards and body (default build/profile-pr/MODEL)")
    parser.add_argument("--workload", default="gameplay")
    parser.add_argument("--output-class", default="handheld", choices=["handheld", "tv"])
    parser.add_argument("--signing-key", help="the tester feed's signing seed file")
    parser.add_argument("--worktree", default=str(ROOT), help="worktree the runner job and the pull request use")
    parser.add_argument("--map", default="a10", help="map for the screenshots when no --recording is given")
    parser.add_argument("--recording", help="input recording the screenshots replay")
    parser.add_argument("--frames", type=int, default=600, help="the frame whose picture is kept")
    parser.add_argument("--exit-after", type=float, default=20.0, help="seconds of game time before the run ends")
    parser.add_argument("--maps", help="extracted maps folder for the runner")
    parser.add_argument("--app", help="native runner app")
    parser.add_argument("--host", help="run-job.sh host for the screenshot job (default: its own choice)")
    options = parser.parse_args(arguments)
    try:
        return build(options)
    except Failure as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
