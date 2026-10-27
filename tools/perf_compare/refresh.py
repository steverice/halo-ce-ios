#!/usr/bin/env python3
"""Refresh black-box comparisons, or deterministically render the public summary."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import fnmatch
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess
import sys
import time
import urllib.request
import urllib.error
import uuid

import adapters
import windows

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
SUMMARY = ROOT / 'tools/perf_compare/summary.json'
PAGE = ROOT / 'docs/benchmarks.md'


def select_release(releases, pattern):
    for release in releases:
        if release.get('draft'):
            continue
        assets = [asset for asset in release['assets'] if fnmatch.fnmatchcase(asset['name'], pattern)]
        if len(assets) > 1:
            raise ValueError('ambiguous release asset')
        if assets:
            return release['tag_name'], assets[0]
    raise ValueError('no matching release asset')


def digest(path):
    sha = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            sha.update(block)
    return sha.hexdigest()


def public_download(url, path=None):
    request = urllib.request.Request(url, headers={'User-Agent': 'VISR-performance-comparison'})
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            if path is None:
                return json.load(response)
            with path.open('wb') as stream:
                while block := response.read(1024 * 1024):
                    stream.write(block)
    except urllib.error.HTTPError as error:
        if error.code in (401, 403):
            raise adapters.PermissionDenied(f'GET {url}: HTTP {error.code} {error.reason}') from error
        raise


def release_metadata(repo):
    try:
        return json.loads(adapters.run(['gh', 'api', f'repos/{repo}/releases']))
    except adapters.PermissionDenied:
        raise
    except (RuntimeError, FileNotFoundError) as error:
        # Missing CLI login is a local prerequisite, not a rejected network request.
        # Never retry an authorization denial through a different transport.
        if not isinstance(error, FileNotFoundError) and 'To get started with GitHub CLI' not in str(error):
            raise
        return public_download(f'https://api.github.com/repos/{repo}/releases')


def release_download(repo, tag, asset, folder):
    try:
        adapters.run(['gh', 'release', 'download', tag, '--repo', repo, '--pattern', asset['name'],
                      '--dir', str(folder)])
    except adapters.PermissionDenied:
        raise
    except (RuntimeError, FileNotFoundError) as error:
        if not isinstance(error, FileNotFoundError) and 'To get started with GitHub CLI' not in str(error):
            raise
        public_download(asset['browser_download_url'], folder / asset['name'])


def fetch(target, cache):
    """Fetch binaries only. Include prereleases because some targets publish only those."""
    if target['id'] == 'visr':
        binary = adapters.executable(Path(target['app']))
        bundle = bundle_identity(Path(target['app']))
        return {'version': target.get('version') or bundle['version'], 'build': bundle['build'],
                'source': target.get('source', ''), 'release_kind': target.get('release_kind', ''),
                'artifact': target.get('artifact', ''), 'sha256': digest(binary)}
    if target['id'] == 'opence-web':
        path = cache / 'web.html'
        with urllib.request.urlopen(target['url'], timeout=60) as response:
            path.write_bytes(response.read())
        # Entry-document hash does not pin separately loaded WASM/JS; say so in the method.
        return {'version': target.get('version', 'hosted'), 'build': target.get('build', ''),
                'network_version': target.get('network_version', ''), 'release_kind': 'hosted web build',
                'artifact': 'hosted entry document', 'sha256': digest(path)}
    releases = release_metadata(target['repo'])
    tag, asset = select_release(releases, target['asset'])
    folder = cache / target['id'] / tag
    folder.mkdir(parents=True, exist_ok=True)
    archive = folder / asset['name']
    if not archive.exists():
        release_download(target['repo'], tag, asset, folder)
    identity = {'version': tag, 'sha256': digest(archive), 'release_kind': 'release',
                'artifact': 'release ' + asset['name'], 'source': release_commit(target['repo'], tag)}
    # Always launch the bundle from the release whose archive was hashed.
    dest = folder / 'unpacked'
    if not dest.exists():
        dest.mkdir()
        if archive.suffix == '.zip':
            adapters.run(['ditto', '-x', '-k', str(archive), str(dest)])
        elif archive.suffix == '.dmg':
            # hdiutil's plist lists the exact owned mount; detach it even when copying fails.
            import plistlib
            mounted = plistlib.loads(adapters.run(
                ['hdiutil', 'attach', '-readonly', '-nobrowse', '-plist', str(archive)]).encode())
            entities = [item for item in mounted['system-entities'] if 'mount-point' in item]
            try:
                for item in entities:
                    for app in Path(item['mount-point']).glob('*.app'):
                        adapters.run(['ditto', str(app), str(dest / app.name)])
            finally:
                for item in entities:
                    adapters.run(['hdiutil', 'detach', item['mount-point']])
        else:
            raise ValueError('unsupported release archive')
    apps = list(dest.glob('**/*.app'))
    apps = [app for app in apps if not any(parent.suffix == '.app' for parent in app.parents)]
    if len(apps) != 1:
        raise ValueError('release app bundle ambiguous or missing')
    target['app'] = str(apps[0])
    bundle = bundle_identity(apps[0])
    identity['build'] = bundle['build']
    # Some projects tag releases by build number; the bundle's short version is then the product version.
    if not re.fullmatch(r'v?[0-9]+(\.[0-9]+)+[a-z]?', tag) and bundle['version']:
        identity['version'] = bundle['version']
    return identity


def bundle_identity(app):
    """Version and build as the bundle declares them; reading Info.plist does not read the app's code."""
    import plistlib
    with (app / 'Contents/Info.plist').open('rb') as stream:
        info = plistlib.load(stream)
    return {'version': str(info.get('CFBundleShortVersionString', '')), 'build': str(info.get('CFBundleVersion', ''))}


def release_commit(repo, tag):
    """The commit a release tag names, from public metadata only."""
    try:
        ref = json.loads(adapters.run(['gh', 'api', f'repos/{repo}/git/ref/tags/{tag}']))
    except adapters.PermissionDenied:
        raise
    except (RuntimeError, FileNotFoundError):
        try:
            ref = public_download(f'https://api.github.com/repos/{repo}/git/ref/tags/{tag}')
        except Exception:
            return ''
    obj = ref.get('object', {})
    if obj.get('type') == 'tag':
        try:
            obj = json.loads(adapters.run(['gh', 'api', f'repos/{repo}/git/tags/{obj["sha"]}']))['object']
        except Exception:
            return ''
    sha = str(obj.get('sha', ''))
    return sha[:9] if re.fullmatch(r'[0-9a-f]{40}', sha) else ''


def host_os():
    try:
        build = adapters.run(['sw_vers', '-buildVersion']).strip()
    except (RuntimeError, FileNotFoundError):
        build = ''
    return f'macOS {platform.mac_ver()[0]} ({build})' if build else f'macOS {platform.mac_ver()[0]}'


def validate_config(config):
    if config.get('quality_preset', 'defaults') not in {'defaults', 'matched-stock'}:
        raise ValueError('unknown quality preset')
    if any(t['id'] == 'opence-web' and t.get('unavailable_reason') != 'safari-remote-automation-disabled'
           for t in config.get('targets', [])) and (config.get('frame_mode', 'capped') != 'capped' or
           config.get('native_delay_seconds', 0) != 0 or config.get('quality_preset', 'defaults') != 'defaults'):
        raise ValueError('Safari does not support native mode, delay, or quality overrides')
    if config.get('frame_mode', 'capped') not in {'capped', 'uncapped'}:
        raise ValueError('unknown frame mode')
    if not 0 <= config.get('native_delay_seconds', 0) <= 600:
        raise ValueError('native delay must be between 0 and 600 seconds')
    if config.get('rounds', 3) != 3:
        raise ValueError('three rounds are required')
    if config.get('seconds', 60) != 60 or config.get('warmup_seconds', 15) < 15:
        raise ValueError('60 seconds and at least 15 seconds warm-up are required')
    keys = windows.resolve(config)
    if config.get('native_delay_seconds', 0) and keys not in (['a10', 'b30'], ['b30'], ['c10', 'b30']):
        raise ValueError('a delay applies to the opening windows only')
    if config.get('cpu_policy') == 'baseline' and any(
            target['id'] == 'opence-web' and
            target.get('unavailable_reason') != 'safari-remote-automation-disabled'
            for target in config.get('targets', [])):
        raise ValueError('baseline CPU policy requires Safari targets to be skipped')
    if config.get('display_hz', 60) <= 0:
        raise ValueError('display rate must be positive')


def failure_code(error):
    if isinstance(error, FileNotFoundError):
        return 'missing-file'
    if isinstance(error, adapters.PermissionDenied):
        return 'permission-denied'
    message = str(error)
    known = {'host-not-quiet', 'display-locked', 'console-unavailable', 'scenario-not-ready',
             'browser-game-setup-required', 'app-exited', 'browser-hidden', 'no-intervals',
             'scene-unverified', 'short-window', 'app-not-frontmost', 'app-window-unavailable', 'resolution-mismatch'}
    return message if message in known else 'adapter-failed'


def page_text(summary):
    """The public page: charts from the committed results snapshot, then the methodology."""
    import charts
    from page import render_page
    return render_page(summary, charts.load_snapshot(charts.SNAPSHOT), charts.load_specs(charts.SPECS))


def write_outputs(page_path, summary, check=False):
    """The Markdown page and, for the default page, the Pages version with its copy of the results snapshot."""
    import charts
    import site_page
    from build_site import load_facts
    from markdown_it import MarkdownIt
    status = write_page(page_path, page_text(summary), check)
    if Path(page_path) == PAGE:
        rows, specs = charts.load_snapshot(charts.SNAPSHOT), charts.load_specs(charts.SPECS)
        legal = MarkdownIt('commonmark', {'html': False}).render(load_facts()['shared']['legal']).rstrip()
        (ROOT / 'docs/site/benchmarks').mkdir(exist_ok=True)
        status |= write_page(ROOT / 'docs/site/benchmarks.html', site_page.render_site_page(summary, rows, specs, legal), check)
        status |= write_page(ROOT / 'docs/site/benchmarks/results.csv', charts.SNAPSHOT.read_text(encoding='utf-8'), check)
    return status


def write_page(path, content, check=False):
    if check:
        if not path.exists() or path.read_text() != content:
            print(f'stale generated file: {path.name}', file=sys.stderr)
            return 1
        return 0
    path.write_text(content)
    return 0


def wait_quiet(seconds):
    deadline = time.monotonic() + seconds
    while True:
        try:
            adapters.host_state()
            return
        except adapters.PermissionDenied:
            raise
        except RuntimeError as error:
            if str(error) != 'host-not-quiet' or time.monotonic() >= deadline:
                raise
            time.sleep(min(5, max(0, deadline - time.monotonic())))


def bind_routed_visr(config):
    version = config.pop('routed_visr', None)
    if version:
        for target in config['targets']:
            if target['id'] == 'visr':
                build_root = Path(version['build_root']) if isinstance(version, dict) else ROOT
                target['app'] = str(build_root / 'build/macos/app-native/host/Release/VISR.app')
                if isinstance(version, dict):
                    for field in ('version', 'source', 'release_kind', 'artifact'):
                        if version.get(field):
                            target[field] = version[field]
                else:
                    target['version'] = 'main'
                    target['source'] = version[:9]
                    target['release_kind'] = 'pre-release main build'


def routed_source(route):
    """Resolve the measured VISR source: a release tag when the route names one, otherwise live main."""
    ref = route.get('ref', 'refs/heads/main')
    match = re.fullmatch(r'refs/tags/visr-v([0-9]+\.[0-9]+\.[0-9]+)', ref)
    if ref != 'refs/heads/main' and not match:
        raise ValueError('route ref must be refs/heads/main or a refs/tags/visr-vX.Y.Z release tag')
    if match and not re.fullmatch(r'[0-9]{8}\.[0-9]{1,6}', str(route.get('build_number', ''))):
        raise ValueError('a release route needs the release build number')
    lines = adapters.run(['git', 'ls-remote', 'https://github.com/steverice/visr.git', ref, ref + '^{}']).split('\n')
    fields = [line.split() for line in lines if line.strip()]
    shas = {item[1]: item[0] for item in fields if len(item) == 2}
    sha = shas.get(ref + '^{}', shas.get(ref)) or (fields[0][0] if fields and len(fields[0]) == 1 else '')
    if not sha:
        raise RuntimeError('route ref not found: ' + ref)
    if match:
        build = ['python3', 'tools/ios_build.py', '--native-mac', '--marketing-version', match.group(1),
                 '--build-number', str(route['build_number']), '--pgo', 'use', '--lto', 'full', '--host-pgo', 'use',
                 '--bink', 'on', '--render-validation', 'off', '--jobs', '4']
        identity = {'version': match.group(1), 'release_kind': 'release',
                    'artifact': f'ad hoc Mac build of tag visr-v{match.group(1)} with the release build number'}
    else:
        build = ['python3', 'tools/macos_build.py', '--renderer', 'metal', '--jobs', '4']
        identity = {'version': 'main', 'release_kind': 'pre-release main build', 'artifact': 'ad hoc Mac build of main'}
    return ref, sha, build, {**identity, 'source': sha[:9]}


def refresh(config, summary_path, page_path):
    from results import gpu_metrics, merge, metrics, parse_hud, parse_metal_trace, parse_metal_trace_gpu, parse_raf
    bind_routed_visr(config)
    validate_config(config)
    window_keys = windows.resolve(config)
    start = time.monotonic()
    out = Path(config['output']).resolve() / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    out.mkdir(parents=True)
    machine, os_name = config['public_machine'], host_os()
    summary = {'schema': 2, 'records': []}
    records = {}
    errors = []
    denied = None
    try:
        for target in config['targets']:
            record = {'port': target['id'], 'version': target.get('version', ''), 'build': '', 'source': '',
                      'sha256': '', 'status': 'unavailable', 'machine': machine, 'os': os_name, 'browser': '',
                      'date': datetime.now(timezone.utc).date().isoformat(),
                      'bundle_storage': target.get('bundle_storage', ''),
                      'method': 'Safari RAF callbacks (entry-document hash)' if target['id'] == 'opence-web'
                      else ('metalperftrace on-glass intervals' if config.get('native_method') == 'metalperftrace'
                            else 'Metal HUD present intervals'), 'reason': '', 'runs': []}
            if config.get('native_scenario') == 'opening-sequence':
                record['native_scenario'] = 'opening-sequence'
            if target['id'] == 'opence-web' and target.get('scenario_url'):
                record['web_load_seconds'] = target.get('load_seconds', 0)
            if config.get('cpu_policy') == 'baseline' and target['id'] != 'opence-web':
                record['cpu_policy'] = 'baseline'
            summary['records'].append(record)
            record['frame_mode'] = config.get('frame_mode', 'capped')
            record['native_delay_seconds'] = config.get('native_delay_seconds', 0)
            record['scenarios'] = window_keys
            record['quality_preset'] = config.get('quality_preset', 'defaults')
            record['warmup_seconds'] = config.get('warmup_seconds', 15)
            records[target['id']] = record
            if target.get('unavailable_reason') == 'safari-remote-automation-disabled':
                record['reason'] = target['unavailable_reason']
                continue
            try:
                # Release bundles can live apart from raw evidence, so every app loads from the same kind of storage.
                cache = Path(config['release_cache']) if config.get('release_cache') else out
                cache.mkdir(parents=True, exist_ok=True)
                record.update(fetch(target, cache))
                target['sha256'] = record['sha256']
                record['status'] = 'pending'
            except PermissionError as error:
                raise adapters.PermissionDenied(f'fetch {target["id"]}: {error}') from error
            except adapters.PermissionDenied:
                raise
            except Exception as error:
                record['reason'] = failure_code(error)
                errors.append({'target': target['id'], 'stage': 'fetch', 'error': str(error)})
        try:
            if config.get('cpu_policy') == 'baseline':
                adapters.host_state(check_load=False)
            else:
                wait_quiet(config.get('quiet_wait_seconds', 180))
        except adapters.PermissionDenied:
            raise
        except Exception as error:
            errors.append({'stage': 'quiet-preflight', 'error': str(error)})
            for record in summary['records']:
                if record['status'] == 'pending':
                    record.update(status='unavailable', reason=failure_code(error))
        targets = config['targets']
        for round_index in range(3):
            rotated = targets[round_index:] + targets[:round_index]
            for target in rotated:
                record = records[target['id']]
                if record['status'] == 'unavailable':
                    continue
                for scenario in window_keys:
                    if record['status'] == 'unavailable':
                        break
                    folder = out / f'{target["id"]}-{scenario}-{round_index + 1}'
                    folder.mkdir()
                    print(f'Round {round_index + 1}: {target["id"]} {scenario} '
                          f'{config.get("frame_mode", "capped")} +{config.get("native_delay_seconds", 0)} s', flush=True)
                    try:
                        before = adapters.host_state(check_load=False)
                        (folder / 'host-before.json').write_text(json.dumps(before))
                        adapter = adapters.safari if target['id'] == 'opence-web' else adapters.native
                        run_record = adapter(target, scenario, folder, config)
                        if target['id'] != 'opence-web' and run_record.get('scene_verified') and run_record['resolution'] != '1280×960':
                            raise RuntimeError('resolution-mismatch')
                        if target['id'] == 'opence-web':
                            intervals = parse_raf(json.loads((folder / 'raf.json').read_text())['timestamps'])
                        elif config.get('native_method') == 'metalperftrace':
                            intervals = parse_metal_trace(json.loads((folder / 'timeline.json').read_text()),
                                                          run_record['resolution'])
                        else:
                            intervals = parse_hud((folder / 'hud.log').read_text())
                        if not intervals:
                            raise RuntimeError('no-intervals')
                        measured = metrics(intervals, config.get('display_hz', 60))
                        if config.get('native_method') == 'metalperftrace' and target['id'] != 'opence-web':
                            measured.update(gpu_metrics(parse_metal_trace_gpu(json.loads((folder / 'timeline.json').read_text()))))
                        if measured['duration_s'] < 58:
                            raise RuntimeError('short-window')
                        if not run_record.pop('scene_verified'):
                            raise RuntimeError('scene-unverified')
                        for field in ('browser', 'version', 'build', 'source', 'release_kind', 'network_version'):
                            if run_record.get(field):
                                record[field] = run_record.pop(field)
                            run_record.pop(field, None)
                        record['runs'].append({**run_record, 'scenario': scenario, 'round': round_index + 1,
                                               'machine': machine, 'os': os_name,
                                               'metrics': measured, 'display_hz': config.get('display_hz', 60),
                                               'display_rate_basis': 'operator-qualified'})
                    except PermissionError as error:
                        raise adapters.PermissionDenied(f'run {target["id"]}: {error}') from error
                    except adapters.PermissionDenied:
                        raise
                    except Exception as error:
                        record['reason'] = failure_code(error)
                        if record['reason'] in {'console-unavailable', 'browser-game-setup-required', 'scene-unverified', 'no-intervals', 'host-not-quiet', 'display-locked', 'resolution-mismatch'}:
                            record['status'] = 'unavailable'
                        errors.append({'target': target['id'], 'scenario': scenario, 'round': round_index + 1,
                                       'error': str(error)})
            (out / 'errors.json').write_text(json.dumps(errors, indent=2))
        for record in summary['records']:
            if len(record['runs']) == 3 * len(window_keys):
                record['status'] = 'ok'
                record['reason'] = ''
            elif record['runs']:
                record['status'] = 'incomplete'
            else:
                record['status'] = 'unavailable'
    except adapters.PermissionDenied as error:
        denied = error
        errors.append({'permission_denied': str(error)})
        for record in summary['records']:
            if record['status'] == 'pending':
                record.update(status='unavailable', reason='permission-denied')
    finally:
        for record in summary['records']:
            record['matrix_seconds'] = round(time.monotonic() - start, 1)
        (out / 'errors.json').write_text(json.dumps(errors, indent=2))
        (out / 'errors.json').write_text(json.dumps(errors, indent=2))
        # The private copy is this matrix alone; the public summary keeps every earlier record.
        (out / 'matrix-summary.json').write_text(json.dumps(summary, indent=2, sort_keys=True) + '\n')
        retained = json.loads(summary_path.read_text()) if summary_path.exists() else {'schema': 2}
        merged = merge(retained, summary)
        summary_path.write_text(json.dumps(merged, indent=2, sort_keys=True) + '\n')
        write_outputs(page_path, merged)
        print(f'Refresh: {round(time.monotonic() - start, 1)} seconds; private evidence: {out}')
    if denied:
        print(f'STOP: permission system denied {denied}', file=sys.stderr)
        return 3
    return int(bool(errors) or any(record['status'] != 'ok' for record in summary['records']))


def routed_refresh(config, summary_path, page_path):
    """Serialize the reusable source cache while retaining unique result transfers."""
    import fcntl
    base = Path(config['route']['local_scratch'])
    base.mkdir(parents=True, exist_ok=True)
    with (base / 'refresh.lock').open('w') as lock:
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError('another refresh owns this source cache') from error
        return _routed_refresh(config, summary_path, page_path)


def _routed_refresh(config, summary_path, page_path):
    """Build verified live-main runtime sources, then measure through the dedicated host lock."""
    from results import merge
    started = time.monotonic()
    route = config.pop('route')
    transfer_id = uuid.uuid4().hex
    local = Path(route['local_scratch']) / transfer_id
    remote = Path(route['remote_scratch']) / transfer_id
    local.mkdir(parents=True, exist_ok=True)
    ref, live, build_command, identity = routed_source(route)
    adapters.run(['git', 'fetch', '--no-tags', 'https://github.com/steverice/visr.git', ref], cwd=ROOT)
    source = Path(route['local_scratch']) / 'build-source'
    if (source / '.git').exists():
        if adapters.run(['git', 'status', '--porcelain'], cwd=source).strip():
            raise RuntimeError('comparison source cache has local changes')
        adapters.run(['git', 'checkout', '--detach', live], cwd=source)
    else:
        adapters.run(['git', 'worktree', 'add', '--detach', str(source), live], cwd=ROOT)
    config['routed_visr'] = {**identity, 'build_root': str(Path(route['remote_scratch']) / 'build-source')}
    base = [route['router'], '--host', route['host']]
    # No timeout on queued jobs: the router owns queue attendance and keeps its receipt.
    build = [*base, '--kind', 'build', 'perf-compare-refresh-build', str(source), *build_command]
    result = subprocess.run(build, cwd=ROOT)
    if result.returncode:
        raise RuntimeError('routed build failed')
    config['output'] = str(remote / 'raw')
    # The router maps each local source path into its remote shim home for fetching.
    command = [*base, '--kind', 'runner', '--fetch', str(local / 'summary.json'),
               '--fetch', str(local / 'benchmarks.md'), 'perf-compare-refresh-matrix', str(ROOT),
               'caffeinate', '-d', 'python3', 'tools/perf_compare/refresh.py', 'refresh', '--config-json', json.dumps(config),
               '--summary', str(remote / 'summary.json'), '--output', str(remote / 'benchmarks.md')]
    result = subprocess.run(command, cwd=ROOT)
    if (local / 'summary.json').exists():
        summary = json.loads((local / 'summary.json').read_text())
        retained = json.loads(summary_path.read_text()) if summary_path.exists() else {'schema': 2}
        merged = merge(retained, summary)
        summary_path.write_text(json.dumps(merged, indent=2, sort_keys=True) + '\n')
        write_outputs(page_path, merged)
        print(f'Routed refresh: {round(time.monotonic() - started, 1)} seconds')
    elif result.returncode == 0:
        print('routed refresh failed to transfer a fresh summary', file=sys.stderr)
        return 1
    return result.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['refresh', 'render'])
    parser.add_argument('--config', type=Path)
    parser.add_argument('--config-json', help=argparse.SUPPRESS)
    parser.add_argument('--summary', type=Path, default=SUMMARY)
    parser.add_argument('--output', type=Path, default=PAGE)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    if args.command == 'render':
        return write_outputs(args.output, json.loads(args.summary.read_text()), args.check)
    if (args.config is None and args.config_json is None) or args.check:
        parser.error('refresh requires --config and does not accept --check')
    config = json.loads(args.config_json) if args.config_json else json.loads(args.config.read_text())
    if config.get('route'):
        return routed_refresh(config, args.summary, args.output)
    return refresh(config, args.summary, args.output)


if __name__ == '__main__':
    raise SystemExit(main())
