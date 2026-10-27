"""Console-driven sampling windows: battles, cutscenes and the scripted-bot window.

The game's telnet console echoes each expression and then prints the value on its own line, lowercased. Everything
here talks to it through `Console`, and measures through the `Hooks` the Mac adapter supplies, so the window logic
runs against a recorded transcript in tests.
"""
from __future__ import annotations

import socket
import threading
import time

TICKS_PER_SECOND = 30
CUTSCENE_GRACE = 1.5  # a cutscene must finish within this multiple of its expected length


class RealClock:
    def now(self):
        return time.monotonic()

    def sleep(self, seconds):
        time.sleep(seconds)


class Console:
    """A telnet console session: a reader thread fills a buffer; `send` writes one line."""

    def __init__(self, send, initial=''):
        self._send = send
        self.buf = initial
        self.lock = threading.Lock()
        self.sent = []

    @classmethod
    def attach(cls, connection: socket.socket, initial=''):
        """Wrap a connected socket and start reading it."""
        connection.settimeout(None)
        console = cls(lambda line: connection.sendall((line + '\n').encode('latin1')), initial)

        def read():
            while True:
                try:
                    chunk = connection.recv(65536)
                except OSError:
                    return
                if not chunk:
                    return
                console.feed(chunk.decode('latin1'))
        threading.Thread(target=read, daemon=True).start()
        return console

    def feed(self, text):
        with self.lock:
            self.buf += text

    def send(self, line):
        self.sent.append(line)
        self._send(line)

    def mark(self):
        with self.lock:
            return len(self.buf)

    def lines_since(self, mark=0, partial=False):
        """Lines after the mark, stripped. A line still being written is left out unless `partial`: a print
        arrives without a trailing newline (the game prints `\\r\\nxpdone`)."""
        with self.lock:
            text = self.buf[mark:]
        lines = text.replace('\r', '').split('\n')
        return [line.strip() for line in (lines if partial else lines[:-1])]

    def seen(self, token, since=0):
        """True once the console printed `token` on a line of its own. Output is lowercase; the echo of the
        expression that prints it (which contains the token inside quotes) never matches."""
        want = token.lower()
        return any(line.strip('"') == want for line in self.lines_since(since, partial=True))

    def query(self, expression, clock, timeout=3.0):
        """Send an expression and return the value line, or None when the console printed nothing back."""
        mark = self.mark()
        self.send(expression)
        deadline = clock.now() + timeout
        while True:
            replies = [line for line in self.lines_since(mark) if line and not line.startswith('(')]
            if replies:
                return replies[0]
            if clock.now() >= deadline:
                return None
            clock.sleep(0.05)


def number(text):
    """The leading number of a console reply, or None."""
    try:
        return float(str(text).split()[0])
    except (ValueError, IndexError):
        return None


class Hooks:
    """What the adapter measures. `prepare` does the slow setup (trace setup, its settle wait, the background CPU
    baseline) while the level runs, before the window opens; `begin` only marks the window's start, `mid` takes the
    mid-window screenshot and `end` closes the window and collects the trace. Each returns nothing."""

    def prepare(self): ...
    def begin(self): ...
    def mid(self): ...
    def end(self): ...


def _read_all(console, expressions, clock):
    return {expression: console.query(expression, clock) for expression in expressions}


def _living(window):
    return [f'(ai_living_count {name})' for name in window.get('encounters', [])]


def run_battle(window, console, hooks, clock, lead_in=10.0):
    """Deathless player, one batch of commands, a warm-up, then the sample; counts are read before and after."""
    clock.sleep(lead_in)
    hooks.prepare()
    console.send('(set cheat_deathless_player true)')
    for command in window['commands']:
        console.send(command)
        clock.sleep(0.2)
    clock.sleep(window['warmup_s'])
    start_reads = _read_all(console, _living(window) + ['(game_time)', '(unit_get_health (unit (list_get (players) 0)))'], clock)
    hooks.begin()
    started = clock.now()
    half = window['sample_s'] / 2
    clock.sleep(half)
    hooks.mid()
    clock.sleep(max(0.0, window['sample_s'] - (clock.now() - started)))
    hooks.end()
    seconds = clock.now() - started
    end_reads = _read_all(console, window['fingerprints'] + ['(game_time)'], clock)
    return {'kind': 'battle', 'seconds': seconds, 'start': start_reads, 'end': end_reads, 'ended': 'time'}


def run_cutscene(window, console, hooks, clock, prints=True, lead_in=20.0):
    """Call the script from the console; sample until its print arrives (or its tick budget passes)."""
    clock.sleep(lead_in)
    hooks.prepare()
    expected = window['expected_ticks']
    budget = expected / TICKS_PER_SECOND
    start_tick = number(console.query('(game_time)', clock))
    mark = console.mark()
    console.send(f'(begin ({window["script"]}) (print "XPDONE"))')
    hooks.begin()
    started = clock.now()
    shot_done, ended = False, 'timeout'
    while clock.now() - started < budget * CUTSCENE_GRACE:
        elapsed = clock.now() - started
        if not shot_done and elapsed >= min(8.0, budget / 2):
            hooks.mid()
            shot_done = True
        if prints and console.seen('xpdone', mark):
            ended = 'print'
            break
        if not prints and start_tick is not None:
            now_tick = number(console.query('(game_time)', clock, timeout=1.0))
            if now_tick is not None and now_tick >= start_tick + expected:
                ended = 'ticks'
                break
        clock.sleep(0.1)
    seconds = clock.now() - started
    if not shot_done:
        hooks.mid()
    hooks.end()
    if not prints and ended == 'timeout' and start_tick is None:
        # Nothing readable from this console: the window is the expected length on the wall clock.
        ended = 'wall-clock'
    end_tick = number(console.query('(game_time)', clock, timeout=1.0))
    return {'kind': 'cutscene', 'seconds': seconds, 'ended': ended, 'expected_ticks': expected,
            'start_tick': start_tick, 'end_tick': end_tick, 'start': {}, 'end': {}}


def run_bot(window, console, hooks, clock, prints=True):
    """The scripted player for `ticks` game ticks from `start_tick`; the game freezes at the end so reads are exact."""
    t0, ticks = window['start_tick'], window['ticks']
    hooks.prepare()
    console.send('(set cheat_deathless_player true)')
    mark = console.mark()
    console.send(f'(begin (sleep_until (>= (game_time) {t0}) 1) (print "XP_START") (sleep {ticks}) (print "XP_END") (game_speed 0))')
    started = clock.now()
    limit = (t0 + ticks) / TICKS_PER_SECOND * CUTSCENE_GRACE + 30
    begun = False
    start_reads, ended = {}, 'timeout'
    shot_done = False
    began_at = None
    while clock.now() - started < limit:
        tick = None if prints else number(console.query('(game_time)', clock, timeout=0.5))
        if not begun and (console.seen('xp_start', mark) if prints else tick is not None and tick >= t0):
            start_reads = _read_all(console, window['fingerprints'], clock)
            began_at = clock.now()
            hooks.begin()
            begun = True
        if begun and not shot_done and clock.now() - began_at >= ticks / TICKS_PER_SECOND / 2:
            hooks.mid()
            shot_done = True
        if begun and (console.seen('xp_end', mark) if prints else tick is not None and tick >= t0 + ticks):
            ended = 'print'
            break
        clock.sleep(0.1)
    if not begun:
        return {'kind': 'bot', 'seconds': 0.0, 'ended': 'never-started', 'start': {}, 'end': {}}
    seconds = clock.now() - began_at
    hooks.end()
    end_reads = _read_all(console, window['fingerprints'], clock)
    return {'kind': 'bot', 'seconds': seconds, 'ended': ended, 'start': start_reads, 'end': end_reads}


def run_window(window, console, hooks, clock=None, prints=True):
    clock = clock or RealClock()
    kind = window['kind']
    if kind == 'battle':
        return run_battle(window, console, hooks, clock)
    if kind == 'cutscene':
        return run_cutscene(window, console, hooks, clock, prints)
    if kind == 'bot':
        return run_bot(window, console, hooks, clock, prints)
    raise ValueError('not a console window: ' + kind)
