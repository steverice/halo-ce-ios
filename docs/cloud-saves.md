# iCloud saves

A build made with `--cloud-saves` copies your profiles and campaign saves to the private part of your own iCloud account, so they follow you between iPhone, iPad, Apple Vision Pro, Apple TV and the Mac, and come back on an Apple TV after tvOS has emptied the app's storage. The setting starts off on every device. Until you turn it on, VISR makes no CloudKit calls, waits for nothing from iCloud, and shows no iCloud notices. Builds without the option do not contain the sync engine.

## What is copied

| | |
| --- | --- |
| Copied | Profiles (name, armor color, control settings), which campaign levels are finished, the checkpoint that **Save and Quit** writes, and custom game types |
| Never copied | The game data from your disc, upscaled textures, movies, logs, `config.toml`, and which profile or map this device used last |

On disk these are the folders under `save/u/UDATA`. A profile is half a kilobyte; a checkpoint is 20 MB on disk and usually well under 1 MB once compressed.

## How it behaves

- **At launch**, before the game starts, VISR compares the saves on the device with the ones in iCloud. Missing ones are restored and newer ones downloaded. It waits up to 8 seconds for iCloud, or 20 on a device that has no saves; after that the game starts with what is on the device.
- **While you play**, saves are uploaded within about 15 seconds of the game writing them, again when the app leaves the screen, and once more as it exits. Nothing is downloaded during play, so another device's progress shows up at the next launch.
- **If two devices were played separately**, the save that is further along is kept: for a profile, the one with more levels finished; for a checkpoint, the one on the later level; otherwise the more recent. The other is set aside in iCloud, and the next launch says which was kept and offers **Use the other one**. The five most recent set-aside copies of each profile and checkpoint are kept.
- **Checkpoints belong to a version of the game and to its map files.** A checkpoint only loads in a build with the same saved-game layout, against the same map file. Each combination has its own checkpoint in iCloud, so an older build on one device does not replace a newer build's checkpoint on another, and a device with a different disc import or replacement maps keeps its own. On such a device you see the profile and its finished levels, and no checkpoint to continue until you make one there. Profiles and finished levels are shared by all of them.
- **Deleting a profile** in the game deletes it on your other devices at their next launch, unless one of them has played it since. The deleted profile stays in iCloud and can be restored.
- **Signed out of iCloud, or offline**: the game plays from the saves on the device and catches up later. Nothing on the device is deleted because iCloud is empty or unreachable.
- **A different iCloud account** on the device, or VISR's data deleted from iCloud: VISR asks once at launch what to do and moves nothing until you answer.

VISR never replaces or deletes a save on the device unless iCloud holds those exact bytes.

## Settings

On every Apple platform, open **Network Setup** in the game's profile settings to find **iCloud Saves** and its status line. Use **OK** to save the change; **Cancel** leaves it as it was. The setting belongs to this device, even though the menu is reached through a profile.

On iPhone, iPad and Apple Vision Pro, VISR's page in the Settings app exposes the same stored setting and status, plus recovery actions:

| Row | |
| --- | --- |
| iCloud saves | Off by default. Turning it on takes effect at the next launch; turning it off stops further sync operations. An operation already sent to iCloud may finish |
| Status | For example "Up to date · 3 saves · 2:14 PM", "Waiting for iCloud", "Not signed in to iCloud", "iCloud storage is full" |
| Restore the last set-aside save | Acts at the next launch: the most recent set-aside or deleted save becomes the current one, and the current one is set aside |
| Delete saves from iCloud | Acts at the next launch: removes VISR's saves from iCloud, leaves the saves on this device, and turns iCloud saves off here. Your other devices then ask whether to upload theirs again |

Apple TV and the Mac use the in-game row. On the Mac, recovery actions can also be requested with `defaults write <bundle id> cloud_saves_restore -bool YES` or `defaults write <bundle id> cloud_saves_delete -bool YES`; they run at the next launch with iCloud Saves enabled. Turning the setting off leaves both the saves on this device and the copies already in iCloud in place. Turning it back on during the same run still waits for the next launch.

The first time you turn it on, existing saves on this device are compared with iCloud using the same conflict rules as later launches. If both have different progress, VISR keeps the winner and sets the other aside before replacing anything on this device. The launch notice offers **Use the other one**. Nothing local is deleted just because iCloud is empty or cannot be reached.

`HALO_CLOUD_SAVES=0` in the environment turns the feature off for one run. A run started by `tools/mac_run.py` never uses it.

## Privacy

- The saves go to the private database of your own iCloud account through Apple's CloudKit. There is no server of ours, no public or shared database, and no analytics. The project's developers cannot read anyone's private database.
- What is stored there: the save files listed above, the device's model name ("iPhone", "Apple TV"), a random identifier made on each device for this purpose, timestamps, and the app's version.
- CloudKit encrypts it; with Advanced Data Protection turned on it is end-to-end encrypted.
- It counts against your iCloud storage, and you can remove it with **Delete saves from iCloud** or from the iCloud settings of your Apple account.

## Building with it

```sh
python3 tools/ios_build.py --team YOUR_TEAM_ID --bundle-id com.yourname.visr \
  --cloud-saves --cloud-container iCloud.com.yourname.visr
```

- You need a paid Apple developer team. A free account cannot use iCloud, and an unsigned build re-signed by a sideloading tool loses the entitlement; in both cases the app notices at launch and runs exactly as a build without the option.
- The container belongs to your team. Use the same one for every platform's build (`--tvos`, `--visionos`, `--native-mac`, or Catalyst `--mac`) so they share saves. A container cannot be renamed or deleted once it exists.
- The first signed build with the option lets Xcode's automatic signing enable the iCloud capability with CloudKit on that App ID, attach the container, and make a new provisioning profile. If the build cannot create the container, add it once in Xcode (Signing & Capabilities, iCloud, Containers) or on the developer website, then build again.
- `--native-mac --cloud-saves` and Catalyst `--mac --cloud-saves` need `--team` as well: macOS does not run an ad hoc signed app that carries iCloud entitlements.
- Development-signed builds use the container's Development environment, where CloudKit creates the record type the first time a save is uploaded. An App Store distribution build uses Production, which needs the schema deployed once from the CloudKit Console.
- CloudKit may answer "Invalid bundle ID for container" for a while after the capability is first enabled.

Release builds include support only when the reviewed `RELEASE_BUILD_POLICY` sets `cloud_saves` to `on`; an absent key means `off`. The player still has to opt in. Release signing checks that the existing profile allows CloudKit and `iCloud.org.steverice.visr`; mobile profiles also need production push entitlements. See [Releasing](releasing.md).

The native AppKit target accepts `--cloud-saves`; `--unsigned` can compile it without account access, but that output cannot sync without a valid signature and embedded profile. Its App Store packaging is handled separately from the mobile release commands.

What the option adds: `port/ios/VISR-cloud.entitlements`, the CloudKit framework, and three source files. `Info.plist` does not change.

## For developers

| File | |
| --- | --- |
| `port/runtime/cloud_saves.c`, `.h` | The rules: which files, when to upload or download, conflicts, deletions, the state file. Portable C behind a store interface, with no cloud API |
| `port/ios/host/cloud_store_cloudkit.m` | The store on CloudKit: zone `Saves`, record type `SaveSlot` |
| `port/ios/host/host_cloud_saves.m` | When it runs: the queue, the wait before the game, notices, the Settings rows |
| `port/ios/tests/cloud_saves_probe.c` | The rules against an in-memory store shared by several simulated devices; part of `tools/ios_test.py` |

Record type `SaveSlot` in zone `Saves`:

| Field | Type | |
| --- | --- | --- |
| `format`, `minFormat` | Int64 | The record's layout version, and the oldest reader that may use or replace it |
| `unit` | String | `slot-<id>` (a profile or game type) or `save-<id>-<key>` (a checkpoint of one layout); a set-aside copy names the unit it is a copy of |
| `contentHash` | String | SHA-256 of the unit's files |
| `meta` | String, encrypted | `key=value` lines: device, profile name, levels finished, checkpoint level, and so on |
| `payload` | Asset | The files, deflated |

A checkpoint's `<key>` is a hash of what the game requires before it loads one (the disc build, the game state's layout checksum and size, the map file's checksum) and of `HALO_PORT_SAVED_GAME_VERSION` in `port/linux/include/halo_port_capacity.h`. **Raise that number when a change gives a structure in the game state another meaning without changing a size** (a field reordered, reused or reinterpreted): the game cannot tell such a checkpoint from its own, and the number is what keeps one from reaching a device on the other side of the change.

The local bookkeeping is `cloud-saves/state` in the app's data folder; it is left out of device backups. Every decision is logged to `ios-runtime.log` with the prefix `cloud saves:`.

The CloudKit operations and the read of a file the game may be writing follow the CloudKit save mirror in bmanturner's [halo-ce-universal](https://github.com/bmanturner/halo-ce-universal) (`port/tvos/host_cloud_saves.m`, CC0).
