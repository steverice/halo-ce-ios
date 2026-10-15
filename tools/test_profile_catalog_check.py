"""Synthetic hardware catalogs must fail closed before they can supply defaults."""
from copy import deepcopy
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import profile_check


def test_default_folder_checks_catalog(tmp_path, monkeypatch):
    for platform in profile_check.PLATFORMS:
        (tmp_path / f"{platform}.toml").write_text(
            f'schema = 1\nname = "Class"\nkind = "class"\n'
            f'[match]\nplatform = "{platform}"\ngpu_family = [1, 0]\n')
    (tmp_path / "hardware").mkdir()
    (tmp_path / "hardware/catalog.toml").write_text('schema = 99\n')
    monkeypatch.setattr(profile_check, "PROFILES", tmp_path)
    assert profile_check.main([]) == 1


def catalog():
    return {"schema": 1, "version": "test-1", "hardware": [
        {"id": "a9", "platform": "ios", "models": ["iPhone8,1"], "tier": 10,
         "gpu_family": 3, "basis": "assumed", "source": "Synthetic conservative assessment"}],
        "fallback": [{"hardware": "a9", "profile": "phone.toml", "minimum_memory_mb": 2048,
                      "minimum_gpu_family": 3, "maximum_pixels": [1000000, 1000000, 1000000],
                      "maximum_refresh_hertz": 60}]}


def profiles():
    return {"phone.toml": {"kind": "model", "match": {"platform": "ios", "models": ["iPhone8,1"]}},
            "class.toml": {"kind": "class", "match": {"platform": "ios", "gpu_family": [1, 0]}}}


def check(table, tables=None):
    import profile_catalog_check
    return profile_catalog_check.check_table(table, profiles() if tables is None else tables)


def test_valid_catalog_and_untiered_exact_only_hardware():
    table = catalog()
    other = {**table["hardware"][0], "id": "unknown", "models": ["iPhone99,1"]}
    del other["tier"]
    table["hardware"].append(other)
    assert check(table) == []


@pytest.mark.parametrize("section,field,value,word", [
    (None, "schema", True, "schema"), (None, "schema", 2, "schema"),
    (None, "version", "", "version"), (None, "version", "v" * 49, "version"),
    (None, "and_newer", True, "unknown"), (None, "hardware", {}, "hardware"),
    (None, "fallback", [False], "fallback[0]"),
    ("hardware", "id", "", "id"), ("hardware", "platform", "android", "platform"),
    ("hardware", "models", ["bad/name"], "models"), ("hardware", "chips", [True], "chips"),
    ("hardware", "models", ["a"] * 33, "models"),
    ("hardware", "tier", True, "tier"), ("hardware", "tier", 0, "tier"),
    ("hardware", "tier", 1000001, "tier"), ("hardware", "gpu_family", 33, "gpu_family"),
    ("hardware", "gpu_family", True, "gpu_family"), ("hardware", "basis", "guess", "basis"),
    ("hardware", "gpu_family", 0, "gpu_family"),
    ("hardware", "source", " ", "source"), ("hardware", "and_newer", True, "unknown"),
    ("fallback", "hardware", "missing", "unknown hardware"),
    ("fallback", "profile", "../phone.toml", "profile"),
    ("fallback", "profile", "missing.toml", "profile"),
    ("fallback", "minimum_memory_mb", True, "minimum_memory_mb"),
    ("fallback", "minimum_memory_mb", -1, "minimum_memory_mb"),
    ("fallback", "minimum_gpu_family", 0, "minimum_gpu_family"),
    ("fallback", "minimum_gpu_family", True, "minimum_gpu_family"),
    ("fallback", "maximum_pixels", [1, 2], "maximum_pixels"),
    ("fallback", "maximum_pixels", [1, True, 3], "maximum_pixels"),
    ("fallback", "maximum_pixels", [1, 0, 3], "maximum_pixels"),
    ("fallback", "maximum_pixels", [1, 100000001, 3], "maximum_pixels"),
    ("fallback", "maximum_refresh_hertz", 1001, "maximum_refresh_hertz"),
    ("fallback", "maximum_refresh_hertz", 0, "maximum_refresh_hertz"),
    ("fallback", "maximum_refresh_hertz", True, "maximum_refresh_hertz"),
    ("fallback", "extra", 1, "unknown"),
])
def test_rejects_malformed_fields(section, field, value, word):
    table = catalog()
    target = table if section is None else table[section][0]
    target[field] = value
    assert any(word in problem for problem in check(table)), check(table)


@pytest.mark.parametrize("section", ["hardware", "fallback"])
def test_bounds_entry_arrays(section):
    table = catalog()
    table[section] *= 129
    assert any("128" in problem for problem in check(table))


@pytest.mark.parametrize("section,fields", [(None, ["schema", "version", "hardware", "fallback"]),
    ("hardware", ["id", "platform", "gpu_family", "basis", "source"]),
    ("fallback", list(catalog()["fallback"][0]))])
def test_required_fields(section, fields):
    for field in fields:
        table = catalog()
        del (table if section is None else table[section][0])[field]
        assert check(table), (section, field)


def test_requires_identity_and_unique_ids_and_platform_identities():
    table = catalog()
    del table["hardware"][0]["models"]
    assert any("identity" in p for p in check(table))
    table = catalog()
    table["hardware"].append(deepcopy(table["hardware"][0]))
    assert any("duplicate id" in p for p in check(table))
    table["hardware"][1]["id"] = "another"
    assert any("ambiguous" in p for p in check(table))
    table["hardware"][1]["platform"] = "tvos"
    assert check(table) == []


def test_mac_models_can_distinguish_disjoint_chips_only():
    table = catalog()
    table["fallback"] = []
    entry = table["hardware"][0]
    entry.update(platform="mac", models=["Mac14,1"], chips=["Apple M2"])
    table["hardware"].append({**entry, "id": "m2-pro", "chips": ["Apple M2 Pro"]})
    assert check(table) == []
    table["hardware"][1]["chips"] = ["Apple M2"]
    assert any("ambiguous" in p for p in check(table))
    del table["hardware"][1]["chips"]
    assert any("ambiguous" in p for p in check(table))


def test_model_only_and_chip_only_mac_entries_overlap():
    table = catalog()
    table["fallback"] = []
    entry = table["hardware"][0]
    entry.update(platform="mac", models=["Mac14,1"])
    chip_entry = {**entry, "id": "m2", "chips": ["Apple M2"]}
    del chip_entry["models"]
    table["hardware"].append(chip_entry)
    assert any("ambiguous" in problem for problem in check(table))


def test_disjoint_mac_models_can_share_a_chip_constraint():
    table = catalog()
    table["fallback"] = []
    entry = table["hardware"][0]
    entry.update(platform="mac", models=["Mac14,1"], chips=["Apple M2"])
    table["hardware"].append({**entry, "id": "another-m2", "models": ["Mac14,2"]})
    assert check(table) == []


def test_fallback_requires_tier_platform_identity_and_model_profile():
    table = catalog()
    del table["hardware"][0]["tier"]
    assert any("tier" in p for p in check(table))
    for field, value, word in [("platform", "tvos", "platform"),
                                ("models", ["iPhone9,1"], "identity")]:
        tables = profiles()
        tables["phone.toml"]["match"][field] = value
        assert any(word in p for p in check(catalog(), tables))
    tables = profiles()
    tables["phone.toml"]["kind"] = "class"
    assert any("model" in p for p in check(catalog(), tables))


def test_duplicate_platform_tier_fallback_is_rejected():
    table = catalog()
    table["fallback"].append(deepcopy(table["fallback"][0]))
    assert any("duplicate fallback" in p for p in check(table))


def test_source_class_must_cover_exact_source_gpu_family():
    tables = profiles()
    tables["class.toml"]["match"]["gpu_family"] = [4, 0]
    assert any("source class" in p for p in check(catalog(), tables))
    tables["class.toml"]["match"]["gpu_family"] = [1, 2]
    assert any("source class" in p for p in check(catalog(), tables))
    tables["class.toml"]["match"]["gpu_family"] = [3, 3]
    assert check(catalog(), tables) == []


def test_file_rejects_missing_invalid_and_oversized_catalog(tmp_path):
    import profile_catalog_check
    path = tmp_path / "catalog.toml"
    assert profile_catalog_check.check_catalog(path, profiles())
    path.write_text("not valid = [")
    assert any("parse" in p for p in profile_catalog_check.check_catalog(path, profiles()))
    path.write_text("#" * (64 * 1024 + 1))
    assert any("65536" in p for p in profile_catalog_check.check_catalog(path, profiles()))


def test_file_checks_real_profile_references(tmp_path):
    import profile_catalog_check
    hardware = tmp_path / "hardware"
    hardware.mkdir()
    path = hardware / "catalog.toml"
    path.write_text('''schema = 1
version = "test"
[[hardware]]
id = "a9"
platform = "ios"
models = ["iPhone8,1"]
tier = 10
gpu_family = 3
basis = "assumed"
source = "Synthetic test evidence"
[[fallback]]
hardware = "a9"
profile = "phone.toml"
minimum_memory_mb = 2048
minimum_gpu_family = 3
maximum_pixels = [1000000, 1000000, 1000000]
maximum_refresh_hertz = 60
''')
    (tmp_path / "class.toml").write_text('''schema = 1
name = "Class"
kind = "class"
[match]
platform = "ios"
gpu_family = [1, 0]
''')
    profile = tmp_path / "phone.toml"
    good = '''schema = 1
name = "Phone"
kind = "model"
[match]
platform = "ios"
models = ["iPhone8,1"]
'''
    profile.write_text(good)
    assert profile_catalog_check.check_catalog(path) == []
    assert profile_check.main([str(profile)]) == 0
    profile.write_text(good + "[display]\nrender_height = 540\n")
    assert any("valid model profile" in p for p in profile_catalog_check.check_catalog(path))


def test_chip_only_source_and_duplicate_source_class():
    table, tables = catalog(), profiles()
    table["hardware"][0]["chips"] = ["Apple A9"]
    del table["hardware"][0]["models"]
    tables["phone.toml"]["match"]["chips"] = ["Apple A9"]
    assert check(table, tables) == []
    tables["second-class.toml"] = deepcopy(tables["class.toml"])
    assert any("source class" in p for p in check(table, tables))


@pytest.mark.parametrize("section,field,value", [
    ("hardware", "id", "invalid id"), ("hardware", "id", "a.b"),
    ("hardware", "id", "café"), (None, "version", "é" * 25),
    ("fallback", "minimum_memory_mb", 1000001),
    ("fallback", "profile", "a" * 59 + ".toml"),
])
def test_matches_runtime_storage_bounds(section, field, value):
    table = catalog()
    target = table if section is None else table[section][0]
    target[field] = value
    tables = profiles()
    if field == "profile":
        tables[value] = deepcopy(tables["phone.toml"])
    assert any(field in problem for problem in check(table, tables))


def test_empty_catalog_and_utf8_version_byte_boundary():
    assert check({"schema": 1, "version": "é" * 24, "hardware": [], "fallback": []}) == []
