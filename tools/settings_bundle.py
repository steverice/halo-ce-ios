#!/usr/bin/env python3
"""Write the app's Settings bundle with the iCloud saves rows, for builds with --cloud-saves.

The rows exist only in those builds (a switch that does nothing would be a lie in the others), so the
bundle in port/ios stays as it is and the build takes a copy with the rows added."""
import argparse
import plistlib
from pathlib import Path

CLOUD_ROWS = [
    {'Type': 'PSGroupSpecifier', 'Title': 'iCloud saves',
     'FooterText': 'Profiles, finished levels, the Save and Quit checkpoint and custom game types are copied to your '
                   'private iCloud storage and follow you to your other devices. Game data, textures and settings stay '
                   'on this device. Changes to these switches take effect the next time VISR starts.'},
    {'Type': 'PSToggleSwitchSpecifier', 'Title': 'iCloud saves', 'Key': 'cloud_saves', 'DefaultValue': True},
    {'Type': 'PSTitleValueSpecifier', 'Title': 'Status', 'Key': 'cloud_saves_status', 'DefaultValue': 'Off'},
    {'Type': 'PSToggleSwitchSpecifier', 'Title': 'Restore the last set-aside save', 'Key': 'cloud_saves_restore',
     'DefaultValue': False},
    {'Type': 'PSToggleSwitchSpecifier', 'Title': 'Delete saves from iCloud', 'Key': 'cloud_saves_delete',
     'DefaultValue': False},
]


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
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for path in args.source.iterdir():
        if path.name != 'Root.plist':
            (args.output / path.name).write_bytes(path.read_bytes())
    root = with_cloud_rows(plistlib.loads((args.source / 'Root.plist').read_bytes()))
    (args.output / 'Root.plist').write_bytes(plistlib.dumps(root, fmt=plistlib.FMT_XML, sort_keys=False))


if __name__ == '__main__':
    main()
