"""Decode bounded game performance assets and rank context epochs.

Correlations describe paired observations and do not establish cause. Interval
lateness is a proxy unless presentation evidence is supplied by the recorder.
"""
import argparse
import base64
import binascii
import csv
import zlib
import hashlib
import re
import json
import math
from pathlib import Path
import struct

MAX_COMPRESSED_BYTES = 4 * 1024 * 1024
MAX_DECODED_BYTES = 32 * 1024 * 1024
COLUMNS = ('intervalUS', 'elapsedMS', 'cpu', 'simulation', 'network', 'render', 'gpu', 'budget', 'ticks', 'flags')
TIMINGS = ('interval', 'cpu', 'simulation', 'network', 'render', 'gpu', 'budget')
CONTEXT = ('model', 'platform', 'profileID', 'profileHash', 'mapFingerprint', 'mode', 'playerCount',
           'localPlayerCount', 'networkPlayerCount', 'appVersion', 'appBuild', 'guestSHA256', 'output', 'role', 'renderer', 'width', 'height', 'refreshMilliHz', 'os', 'osVersion', 'mapFormat')


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON key: ' + key)
        result[key] = value
    return result


def _finite_float(text):
    value = float(text)
    if not math.isfinite(value):
        raise ValueError('nonfinite JSON number')
    return value


def load_report(path, expected_sha256=None):
    """Read plain JSON or gzip without materializing an unbounded asset."""
    path = Path(path)
    with path.open('rb') as source:
        magic = source.read(2)
        source.seek(0)
        compressed = magic == b'\x1f\x8b'
        if compressed and path.stat().st_size > MAX_COMPRESSED_BYTES:
            raise ValueError('compressed report exceeds size limit')
        try:
            if compressed:
                packed = source.read(MAX_COMPRESSED_BYTES + 1)
                if len(packed) > MAX_COMPRESSED_BYTES:
                    raise ValueError('compressed report exceeds size limit')
                decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
                raw = decoder.decompress(packed, MAX_DECODED_BYTES + 1)
                if len(raw) > MAX_DECODED_BYTES:
                    raise ValueError('decoded report exceeds size limit')
                if not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
                    raise ValueError('invalid gzip report: truncated or trailing data')
            else:
                raw = source.read(MAX_DECODED_BYTES + 1)
        except (OSError, EOFError, zlib.error) as error:
            raise ValueError('invalid gzip report') from error
    if len(raw) > MAX_DECODED_BYTES:
        raise ValueError('decoded report exceeds size limit')
    if expected_sha256 is not None and hashlib.sha256(raw).hexdigest() != expected_sha256:
        raise ValueError('payload SHA256 mismatch')
    try:
        value = json.loads(raw, object_pairs_hook=_unique_object, parse_float=_finite_float, parse_constant=lambda value: (_ for _ in ()).throw(ValueError('nonfinite JSON number')))
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValueError('invalid report JSON') from error
    if not isinstance(value, dict):
        raise ValueError('report must be an object')
    return value


RECORD_FIELDS = ('schema', 'durationMS', 'frameCount', 'model', 'platform', 'profileID',
                 'mapFingerprint', 'renderer', 'mode', 'appVersion', 'appBuild', 'guestSHA256')


def validate_report(report, duplicated_fields=None):
    """Validate an asset and optional CloudKit metadata; return the same object.

    Use load_report(..., expected_sha256=record['payloadSHA256']) before this
    function to verify the hash of the exact uncompressed JSON bytes.
    """
    if not isinstance(report, dict):
        raise ValueError('report must be an object')
    for key in RECORD_FIELDS:
        value = report.get(key)
        if key in ('schema', 'durationMS', 'frameCount'):
            if type(value) is not int or value < 0 or value > 2**63 - 1:
                raise ValueError('invalid ' + key)
        elif not isinstance(value, str) or not value or len(value) > 4096:
            raise ValueError('invalid ' + key)
    if not isinstance(report.get('nonce'), str) or not re.fullmatch(r'[0-9a-f]{32}', report['nonce']):
        raise ValueError('invalid nonce')
    for key in ('mapFingerprint', 'guestSHA256', 'profileHash'):
        value = report.get(key, 'unknown')
        if value != 'unknown' and (not isinstance(value, str) or not re.fullmatch(r'[0-9a-f]{64}', value)):
            raise ValueError('invalid ' + key)
    for key in ('context', 'graphics'):
        if key in report and not isinstance(report[key], dict):
            raise ValueError('invalid ' + key)
    for key, limit in (('epochs', 4096), ('events', 4096), ('windows', 8), ('gaps', 4096), ('overflow', MAX_DECODED_BYTES // 24 * 7)):
        if key in report and (not isinstance(report[key], list) or len(report[key]) > limit or any(not isinstance(item, dict) for item in report[key])):
            raise ValueError('invalid ' + key)
    count = report['frameCount']
    previous = 0
    for epoch in report.get('epochs', []):
        frame = epoch.get('frame')
        if type(frame) is not int or not previous <= frame <= count or not isinstance(epoch.get('context'), dict):
            raise ValueError('invalid epoch')
        previous = frame
    for key in ('droppedFrames', 'droppedEvents'):
        if key in report and (type(report[key]) is not int or report[key] < 0):
            raise ValueError('invalid ' + key)
    for key, value in (duplicated_fields or {}).items():
        if key in RECORD_FIELDS and (type(value) is not type(report[key]) or value != report[key]):
            raise ValueError('duplicated field mismatch: ' + key)
    decode_frames(report)
    return report


def decode_frames(report):
    """Decode schema 1 columns; unavailable or unpreserved overflow stays null."""
    trace = report.get('trace', {})
    if type(report.get('schema')) is not int or report.get('schema') != 1 or not isinstance(trace, dict) or trace.get('encoding') != 'game-perf-columns-1':
        raise ValueError('unsupported game performance encoding')
    count = report.get('frameCount')
    if type(count) is not int or not 0 <= count <= MAX_DECODED_BYTES // 24:
        raise ValueError('invalid frame count')
    columns = {}
    for name in COLUMNS:
        width, code = (4, 'I') if name in COLUMNS[:2] else (2, 'H')
        try:
            raw = base64.b64decode(trace[name], validate=True)
        except (KeyError, ValueError, TypeError, binascii.Error) as error:
            raise ValueError(f'invalid column {name}') from error
        if len(raw) != count * width:
            raise ValueError(f'invalid column length: {name}')
        columns[name] = struct.unpack('<' + str(count) + code, raw)
    originals = {}
    for item in report.get('overflow', trace.get('overflow', [])):
        frame, field, value = item.get('frame'), item.get('field'), item.get('valueUS')
        if type(frame) is not int or not 0 <= frame < count or field not in TIMINGS + ('intervalUS',) or type(value) is not int or value < 0:
            raise ValueError('invalid overflow entry')
        originals[frame, 'interval' if field == 'intervalUS' else field] = value / 1000
    frames = []
    for index in range(count):
        row = {'elapsedMS': columns['elapsedMS'][index], 'ticks': columns['ticks'][index], 'flags': columns['flags'][index]}
        for field in TIMINGS:
            raw = columns['intervalUS' if field == 'interval' else field][index]
            sentinel = 0xffffffff if field == 'interval' else 65535
            # 65534 is saturated: without an original value its duration is unknown.
            unavailable = raw == sentinel or (field != 'interval' and raw == 65534 and row['flags'] & 8)
            row[field] = originals.get((index, field), None if unavailable else raw / (1000 if field == 'interval' else 10))
        if row['flags'] & 4:
            row['gpu'] = None
        frames.append(row)
    return frames


def percentile(values, fraction):
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * fraction) - 1)] if values else None


def correlation(pairs):
    n = len(pairs)
    if n < 2:
        return {'samples': n, 'pearson': None}
    xs, ys = zip(*pairs)
    mx, my = sum(xs) / n, sum(ys) / n
    denominator = math.sqrt(sum((x - mx) ** 2 for x in xs) * sum((y - my) ** 2 for y in ys))
    return {'samples': n, 'pearson': sum((x - mx) * (y - my) for x, y in pairs) / denominator if denominator else None}


def aggregate_reports(reports):
    """Group matching epochs; retain game means separately from pooled frames."""
    groups = {}
    for game, report in enumerate(reports):
        frames = decode_frames(report)
        context = {key: report.get(key) for key in CONTEXT}
        context.update({key: value for key, value in report.get('context', {}).items() if key in CONTEXT})
        epochs = report.get('epochs', [])
        # The first frame-zero epoch is the initial snapshot; the envelope may
        # contain final settings and must not leak them backward in time.
        if epochs and epochs[0].get('frame') == 0:
            context = {key: epochs[0].get('context', {}).get(key) for key in CONTEXT}
        if any(type(e.get('frame')) is not int or not 0 <= e['frame'] <= len(frames) for e in epochs):
            raise ValueError('invalid epoch boundary')
        if [e['frame'] for e in epochs] != sorted(e['frame'] for e in epochs):
            raise ValueError('epochs must be ordered')
        changes = {}
        for epoch in epochs:
            changes.setdefault(epoch['frame'], []).append(epoch.get('context', {}))
        network = {s['frame']: s for s in report.get('networkSamples', []) if type(s.get('frame')) is int}
        for index, row in enumerate(frames):
            for change in changes.get(index, []):
                context.update({key: value for key, value in change.items() if key in CONTEXT})
            key = json.dumps(context, sort_keys=True)
            group = groups.setdefault(key, {'context': dict(context), 'rows': [], 'games': {}, 'network': [], 'rtt': [], 'loss': [], 'loadingFrames': 0, 'loadingRows': [], 'reports': {}, 'summaries': {}, 'gapFrames': 0, 'truncatedGames': set()})
            group['reports'][game] = report.get('droppedFrames', 0)
            group['summaries'][game] = {'retainedTrace': report.get('traceSummary'), 'approximateAllFrames': report.get('summary')}
            if report.get('truncated'):
                group['truncatedGames'].add(game)
            if row['flags'] & 16:
                group['gapFrames'] += 1
                continue
            if row['flags'] & 1:
                group['loadingFrames'] += 1
                group['loadingRows'].append(row)
                continue
            group['rows'].append(row)
            group['games'].setdefault(game, []).append(row)
            interval = row['interval']
            if interval is not None:
                if row['network'] is not None:
                    group['network'].append((row['network'], interval))
                sample = network.get(index, {})
                rtt = sample.get('rttMS')
                if isinstance(rtt, (int, float)) and math.isfinite(rtt) and rtt >= 0:
                    group['rtt'].append((rtt, interval))
                lost, expected = sample.get('lostPackets'), sample.get('expectedPackets')
                if isinstance(lost, int) and isinstance(expected, int) and 0 <= lost <= expected and expected > 0:
                    group['loss'].append((lost / expected, interval))
    result = []
    for group in groups.values():
        rows = group['rows']
        def late_share(items):
            eligible = [r for r in items if r['interval'] is not None and r['budget'] is not None and r['budget'] > 0]
            # The recorder's flag uses original precision before quantization.
            late = sum(bool(r['flags'] & 32) for r in eligible)
            return late / len(eligible) if eligible else None
        shares = [late_share(items) for items in group['games'].values()]
        shares = [value for value in shares if value is not None]
        def summarize(items):
            timings = {}
            for field in TIMINGS:
                values = [row[field] for row in items if row[field] is not None]
                timings[field] = {'samples': len(values), 'coverage': len(values) / len(items) if items else None,
                                  **{name: percentile(values, fraction) for name, fraction in (('p50', .5), ('p90', .9), ('p95', .95), ('p99', .99), ('max', 1))}}
            return timings
        timings = summarize(rows)
        result.append({'context': group['context'], 'games': len(group['reports']), 'frames': len(rows),
                       'loadingFrames': group['loadingFrames'], 'gapFrames': group['gapFrames'], 'truncatedGames': len(group['truncatedGames']),
                       'frameWeighted': {'lateShare': late_share(rows)}, 'perGame': {'lateShare': sum(shares) / len(shares) if shares else None},
                       'loadingTimings': summarize(group['loadingRows']),
                       'reportDroppedFrames': sum(group['reports'].values()),
                       'reportSummaries': list(group['summaries'].values()),
                       'summaryScope': 'Report summaries cover the whole report, not this context epoch.',
                       'latenessBasis': 'frame-start interval over budget proxy', 'timings': timings,
                       'correlations': {name + 'Interval': correlation(group[source]) for name, source in (('network', 'network'), ('rtt', 'rtt'), ('loss', 'loss'))}})
    return sorted(result, key=lambda g: (g['frameWeighted']['lateShare'] or 0, g['timings']['interval']['p99'] or 0), reverse=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reports', nargs='+', type=Path)
    parser.add_argument('--format', choices=('json', 'csv'), default='json')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--rank', choices=('late-share', 'p99'), default='late-share')
    args = parser.parse_args(argv)
    result = aggregate_reports([validate_report(load_report(path)) for path in args.reports])
    if args.rank == 'p99':
        result.sort(key=lambda group: group['timings']['interval']['p99'] or 0, reverse=True)
    import sys
    stream = args.output.open('w', newline='') if args.output else sys.stdout
    try:
        if args.format == 'json':
            json.dump(result, stream, indent=2, allow_nan=False)
            stream.write('\n')
        else:
            fields = list(CONTEXT) + ['games', 'frames', 'loadingFrames', 'gapFrames', 'truncatedGames', 'reportDroppedFrames', 'lateShare', 'perGameLateShare', 'p99MS']
            fields += [field + suffix for field in TIMINGS for suffix in ('Samples', 'Coverage', 'P50', 'P90', 'P95', 'P99', 'Max')]
            fields += [name + suffix for name in ('networkInterval', 'rttInterval', 'lossInterval') for suffix in ('Samples', 'Pearson')]
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            for group in result:
                row = {**group['context'], **{k: group[k] for k in fields if k in group},
                       'lateShare': group['frameWeighted']['lateShare'], 'perGameLateShare': group['perGame']['lateShare'],
                       'p99MS': group['timings']['interval']['p99'], 'gpuSamples': group['timings']['gpu']['samples']}
                for field in TIMINGS:
                    row.update({field + suffix: group['timings'][field][key] for suffix, key in
                                (('Samples', 'samples'), ('Coverage', 'coverage'), ('P50', 'p50'), ('P90', 'p90'), ('P95', 'p95'), ('P99', 'p99'), ('Max', 'max'))})
                for name, values in group['correlations'].items():
                    row.update({name + 'Samples': values['samples'], name + 'Pearson': values['pearson']})
                writer.writerow(row)
    finally:
        if args.output:
            stream.close()


if __name__ == '__main__':
    main()
