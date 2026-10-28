from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).parent / 'perf_compare'))
import consolewin  # noqa: E402
import windows  # noqa: E402

WINDOWS = windows.load()


class FakeGame:
    """A console whose replies come from a table, on a clock that jumps instead of waiting."""

    def __init__(self, replies=None, prints=True, timed=()):
        self.t = 0.0
        self.replies = replies or {}
        self.prints = prints
        self.timed = sorted(timed)  # (seconds, text) the game prints on its own
        self.console = consolewin.Console(self.on_send)
        self.events = []
        self.sent_at = []

    # clock
    def now(self):
        return self.t

    def sleep(self, seconds):
        self.t += seconds
        while self.timed and self.timed[0][0] <= self.t:
            self.console.feed(self.timed.pop(0)[1])

    def on_send(self, line):
        self.sent_at.append((self.t, line))
        reply = self.replies(line, self.t) if callable(self.replies) else self.replies.get(line)
        self.console.feed(line + '\r\n')  # the echo
        if reply is not None and self.prints:
            self.console.feed(f'{reply}\r\n')


class Hooks(consolewin.Hooks):
    def __init__(self, game):
        self.game, self.log = game, []

    def prepare(self):
        self.log.append(('prepare', self.game.t))

    def begin(self):
        self.log.append(('begin', self.game.t))

    def mid(self):
        self.log.append(('mid', self.game.t))

    def end(self):
        self.log.append(('end', self.game.t))


def test_seen_matches_the_lowercase_print_not_the_echo():
    console = consolewin.Console(lambda line: None)
    console.feed('(begin (x70_bridge) (print "XPDONE"))\r\n')
    assert not console.seen('xpdone')
    console.feed('xpdone\r\n')
    assert console.seen('XPDONE')
    assert not console.seen('xpdone', console.mark())


def test_query_returns_the_value_after_the_echo_or_none_when_the_console_is_silent():
    game = FakeGame({'(game_time)': '1234'})
    assert consolewin.number(game.console.query('(game_time)', game)) == 1234
    quiet = FakeGame({'(game_time)': '1234'}, prints=False)
    assert quiet.console.query('(game_time)', quiet, timeout=1.0) is None


def test_battle_sends_deathless_then_commands_warms_up_then_samples_and_reads_counts():
    window = WINDOWS['b40-exta']
    game = FakeGame(lambda line, t: {'(ai_living_count ext_a_area_a_cov)': '40' if t < 60 else '12',
                                     '(ai_living_count ext_a_area_a_marines)': '5',
                                     '(unit_get_health (unit (list_get (players) 0)))': '1',
                                     '(game_time)': str(int(t * 30))}.get(line))
    hooks = Hooks(game)
    out = consolewin.run_window(window, game.console, hooks, game)
    sent = [line for _, line in game.sent_at]
    assert sent[0] == '(set cheat_deathless_player true)' and sent[1:7] == window['commands']
    assert [name for name, _ in hooks.log] == ['prepare', 'begin', 'mid', 'end']
    _, begin, mid, end = (t for _, t in hooks.log)
    assert begin - game.sent_at[6][0] >= window['warmup_s']
    assert end - begin == pytest.approx(60) and mid - begin == pytest.approx(30)
    assert out['start']['(ai_living_count ext_a_area_a_cov)'] == '40'
    assert out['end']['(ai_living_count ext_a_area_a_cov)'] == '12'
    assert out['seconds'] == pytest.approx(60)


def test_cutscene_ends_on_the_print_and_calls_the_script_after_the_lead_in():
    window = WINDOWS['x70-bridge']
    game = FakeGame({'(game_time)': '100'}, timed=[(20 + 126.3, 'xpdone\r\n')])
    hooks = Hooks(game)
    out = consolewin.run_window(window, game.console, hooks, game)
    call = [(t, line) for t, line in game.sent_at if 'x70_bridge' in line]
    assert call and call[0][1] == '(begin (x70_bridge) (print "XPDONE"))' and call[0][0] >= 20
    assert out['ended'] == 'print' and out['seconds'] == pytest.approx(126.3, abs=0.2)
    assert [name for name, _ in hooks.log] == ['prepare', 'begin', 'mid', 'end']


def test_cutscene_without_a_print_times_out_at_one_and_a_half_times_the_expected_length():
    window = WINDOWS['cutscene-rescue']
    game = FakeGame({'(game_time)': '100'})
    out = consolewin.run_window(window, game.console, Hooks(game), game)
    assert out['ended'] == 'timeout' and out['seconds'] >= 1.5 * 3180 / 30


def test_cutscene_on_a_silent_console_falls_back_to_the_wall_clock():
    window = WINDOWS['x70-bridge']
    game = FakeGame({}, prints=False)
    out = consolewin.run_window(window, game.console, Hooks(game), game, prints=False)
    assert out['ended'] == 'wall-clock' and out['start_tick'] is None


def test_cutscene_without_prints_ends_when_game_time_reaches_the_expected_tick():
    window = WINDOWS['x70-bridge']
    game = FakeGame(lambda line, t: str(int((t - 20) * 30) + 100) if line == '(game_time)' else None, prints=True)
    out = consolewin.run_window(window, game.console, Hooks(game), game, prints=False)
    assert out['ended'] == 'ticks' and out['seconds'] == pytest.approx(window['expected_ticks'] / 30, abs=1.0)


def test_bot_window_runs_from_start_to_end_print_and_reads_fingerprints_at_both_ends():
    window = WINDOWS['b30-bot']
    game = FakeGame({'(game_time)': '900', '(unit_get_health (unit (list_get (players) 0)))': '0.9',
                     '(unit_get_total_grenade_count (unit (list_get (players) 0)))': '3'},
                    timed=[(30, 'xp_start\r\n'), (90, 'xp_end\r\n')])
    hooks = Hooks(game)
    out = consolewin.run_window(window, game.console, hooks, game)
    assert [name for name, _ in hooks.log] == ['prepare', 'begin', 'mid', 'end']
    assert out['ended'] == 'print' and out['seconds'] == pytest.approx(60, abs=0.5)
    assert out['start']['(game_time)'] == '900' and out['end']['(unit_get_total_grenade_count (unit (list_get (players) 0)))'] == '3'
    schedule = [line for _, line in game.sent_at if 'sleep_until' in line][0]
    assert '(>= (game_time) 900)' in schedule and '(sleep 1800)' in schedule and '(game_speed 0)' in schedule


def test_bot_window_that_never_starts_is_reported():
    game = FakeGame({'(game_time)': '1'})
    out = consolewin.run_window(WINDOWS['b30-bot'], game.console, Hooks(game), game)
    assert out['ended'] == 'never-started'


def test_opening_is_not_a_console_window():
    game = FakeGame()
    with pytest.raises(ValueError):
        consolewin.run_window(WINDOWS['c10'], game.console, Hooks(game), game)


def test_sync_waits_for_the_token_after_queued_echoes():
    game = FakeGame(timed=[(3.0, 'xpsync\r\n')])
    assert game.console.sync(game, timeout=10) is True and game.t >= 3.0
    assert FakeGame().console.sync(FakeGame(), timeout=1) is False


def test_hosted_session_warms_up_samples_and_reads_players_and_time_at_both_ends():
    window = WINDOWS['mp-chillout']
    game = FakeGame(lambda line, t: {'(list_count (players))': '1', '(game_time)': str(int(t * 30)), '(print "XPSYNC")': 'xpsync'}.get(line))
    hooks = Hooks(game)
    out = consolewin.run_window(window, game.console, hooks, game)
    assert [name for name, _ in hooks.log] == ['prepare', 'begin', 'mid', 'end']
    assert hooks.log[0][1] == 15 and out['seconds'] == pytest.approx(60, abs=0.5)
    assert out['start']['(list_count (players))'] == '1' and int(out['end']['(game_time)']) > int(out['start']['(game_time)'])
    assert out['kind'] == 'match'


def test_hosted_settings_per_port():
    import adapters
    match, coop = WINDOWS['mp-chillout'], WINDOWS['coop-c10']
    assert adapters.hosted_spec(match) == 'host:chillout'
    assert adapters.hosted_spec(coop) == 'host:levels\\c10\\c10:coop'
    debug, network, env = adapters.hosted_settings('visr', coop)
    assert 'network_test = "host:levels\\\\c10\\\\c10:coop"' in debug  # escaped for TOML
    assert 'enemy_multiplier = 1' in debug and 'network_test_players = 2' in debug and not network and not env
    assert not any('test_input' in line for line in debug)  # co-op takes no input
    debug, network, env = adapters.hosted_settings('opence-macos', coop)
    assert network == ['coop_enemies_mode = "none"'] and 'enemy_multiplier = 1' not in debug
    debug, network, env = adapters.hosted_settings('halo-og', match)
    assert 'test_input = "bot:1"' in debug and not network
    debug, network, env = adapters.hosted_settings('chupathingy', coop)
    assert env['HALO_NETWORK_TEST_LOCAL_PLAYERS'] == '2' and env['HALO_NET_COOP_ENEMIES_MODE'] == 'none' and not debug
    assert adapters.hosted_settings('chupathingy', match)[2]['HALO_START_WITH_ONE_PLAYER'] == '1'
