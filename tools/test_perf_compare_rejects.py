import ast
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).parent / 'perf_compare'))
import consolewin  # noqa: E402
import rejects  # noqa: E402
import windows  # noqa: E402

W = windows.load()

# Excerpts of recorded VISR console transcripts ("<time> <<< 'chunk'" is output, ">>>" is a line we sent).
COUNT_QUERY = """\
1793131576.29 >>> (ai_living_count hangar_cov_first_floor)
1793131576.29 <<< '('
1793131576.29 <<< 'ai_li'
1793131576.29 <<< 'v'
1793131576.29 <<< 'in'
1793131576.29 <<< 'g'
1793131576.29 <<< '_'
1793131576.29 <<< 'c'
1793131576.29 <<< 'ov_'
1793131576.29 <<< 'fi'
1793131576.29 <<< 'rs'
1793131576.29 <<< 't'
1793131576.29 <<< '_floor)\\r\\n40\\r\\n'
"""
CUTSCENE_PRINT = """\
1793121575.28 >>> (begin (x70_bridge) (print "XPDONE"))
1793121575.28 <<< '(b'
1793121575.28 <<< 'e'
1793121575.28 <<< ')'
1793121575.28 <<< ')'
1793121575.28 <<< '\\r\\n'
1793121701.52 <<< '\\r\\n'
1793121701.52 <<< 'xpdone'
"""


def replay(transcript):
    """A Console fed from a recorded transcript: chunks in order, no trailing newline added."""
    console = consolewin.Console(lambda line: None)
    for entry in transcript.splitlines():
        if ' <<< ' in entry:
            console.feed(ast.literal_eval(entry.split(' <<< ', 1)[1]))
    return console


def test_recorded_count_reply_parses_through_the_fragmented_echo():
    console = replay(COUNT_QUERY)
    assert [line for line in console.lines_since() if line and not line.startswith('(')] == ['40']


def test_recorded_print_without_a_trailing_newline_is_seen():
    assert replay(CUTSCENE_PRINT).seen('xpdone')
    assert not replay(CUTSCENE_PRINT.rsplit('1793121701.52', 2)[0]).seen('xpdone')


def cutscene(seconds, ended='print', name='x70-bridge'):
    return W[name], {'kind': 'cutscene', 'seconds': seconds, 'ended': ended}


@pytest.mark.parametrize('seconds, ended, code', [
    (126.3, 'print', None), (126.3, 'ticks', None), (121.0, 'print', None),
    (100.0, 'print', 'cutscene-incomplete'),  # under 95% of 3,790 ticks (120.0 s)
    (190.0, 'timeout', 'cutscene-incomplete'), (190.0, 'wall-clock', 'cutscene-incomplete'),
])
def test_cutscene_rejects(seconds, ended, code):
    window, outcome = cutscene(seconds, ended)
    if code:
        with pytest.raises(ValueError, match=code):
            rejects.check(window, outcome)
    else:
        assert rejects.check(window, outcome) == []


def battle(placed=('40', '3'), start_time='9000', end_time='11000'):
    window = W['b40-exta']
    start = {f'(ai_living_count {name})': count for name, count in zip(window['encounters'], placed)}
    return window, {'kind': 'battle', 'start': {**start, '(game_time)': start_time}, 'end': {'(game_time)': end_time}}


def test_battle_rejects_nothing_placed_and_reverts_but_not_a_zero_health_deathless_player():
    assert rejects.check(*battle()) == []
    with pytest.raises(ValueError, match='nothing-placed'):
        rejects.check(*battle(placed=('40', '0')))
    with pytest.raises(ValueError, match='player-reverted'):
        rejects.check(*battle(end_time='9500'))  # game time fell back or stalled: a checkpoint reload
    assert rejects.check(*battle(end_time='10700')) == []  # 1,700 of 1,800 ticks: a slow port, not a revert
    window, outcome = battle()
    outcome['end']['(unit_get_health (unit (list_get (players) 0)))'] = '0'
    assert rejects.check(window, outcome) == []  # deathless: zero health is not a death


def test_battle_on_a_silent_console_is_flagged_not_rejected():
    window = W['b40-exta']
    notes = rejects.check(window, {'kind': 'battle', 'start': {}, 'end': {}})
    assert notes and 'unreadable' in notes[0]


def bot(end_tick='2700', grenades=('2', '4'), ended='print'):
    window = W['b30-bot']
    return window, {'kind': 'bot', 'ended': ended, 'start': {rejects.GRENADES: grenades[0]},
                    'end': {'(game_time)': end_tick, rejects.GRENADES: grenades[1]}}


def test_bot_window_rejects_drift_inactivity_and_a_missed_start():
    assert rejects.check(*bot()) == []
    assert rejects.check(*bot(end_tick='2702')) == []
    with pytest.raises(ValueError, match='bot-tick-drift'):
        rejects.check(*bot(end_tick='2710'))
    with pytest.raises(ValueError, match='bot-inactive'):
        rejects.check(*bot(grenades=('3', '3')))
    with pytest.raises(ValueError, match='bot-did-not-start'):
        rejects.check(*bot(ended='never-started'))


def session(window, players=('1', '1'), ticks=(500, 2300)):
    reads = lambda count, tick: {'(list_count (players))': count, '(game_time)': None if tick is None else str(tick)}
    return {'start': reads(players[0], ticks[0]), 'end': reads(players[1], ticks[1]), 'seconds': 60.0}


def test_hosted_windows_reject_a_wrong_player_count_and_a_stalled_game():
    match, coop = W['mp-chillout'], W['coop-c10']
    assert rejects.check(match, session(match)) == []
    assert rejects.check(coop, session(coop, ('2', '2'))) == []
    with pytest.raises(ValueError, match='players-missing'):
        rejects.check(match, session(match, ('1', '2')))
    with pytest.raises(ValueError, match='players-missing'):
        rejects.check(coop, session(coop, ('1', '1')))
    with pytest.raises(ValueError, match='game-stalled'):
        rejects.check(match, session(match, ticks=(500, 600)))


def test_hosted_windows_on_a_silent_console_are_flagged_not_rejected():
    match = W['mp-chillout']
    notes = rejects.check(match, session(match, (None, None), (None, None)))
    assert any('player count unreadable' in n for n in notes) and any('game time unreadable' in n for n in notes)


def test_outlier_rounds_are_flagged_against_the_other_rounds():
    window = W['b40-exta']
    key = window['fingerprints'][0]
    rounds = [{'end': {key: v}} for v in ('27', '27', '27', '4')]
    flags = rejects.outliers(window, rounds)
    assert list(flags) == [3] and '4' in flags[3][0]
    assert rejects.outliers(window, [{'end': {key: v}} for v in ('27', '27', '27')]) == {}
    assert rejects.outliers(window, [{'end': {key: '1'}}, {'end': {key: '9'}}]) == {}  # too few rounds to judge


def test_every_reject_code_is_known_to_the_harness():
    import refresh
    assert set(rejects.CODES) <= {code for code in rejects.CODES}
    for code in rejects.CODES:
        assert refresh.failure_code(RuntimeError(code)) == code
