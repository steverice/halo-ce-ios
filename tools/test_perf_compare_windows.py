import copy
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).parent / 'perf_compare'))
import windows  # noqa: E402


def table():
    return copy.deepcopy(windows.load())


def test_shipped_windows_are_valid_and_cover_the_suites():
    t = windows.load()
    assert {w['suite'] for w in t.values()} == {'campaign', 'combat', 'cutscenes', 'multiplayer', 'co-op'}
    assert set(t) == {'c10', 'b30', 'a50-hangar', 'b40-exta', 'b30-bot', 'x70-bridge', 'cutscene-rescue',
                      'mp-chillout', 'mp-bloodgulch', 'coop-c10', 'coop-b30'}
    assert [t[k]['map'] for k in ('mp-chillout', 'mp-bloodgulch')] == ['chillout', 'bloodgulch']
    assert [t[k]['map'] for k in ('coop-c10', 'coop-b30')] == ['c10', 'b30']
    assert t['mp-chillout']['players'] == 1 and t['coop-c10']['players'] == 2
    assert t['x70-bridge']['map'] == 'd40' and t['x70-bridge']['expected_ticks'] == 3790
    assert t['cutscene-rescue']['map'] == 'a50' and t['cutscene-rescue']['expected_ticks'] == 3180
    assert t['b30-bot']['ticks'] == 1800


@pytest.mark.parametrize('mutate, message', [
    (lambda w: w['c10'].update(bogus=1), 'unknown keys bogus'),
    (lambda w: w['a50-hangar'].update(commands=[]), 'needs commands'),
    (lambda w: w['a50-hangar'].pop('commands'), 'missing commands'),
    (lambda w: w['x70-bridge'].pop('script'), 'missing script'),
    (lambda w: w['x70-bridge'].update(expected_ticks=0), 'positive integer'),
    (lambda w: w['c10'].update(suite='speedrun'), 'unknown suite'),
    (lambda w: w['c10'].update(kind='movie'), 'unknown kind'),
    (lambda w: w['c10'].update(map='../c10'), 'map must be'),
    (lambda w: w['c10'].update(sample_s=30), 'sample_s must be 60'),
    (lambda w: w['c10'].update(commands=['(x)']), 'unknown keys commands'),
    (lambda w: w['b30-bot'].update(test_input='keyboard'), 'test_input'),
    (lambda w: w['a50-hangar'].update(fingerprints=[3]), 'list of console lines'),
    (lambda w: w['mp-chillout'].update(map='c10'), 'multiplayer map'),
    (lambda w: w['mp-chillout'].update(players=2), 'one local player'),
    (lambda w: w['mp-chillout'].pop('test_input'), 'missing test_input'),
    (lambda w: w['coop-c10'].update(map='chillout'), 'campaign level'),
    (lambda w: w['coop-c10'].update(players=1), 'two local players'),
    (lambda w: w['coop-c10'].update(suite='multiplayer'), 'kind and suite disagree'),
    (lambda w: w['coop-b30'].update(test_input='bot:1'), 'unknown keys test_input'),
])
def test_validation_rejects(mutate, message):
    t = table()
    mutate(t)
    with pytest.raises(ValueError, match=message):
        windows.validate(t)


def test_empty_table_is_rejected():
    with pytest.raises(ValueError):
        windows.validate({})


def test_resolve_defaults_windows_and_legacy_scenarios():
    assert windows.resolve({}) == ['c10', 'b30']
    assert windows.resolve({'windows': ['x70-bridge', 'c10']}) == ['x70-bridge', 'c10']
    assert windows.resolve({'scenarios': ['a10', 'b30']}) == ['a10', 'b30']
    assert windows.resolve({'scenarios': ['b30'], 'native_delay_seconds': 90}) == ['b30']
    for bad in ({'windows': []}, {'windows': ['nope']}, {'windows': ['c10', 'c10']},
                {'windows': ['c10'], 'scenarios': ['b30']}, {'scenarios': ['c40']}, {'scenarios': ['b30']}):
        with pytest.raises(ValueError):
            windows.resolve(bad)


def test_map_and_suite_lookup_keep_legacy_keys():
    assert windows.map_for('a50-hangar') == 'a50'
    assert windows.map_for('a10') == 'a10'
    assert windows.suite_for('b40-exta') == 'combat'
    assert windows.suite_for('a10') == 'campaign'
    with pytest.raises(ValueError):
        windows.map_for('zz')
