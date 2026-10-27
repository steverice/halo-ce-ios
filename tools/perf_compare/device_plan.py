"""What a VISR device run needs for one window: `init.txt` lines, debug keys and the run length.

Console windows start the level as a one-player hosted game (`debug.network_test`) and hold `init.txt` back with
`debug.init_after` until the level runs, so `init.txt` can carry console commands. Used by the device runner in
step-tools; the in-app VISR suite is not involved.
"""
from __future__ import annotations

import argparse
import json

import windows

INIT_AFTER_S = 25.0   # proven on the A14 iPad for the battles
SLACK_S = 30.0        # load time and teardown beyond the window
TICKS_PER_SECOND = 30


def plan(key: str, table: dict | None = None) -> dict:
    table = windows.load() if table is None else table
    if key not in table:
        raise ValueError(f'unknown window {key}')
    win = table[key]
    kind, level = win['kind'], win['map']
    deathless = '(set cheat_deathless_player true)'
    debug = {'frame_log': '"on"', 'gpu_stats': 'true'}
    if kind == 'opening':
        init = ['game_difficulty_set normal', f'map_name {level}', deathless]
        length = win['warmup_s'] + win['sample_s'] + SLACK_S
    else:
        init = [deathless]
        debug['network_test'] = "'host:levels\\" + level + '\\' + level + ":coop'"
        debug['init_after'] = f'{INIT_AFTER_S}'
        if kind == 'battle':
            init += win['commands']
            length = INIT_AFTER_S + win['warmup_s'] + win['sample_s'] + SLACK_S
        elif kind == 'cutscene':
            init.append(f'(begin ({win["script"]}) (print "XPDONE"))')
            length = INIT_AFTER_S + win['expected_ticks'] / TICKS_PER_SECOND * 1.5 + SLACK_S
        else:
            init.append(f'(begin (sleep_until (>= (game_time) {win["start_tick"]}) 1) (print "XP_START") '
                        f'(sleep {win["ticks"]}) (print "XP_END") (game_speed 0))')
            debug['test_input'] = f'"{win["test_input"]}"'
            length = INIT_AFTER_S + (win['start_tick'] + win['ticks']) / TICKS_PER_SECOND * 1.5 + SLACK_S
    debug['exit_after'] = f'{round(length)}.0'
    return {'window': key, 'kind': kind, 'map': level, 'init': init, 'debug': debug, 'exit_after': float(round(length))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('window')
    parser.add_argument('--init-out', help='write init.txt here')
    parser.add_argument('--debug-out', help='write the [debug] keys (one `key = value` per line) here')
    args = parser.parse_args()
    result = plan(args.window)
    if args.init_out:
        open(args.init_out, 'w').write('\n'.join(result['init']) + '\n')
    if args.debug_out:
        open(args.debug_out, 'w').write(''.join(f'{k} = {v}\n' for k, v in result['debug'].items()))
    print(json.dumps({'exit_after': result['exit_after'], 'kind': result['kind']}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
