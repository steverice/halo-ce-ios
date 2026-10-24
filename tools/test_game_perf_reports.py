"""Bounded game-report ingestion and epoch statistics."""
import base64
import gzip
import json
import struct

import pytest

import game_perf_reports as perf


def report(intervals=(10000, 30000), flags=None):
    n = len(intervals)
    columns = {"intervalUS": intervals, "elapsedMS": [i * 20 for i in range(n)],
               "cpu": [100] * n, "simulation": [20] * n, "network": list(range(n)),
               "render": [30] * n, "gpu": [65535] * n, "budget": [167] * n,
               "ticks": [1] * n, "flags": flags if flags is not None else [32 if value > 16700 else 0 for value in intervals]}
    trace = {key: base64.b64encode(struct.pack('<' + ('I' if key in ('intervalUS', 'elapsedMS') else 'H') * n, *values)).decode()
             for key, values in columns.items()}
    return {"nonce": "0" * 32, "durationMS": n * 20, "model": "test", "platform": "test", "profileID": "p",
            "mapFingerprint": "unknown", "renderer": "metal", "mode": "campaign", "appVersion": "1",
            "appBuild": "1", "guestSHA256": "unknown", "schema": 1, "frameCount": n, "context": {"model": "test", "profileID": "p"},
            "trace": {"encoding": "game-perf-columns-1", **trace}, "epochs": []}


def test_missing_gpu_and_full_precision_overflow():
    value = report()
    value['trace']['overflow'] = [{"frame": 1, "field": "cpu", "valueUS": 7000000}]
    frames = perf.decode_frames(value)
    assert frames[0]['gpu'] is None
    assert frames[1]['cpu'] == 7000
    assert frames[1]['interval'] == 30


def test_bad_version_and_column_length_are_refused():
    value = report()
    value['schema'] = 9
    with pytest.raises(ValueError):
        perf.decode_frames(value)
    value['schema'] = 1
    value['trace']['cpu'] = ''
    with pytest.raises(ValueError):
        perf.decode_frames(value)


def test_bounded_gzip_and_plain_loader(tmp_path, monkeypatch):
    path = tmp_path / 'report.json.gz'
    path.write_bytes(gzip.compress(json.dumps(report()).encode()))
    assert perf.load_report(path)['schema'] == 1
    monkeypatch.setattr(perf, 'MAX_DECODED_BYTES', 20)
    with pytest.raises(ValueError, match='decoded'):
        perf.load_report(path)
    monkeypatch.setattr(perf, 'MAX_COMPRESSED_BYTES', 10)
    with pytest.raises(ValueError, match='compressed'):
        perf.load_report(path)


def test_epochs_loading_unknowns_and_per_game_weighting():
    first = report((10000, 30000))
    first['epochs'] = [{"frame": 1, "context": {"playerCount": 2}}]
    groups = perf.aggregate_reports([first])
    assert len(groups) == 2
    assert sorted(g['frameWeighted']['lateShare'] for g in groups) == [0, 1]
    combined = perf.aggregate_reports([report((30000,)), report((10000,) * 9)])[0]
    assert combined['perGame']['lateShare'] == .5
    assert combined['frameWeighted']['lateShare'] == .1
    assert combined['timings']['gpu']['samples'] == 0
    assert combined['timings']['interval']['p99'] == 30


def test_network_correlations_require_matched_observations():
    value = report((10000, 20000, 30000))
    value['networkSamples'] = [{"frame": i, "rttMS": i * 10, "lostPackets": i, "expectedPackets": 10} for i in range(3)]
    group = perf.aggregate_reports([value])[0]
    assert group['correlations']['networkInterval']['samples'] == 3
    assert group['correlations']['rttInterval']['pearson'] == pytest.approx(1)
    assert group['correlations']['lossInterval']['pearson'] == pytest.approx(1)


def test_late_flag_keeps_original_precision_and_loading_is_separate():
    value = report((16701, 90000), flags=[32, 1])
    group = perf.aggregate_reports([value])[0]
    assert group['frames'] == 1
    assert group['loadingFrames'] == 1
    assert group['frameWeighted']['lateShare'] == 1


def test_saturated_value_without_original_is_unknown():
    value = report(flags=[8, 0])
    value['trace']['cpu'] = base64.b64encode(struct.pack('<HH', 65534, 65535)).decode()
    assert all(row['cpu'] is None for row in perf.decode_frames(value))


def test_csv_includes_coverage_and_correlations(tmp_path):
    source, output = tmp_path / 'source.json', tmp_path / 'out.csv'
    source.write_text(json.dumps(report()))
    perf.main([str(source), '--format', 'csv', '--output', str(output), '--rank', 'p99'])
    header = output.read_text().splitlines()[0]
    assert 'gpuCoverage' in header
    assert 'networkIntervalSamples' in header
    assert 'rttIntervalPearson' in header


@pytest.mark.parametrize('trace', [None, [], 'bad'])
def test_nonobject_trace_is_refused(trace):
    value = report()
    value['trace'] = trace
    with pytest.raises(ValueError):
        perf.decode_frames(value)


def test_maximum_quantized_value_without_overflow_flag_remains_valid():
    value = report()
    value['trace']['cpu'] = base64.b64encode(struct.pack('<HH', 65534, 10)).decode()
    assert perf.decode_frames(value)[0]['cpu'] == 6553.4


def test_loading_quantiles_and_report_drop_counts_are_separate():
    value = report((10000, 90000), flags=[0, 1])
    value['droppedFrames'] = 12
    group = perf.aggregate_reports([value])[0]
    assert group['timings']['interval']['max'] == 10
    assert group['loadingTimings']['interval']['max'] == 90
    assert group['reportDroppedFrames'] == 12


def test_public_validation_checks_metadata_and_cloud_duplicates():
    value = report()
    value.update(nonce='0' * 32, durationMS=40, model='test', platform='test', profileID='p',
                 mapFingerprint='unknown', renderer='metal', mode='campaign', appVersion='1',
                 appBuild='1', guestSHA256='unknown')
    perf.validate_report(value, {'schema': 1, 'model': 'test'})
    with pytest.raises(ValueError, match='model'):
        perf.validate_report(value, {'model': 'other'})
    del value['nonce']
    with pytest.raises(ValueError, match='nonce'):
        perf.validate_report(value)


def test_loader_verifies_hash_of_uncompressed_payload(tmp_path):
    import hashlib
    raw = json.dumps(report()).encode()
    path = tmp_path / 'report.gz'
    path.write_bytes(gzip.compress(raw))
    assert perf.load_report(path, expected_sha256=hashlib.sha256(raw).hexdigest())['schema'] == 1
    with pytest.raises(ValueError, match='SHA256'):
        perf.load_report(path, expected_sha256='0' * 64)


def test_cli_refuses_report_without_required_metadata(tmp_path):
    value = report()
    value.pop('nonce', None)
    source = tmp_path / 'invalid.json'
    source.write_text(json.dumps(value))
    with pytest.raises(ValueError, match='nonce'):
        perf.main([str(source)])


def test_initial_epoch_does_not_inherit_final_profile_hash():
    value = report()
    value['profileHash'] = 'a' * 64
    value['epochs'] = [{'frame': 0, 'context': {'model': 'test', 'profileID': 'p'}},
                       {'frame': 1, 'context': {'profileHash': 'a' * 64}}]
    groups = perf.aggregate_reports([value])
    assert len(groups) == 2
    assert {group['context']['profileHash'] for group in groups} == {None, 'a' * 64}


@pytest.mark.parametrize('trailer', [b'garbage', gzip.compress(b'{}')])
def test_loader_rejects_gzip_trailing_bytes_and_members(tmp_path, trailer):
    path = tmp_path / 'report.gz'
    path.write_bytes(gzip.compress(json.dumps(report()).encode()) + trailer)
    with pytest.raises(ValueError, match='gzip'):
        perf.load_report(path)


def test_loader_rejects_duplicate_json_keys_at_any_depth(tmp_path):
    path = tmp_path / 'report.json'
    for raw in (b'{"schema":1,"schema":2}', b'{"context":{"model":"one","model":"two"}}'):
        path.write_bytes(raw)
        with pytest.raises(ValueError, match='duplicate'):
            perf.load_report(path)


def test_loader_rejects_numbers_that_overflow_float(tmp_path):
    path = tmp_path / 'report.json'
    path.write_bytes(b'{"context":{"refreshHz":1e999}}')
    with pytest.raises(ValueError, match='nonfinite'):
        perf.load_report(path)
