#!/usr/bin/env python3
"""Write a Settings bundle with rows for the build's enabled cloud features."""
import argparse
import plistlib
from pathlib import Path

CLOUD_ROWS = [
    {'Type': 'PSGroupSpecifier', 'Title': 'iCloud Saves',
     'FooterText': 'Profiles, finished levels, the Save and Quit checkpoint and custom game types are copied to your '
                   'private iCloud storage and follow you to your other devices. Game data, textures and settings stay '
                   'on this device. Turning iCloud Saves on takes effect at the next launch. Turning it off stops further sync operations. '
                   'Recovery actions take effect at the next launch with iCloud Saves on.'},
    {'Type': 'PSToggleSwitchSpecifier', 'Title': 'iCloud Saves', 'Key': 'cloud_saves', 'DefaultValue': False},
    {'Type': 'PSTitleValueSpecifier', 'Title': 'Status', 'Key': 'cloud_saves_status', 'DefaultValue': 'Off'},
    {'Type': 'PSToggleSwitchSpecifier', 'Title': 'Restore the last set-aside save', 'Key': 'cloud_saves_restore',
     'DefaultValue': False},
    {'Type': 'PSToggleSwitchSpecifier', 'Title': 'Delete saves from iCloud', 'Key': 'cloud_saves_delete',
     'DefaultValue': False},
]


SHADER_ROWS = [
    {'Type': 'PSGroupSpecifier', 'Title': 'Shader improvements',
     'FooterText': 'VISR automatically sends missing shader descriptions and device model information to improve loading. '
                   "Reports exclude gameplay and personal file names. Apple's upload service attaches an account-specific identifier. "
                   'Turn off Share shader improvements in settings to stop future contributions.'},
    {'Type': 'PSToggleSwitchSpecifier', 'Title': 'Share shader improvements',
     'Key': 'share_shader_improvements', 'DefaultValue': True},
]


def with_shader_rows(root):
    """Add the contribution control only when its build capability is enabled."""
    keys = {row.get('Key') for row in root['PreferenceSpecifiers']}
    if any(row['Key'] in keys for row in SHADER_ROWS if 'Key' in row):
        raise ValueError('the Settings bundle already has a shader improvements row')
    return {**root, 'PreferenceSpecifiers': [*root['PreferenceSpecifiers'], *SHADER_ROWS]}


def with_cloud_rows(root):
    """the bundle's Root.plist content with the iCloud saves group after the rows it has"""
    keys = {row.get('Key') for row in root['PreferenceSpecifiers']}
    if any(row['Key'] in keys for row in CLOUD_ROWS if 'Key' in row):
        raise ValueError('the Settings bundle already has an iCloud saves row')
    return {**root, 'PreferenceSpecifiers': [*root['PreferenceSpecifiers'], *CLOUD_ROWS]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='the Settings.bundle to start from')
    parser.add_argument('output', type=Path, help='the Settings.bundle to write')
    parser.add_argument('--cloud-saves', choices=('on', 'off'), default='on')
    parser.add_argument('--shader-upload', choices=('on', 'off'), default='off')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for path in args.source.iterdir():
        if path.name != 'Root.plist':
            (args.output / path.name).write_bytes(path.read_bytes())
    root = plistlib.loads((args.source / 'Root.plist').read_bytes())
    if args.cloud_saves == 'on':
        root = with_cloud_rows(root)
    if args.shader_upload == 'on':
        root = with_shader_rows(root)
    (args.output / 'Root.plist').write_bytes(plistlib.dumps(root, fmt=plistlib.FMT_XML, sort_keys=False))


if __name__ == '__main__':
    main()
