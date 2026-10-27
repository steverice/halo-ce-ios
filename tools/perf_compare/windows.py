"""Load and validate the cross-port suite's window definitions (`windows.toml`)."""
from __future__ import annotations

from pathlib import Path
import re
import tomllib

WINDOWS = Path(__file__).with_name('windows.toml')
SUITES = ('campaign', 'combat', 'cutscenes', 'multiplayer', 'co-op')
KINDS = ('opening', 'battle', 'cutscene', 'bot')
# Windows the sheet already holds from the previous suite: the map name was the window key.
LEGACY = ('a10', 'b30')
DEFAULT_WINDOWS = ['c10', 'b30']

COMMON = {'suite', 'map', 'kind', 'reference_shot', 'fingerprints'}
ALLOWED = {
    'opening': COMMON | {'warmup_s', 'sample_s'},
    'battle': COMMON | {'warmup_s', 'sample_s', 'commands', 'encounters'},
    'cutscene': COMMON | {'script', 'expected_ticks'},
    'bot': COMMON | {'test_input', 'start_tick', 'ticks'},
}
REQUIRED = {
    'opening': {'warmup_s', 'sample_s'},
    'battle': {'warmup_s', 'sample_s', 'commands', 'encounters'},
    'cutscene': {'script', 'expected_ticks'},
    'bot': {'test_input', 'start_tick', 'ticks'},
}
KEY = re.compile(r'[a-z0-9][a-z0-9-]*')
MAP = re.compile(r'[a-d][0-9]{2}')
SCRIPT = re.compile(r'[a-z0-9_]+')


def validate(table: dict) -> dict:
    """Return the table if every window is well formed; raise ValueError naming the window otherwise."""
    if not table:
        raise ValueError('no windows defined')
    for key, window in table.items():
        def bad(message):
            raise ValueError(f'window {key}: {message}')
        if not KEY.fullmatch(key):
            bad('invalid key')
        if not isinstance(window, dict):
            bad('not a table')
        if window.get('suite') not in SUITES:
            bad('unknown suite')
        kind = window.get('kind')
        if kind not in KINDS:
            bad('unknown kind')
        unknown = set(window) - ALLOWED[kind]
        if unknown:
            bad('unknown keys ' + ', '.join(sorted(unknown)))
        missing = REQUIRED[kind] - set(window)
        if missing:
            bad('missing ' + ', '.join(sorted(missing)))
        if not isinstance(window.get('map'), str) or not MAP.fullmatch(window['map']):
            bad('map must be a stock campaign level such as c10')
        for name in ('warmup_s', 'sample_s', 'expected_ticks', 'start_tick', 'ticks'):
            if name in window and (not isinstance(window[name], int) or isinstance(window[name], bool) or window[name] <= 0):
                bad(f'{name} must be a positive integer')
        for name in ('commands', 'encounters', 'fingerprints'):
            if name in window:
                value = window[name]
                if (not isinstance(value, list) or not all(isinstance(item, str) and item.strip() for item in value)):
                    bad(f'{name} must be a list of console lines')
        if kind == 'battle' and (not window['commands'] or not window['encounters']):
            bad('a battle needs commands and encounters')
        if kind == 'cutscene' and not SCRIPT.fullmatch(str(window['script'])):
            bad('script must be a script name')
        if kind == 'bot' and not re.fullmatch(r'bot:[0-9]+', str(window['test_input'])):
            bad('test_input must look like bot:1')
        shot = window.get('reference_shot')
        if shot is not None and not (isinstance(shot, str) and KEY.fullmatch(shot)):
            bad('reference_shot must be an image name')
        if kind in ('opening', 'battle') and window['sample_s'] != 60:
            bad('sample_s must be 60')
    return table


def load(path: Path = WINDOWS) -> dict:
    with Path(path).open('rb') as stream:
        return validate(tomllib.load(stream))


def resolve(config: dict, table: dict | None = None) -> list[str]:
    """The window keys a refresh config measures: `windows`, or the previous suite's `scenarios`."""
    table = load() if table is None else table
    if 'windows' in config:
        keys = config['windows']
        if (not isinstance(keys, list) or not keys or len(set(keys)) != len(keys)
                or any(key not in table for key in keys)):
            raise ValueError('windows must be distinct keys from windows.toml')
        if 'scenarios' in config:
            raise ValueError('use windows or scenarios, not both')
        return list(keys)
    if 'scenarios' in config:
        keys = config['scenarios']
        # The previous suite measured the openings of a10 and b30, or b30 alone with a delay.
        if keys != ['a10', 'b30'] and not (keys == ['b30'] and config.get('native_delay_seconds', 0) > 0):
            raise ValueError('a10 and b30 scenarios are required')
        return list(keys)
    return list(DEFAULT_WINDOWS)


def map_for(key: str, table: dict | None = None) -> str:
    """The level a window plays. A legacy key (a10) is its own map name."""
    table = load() if table is None else table
    if key in table:
        return table[key]['map']
    if key in LEGACY:
        return key
    raise ValueError(f'unknown window {key}')


def suite_for(key: str, default: str = 'campaign', table: dict | None = None) -> str:
    table = load() if table is None else table
    return table[key]['suite'] if key in table else default
