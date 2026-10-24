"""Refresh validation does not need apps, devices or game data."""
import importlib.util
import sys
from pathlib import Path
import pytest

sys.path.insert(0, str(Path(__file__).parent / 'perf_compare'))


def engine():
    path = Path(__file__).parent / 'perf_compare/refresh.py'
    assert path.is_file(), 'refresh engine is missing'
    spec = importlib.util.spec_from_file_location('perf_refresh', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_release_selection_includes_prerelease_and_ignores_signatures():
    m = engine()
    releases = [{'tag_name': 'v2', 'draft': False, 'assets': [
        {'name': 'macos.zip.sig'}, {'name': 'macos.zip', 'browser_download_url': 'https://example.org/a'}]}]
    tag, asset = m.select_release(releases, '*macos*.zip')
    assert tag == 'v2' and asset['name'] == 'macos.zip'


def test_sweep_export_excludes_invalid_and_private_fields():
    m = engine()
    source = [dict(valid=True, status='ok', device='alias', workload='a10', fps=60,
                   render_size=[1280, 720], timing_basis='offscreen_uncapped', report='/private/report.json'),
              dict(valid=False, status='diverged', device='alias', fps=999)]
    result = m.export_sweep(source, {'alias': 'iPad Pro (M1)'}, 'abc123')
    assert len(result) == 1 and result[0]['device'] == 'iPad Pro (M1)'
    assert '/private' not in str(result) and 'alias' not in str(result)


def test_unmapped_devices_do_not_leak():
    assert engine().export_sweep([dict(valid=True, status='ok', device='secret')], {}, 'build') == []


def test_rejects_nonstandard_matrix_before_launch():
    with pytest.raises(ValueError, match='three'):
        engine().validate_config({'rounds': 1, 'targets': []})


def test_table_check_does_not_write(tmp_path):
    m = engine()
    path = tmp_path / 'page.md'
    path.write_text('old')
    assert m.write_page(path, 'new', check=True) == 1
    assert path.read_text() == 'old'


def test_public_failure_reasons_are_codes():
    m = engine()
    assert m.failure_code(FileNotFoundError('/Users/private/app')) == 'missing-file'
    assert '/Users' not in m.failure_code(RuntimeError('/Users/private'))


def test_native_qualification_is_bound_to_release_hash():
    m = engine()
    target = {'sha256': 'a' * 64, 'qualification': {'sha256': 'b' * 64, 'display_hz': 60,
              'scenarios': {'a10': {'resolution': '1280×720', 'cap': '60'}}}}
    assert m.adapters.qualification(target, 'a10')['scene_verified'] is False
    target['qualification']['sha256'] = 'a' * 64
    assert m.adapters.qualification(target, 'a10')['scene_verified'] is True


def test_sweep_export_does_not_copy_arbitrary_report_basename():
    result = engine().export_sweep([dict(valid=True, status='ok', device='alias', workload='a10',
        render_size=[1280, 720], fps=60, timing_basis='offscreen_uncapped',
        report='/private/report-my-personal-name.json')], {'alias': 'iPad Pro (M1)'}, 'abc123')
    assert 'personal-name' not in str(result)


def test_denial_preserved_when_sweep_is_missing(tmp_path, monkeypatch):
    m = engine()
    def denied(*args):
        raise m.adapters.PermissionDenied('specific command: not authorized')
    monkeypatch.setattr(m, 'fetch', denied)
    config = {'output': str(tmp_path / 'raw'), 'public_machine': 'Mac mini (M4)',
              'targets': [{'id': 'visr'}], 'sweep': {'path': str(tmp_path / 'absent')}}
    summary = tmp_path / 'summary.json'
    assert m.refresh(config, summary, tmp_path / 'page.md') == 3
    assert summary.exists()
    assert 'specific command' in next((tmp_path / 'raw').glob('*/errors.json')).read_text()


def test_qualification_requires_recorded_display_rate():
    m = engine()
    target = {'sha256': 'a' * 64, 'qualification': {'sha256': 'a' * 64,
              'scenarios': {'a10': {'resolution': '1280×720', 'cap': '60'}}}}
    assert not m.adapters.qualification(target, 'a10')['scene_verified']


def test_routed_refresh_stops_after_failed_build(tmp_path, monkeypatch):
    m = engine()
    calls = []
    def command(argv, **kwargs):
        calls.append(argv)
        if argv[0] == 'git':
            return 'a' * 40 + '\trefs/heads/main\n' if 'ls-remote' in argv else ''
        raise RuntimeError('build failed')
    monkeypatch.setattr(m.adapters, 'run', command)
    def fail_build(argv, **kwargs):
        calls.append(argv)
        raise RuntimeError('build failed')
    monkeypatch.setattr(m.subprocess, 'run', fail_build)
    config = {'route': {'router': '/router', 'host': 'dedicated', 'local_scratch': str(tmp_path),
                        'remote_scratch': '/remote/scratch'}, 'targets': [{'id': 'visr'}]}
    with pytest.raises(RuntimeError, match='build failed'):
        m.routed_refresh(config, tmp_path / 'summary', tmp_path / 'page')
    assert sum('--kind' in argv for argv in calls) == 1
    assert not any('runner' in argv for argv in calls)


def test_qualification_rejects_display_rate_mismatch():
    m = engine()
    target = {'sha256': 'a' * 64, 'qualification': {'sha256': 'a' * 64, 'display_hz': 120,
              'scenarios': {'a10': {'resolution': '1280×720', 'cap': '60'}}}}
    assert not m.adapters.qualification(target, 'a10', display_hz=60)['scene_verified']


def test_fatal_unavailable_target_is_not_relaunched_six_times(tmp_path, monkeypatch):
    m = engine()
    calls = []
    monkeypatch.setattr(m, 'fetch', lambda *args: {'version': 'v1', 'sha256': 'a' * 64})
    monkeypatch.setattr(m.adapters, 'host_state', lambda **kwargs: {})
    def unavailable(target, scenario, *args):
        calls.append(scenario)
        raise RuntimeError('console-unavailable')
    monkeypatch.setattr(m.adapters, 'native', unavailable)
    config = {'output': str(tmp_path / 'raw'), 'public_machine': 'Mac mini (M4)', 'targets': [{'id': 'visr'}]}
    assert m.refresh(config, tmp_path / 'summary.json', tmp_path / 'page.md') == 1
    assert calls == ['a10']


def test_sweep_export_groups_repetitions_without_mixing_settings():
    m = engine()
    common = dict(valid=True, status='ok', device='alias', workload='a10', render_size=[1280,720],
                  timing_basis='offscreen_uncapped', thermal_worst=0, phase='tiers')
    rows = [{**common, 'fps': 58, 'id': 'r1', 'tag': 'tier-l720x1', 'report': '/p/r1.json'},
            {**common, 'fps': 62, 'id': 'r2', 'tag': 'tier-l720x1', 'report': '/p/r2.json'},
            {**common, 'fps': 90, 'id': 'r1', 'tag': 'tier-l720x1p5', 'report': '/p/r3.json'}]
    result = m.export_sweep(rows, {'alias': 'iPad Pro (M1)'}, 'abc123')
    assert len(result) == 2
    row = next(row for row in result if row['configuration'] == 'tiers/tier-l720x1')
    assert row['fps'] == '58.00–62.00' and row['trials'] == 2


def test_private_qualification_text_cannot_enter_public_conditions():
    target = {'sha256': 'a' * 64, 'qualification': {'sha256': 'a' * 64, 'display_hz': 60,
              'scenarios': {'a10': {'resolution': '/Users/private', 'cap': '60'}}}}
    value = engine().adapters.qualification(target, 'a10')
    assert '/Users' not in str(value) and not value['scene_verified']


def test_routed_visr_uses_the_bundle_built_in_this_worktree():
    m = engine()
    config = {'routed_visr': 'a' * 40, 'targets': [{'id': 'visr', 'app': '/stale/VISR.app', 'version': 'old'}]}
    m.bind_routed_visr(config)
    assert config['targets'][0]['app'] == str(m.ROOT / 'build/macos/app-native/host/Release/VISR.app')
    assert config['targets'][0]['version'] == 'a' * 40


def test_routed_runner_failure_cannot_import_previous_summary(tmp_path, monkeypatch):
    from types import SimpleNamespace
    m = engine()
    local = tmp_path / 'transfer'
    local.mkdir()
    (local / 'summary.json').write_text('{"date":"old"}')
    def command(argv, **kwargs):
        return 'a' * 40 + '\trefs/heads/main\n' if 'ls-remote' in argv else ''
    monkeypatch.setattr(m.adapters, 'run', command)
    monkeypatch.setattr(m.subprocess, 'run', lambda argv, **kwargs: SimpleNamespace(returncode=1 if 'runner' in argv else 0))
    config = {'route': {'router': '/router', 'host': 'dedicated', 'local_scratch': str(local),
                        'remote_scratch': '/remote/scratch'}, 'targets': [{'id': 'visr', 'app': '/stale/app'}]}
    summary = tmp_path / 'summary.json'
    assert m.routed_refresh(config, summary, tmp_path / 'page.md') == 1
    assert not summary.exists()


def test_routed_success_requires_fresh_summary_transfer(tmp_path, monkeypatch):
    from types import SimpleNamespace
    m = engine()
    monkeypatch.setattr(m.adapters, 'run', lambda argv, **kwargs: 'a' * 40 if 'ls-remote' in argv else '')
    monkeypatch.setattr(m.subprocess, 'run', lambda *args, **kwargs: SimpleNamespace(returncode=0))
    config = {'route': {'router': '/router', 'host': 'dedicated', 'local_scratch': str(tmp_path / 'transfer'),
                        'remote_scratch': '/remote/scratch'}, 'targets': []}
    assert m.routed_refresh(config, tmp_path / 'summary.json', tmp_path / 'page.md') == 1


def test_routed_build_uses_fresh_live_main_checkout(tmp_path, monkeypatch):
    from types import SimpleNamespace
    m = engine()
    commands = []
    def git(argv, **kwargs):
        commands.append(argv)
        return 'a' * 40 if 'ls-remote' in argv else ''
    monkeypatch.setattr(m.adapters, 'run', git)
    monkeypatch.setattr(m.subprocess, 'run', lambda argv, **kwargs: commands.append(argv) or SimpleNamespace(returncode=0))
    config = {'route': {'router': '/router', 'host': 'dedicated', 'local_scratch': str(tmp_path / 'transfer'),
                        'remote_scratch': '/remote/scratch'}, 'targets': [{'id': 'visr', 'app': '/old/app'}]}
    m.routed_refresh(config, tmp_path / 'summary.json', tmp_path / 'page.md')
    checkout = next(argv for argv in commands if 'worktree' in argv and 'add' in argv)
    assert checkout[-1] == 'a' * 40
    build = next(argv for argv in commands if '--kind' in argv and 'build' in argv)
    assert build[build.index('perf-compare-refresh-build') + 1].endswith('/build-source')


def test_waits_for_quiet_without_repairing_locked_display(monkeypatch):
    m = engine()
    calls = []
    def state():
        calls.append(1)
        if len(calls) == 1:
            raise RuntimeError('host-not-quiet')
        return {}
    monkeypatch.setattr(m.adapters, 'host_state', state)
    monkeypatch.setattr(m.time, 'sleep', lambda seconds: None)
    m.wait_quiet(1)
    assert len(calls) == 2
    def locked():
        raise RuntimeError('display-locked')
    monkeypatch.setattr(m.adapters, 'host_state', locked)
    with pytest.raises(RuntimeError, match='display-locked'):
        m.wait_quiet(1)


def test_public_release_metadata_does_not_require_new_github_login(monkeypatch):
    import io
    m = engine()
    def unconfigured(*args, **kwargs):
        raise RuntimeError('To get started with GitHub CLI, please run: gh auth login')
    monkeypatch.setattr(m.adapters, 'run', unconfigured)
    monkeypatch.setattr(m.urllib.request, 'urlopen', lambda *args, **kwargs: io.BytesIO(b'[{"tag_name":"v1","assets":[]}]'))
    assert m.release_metadata('example/project')[0]['tag_name'] == 'v1'


def test_public_fallback_does_not_retry_real_authorization_denial(monkeypatch):
    m = engine()
    monkeypatch.setattr(m.adapters, 'run', lambda *args, **kwargs: (_ for _ in ()).throw(m.adapters.PermissionDenied('gh api: forbidden')))
    with pytest.raises(m.adapters.PermissionDenied):
        m.release_metadata('example/project')


def test_foreground_activation_waits_for_app_transition(monkeypatch):
    m = engine()
    responses = iter(['{"pid":1}', '{"pid":42}'])
    monkeypatch.setattr(m.adapters, 'run', lambda *args, **kwargs: next(responses))
    monkeypatch.setattr(m.adapters.time, 'sleep', lambda *args: None)
    m.adapters.frontmost(42, activate=True)


def test_running_game_load_does_not_count_as_background_contention(monkeypatch):
    m = engine()
    monkeypatch.setattr(m.adapters, 'run', lambda *args, **kwargs: '')
    monkeypatch.setattr(m.adapters.os, 'getloadavg', lambda: (4, 4, 4))
    monkeypatch.setattr(m.adapters, 'thermal', lambda: {'state': 'nominal'})
    assert m.adapters.host_state(check_load=False)['load'] == 4
    snapshots = iter([{1: 0}, {1: 10}])
    monkeypatch.setattr(m.adapters, 'cpu_snapshot', lambda: next(snapshots))
    monkeypatch.setattr(m.adapters.time, 'sleep', lambda *args: None)
    with pytest.raises(RuntimeError, match='host-not-quiet'):
        m.adapters.host_state()


def test_background_cpu_excludes_only_owned_process():
    assert engine().adapters.background_cpu({1: 10, 2: 5}, {1: 70, 2: 5.5}, {1}) == 0.5


def test_background_cpu_includes_process_started_during_window():
    assert engine().adapters.background_cpu({1: 10}, {1: 70, 2: 20}, {1}) == 20


def test_load_average_tail_does_not_reject_currently_quiet_host(monkeypatch):
    m = engine()
    monkeypatch.setattr(m.adapters, 'run', lambda *args, **kwargs: '')
    monkeypatch.setattr(m.adapters.os, 'getloadavg', lambda: (10, 10, 10))
    monkeypatch.setattr(m.adapters, 'thermal', lambda: {'state': 'nominal'})
    snapshots = iter([{1: 0}, {1: 0}])
    monkeypatch.setattr(m.adapters, 'cpu_snapshot', lambda: next(snapshots))
    monkeypatch.setattr(m.adapters.time, 'sleep', lambda *args: None)
    assert m.adapters.host_state()['load'] == 10


def test_baseline_accepts_steady_load_but_rejects_material_increase():
    adapters = engine().adapters
    assert not adapters.background_changed(2.1, 2.0)
    assert adapters.background_changed(2.6, 2.0)
    assert adapters.background_changed(1.1, None)


def test_disabled_safari_is_never_fetched_or_launched(tmp_path, monkeypatch):
    m = engine()
    def forbidden(*args, **kwargs):
        pytest.fail('disabled Safari must not be contacted')
    monkeypatch.setattr(m, 'fetch', forbidden)
    monkeypatch.setattr(m.adapters, 'safari', forbidden)
    monkeypatch.setattr(m.adapters, 'host_state', lambda **kwargs: {})
    config = {'output': str(tmp_path / 'raw'), 'public_machine': 'Mac mini (M4)',
              'cpu_policy': 'baseline', 'targets': [{'id': 'opence-web',
              'unavailable_reason': 'safari-remote-automation-disabled'}]}
    summary = tmp_path / 'summary.json'
    assert m.refresh(config, summary, tmp_path / 'page.md') == 1
    import json
    record = json.loads(summary.read_text())['targets'][0]
    assert record['reason'] == 'safari-remote-automation-disabled'
    assert 'half a core' in (tmp_path / 'page.md').read_text()


def test_readiness_requires_a_complete_output_line_instead_of_console_echo():
    ready = engine().adapters.console_ready
    assert not ready('(if (> (list_count (players)) 0) (print "PERF_READY"))\r\n')
    assert not ready('perf_ready')
    assert ready('> (print "PERF_READY")\r\nperf_ready\r\n')


def test_contention_retry_keeps_only_successful_evidence_at_run_root(tmp_path, monkeypatch):
    m = engine()
    calls = []
    def attempt(target, scenario, folder, config):
        calls.append(folder)
        if len(calls) == 1:
            (folder / 'hud.log').write_text('rejected')
            raise RuntimeError('host-not-quiet')
        (folder / 'hud.log').write_text('accepted')
        return {'thermal': 'nominal'}
    monkeypatch.setattr(m.adapters, '_native', attempt)
    assert m.adapters.native({}, 'a10', tmp_path, {'cpu_policy': 'baseline'}) == {'thermal': 'nominal'}
    assert len(calls) == 2
    assert (tmp_path / 'attempt-error.txt').read_text() == 'host-not-quiet'
    assert (tmp_path / 'hud.log').read_text() == 'accepted'
    assert (tmp_path / 'rejected-hud.log').read_text() == 'rejected'


def test_baseline_excludes_game_cpu(monkeypatch):
    m = engine()
    snapshots = iter([{1: 0, 2: 0}, {1: 50, 2: 5}])
    stamps = iter([0, 5])
    monkeypatch.setattr(m.adapters, 'cpu_snapshot', lambda: next(snapshots))
    monkeypatch.setattr(m.adapters.time, 'monotonic', lambda: next(stamps))
    monkeypatch.setattr(m.adapters.time, 'sleep', lambda seconds: None)
    assert m.adapters.sample_background(owned={1}) == 1


def test_native_baseline_cannot_skip_the_quiet_gate_for_active_safari():
    with pytest.raises(ValueError, match='Safari targets to be skipped'):
        engine().validate_config({'cpu_policy': 'baseline', 'targets': [{'id': 'opence-web'}]})
