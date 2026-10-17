"""Portable evidence extraction preserves intervals and quarantines historical dependencies."""
import copy
import hashlib
import gzip
import sys
import json

import pytest

from tools import benchmark_shaders as shaders


def capture(text="vs 1 0 0\n", status="captured", scope="run_append_interval"):
    return {"format": 1, "map": "a10", "status": status, "scope": scope, "text": text,
            "bytes": len(text.encode()), "sha256": hashlib.sha256(text.encode()).hexdigest()}


def report():
    return {"code": "ABCDEFGH", "app": {"image": "a" * 64, "build": "20261016.1"},
            "runs": [{"index": 0, "map": "a10", "shader_capture": capture()}],
            "shader_dependency_candidates": {"a10": capture(scope="cumulative_unattributed_dependency_candidates")}}


def test_roundtrip_preserves_intervals_and_separates_dependencies(tmp_path):
    source = report()
    previous = copy.deepcopy(source)
    previous["code"] = "ABCDEFGJ"
    source["shader_output_intervals"] = [previous]
    target = tmp_path / "out"
    result = shaders.extract(json.loads(json.dumps(source)), target)
    assert (target / "intervals/ABCDEFGH-00/a10.txt").read_text() == capture()["text"]
    assert (target / "intervals/ABCDEFGJ-00/a10.txt").is_file()
    assert (target / "dependency-candidates/a10.txt").is_file()
    assert len(result["files"]) == 3
    assert "pending" in result["descriptor_validation"]
    with pytest.raises(FileExistsError):
        shaders.extract(source, target)


@pytest.mark.parametrize("field,value", [("sha256", "0" * 64), ("bytes", 1),
    ("scope", "cumulative"), ("map", "../secret"), ("map", "b30"), ("status", "no_new_records")])
def test_bad_or_misattributed_payload_refuses_before_writing(tmp_path, field, value):
    source = report()
    source["runs"][0]["shader_capture"][field] = value
    with pytest.raises(ValueError):
        shaders.extract(source, tmp_path / "out")
    assert not (tmp_path / "out").exists()


def test_old_missing_disabled_and_empty_are_distinct(tmp_path):
    source = report()
    source["runs"] = [{"index": 0, "map": "a10"},
        {"index": 1, "map": "a10", "shader_capture": {"status": "disabled"}},
        {"index": 2, "map": "a10", "shader_capture": capture("", "no_new_records")}]
    source.pop("shader_dependency_candidates")
    result = shaders.extract(source, tmp_path / "out")
    assert [r["status"] for r in result["runs"]] == ["unavailable", "disabled", "no_new_records"]
    assert len(result["files"]) == 1


def test_prior_output_guest_cannot_be_mixed(tmp_path):
    source = report()
    prior = copy.deepcopy(source)
    prior["app"]["image"] = "b" * 64
    source["shader_output_intervals"] = [prior]
    with pytest.raises(ValueError, match="app identity"):
        shaders.extract(source, tmp_path / "out")


def test_compressed_report_cli_roundtrip(tmp_path, monkeypatch):
    source = tmp_path / "report.json.gz"
    with gzip.open(source, "wt", encoding="utf-8") as stream:
        json.dump(report(), stream)
    target = tmp_path / "out"
    monkeypatch.setattr(sys, "argv", ["benchmark_shaders.py", str(source), "--out", str(target)])
    shaders.main()
    evidence = json.loads((target / "evidence.json").read_text())
    raw = (target / "intervals/ABCDEFGH-00/a10.txt").read_bytes()
    assert hashlib.sha256(raw).hexdigest() == evidence["files"]["intervals/ABCDEFGH-00/a10.txt"]


def test_oversized_compressed_report_is_refused(tmp_path, monkeypatch):
    source = tmp_path / "report.json.gz"
    with gzip.open(source, "wb") as stream:
        stream.write(b"x" * 20)
    monkeypatch.setattr(shaders, "MAXIMUM", 10)
    target = tmp_path / "out"
    monkeypatch.setattr(sys, "argv", ["benchmark_shaders.py", str(source), "--out", str(target)])
    with pytest.raises(SystemExit):
        shaders.main()
    assert not target.exists()
