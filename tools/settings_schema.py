#!/usr/bin/env python3
"""Generate editor schemas and Markdown references from the C setting definitions.

Run without arguments to refresh checked-in artifacts, --check to check freshness,
or --markdown config|profile to print a reference table. No compiler or optional
Python packages are needed to generate the artifacts.
"""
import argparse
import ast
import json
import re
from pathlib import Path

import profile_check

ROOT = Path(__file__).resolve().parents[1]
DRAFT = 'https://json-schema.org/draft/2020-12/schema'
C_STRING = r'"(?:\\.|[^"\\])*"'


def c_string(expression):
    return ''.join(ast.literal_eval(literal) for literal in re.findall(C_STRING, expression))


def config_settings(text=None):
    """Read every table entry; fail closed if the C table gains unfamiliar syntax."""
    text = (ROOT / 'port/linux/src/port_config.c').read_text() if text is None else text
    prefix, table = text.split('static const struct config_setting config_settings[] =', 1)
    table = table.split('\n};', 1)[0]
    table = re.sub(r'/\*.*?\*/', '', table, flags=re.S)
    macros = {}
    for build in ('apple', 'desktop'):
        active, stack = True, []
        for line in re.sub(r'/\*.*?\*/', '', prefix, flags=re.S).splitlines():
            directive = re.match(r'^#(ifdef|ifndef|else|endif)\b\s*(\w+)?', line)
            if directive:
                operation, symbol = directive.groups()
                if operation in ('ifdef', 'ifndef'):
                    condition = symbol == 'HALO_IOS' and build == 'apple'
                    if operation == 'ifndef':
                        condition = not condition
                    stack.append((active, condition))
                    active = active and condition
                elif operation == 'else':
                    parent, condition = stack[-1]
                    active = parent and not condition
                elif stack:
                    active, _ = stack.pop()
            definition = re.match(r'^#define (DEFAULT_\w+)\s+(' + C_STRING + r')\s*$', line)
            if active and definition:
                macro, literal = definition.groups()
                macros.setdefault(macro, {})[build] = c_string(literal)
    pattern = (r'\{\s*(' + C_STRING + r')\s*,\s*_config_(boolean|integer|real|string)\s*,\s*('
               + C_STRING + r'|DEFAULT_\w+)\s*,\s*(' + C_STRING + r')\s*,\s*'
               r'(_environment_\w+)\s*,\s*(_platform_\w+)\s*,\s*((?:' + C_STRING + r'\s*)+)\}')
    entries = list(re.finditer(pattern, table))
    remainder = re.sub(pattern, '', table)
    if remainder.strip(' \t\n{},') or not entries:
        raise ValueError('unrecognized config_settings syntax; update the generator')
    result = {}
    for entry in entries:
        name, kind, default, environment, style, platform, comment = entry.groups()
        name = c_string(name)
        if name in result:
            raise ValueError(f'duplicate setting {name}')
        if default.startswith('DEFAULT_'):
            values = macros.get(default)
            if not values or set(values) != {'apple', 'desktop'}:
                raise ValueError(f'{name}: unknown build default {default}')
        else:
            values = dict.fromkeys(('apple', 'desktop'), c_string(default))
        defaults = {build: json.loads(value) for build, value in values.items()}
        platforms = {'_platform_all': ['apple', 'desktop'], '_platform_ios': ['apple'],
                     '_platform_desktop': ['desktop'], '_platform_windows': ['windows']}[platform]
        item = {'type': {'real': 'number'}.get(kind, kind), 'default': defaults['apple'],
                'description': ' '.join(c_string(comment).split()),
                'x-environment': c_string(environment), 'x-environment-style': style.removeprefix('_environment_'),
                'x-platforms': platforms}
        if defaults['apple'] != defaults['desktop']:
            item['x-defaults'] = defaults
        result[name] = item
    return result


def object_schema(properties=None, required=()):
    result = {'type': 'object', 'properties': properties or {}, 'additionalProperties': False}
    if required:
        result['required'] = list(required)
    return result


def nested(settings):
    sections = {}
    for name, item in settings.items():
        section, key = name.split('.')
        sections.setdefault(section, object_schema())['properties'][key] = item
    return object_schema(sections)


def config_schema(settings):
    # Profile limits are intentionally narrower than config limits. Until the C
    # config table supplies general bounds, expose only its types and defaults.
    return {'$schema': DRAFT, 'title': 'VISR config.toml', **nested(settings)}


def profile_value(name, key, settings):
    kind, minimum, maximum, choices = key
    item = {'type': {'real': 'number'}.get(kind, kind)}
    if minimum is not None:
        item.update(minimum=minimum, maximum=maximum)
    if choices:
        named = [choice for choice in choices if choice != '#']
        if '#' in choices:
            item['oneOf'] = [{'enum': named}, {'pattern': r'^(?:[3-9][0-9]|[1-9][0-9]{2}|1000)$'}]
        else:
            item['enum'] = named
    if name in settings:
        item['default'] = settings[name]['default']
        item['description'] = settings[name]['description']
    else:
        item['description'] = 'Profile-only output height cap in pixels; zero leaves the external mode uncapped.'
    item['description'] += ' Profile values outside the accepted set are refused, not clamped.'
    return item


def identity_array():
    return {'type': 'array', 'maxItems': 32, 'items': {'type': 'string', 'pattern': r'^[A-Za-z0-9 ,.-]{1,48}$'}}


def profile_schema(settings):
    keys = profile_check.profile_keys()
    scenes, external = profile_check.scene_keys(), profile_check.external_keys()
    values = {name: profile_value(name, key, settings) for name, key in keys.items()}
    def context(names):
        return nested({name: values[name] for name in values if name in names})
    def scene_schema(names):
        return object_schema({scene: context(names & scenes) for scene in profile_check.SCENES})
    properties = context(set(values) - {'display.output_height'})['properties']
    properties.update({
        'schema': {'type': 'integer', 'const': 1},
        'name': {'type': 'string', 'minLength': 1, 'maxLength': 63, 'pattern': r'^[^\x00-\x1f\x7f]+$'},
        'kind': {'enum': ['class', 'model']}, 'status': {'enum': list(profile_check.STATUSES)},
        'match': object_schema({'platform': {'enum': list(profile_check.PLATFORMS)},
            'models': identity_array(), 'chips': identity_array(),
            'gpu_family': {'type': 'array', 'prefixItems': [{'type': 'integer', 'minimum': 1, 'maximum': 32},
                {'type': 'integer', 'minimum': 0, 'maximum': 32}], 'minItems': 2, 'maxItems': 2}}, ['platform']),
        'scene': scene_schema(set(values)),
        'external': object_schema({**context(external)['properties'], 'scene': scene_schema(external)}),
    })
    evidence = {}
    for name in values:
        contexts = []
        if name != 'display.output_height':
            contexts.append(name)
            if name in scenes:
                contexts += [f'scene.{scene}.{name}' for scene in profile_check.SCENES]
        if name in external:
            contexts.append(f'external.{name}')
            if name in scenes:
                contexts += [f'external.scene.{scene}.{name}' for scene in profile_check.SCENES]
        for shown in contexts:
            fields = {'basis': {'enum': list(profile_check.BASES)},
                      'why': {'type': 'string', 'minLength': 20}, 'source': {'type': 'string', 'minLength': 10}}
            required = ['source']
            if shown.startswith('external.'):
                fields['output'] = {'const': 'external'}
                required.append('output')
            evidence[shown] = {**object_schema(fields, ['basis', 'why']), 'allOf': [
                {'if': {'properties': {'basis': {'const': 'measured'}}, 'required': ['basis']},
                 'then': {'required': required}}]}
    properties['evidence'] = object_schema(evidence)
    result = {'$schema': DRAFT, 'title': 'VISR device profile',
              'description': 'Gameplay values, optional menu/cutscene values and external-output values. Only PROFILE_SCENE keys may vary by scene and PROFILE_EXTERNAL keys by output. Run profile_check.py for evidence and cross-file checks.',
              **object_schema(properties, ['schema', 'name', 'kind', 'match'])}
    result['allOf'] = [
        {'if': {'properties': {'kind': {'const': 'class'}}},
         'then': {'properties': {'match': {'required': ['gpu_family'], 'not': {'anyOf': [
             {'required': ['models']}, {'required': ['chips']}]}}}},
         'else': {'properties': {'match': {'not': {'required': ['gpu_family']}, 'anyOf': [
             {'required': ['models'], 'properties': {'models': {'minItems': 1}}},
             {'required': ['chips'], 'properties': {'chips': {'minItems': 1}}}]}}}}]
    return result


def catalog_schema():
    def integer(low, high):
        return {'type': 'integer', 'minimum': low, 'maximum': high}
    hardware = object_schema({
        'id': {'type': 'string', 'pattern': r'^[A-Za-z0-9_-]{1,48}$'},
        'platform': {'enum': list(profile_check.PLATFORMS)}, 'models': identity_array(), 'chips': identity_array(),
        'tier': integer(1, 1000000), 'gpu_family': integer(1, 32),
        'basis': {'enum': list(profile_check.BASES)}, 'source': {'type': 'string', 'pattern': r'\S'}},
        ['id', 'platform', 'gpu_family', 'basis', 'source'])
    hardware['anyOf'] = [{'required': [field], 'properties': {field: {'minItems': 1}}} for field in ['models', 'chips']]
    fallback = object_schema({
        'hardware': {'type': 'string', 'minLength': 1},
        'profile': {'type': 'string', 'maxLength': 63, 'pattern': r'^[a-z0-9]+(-[a-z0-9]+)*\.toml$'},
        'minimum_memory_mb': integer(0, 1000000), 'minimum_gpu_family': integer(1, 32),
        'maximum_pixels': {'type': 'array', 'items': integer(1, 100000000), 'minItems': 3, 'maxItems': 3},
        'maximum_refresh_hertz': integer(1, 1000)},
        ['hardware', 'profile', 'minimum_memory_mb', 'minimum_gpu_family', 'maximum_pixels', 'maximum_refresh_hertz'])
    return {'$schema': DRAFT, 'title': 'VISR hardware capability catalog',
            'description': 'Run profile_check.py to check unique identities, tier coverage, references and UTF-8 byte limits.',
            **object_schema({'schema': {'type': 'integer', 'const': 1},
                'version': {'type': 'string', 'maxLength': 48, 'pattern': r'\S'},
                'hardware': {'type': 'array', 'maxItems': 128, 'items': hardware},
                'fallback': {'type': 'array', 'maxItems': 128, 'items': fallback}},
                ['schema', 'version', 'hardware', 'fallback'])}


def markdown(kind, settings):
    def escape(text):
        return str(text).replace('|', '&#124;').replace('\n', ' ')
    def literal(value):
        return '`' + json.dumps(value, ensure_ascii=False) + '`'
    keys = profile_check.profile_keys()
    rows = []
    names = list(settings) if kind == 'config' else list(keys)
    section = None
    for name in names:
        current = name.split('.')[0]
        if section != current:
            section = current
            rows += [f'### {section}', '', '| Setting | Type and accepted values | Default (Apple) | '
                     + ('Platforms | Environment | ' if kind == 'config' else 'Contexts | ') + 'Description |',
                     '| --- | --- | --- | ' + ('--- | --- | ' if kind == 'config' else '--- | ') + '--- |']
        item = settings.get(name)
        default = literal(item['default']) if item else 'Profile only'
        if item and 'x-defaults' in item:
            default += '; desktop ' + literal(item['x-defaults']['desktop'])
        if kind == 'config':
            accepted = item['type']
            extra = ', '.join(item['x-platforms']) + ' | `' + item['x-environment'] + '`'
        else:
            typ, low, high, choices = keys[name]
            accepted = typ
            if low is not None:
                accepted += f', {low:g} to {high:g}'
            if choices:
                accepted += ': ' + ', '.join('quoted 30 to 1000' if c == '#' else f'`{c}`' for c in choices)
            contexts = ['external only'] if name == 'display.output_height' else ['gameplay']
            if name in profile_check.scene_keys():
                contexts.append('ⓢ scene')
            if name in profile_check.external_keys():
                contexts.append('external')
            extra = ', '.join(contexts)
        description = (item['description'] if item else 'External output height cap in pixels; zero leaves it uncapped.')
        # A period in a setting name is not the end of a sentence.
        description = re.split(r'(?<=[.!?])\s+(?=[A-Z]|false\b|true\b)', description, maxsplit=1)[0]
        rows.append(f'| `{name}` | {escape(accepted)} | {default} | {extra} | {escape(description)} |')
    return '\n'.join(rows) + '\n'


def replace_generated(text, kind, content):
    start, end = f'<!-- BEGIN GENERATED SETTINGS {kind} -->', f'<!-- END GENERATED SETTINGS {kind} -->'
    block = f'{start}\n\n{content.rstrip()}\n\n{end}'
    if start in text:
        return re.sub(re.escape(start) + r'.*?' + re.escape(end), lambda _: block, text, flags=re.S)
    if kind == 'config':
        first = text.index('| Setting | Default | Environment variable | Function |')
        last = text.index('\n\nWith Mesa drivers', first)
        return text[:first] + block + text[last:]
    return text.rstrip() + '\n\n## Profile settings reference\n\n' + block + '\n'


def artifacts():
    settings = config_settings()
    schemas = {'config': config_schema(settings), 'profile': profile_schema(settings), 'catalog': catalog_schema()}
    output = {f'port/settings/{kind}.schema.json': json.dumps(value, indent=2, ensure_ascii=False) + '\n'
              for kind, value in schemas.items()}
    for kind, path in [('config', 'port/linux/README.md'), ('profile', 'profiles/README.md')]:
        output[path] = replace_generated((ROOT / path).read_text(), kind, markdown(kind, settings))
    return output


def write_artifacts(expected, check=False):
    stale = []
    for relative, content in expected.items():
        path = ROOT / relative
        if not path.exists() or path.read_text() != content:
            stale.append(relative)
            if not check:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
    if check and stale:
        print('Stale generated settings artifacts: ' + ', '.join(stale) + '; run python3 tools/settings_schema.py')
    return int(check and bool(stale))


def main(arguments=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='fail if schemas or reference tables need regeneration')
    parser.add_argument('--markdown', choices=['config', 'profile'], help='print a generated reference table')
    args = parser.parse_args(arguments)
    if args.markdown:
        print(markdown(args.markdown, config_settings()), end='')
        return 0
    return write_artifacts(artifacts(), args.check)


if __name__ == '__main__':
    raise SystemExit(main())
