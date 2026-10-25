"""Asset-free checks for the network comparison tool."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

import pytest

import net_lockstep as net


def record(tick, x=1, health=1, score=0):
    return (f'network test: tick {tick} player 0: ({x:.3f} 2.000 3.000) h{health:.2f}/1.00 '
            f'g2/2 w a10/0 f10 l10/0 as0/0 st0 thr1.00 123:60 s{score} k0 d0 f0 t0 m0 '
            'player 1: (4.000 5.000 6.000) h1.00/1.00 g2/2 w a0/0 f0 l0/0 as0/0 st0 thr0.00 123:60 s0 k0 d0 f0 t1 m1 | items 4 | playing to 25 | local 0')


def logs(x=1):
    return '\n'.join(record(t, x) for t in range(30, 361, 30))


def test_parses_real_log_fields():
    sample = net.parse_log(record(30))[30][0]
    assert sample['position'] == [1, 2, 3]
    assert sample['health'] == 1
    assert sample['weapons'] == [[0x123, 60]]
    assert sample['score'] == 0
    assert net.parse_log(record(30).replace('player 1: (4.000 5.000 6.000) h1.00/1.00 g2/2 w a0/0 f0 l0/0 as0/0 st0 thr0.00 123:60', 'player 1: dead'))[30][1]['alive'] is False


def test_identical_evidence_passes():
    assert net.judge(logs(), logs(), duration=10).status == 'PASS'


def test_missing_or_short_evidence_is_error():
    for other in ['', record(30), logs().replace('player 1:', 'player 2:')]:
        assert net.judge(logs(), other, duration=10).status == 'ERROR'


def test_no_matching_ticks_is_error():
    assert net.judge(logs(), '\n'.join(record(t) for t in range(31, 362, 30)), duration=10).status == 'ERROR'


def test_divergence_beats_crash_and_short_run():
    result = net.judge(logs(), logs(10), duration=120, returncodes=(0, 9))
    assert result.status == 'FAIL'
    assert 'position' in result.detail and '30' in result.detail
    assert net.combine([net.Result('ERROR', 'crash'), result]).status == 'FAIL'


def test_crash_without_divergence_is_error():
    assert net.judge(logs(), logs(), duration=10, returncodes=(0, -11)).status == 'ERROR'


def test_transient_prediction_difference_does_not_fail():
    other = logs().replace(record(60), record(60, 10))
    assert net.judge(logs(), other, duration=10).status == 'PASS'


def test_malformed_state_is_error():
    for bad in [logs().replace('1.000', 'nan'), logs().replace('h1.00/1.00', 'hbroken'),
                logs() + '\n' + record(30, 5)]:
        assert net.judge(logs(), bad, duration=10).status == 'ERROR'


def test_normalized_adapter_log():
    observations = net.parse_log(logs())
    normalized = '\n'.join('lockstep: ' + json.dumps({'tick': t, 'players': p}) for t, p in observations.items())
    assert net.judge(logs(), normalized, duration=10).status == 'PASS'


def test_holes_cannot_count_as_continuous_gameplay():
    partial = '\n'.join(record(t) for t in [30, 60, 90, 360])
    assert net.judge(partial, partial, duration=10).status == 'ERROR'


def test_cleanup_kills_own_process_group(tmp_path):
    log = tmp_path / 'log'
    with log.open('w') as stream:
        child = net.start_peer([sys.executable, '-c', 'import time; time.sleep(60)'], tmp_path, {}, stream)
        net.stop_peers([child])
        assert child.process.poll() is not None
        with pytest.raises(ProcessLookupError):
            os.kill(child.process.pid, 0)


def test_fail_precedes_later_malformed_evidence():
    assert net.judge(logs(), logs(10) + '\n' + record(390).replace('h1.00', 'hbad'), duration=10).status == 'FAIL'


def test_persistence_cannot_bridge_missing_samples():
    a = '\n'.join(record(t) for t in [30, 300, 600])
    b = '\n'.join(record(t, 10) for t in [30, 300, 600])
    assert net.judge(a, b, duration=20).status == 'ERROR'


def test_adapter_rejects_invalid_shape():
    for players in [{0: {'alive': True}, 1: {'alive': False}},
                    {0: {'alive': False}, 1: {'alive': False}}]:
        assert net.judge(logs(), 'lockstep: ' + json.dumps({'tick': 30, 'players': players}), duration=10).status == 'ERROR'


def test_preparation_sets_native_config_and_distinct_addresses(tmp_path):
    command, env = net.prepare_peer(Path('/app/VISR'), tmp_path / 'peer', tmp_path, 'client', 'chillout', 120, True, {})
    text = (tmp_path / 'peer/config.toml').read_text()
    assert 'network_test = "join"' in text
    assert 'network_test_shoot = 7.0' in text
    assert '127.0.0.3' in text and env['HALO_NET_ADDRESS'] == '127.0.0.3'
    assert (tmp_path / 'peer/stderr.log').exists()


def test_foreign_launch_and_log_hooks(tmp_path):
    command, env = net.prepare_peer(Path('/foreign/game'), tmp_path / 'peer', tmp_path, 'host', 'chillout', 120, False,
                                  {'launch': ['{binary}', '--host', '{address}', '{map}'], 'env': {'DATA': '{data}'}})
    assert command == ['/foreign/game', '--host', '127.0.0.2', 'chillout']
    assert env['DATA'] == str(tmp_path / 'peer')
    (tmp_path / 'peer/foreign.log').write_text('hello')
    assert net.read_log(tmp_path / 'peer', {'log': 'foreign.log', 'normalize': [sys.executable, '-c',
        'import pathlib,sys; print(pathlib.Path(sys.argv[1]).read_text().upper())', '{log}']}) == 'HELLO\n'


@pytest.mark.parametrize('spec', [{'launch': 'game'}, {'normalize': []}, {'env': {'X': 2}}, {'log': 3}, {'unknown': []}])
def test_invalid_adapter_configuration(spec):
    with pytest.raises(ValueError):
        net.validate_adapter(spec)


def test_normalized_nested_and_scalar_types_are_checked():
    players = net.parse_log(record(30))[30]
    for field, bad in [('score', [0]), ('position', [[1], 2, 3]), ('grenades', [2]), ('weapons', [[123]])]:
        broken = json.loads(json.dumps(players))
        broken['0'][field] = bad
        text = '\n'.join('lockstep: ' + json.dumps({'tick': t, 'players': broken}) for t in range(30, 361, 30))
        assert net.judge(text, text, duration=10).status == 'ERROR'
    assert net.judge(logs(), 'lockstep: {"tick":30,"players":[]}', duration=10).status == 'ERROR'


def test_rewound_game_clock_is_error():
    assert net.judge(logs(), logs() + '\n' + record(15), duration=10).status == 'ERROR'


def test_preexisting_adapter_evidence_is_rejected(tmp_path):
    path = tmp_path / 'old.log'
    path.write_text(logs())
    with pytest.raises(ValueError, match='preexisting'):
        net.check_log_destination(tmp_path / 'peer', {'log': str(path)})


def test_parent_death_stops_peer(tmp_path):
    pid_file = tmp_path / 'pid'
    child_code = 'import os,pathlib,time; pathlib.Path(' + repr(str(pid_file)) + ').write_text(str(os.getpid())); time.sleep(60)'
    owner_code = ('import pathlib,sys,time; sys.path.insert(0,' + repr(str(Path(net.__file__).parent)) + '); import net_lockstep as n; '
                  's=open(' + repr(str(tmp_path / 'log')) + ',"w"); '
                  'p=n.start_peer([sys.executable,"-c",' + repr(child_code) + '],pathlib.Path(' + repr(str(tmp_path)) + '),{},s); time.sleep(60)')
    owner = subprocess.Popen([sys.executable, '-c', owner_code])
    try:
        deadline = time.monotonic() + 10
        while not pid_file.exists() and time.monotonic() < deadline:
            time.sleep(.05)
        assert pid_file.exists()
        pid = int(pid_file.read_text())
        owner.kill()
        owner.wait()
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                break
            time.sleep(.1)
        else:
            pytest.fail('peer survived checker death')
    finally:
        if owner.poll() is None:
            owner.kill()
            owner.wait()


def test_unspawned_peers_cannot_pass():
    dead = logs().replace('player 1: (4.000 5.000 6.000) h1.00/1.00 g2/2 w a0/0 f0 l0/0 as0/0 st0 thr0.00 123:60', 'player 1: dead')
    assert net.judge(dead, dead, duration=10).status == 'ERROR'



@pytest.mark.parametrize('swap, b_hosts, expected', [(False, False, [False]), (True, False, [False, True]),
                                                     (False, True, [True]), (True, True, [True])])
def test_host_orders(swap, b_hosts, expected):
    from types import SimpleNamespace
    assert net.host_orders(SimpleNamespace(swap=swap, b_hosts=b_hosts)) == expected


def test_b_hosts_needs_build_b(tmp_path, monkeypatch):
    monkeypatch.setattr(sys, 'argv', ['net_lockstep.py', '--build-a', 'a', '--maps', str(tmp_path),
                                      '--b-hosts', '--output', str(tmp_path / 'out')])
    with pytest.raises(SystemExit):
        net.main()
