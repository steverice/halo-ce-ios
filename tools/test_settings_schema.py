"""Source-derived setting types, build defaults, contexts and artifact freshness."""
import copy
import json
from pathlib import Path
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import settings_schema as schema


def test_config_comes_from_every_c_entry():
    settings = schema.config_settings()
    assert len(settings) > 150
    config = schema.config_schema(settings)
    assert sum(len(section['properties']) for section in config['properties'].values()) == len(settings)
    display = config['properties']['display']['properties']
    assert display['render_scale']['type'] == 'number'
    assert display['renderer']['default'] == 'metal'
    assert 'maximum' not in display['shadow_map_size']
    assert display['shadow_map_size']['type'] == 'integer'
    online = config['properties']['network']['properties']['map_download']
    assert online['default'] is False
    assert online['x-defaults'] == {'apple': False, 'desktop': True}
    assert online['x-environment'] == 'HALO_NET_MAP_DOWNLOAD'
    assert config['additionalProperties'] is False


def test_build_defaults_follow_conditions_not_declaration_order():
    source = (schema.ROOT / 'port/linux/src/port_config.c').read_text()
    start = source.index('#ifdef HALO_IOS\n#define DEFAULT_INTERNET_PLAY')
    finish = source.index('#endif', start)
    first, second = source[start:finish].split('#else')
    source = source[:start] + '#ifndef HALO_IOS' + second + '#else\n' + first.split('\n', 1)[1] + source[finish:]
    assert schema.config_settings(source)['network.map_download']['x-defaults'] == {'apple': False, 'desktop': True}


def test_profile_contexts_and_values():
    profile = schema.profile_schema(schema.config_settings())
    validator = pytest.importorskip('jsonschema').Draft202012Validator(profile)
    good = {'schema': 1, 'name': 'Example', 'kind': 'model', 'match': {'platform': 'ios', 'models': ['iPhone18,1']}}
    for field, value in [('shadow_map_size', 1024), ('frame_rate', '30'), ('frame_rate', '1000')]:
        table = copy.deepcopy(good)
        table['display'] = {field: value}
        assert not list(validator.iter_errors(table))
    for field, value in [('shadow_map_size', 2048), ('frame_rate', '29'), ('frame_rate', '1001'), ('renderer', 'vulkan'), ('typo', 1)]:
        table = copy.deepcopy(good)
        table['display'] = {field: value}
        assert list(validator.iter_errors(table)), table
    for context in ['scene', 'external.scene']:
        table = copy.deepcopy(good)
        target = table
        for part in context.split('.'):
            target = target.setdefault(part, {})
        target['menu'] = {'display': {'render_scale': 0.5}}
        assert not list(validator.iter_errors(table))
        target['menu']['display']['shadow_map_size'] = 256
        assert list(validator.iter_errors(table))
    table = copy.deepcopy(good)
    table['external'] = {'display': {'output_height': 1080}}
    assert not list(validator.iter_errors(table))
    table['display'] = {'output_height': 1080}
    assert list(validator.iter_errors(table))


def test_markdown_lists_all_source_keys():
    settings = schema.config_settings()
    for name in settings:
        assert f'`{name}`' in schema.markdown('config', settings)
    for name in schema.profile_check.profile_keys():
        assert f'`{name}`' in schema.markdown('profile', settings)


def test_artifacts_are_current():
    assert schema.main(['--check']) == 0


def test_all_shipped_profiles_and_catalog_validate():
    import profile_check
    jsonschema = pytest.importorskip('jsonschema')
    validator = jsonschema.Draft202012Validator(schema.profile_schema(schema.config_settings()))
    for path in sorted((schema.ROOT / 'profiles').glob('*.toml')):
        assert not list(validator.iter_errors(profile_check.load_toml(path))), path
    catalog = profile_check.load_toml(schema.ROOT / 'profiles/hardware/catalog.toml')
    jsonschema.validate(catalog, schema.catalog_schema())


def test_parser_refuses_unparsed_entries_and_unknown_defaults():
    source = (schema.ROOT / 'port/linux/src/port_config.c').read_text()
    with pytest.raises(ValueError):
        schema.config_settings(source.replace('DEFAULT_MAP_DOWNLOAD,', 'UNKNOWN_DEFAULT,'))
    with pytest.raises(ValueError):
        schema.config_settings(source.replace('_config_real, "1.0", "HALO_RENDER_SCALE"', '_config_missing, "1.0", "HALO_RENDER_SCALE"'))


def test_check_detects_stale_schema_and_markdown(tmp_path, monkeypatch):
    root = tmp_path
    for relative, text in schema.artifacts().items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    monkeypatch.setattr(schema, 'ROOT', root)
    # artifacts are computed before replacing the root; pass the expected snapshot explicitly.
    expected = {str(path.relative_to(root)): path.read_text() for path in root.rglob('*') if path.is_file()}
    assert schema.write_artifacts(expected, check=True) == 0
    for relative in ['port/settings/config.schema.json', 'profiles/README.md']:
        (root / relative).write_text('stale\n')
        assert schema.write_artifacts(expected, check=True) == 1
        (root / relative).write_text(expected[relative])


def test_schemas_are_well_formed_and_config_uses_config_limits():
    jsonschema = pytest.importorskip('jsonschema')
    settings = schema.config_settings()
    for document in [schema.config_schema(settings), schema.profile_schema(settings), schema.catalog_schema()]:
        jsonschema.Draft202012Validator.check_schema(document)
    jsonschema.validate({'display': {'shadow_map_size': 2048}}, schema.config_schema(settings))


@pytest.mark.parametrize('relative,before,after', [
    ('port/linux/src/port_config.c', '128 (as on the Xbox) to 2048', '128 (as on the Xbox) to 4096'),
    ('port/linux/src/port_profile_keys.h', 'shadow_map_size", 128, 1024', 'shadow_map_size", 128, 2048'),
])
def test_source_edits_require_schema_and_document_regeneration(tmp_path, monkeypatch, relative, before, after):
    expected = schema.artifacts()
    for name, content in expected.items():
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    for name in ['port/linux/src/port_config.c', 'port/linux/src/port_profile_keys.h']:
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes((schema.ROOT / name).read_bytes())
    monkeypatch.setattr(schema, 'ROOT', tmp_path)
    monkeypatch.setattr(schema.profile_check, 'KEYS', tmp_path / 'port/linux/src/port_profile_keys.h')
    assert schema.main(['--check']) == 0
    source = tmp_path / relative
    assert before in source.read_text()
    source.write_text(source.read_text().replace(before, after))
    assert schema.main(['--check']) == 1
    assert schema.main([]) == 0
    assert schema.main(['--check']) == 0
