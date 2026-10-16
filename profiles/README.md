# Device profiles

A device profile is the set of graphics settings VISR uses on one Apple device: one file here for each model we have measured, and one for each class of device (every Apple TV, every Mac) for the models we have not. The app picks the files for the device it runs on at every launch.

A profile only ever sets graphics settings, from a fixed list. Controls, sound, network and everything else stay the player's on every device, and anything the player set themselves wins over the profile.

## How a value is chosen

Lowest first; each layer overrides the ones before it.

1. The game's built-in default.
2. The **class** profile: the one `kind = "class"` file whose platform matches and whose GPU family range holds the device's.
3. The **model** profile: an exact `kind = "model"` match, otherwise the strongest compatible source selected by the hardware catalog. An inherited source brings its own class baseline (see below).
4. A **drop-in** file, `device-profile.toml` in the app's data folder, for trying a profile (below).
5. What the player chose: a value set in the menus, with the Settings app's switch, or by editing `config.toml`.
6. A developer's override: a `HALO_*` environment variable or a build's `--render-height`.

Layers 1 to 4 together are the setting's value on this device. `config.toml` shows it, and a small file beside it, `config.origins`, records which of `config.toml`'s values the game wrote as defaults and which somebody chose. A written default follows the profile when the profile changes; a chosen value stays. In the menus, DEFAULTS then OK hands a setting back to the profile.

## Hardware fallback

An exact model or chip profile wins. Otherwise, the app uses `hardware/catalog.toml` to select the strongest compatible source tier at or below this device's tier. If A9 and A12 profiles are available, a cataloged A11 selects A9; adding an A10 profile makes A11 select A10 on its next launch. The app does not infer capability from a model identifier, release date, or chip-name suffix.

The catalog has separate orderings for iOS, tvOS, visionOS, and Mac. A source must fit the receiving device's physical memory, GPU family, render-pixel limits for gameplay/menu/cutscenes, and refresh target. Unknown or unordered hardware keeps its class fallback. Inheritance uses the source's class baseline and model values together, including scene values; the receiving class does not contribute extra settings. Drop-ins and player choices retain their precedence.

The initial catalog covers the M1 iPad Pro variants: the 11-inch model may inherit the measured 12.9-inch profile. That inheritance is an explicit hardware assessment, not a benchmark of the 11-inch model. Other models retain exact/class matching until their ordering is established. Physical runtime checks can reject a catalog candidate.

To add coverage, record the exact device identities, platform, tier, GPU family, evidence basis, and source in a `[[hardware]]` entry. Designate at most one `[[fallback]]` source per platform/tier, referencing a measured model profile and its compatibility envelope. Omit a hardware tier when ordering is uncertain. Use the existing catalog as the schema example and validate the entire folder with `python3 tools/profile_check.py` before publishing a change.

Logs and benchmark reports distinguish inherited selection from exact selection and retain both hardware identities, tiers, and the catalog version. An inherited result does not change the evidence attached to the source's settings.

## Scene classes

A profile's own values are for gameplay. The main menu and cutscenes cost a known amount, since their camera and content are scripted, so a profile may give them their own values:

```toml
[scene.menu.display]
render_scale = 1.0

[scene.cutscene.display]
render_scale = 0.667
```

A build reads the classes `menu` and `cutscene`, and only the settings the game can change between frames (`PROFILE_SCENE` in `port/linux/src/port_profile_keys.h`; today, `display.render_scale`). A scene value applies only while nobody has set that setting: a render scale the player chose is the render scale everywhere.

## What the log says

Every launch names the device, the files in force, and where each profile setting's value came from:

```
device: AppleTV5,3 (tvos, GPU family 2 "Apple A8 GPU", 2017 MB, 1920x1080 at 60 Hz)
device profile: class "Apple TV" (apple-tv.toml), model "Apple TV HD" (apple-tv-hd.toml)
settings: display.render_scale = 0.5 [model Apple TV HD] over 1.0 [default]; menu 1.0 [model Apple TV HD]
settings: display.upscaled_textures = true [config.toml, chosen] over false [class Apple TV] over true [default]
scene: menu (display.render_scale 1.0 [model Apple TV HD])
```

`debug.settings_dump = true` in `config.toml` lists every profile setting, defaults included. The log is `ios-runtime.log` in the app's data folder.

## Writing a profile

1. Copy `TEMPLATE.toml` to a name for the device, lowercase words joined by hyphens (`ipad-air-m2.toml`).
2. Take the model identifier from the log's `device:` line and put it in `[match]`.
3. Set only what should differ from the device's class profile, and only settings in `port/linux/src/port_profile_keys.h`.
4. Give every value an `[evidence."section.key"]` table: `basis` (`measured`, `assumed` or `inherited`), `why` with the numbers, and for a measured value `source` (which run, which scene, which date). A scene class's value is named `[evidence."scene.menu.display.render_scale"]`.
5. Check it: `python3 tools/profile_check.py profiles/ipad-air-m2.toml`.

## Trying a profile

No build is needed. Copy your file into the app's data folder as `device-profile.toml`:

- iPhone, iPad, Apple Vision Pro: the Files app, On My iPhone (or iPad, or Apple Vision Pro) > VISR.
- Mac: `~/Library/Application Support/<the app's bundle identifier>/`.
- Apple TV: it has no Files app and keeps its data in a cache folder the system may empty, so a drop-in there needs a Mac (`xcrun devicectl device copy to`) and may not last.

At the next launch its values lie over the app's own profile for the device, and the log says so in capitals:

```
device profile: DROP-IN FILE device-profile.toml IN FORCE (3 values) over the app's own profile: delete it from the data folder to go back
```

Delete the file to go back. A value the app refuses (a setting a profile may not set, a number outside its range) is left out and named in the log; the rest of the file still applies.

## Sending one

One device in a pull request. Attach what the values rest on:

- the `device:`, `device profile:`, `settings:` and `scene:` lines of the log, and
- with `debug.gpu_stats = true`, the `frames shown for 1/2/3/4/5+ refreshes` lines for the scenes you measured, before and after your change.

Send those lines only, not the whole log: it also holds file paths, which on a Mac include your user name.

A contributed profile is merged with `status = "provisional"` until a second report from the same model agrees with it or a maintainer reproduces it. Changing `display.renderer`, `display.frame_rate` or `display.compressed_textures` needs `basis = "measured"`.

## Limits

A profile file is at most 64 KB. It cannot name a path, a server or a debug switch, hold the game below 30 frames a second, or ask for more than a device can do: what a GPU lacks (compressed textures on an A8, for example) is checked in code and no profile turns it on.

Profiles can also arrive as signed complete snapshots. See [Updating profiles and benchmark recordings](../docs/data-packs.md) for build configuration, publishing and rollback.

## External displays

`[external.display]` supplies output values while an iPhone or iPad drives a wired display. It may set only the `PROFILE_EXTERNAL` keys in `port/linux/src/port_profile_keys.h`: `display.output_height`, `display.render_height`, `display.render_scale` and `display.anti_aliasing`. `display.output_height` is a profile-only output cap, valid only here; zero leaves the mode height uncapped.

`[external.scene.menu.display]` and `[external.scene.cutscene.display]` can further set the keys also marked `PROFILE_SCENE`. Precedence is the built-in profile value, the built-in scene value, the external value, then the external scene value. The highest class, model or drop-in layer that supplies each context value wins. Player choices and environment overrides apply on every output. External values never enter `config.toml` or `config.origins`.

Evidence names include the context: `[evidence."external.display.render_height"]` or `[evidence."external.scene.cutscene.display.render_scale"]`. Measured external evidence also needs `output = "external"` and a source identifying the external-output report, scene and date. The marker declares the source's output context; review the report's `output.kind` before marking a value measured.
