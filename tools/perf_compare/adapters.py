"""Black-box Mac adapters. Raw logs stay in the private output directory."""
from __future__ import annotations

import json
import os
from pathlib import Path
import plistlib
import re
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request


class PermissionDenied(RuntimeError):
    """A security setting requires the operator; never repair it automatically."""


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, timeout=120, **kwargs)
    if result.returncode:
        message = result.stderr or result.stdout
        if re.search(r'not authorized|not permitted|permission denied|remote automation.*(disabled|enable)|must enable', message, re.I):
            raise PermissionDenied(f'{command!r}: {message.strip()}')
        raise RuntimeError(f'{command!r}: {message.strip()}')
    return result.stdout


def thermal():
    text = run(['/usr/bin/osascript', '-l', 'JavaScript', '-e',
                "ObjC.import('Foundation'); Number($.NSProcessInfo.processInfo.thermalState)"]).strip()
    state = {'0': 'nominal', '1': 'fair', '2': 'serious', '3': 'critical'}.get(text, 'unknown')
    return {'state': state, 'pmset': run(['pmset', '-g', 'therm']).strip()}


def host_state(check_load=True):
    state = run(['ioreg', '-n', 'Root', '-d', '1'])
    if re.search(r'CGSSessionScreenIsLocked[^\n]*Yes', state):
        raise RuntimeError('display-locked')
    if check_load:
        started = time.monotonic()
        before = cpu_snapshot()
        time.sleep(2)
        after = cpu_snapshot()
        if not before or not after:
            raise RuntimeError('cpu-telemetry-unavailable')
        cores = background_cpu(before, after, set()) / (time.monotonic() - started)
        if cores > 1.0:
            raise RuntimeError('host-not-quiet')
    return {'thermal': thermal(), 'load': os.getloadavg()[0]}


def executable(app):
    with (app / 'Contents/Info.plist').open('rb') as stream:
        return app / 'Contents/MacOS' / plistlib.load(stream)['CFBundleExecutable']


def frontmost(pid, activate=False):
    script = ("ObjC.import('AppKit');var a=$.NSRunningApplication.runningApplicationWithProcessIdentifier(%d);"
              "%s var f=$.NSWorkspace.sharedWorkspace.frontmostApplication;"
              "JSON.stringify({pid:f.processIdentifier});") % (pid, "if(a)a.activateWithOptions(3);" if activate else "")
    deadline = time.monotonic() + 5
    while True:
        value = json.loads(run(['/usr/bin/osascript', '-l', 'JavaScript', '-e', script]))
        if value['pid'] == pid:
            return
        if not activate or time.monotonic() >= deadline:
            raise RuntimeError('app-not-frontmost')
        time.sleep(0.1)


def capture_window(pid, output):
    script = ("ObjC.import('CoreGraphics');"
              "var l=ObjC.deepUnwrap(ObjC.castRefToObject($.CGWindowListCopyWindowInfo(1,0)));"
              "JSON.stringify(l.filter(w=>w.kCGWindowOwnerPID==%d&&w.kCGWindowLayer==0)"
              ".map(w=>({id:w.kCGWindowNumber,area:w.kCGWindowBounds.Width*w.kCGWindowBounds.Height})));" % pid)
    windows = json.loads(run(['/usr/bin/osascript', '-l', 'JavaScript', '-e', script]))
    if not windows:
        raise RuntimeError('app-window-unavailable')
    owned = max(windows, key=lambda window: window['area'])
    run(['/usr/sbin/screencapture', '-x', '-o', '-l', str(owned['id']), str(output)])


def qualification(target, scenario, display_hz=None):
    proof = target.get('qualification', {})
    matched = bool(target.get('sha256')) and proof.get('sha256') == target.get('sha256')
    if display_hz is not None:
        matched = matched and proof.get('display_hz') == display_hz
    settings = proof.get('scenarios', {}).get(scenario, {}) if matched else {}
    if not re.fullmatch(r'[0-9]{2,5}×[0-9]{2,5}', str(settings.get('resolution', ''))):
        settings = {}
    if settings and not re.fullmatch(r'[0-9]{1,3}|display|browser', str(settings.get('cap', ''))):
        settings = {}
    return {'scene_verified': bool(settings.get('resolution') and settings.get('cap') and proof.get('display_hz')),
            'resolution': settings.get('resolution', 'effective resolution unverified'),
            'cap': settings.get('cap', 'effective cap unverified')}


def cpu_snapshot():
    """Reuse the earlier comparison's outside-process cumulative CPU-time sampling."""
    totals = {}
    for line in run(['ps', '-A', '-o', 'pid=,time=']).splitlines():
        fields = line.split()
        if len(fields) != 2:
            continue
        parts = fields[1].split(':')
        seconds = float(parts[-1]) + sum(float(value) * 60 ** (index + 1)
                                         for index, value in enumerate(reversed(parts[:-1])))
        totals[int(fields[0])] = seconds
    return totals


def background_cpu(before, after, owned):
    return sum(max(0, seconds - before.get(pid, 0)) for pid, seconds in after.items()
               if pid not in owned)


def native(target, scenario, out, config):
    """Reuse isolated data/config and console readiness from the earlier Mac experiments."""
    app = Path(target['app'])
    binary = executable(app)
    data = out / 'data'
    data.mkdir()
    maps = Path(config['maps'])
    if not all((maps / name).is_file() for name in ('ui.map', scenario + '.map')):
        raise FileNotFoundError('maps')
    (data / 'maps').symlink_to(maps, target_is_directory=True)
    saves = data / 'saves'
    saves.mkdir()
    with socket.socket() as port_socket:
        port_socket.bind(('127.0.0.1', 0))
        port = port_socket.getsockname()[1]
    display = ('renderer = "metal"\n' if target['id'] == 'visr' else '')
    display += 'fullscreen = false\nscreen_width = 1280\nscreen_height = 720\nrender_height = 720\n'
    display += 'frame_rate = "60"\nframe_limit = 60\nvsync = true\ninterpolation = true\n'
    text = ('[display]\n' + display + '[audio]\nenabled = false\n[network]\nonline = false\n'
            '[update]\nauto = false\n[community_maps]\nauto_download = false\n'
            '[timer_audio]\nauto_download = false\n[discord]\napplication_id = \"\"\n'
            '[debug]\ntelnet_console = true\ntelnet_console_port = ' + str(port) + '\n')
    (data / 'config.toml').write_text(text)
    (saves / 'config.toml').write_text(text)
    level = scenario if target['id'] == 'visr' else 'levels\\' + scenario + '\\' + scenario
    (data / 'init.txt').write_text('game_difficulty_set normal\nmap_name ' + level + '\n')
    env = {**os.environ, 'HALO_DATA_ROOT': str(data), 'HALO_SAVE_ROOT': str(saves),
           'HALO_HOST_DISPLAY': '1280x720@1', 'HALO_RUNNER': '1', 'MTL_HUD_ENABLED': '1',
           'MTL_HUD_LOG_ENABLED': '1'}
    # Execute only the published bundle's declared host. Never read its code.
    stderr = (out / 'app.log').open('w')
    process = subprocess.Popen([str(binary), *target.get('args', [])], cwd=app.parent, env=env,
                               stdout=stderr, stderr=stderr)
    connection = None
    logger = None
    log_stream = None
    try:
        deadline = time.monotonic() + config.get('startup_seconds', 90)
        while time.monotonic() < deadline and process.poll() is None:
            try:
                connection = socket.create_connection(('127.0.0.1', port), 1)
                connection.settimeout(1)
                break
            except OSError:
                time.sleep(0.5)
        if connection is None:
            raise RuntimeError('console-unavailable')
        ready = False
        while time.monotonic() < deadline and process.poll() is None:
            connection.sendall(b'(if (> (list_count (players)) 0) (print "PERF_READY"))\n')
            try:
                if b'PERF_READY' in connection.recv(65536):
                    ready = True
                    break
            except socket.timeout:
                pass
        if not ready:
            raise RuntimeError('scenario-not-ready')
        frontmost(process.pid, activate=True)
        time.sleep(config.get('warmup_seconds', 15))
        host_state(check_load=False)
        if config.get('capture_windows'):
            capture_window(process.pid, out / 'qualification.png')
        before = thermal()
        log_stream = (out / 'hud.log').open('w')
        cpu_before = cpu_snapshot()
        measured_start = time.monotonic()
        logger = subprocess.Popen(['/usr/bin/log', 'stream', '--style', 'ndjson', '--level', 'debug',
                                   '--predicate', f'processIdentifier == {process.pid} AND subsystem == "com.apple.metal.hud"'],
                                  stdout=log_stream, stderr=subprocess.STDOUT)
        time.sleep(config.get('seconds', 60))
        if process.poll() is not None:
            raise RuntimeError('app-exited')
        host_state(check_load=False)
        background = background_cpu(cpu_before, cpu_snapshot(), {process.pid})
        elapsed = time.monotonic() - measured_start
        (out / 'background-cpu.json').write_text(json.dumps({'seconds': background, 'window': elapsed}))
        if background / elapsed > config.get('max_background_cores', 1.0):
            raise RuntimeError('host-not-quiet')
        frontmost(process.pid)
        return {'thermal': before['state'] + ' → ' + thermal()['state'], **qualification(target, scenario, config.get('display_hz', 60))}
    finally:
        if connection:
            connection.close()
        for child in (logger, process):
            if child is not None and child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
        if log_stream:
            log_stream.close()
        stderr.close()


class WebDriver:
    def __init__(self, port):
        self.url = f'http://127.0.0.1:{port}'
        self.session = ''

    def request(self, method, path, value=None):
        req = urllib.request.Request(self.url + path, method=method,
                                     data=json.dumps(value).encode() if value is not None else None,
                                     headers={'Content-Type': 'application/json'})
        try:
            with urllib.request.urlopen(req, timeout=100) as response:
                result = json.load(response)['value']
        except urllib.error.HTTPError as error:
            text = error.read().decode()
            if re.search(r'not authorized|not permitted|allow remote automation|enable.*automation', text, re.I):
                raise PermissionDenied(f'{method} {path}: {text}') from error
            raise RuntimeError(text) from error
        if isinstance(result, dict) and result.get('error'):
            raise RuntimeError(str(result))
        return result

    def command(self, path, value=None, method='POST'):
        return self.request(method, '/session/' + self.session + path, value)


def safari(target, scenario, out, config):
    with socket.socket() as port_socket:
        port_socket.bind(('127.0.0.1', 0))
        port = port_socket.getsockname()[1]
    driver_log = (out / 'webdriver.log').open('w')
    process = subprocess.Popen(['/usr/bin/safaridriver', '--port', str(port)], stdout=driver_log, stderr=driver_log)
    driver = WebDriver(port)
    try:
        deadline = time.monotonic() + 10
        while True:
            try:
                driver.request('GET', '/status')
                break
            except urllib.error.URLError:
                if time.monotonic() > deadline:
                    raise RuntimeError('webdriver-startup-failed')
                time.sleep(0.2)
        caps = {'browserName': 'safari', 'platformName': 'mac'}
        # Phase 2 capability overrides are private and require an assigned device slot.
        caps.update(target.get('capabilities', {}))
        if caps['platformName'].lower() != 'mac' and not config.get('device_slot'):
            raise ValueError('device-slot-required')
        session = driver.request('POST', '/session', {'capabilities': {'alwaysMatch': caps}})
        driver.session = session['sessionId']
        driver.command('/url', {'url': target['url']})
        # Use only UI selectors/operator-supplied setup. Never inspect downloaded source.
        setup = target.get('setup', {}).get(scenario, [])
        for action in setup:
            deadline = time.monotonic() + config.get('startup_seconds', 90)
            while True:
                try:
                    element = driver.command('/element', {'using': 'css selector', 'value': action['selector']})
                    break
                except PermissionDenied:
                    raise
                except RuntimeError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError('scenario-not-ready')
                    time.sleep(0.5)
            element_id = element['element-6066-11e4-a52e-4f735466cecf']
            driver.command('/element/' + element_id + ('/value' if 'text' in action else '/click'),
                           {'text': action['text']} if 'text' in action else {})
        if not target.get('ready_selector'):
            raise RuntimeError('browser-game-setup-required')
        deadline = time.monotonic() + config.get('startup_seconds', 90)
        while True:
            try:
                driver.command('/element', {'using': 'css selector', 'value': target['ready_selector']})
                break
            except PermissionDenied:
                raise
            except RuntimeError:
                if time.monotonic() >= deadline:
                    raise RuntimeError('scenario-not-ready')
                time.sleep(0.5)
        time.sleep(config.get('warmup_seconds', 15))
        host_state(check_load=False)
        driver.command('/timeouts', {'script': (config.get('seconds', 60) + 10) * 1000})
        script = '''const done = arguments[arguments.length - 1];
const stamps = []; const start = performance.now(); let hidden = document.hidden;
function frame(t) { stamps.push(t); hidden ||= document.hidden;
  if (t - start >= argumentsDuration) done({timestamps: stamps, hidden});
  else requestAnimationFrame(frame); }
const argumentsDuration = arguments[0]; requestAnimationFrame(frame);'''
        result = driver.command('/execute/async', {'script': script, 'args': [config.get('seconds', 60) * 1000]})
        (out / 'raf.json').write_text(json.dumps(result))
        if result['hidden']:
            raise RuntimeError('browser-hidden')
        host_state(check_load=False)
        return {'thermal': thermal()['state'], **qualification(target, scenario, config.get('display_hz', 60))}
    finally:
        original_error = sys.exc_info()[1]
        cleanup_denied = None
        try:
            if driver.session:
                driver.command('', method='DELETE')
        except Exception as cleanup_error:
            if isinstance(cleanup_error, PermissionDenied):
                cleanup_denied = cleanup_error
            # Keep the original exception and still reap the owned driver.
            (out / 'cleanup-error.txt').write_text(str(cleanup_error))
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        driver_log.close()
        if cleanup_denied is not None and not isinstance(original_error, PermissionDenied):
            raise cleanup_denied
