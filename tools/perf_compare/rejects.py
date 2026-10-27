"""Reject rules for console windows (battle, cutscene, bot), applied to a window's outcome from `consolewin`."""
from __future__ import annotations

from consolewin import CUTSCENE_GRACE, TICKS_PER_SECOND, number

CUTSCENE_MIN_SHARE = 0.95
BOT_TICK_TOLERANCE = 2
CODES = ('cutscene-incomplete', 'nothing-placed', 'player-reverted', 'bot-did-not-start', 'bot-tick-drift', 'bot-inactive')
GRENADES = '(unit_get_total_grenade_count (unit (list_get (players) 0)))'


def check(window: dict, outcome: dict) -> list[str]:
    """Raise ValueError with a reject code when the window did not measure what it names. Returns notes (flags
    that do not reject), such as a count that could not be read because the console printed nothing back."""
    kind = window['kind']
    notes: list[str] = []
    if kind == 'cutscene':
        if outcome.get('ended') not in ('print', 'ticks'):
            raise ValueError('cutscene-incomplete')  # no print within 1.5x the expected length, or nothing readable
        if outcome['seconds'] < CUTSCENE_MIN_SHARE * window['expected_ticks'] / TICKS_PER_SECOND:
            raise ValueError('cutscene-incomplete')
    elif kind == 'battle':
        start, end = outcome.get('start', {}), outcome.get('end', {})
        counts = [number(start.get(f'(ai_living_count {name})')) for name in window['encounters']]
        if any(count is not None and count <= 0 for count in counts):
            raise ValueError('nothing-placed')  # an encounter that should be fighting has no actors
        if any(count is None for count in counts):
            notes.append('encounter counts unreadable on this console; placement checked by screenshot only')
        began, ended = number(start.get('(game_time)')), number(end.get('(game_time)'))
        # A checkpoint reload puts game time back; the sample must have advanced about as long as it ran.
        if began is not None and ended is not None and ended < began + 0.9 * window['sample_s'] * TICKS_PER_SECOND:
            raise ValueError('player-reverted')
    elif kind == 'bot':
        if outcome.get('ended') == 'never-started':
            raise ValueError('bot-did-not-start')
        end_tick = number(outcome.get('end', {}).get('(game_time)'))
        if end_tick is not None and abs(end_tick - window['start_tick'] - window['ticks']) > BOT_TICK_TOLERANCE:
            raise ValueError('bot-tick-drift')
        before, after = number(outcome['start'].get(GRENADES)), number(outcome['end'].get(GRENADES))
        if before is not None and after is not None and before == after:
            raise ValueError('bot-inactive')  # the bot never threw a grenade
    return notes


def outliers(window: dict, outcomes: list[dict]) -> dict[int, list[str]]:
    """Per round (by index), the end reads that fall outside the other rounds' range. Flags only; nothing rejects."""
    flags: dict[int, list[str]] = {}
    for expression in window.get('fingerprints', []):
        values = [number(item.get('end', {}).get(expression)) for item in outcomes]
        for index, value in enumerate(values):
            others = [v for i, v in enumerate(values) if i != index and v is not None]
            if value is None or len(others) < 2:
                continue
            if value < min(others) or value > max(others):
                flags.setdefault(index, []).append(f'{expression} ended at {value:g}, outside the other rounds ({min(others):g} to {max(others):g})')
    return flags
