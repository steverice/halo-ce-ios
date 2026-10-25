#!/usr/bin/env python3
"""Compare two desktop peers using VISR's observable network-test state.

Exit 0: PASS, 1: FAIL (persistent divergent state), 2: ERROR (insufficient
or invalid evidence, startup failure, crash). This is a sampled netcode
regression check, not a deterministic simulation checksum or cross-play proof.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
import math
import os
from pathlib import Path
import re
import signal
import select
import socket
import subprocess
import sys
import time

TICK = re.compile(r'network test: tick (\d+)(.*)')
PLAYER = re.compile(r' player (\d+): (.*?)(?= player \d+:| \||$)')
LIVE = re.compile(r'^\(([^)]+)\) h(\S+)/(\S+)(.*?) g(\d+)/(\d+) w(.*?) s(-?\d+) k(-?\d+) d(-?\d+) f(-?\d+) t(-?\d+) m(-?\d+)$')
DEAD = re.compile(r'^dead s(-?\d+) k(-?\d+) d(-?\d+) f(-?\d+) t(-?\d+) m(-?\d+)$')
FIELDS = ('score', 'kills', 'deaths', 'friendly_fire', 'team', 'machine')
CODES = {'PASS': 0, 'FAIL': 1, 'ERROR': 2}
STOP_SIGNAL = 0


@dataclass
class Result:
    status: str
    detail: str
    samples: int = 0
    seconds: float = 0


def combine(results):
    return next((r for status in ('FAIL', 'ERROR', 'PASS') for r in results if r.status == status),
                Result('ERROR', 'no runs'))


def validate_players(players):
    if not isinstance(players, dict) or len(players) < 2 or any(type(k) is not int or k < 0 for k in players):
        raise ValueError('expected at least two identified players')
    def number(value):
        return type(value) in (int, float) and math.isfinite(value)
    def vector(value, size):
        return isinstance(value, list) and len(value) == size and all(number(v) for v in value)
    for state in players.values():
        if not isinstance(state, dict) or type(state.get('alive')) is not bool:
            raise ValueError('missing life state')
        required = set(FIELDS) | {'alive'}
        if state['alive']:
            required |= {'position', 'health', 'shield', 'grenades', 'weapons'}
        if state.keys() != required or not all(type(state[f]) is int for f in FIELDS):
            raise ValueError('incomplete or invalid player state')
        if state['alive']:
            if not vector(state['position'], 3) or not all(number(state[f]) for f in ('health', 'shield')):
                raise ValueError('nonfinite or invalid vitality/position')
            if not vector(state['grenades'], 2) or any(type(g) is not int or g < 0 for g in state['grenades']):
                raise ValueError('invalid grenade counts')
            if not isinstance(state['weapons'], list) or any(not vector(w, 2) or any(type(v) is not int or v < 0 for v in w) for w in state['weapons']):
                raise ValueError('invalid inventory')


def _parse_log(text, samples):
    for line in text.splitlines():
        if 'lockstep: ' in line:
            record = json.loads(line.split('lockstep: ', 1)[1])
            if not isinstance(record, dict) or not isinstance(record.get('players'), dict):
                raise ValueError('invalid normalized record')
            tick, players = record['tick'], {int(k): v for k, v in record['players'].items()}
            if len(players) != len(record['players']):
                raise ValueError('duplicate player identity')
        else:
            match = TICK.search(line)
            if not match:
                continue
            tick, body = int(match[1]), match[2]
            players = {}
            for player in PLAYER.finditer(body):
                index, state = int(player[1]), player[2].strip()
                live, dead = LIVE.fullmatch(state), DEAD.fullmatch(state)
                if index in players or not (live or dead):
                    raise ValueError(f'malformed player at tick {tick}')
                if live:
                    values = {'alive': True, 'position': [float(v) for v in live[1].split()],
                              'health': float(live[2]), 'shield': float(live[3]),
                              'grenades': [int(live[5]), int(live[6])],
                              'weapons': [[int(tag, 16), int(rounds)] for tag, rounds in
                                          re.findall(r'\b([0-9a-f]+):(\d+)\b', live[7])]}
                    values.update(zip(FIELDS, map(int, live.groups()[7:])))
                else:
                    values = {'alive': False, **dict(zip(FIELDS, map(int, dead.groups())))}
                players[index] = values
        if type(tick) is not int or tick < 0:
            raise ValueError('invalid tick')
        validate_players(players)
        if samples and tick < max(samples):
            raise ValueError('game clock rewound')
        if tick in samples and samples[tick] != players:
            raise ValueError(f'conflicting records at tick {tick}')
        samples[tick] = players
    return samples


def parse_log(text):
    samples = {}
    try:
        return _parse_log(text, samples)
    except (ValueError, KeyError, TypeError, OverflowError) as error:
        error.samples = samples
        raise


def differences(a, b, position, vitality, rounds):
    for player in sorted(a.keys() & b.keys()):
        for field in a[player].keys() | b[player].keys():
            x, y = a[player].get(field), b[player].get(field)
            if field == 'position' and x is not None and y is not None:
                different = math.dist(x, y) > position
            elif field in ('health', 'shield') and x is not None and y is not None:
                different = abs(x - y) > vitality
            elif field == 'weapons' and x is not None and y is not None:
                different = len(x) != len(y) or any(p[0] != q[0] or abs(p[1] - q[1]) > rounds for p, q in zip(x, y))
            else:
                different = x != y
            if different:
                yield player, field


def judge(host, client, duration, returncodes=(0, 0), position=1.0, vitality=0.15, rounds=2, persistence=3):
    parsed, errors = [], []
    for text in (host, client):
        try:
            parsed.append(parse_log(text))
        except (ValueError, KeyError, TypeError, OverflowError) as error:
            parsed.append(error.samples)
            errors.append(str(error))
    a, b = parsed
    ticks = sorted(a.keys() & b.keys())
    streaks = {}
    previous = None
    for tick in ticks:
        if previous is not None and tick - previous > 60:
            streaks = {}
        previous = tick
        current = set(differences(a[tick], b[tick], position, vitality, rounds))
        streaks = {key: (streaks.get(key, (0, tick))[0] + 1, streaks.get(key, (0, tick))[1]) for key in current}
        for (player, field), (count, first) in sorted(streaks.items()):
            if count >= persistence:
                return Result('FAIL', f'player {player}, tick {first}, field {field} diverged for {count} samples', len(ticks))
    if errors:
        return Result('ERROR', 'invalid state log: ' + '; '.join(errors), len(ticks))
    if any(code not in (0, None) for code in returncodes):
        return Result('ERROR', f'peer exited unexpectedly: {returncodes}', len(ticks))
    if not ticks or any(a[t].keys() != b[t].keys() for t in ticks):
        return Result('ERROR', 'missing matched ticks or player roster', len(ticks))
    for evidence in (a, b):
        spawned = {p for tick in ticks for p, state in evidence[tick].items() if state['alive']}
        if len(spawned) < 2:
            return Result('ERROR', 'both peers did not spawn players', len(ticks))
    seconds = (ticks[-1] - ticks[0]) / 30
    if seconds < duration or len(ticks) < math.ceil(duration) + 1 or any(y - x > 60 for x, y in zip(ticks, ticks[1:])):
        return Result('ERROR', f'insufficient continuous matched gameplay: {seconds:g}/{duration:g} seconds', len(ticks), seconds)
    return Result('PASS', 'sampled player state stayed within tolerances', len(ticks), seconds)


@dataclass
class Peer:
    process: subprocess.Popen


def start_peer(command, directory, environment, stream):
    env = {k: v for k, v in os.environ.items() if not k.startswith('HALO_')}
    env.update(environment)
    # The supervisor owns the game before it launches it. Parent death closes
    # this pipe even if it occurs during Popen or before registration.
    process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '--supervise', *command],
                               cwd=directory, env=env, stdin=subprocess.PIPE,
                               stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
    return Peer(process)


def kill_group(pid, sig):
    try:
        os.killpg(pid, sig)
    except ProcessLookupError:
        pass


def stop_peers(peers):
    for peer in peers:
        peer.process.stdin.close()
    deadline = time.monotonic() + 3
    for peer in peers:
        try:
            peer.process.wait(timeout=max(0.01, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            kill_group(peer.process.pid, signal.SIGKILL)
            peer.process.wait()


def supervise(command):
    stopping = False
    def stop(number, frame):
        nonlocal stopping
        stopping = True
    for number in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(number, stop)
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL)
    while not stopping and process.poll() is None:
        if select.select([sys.stdin], [], [], 0.1)[0] and not os.read(sys.stdin.fileno(), 1):
            stopping = True
    # The supervisor shares the peer's group and ignores TERM itself until
    # the KILL deadline, so a stubborn child cannot outlive a killed checker.
    kill_group(os.getpgrp(), signal.SIGTERM)
    time.sleep(2)
    kill_group(os.getpgrp(), signal.SIGKILL)


def check_log_destination(folder, spec):
    path = folder / spec.get('log', 'stderr.log')
    if path.exists() and path.stat().st_size:
        raise ValueError('preexisting adapter log cannot supply current evidence')
    return path.resolve()


def read_log(folder, spec):
    path = folder / spec.get('log', 'stderr.log')
    if not path.exists():
        return ''
    if spec.get('normalize'):
        command = [s.format(log=str(path), data=str(folder)) for s in spec['normalize']]
        return subprocess.run(command, capture_output=True, text=True, check=True, timeout=10).stdout
    return path.read_text(errors='replace')


def executable(path):
    path = path.resolve()
    return path / 'Contents/MacOS/VISR' if path.is_dir() else path


def prepare_peer(binary, folder, maps, role, map_name, duration, events, spec, addresses=('127.0.0.2', '127.0.0.3')):
    folder.mkdir(parents=True)
    (folder / 'maps').symlink_to(maps, target_is_directory=True)
    (folder / 'stderr.log').touch()
    # Configuration is private to the run; retail assets alone are shared.
    address, other = addresses if role == 'host' else addresses[::-1]
    settings = {'HALO_DATA_ROOT': str(folder), 'HALO_RUNNER': '1', 'HALO_NETWORK_TEST': f'host:{map_name}' if role == 'host' else 'join',
                'HALO_NET_ADDRESS': address, 'HALO_NET_BROADCAST': other,
                'HALO_NET_LAN_BROADCAST': 'off', 'HALO_NETWORK_TEST_START': '25',
                'HALO_NETWORK_TEST_SCORE': '10000', 'HALO_TEST_INPUT': 'bot:1' if role == 'host' else 'bot:2',
                'HALO_EXIT_AFTER': str(duration + 100), 'HALO_RENDER_HEIGHT': '540', 'HALO_MAX_FPS': '30',
                'HALO_LOG_STDERR': '1', 'HALO_DISPLAY_MODE': 'windowed'}
    if events:
        settings.update(HALO_NETWORK_TEST_SHOOT='7', HALO_NETWORK_TEST_KILL='23')
    (folder / 'config.toml').write_text(
        '[debug]\n' + f'network_test = {json.dumps(settings["HALO_NETWORK_TEST"])}\n'
        'network_test_start = 25.0\nnetwork_test_score = 10000\n'
        + f'test_input = {json.dumps(settings["HALO_TEST_INPUT"])}\n'
        + f'exit_after = {duration + 100}\n'
        + ('network_test_shoot = 7.0\nnetwork_test_kill = 23.0\n' if events else '')
        + '[network]\n' + f'address = "{address}"\nbroadcast = "{other}"\nlan_broadcast = "off"\n'
        + '[display]\nrenderer = "metal"\nrender_height = 540\nframe_rate = "30"\nmode = "windowed"\n')
    context = dict(binary=str(executable(binary)), data=str(folder), role=role, map=map_name,
                   duration=str(duration), address=address, other=other)
    command = [arg.format(**context) for arg in spec.get('launch', ['{binary}'])]
    settings.update({k: v.format(**context) for k, v in spec.get('env', {}).items()})
    return command, settings


class Interrupted(Exception):
    def __init__(self, number):
        self.number = number


def run_match(args, map_name, swapped):
    folder = args.output / f'{map_name}-{"ba" if swapped else "ab"}'
    peers, streams = [], []
    specs = [{}, args.adapter or {}]
    binaries = [args.build_a, args.build_b or args.build_a]
    if swapped:
        specs.reverse()
        binaries.reverse()
    try:
        destinations = [check_log_destination(folder / role, spec) for role, spec in zip(('host', 'client'), specs)]
        if destinations[0] == destinations[1]:
            raise ValueError('peers must have separate log files')
        for i, role in enumerate(('host', 'client')):
            data = folder / role
            command, environment = prepare_peer(binaries[i], data, args.maps, role, map_name, args.duration, args.events, specs[i], (args.host_address, args.client_address))
            stream = (data / 'console.log').open('w')
            streams.append(stream)
            peers.append(start_peer(command, data, environment, stream))
            if STOP_SIGNAL:
                raise Interrupted(STOP_SIGNAL)
        deadline = time.monotonic() + args.duration + args.startup_timeout
        result = Result('ERROR', 'startup/gameplay deadline exceeded')
        while time.monotonic() < deadline:
            if STOP_SIGNAL:
                raise Interrupted(STOP_SIGNAL)
            texts = [read_log(folder / role, spec) for role, spec in zip(('host', 'client'), specs)]
            codes = tuple(peer.process.poll() for peer in peers)
            result = judge(*texts, duration=args.duration, returncodes=codes,
                           position=args.position_tolerance, vitality=args.vitality_tolerance,
                           rounds=args.rounds_tolerance, persistence=args.persistence)
            if result.status != 'ERROR' or any(code is not None for code in codes):
                break
            time.sleep(1)
        return result
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        return Result('ERROR', str(error))
    finally:
        stop_peers(peers)
        for stream in streams:
            stream.close()


def host_orders(args):
    """Whether B hosts, for each run of a map: A, then B with --swap; B alone with --b-hosts."""
    return [True] if args.b_hosts else [False, True] if args.swap else [False]


def validate_adapter(spec):
    if spec is None:
        return
    if not isinstance(spec, dict) or set(spec) - {'launch', 'env', 'log', 'normalize'}:
        raise ValueError('unknown adapter fields')
    for name in ('launch', 'normalize'):
        if name in spec and (not isinstance(spec[name], list) or not spec[name] or
                             any(not isinstance(arg, str) or not arg for arg in spec[name])):
            raise ValueError(f'adapter {name} must be a nonempty argument array')
    if 'log' in spec and (not isinstance(spec['log'], str) or not spec['log']):
        raise ValueError('adapter log must be a nonempty path')
    for name, values, context in [('launch', spec.get('launch', []), dict.fromkeys(('binary', 'role', 'map', 'address', 'other', 'data', 'duration'), 'x')),
                                  ('normalize', spec.get('normalize', []), {'log': 'x', 'data': 'x'})]:
        try:
            for value in values:
                value.format(**context)
        except (KeyError, ValueError, IndexError, AttributeError) as error:
            raise ValueError(f'invalid adapter {name} placeholder: {error}') from error
    env = spec.get('env', {})
    if not isinstance(env, dict) or any(not isinstance(k, str) or not isinstance(v, str) for k, v in env.items()):
        raise ValueError('adapter env must map names to string values')
    try:
        for value in env.values():
            value.format(**dict.fromkeys(('binary', 'role', 'map', 'address', 'other', 'data', 'duration'), 'x'))
    except (KeyError, ValueError, IndexError, AttributeError) as error:
        raise ValueError(f'invalid adapter environment placeholder: {error}') from error


def positive(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError('must be finite and positive')
    return number


def nonnegative(value):
    number = float(value)
    if not math.isfinite(number) or number < 0:
        raise argparse.ArgumentTypeError('must be finite and non-negative')
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-a', required=True, type=Path, help='VISR native .app or desktop executable')
    parser.add_argument('--build-b', type=Path, help='second VISR or foreign desktop executable')
    parser.add_argument('--adapter', type=Path, help='JSON launch/log hooks for build B; see docs/net-lockstep.md')
    parser.add_argument('--maps', required=True, type=Path, help='retail maps directory (shared read-only)')
    parser.add_argument('--map', action='append', dest='levels', default=[])
    parser.add_argument('--duration', type=positive, default=120, help='matched gameplay seconds per map')
    parser.add_argument('--startup-timeout', type=positive, default=90)
    parser.add_argument('--position-tolerance', type=nonnegative, default=1.0)
    parser.add_argument('--vitality-tolerance', type=nonnegative, default=0.15)
    parser.add_argument('--rounds-tolerance', type=int, default=2)
    parser.add_argument('--persistence', type=int, default=3)
    parser.add_argument('--host-address', default='127.0.0.2')
    parser.add_argument('--client-address', default='127.0.0.3')
    parser.add_argument('--swap', action='store_true')
    parser.add_argument('--b-hosts', action='store_true', help='only B hosts (a foreign host of an older version)')
    parser.add_argument('--events', action='store_true')
    parser.add_argument('--output', required=True, type=Path, help='new result directory; never reused')
    args = parser.parse_args()
    if args.rounds_tolerance < 0 or args.persistence < 1:
        parser.error('rounds tolerance must be non-negative; persistence must be positive')
    if args.b_hosts and not args.build_b:
        parser.error('--b-hosts needs --build-b')
    args.levels = args.levels or ['bloodgulch']
    if any(not re.fullmatch(r'[a-zA-Z0-9_-]+', level) for level in args.levels) or len(set(args.levels)) != len(args.levels):
        parser.error('map names must be unique simple names')
    args.output, args.maps = args.output.resolve(), args.maps.resolve()
    results = []
    try:
        args.adapter = json.loads(args.adapter.read_text()) if args.adapter else None
        validate_adapter(args.adapter)
        if args.output.exists():
            raise ValueError('output directory already exists')
        if not args.maps.is_dir():
            raise ValueError('maps directory missing')
        if args.host_address == args.client_address or '127.0.0.1' in (args.host_address, args.client_address):
            raise ValueError('peers need distinct local IPv4 addresses other than 127.0.0.1')
        for address in (args.host_address, args.client_address):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                probe.bind((address, 0))
        for level in args.levels:
            if not (args.maps / f'{level}.map').is_file():
                raise ValueError(f'missing map: {level}')
        args.output.mkdir(parents=True)
        def interrupted(number, frame):
            global STOP_SIGNAL
            STOP_SIGNAL = number
        for number in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(number, interrupted)
        for level in args.levels:
            for swapped in host_orders(args):
                result = run_match(args, level, swapped)
                if STOP_SIGNAL:
                    raise Interrupted(STOP_SIGNAL)
                results.append(result)
                print(f'{result.status}: {level} ({"B hosts" if swapped else "A hosts"}): {result.detail}', flush=True)
        (args.output / 'result.json').write_text(json.dumps([asdict(r) for r in results], indent=2) + '\n')
        return CODES[combine(results).status]
    except Interrupted as error:
        print(f'ERROR: stopped by signal {error.number}', flush=True)
        return 128 + error.number
    except (OSError, ValueError, TypeError) as error:
        print(f'ERROR: {error}', flush=True)
        return 2


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == '--supervise':
        supervise(sys.argv[2:])
    else:
        sys.exit(main())
