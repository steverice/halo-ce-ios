# Device profiles

A device profile is the set of graphics settings VISR uses on one Apple device: one file here for each model we have measured, and one for each class of device (every Apple TV, every Mac) for the models we have not. The app picks the files for the device it runs on at every launch.

A profile only ever sets graphics settings, from a fixed list. Controls, sound, network and everything else stay the player's on every device, and anything the player set themselves wins over the profile.

## How a value is chosen

Lowest first; each layer overrides the ones before it.

1. The game's built-in default.
2. The **class** profile: the one `kind = "class"` file whose platform matches and whose GPU family range holds the device's.
3. The **model** profile: the one `kind = "model"` file that names the device's model identifier (or, on a Mac, its chip).
4. A **drop-in** file, `device-profile.toml` in the app's data folder, for trying a profile (below).
5. What the player chose: a value set in the menus, with the Settings app's switch, or by editing `config.toml`.
6. A developer's override: a `HALO_*` environment variable or a build's `--render-height`.

Layers 1 to 4 together are the setting's value on this device. `config.toml` shows it, and a small file beside it, `config.origins`, records which of `config.toml`'s values the game wrote as defaults and which somebody chose. A written default follows the profile when the profile changes; a chosen value stays. In the menus, DEFAULTS then OK hands a setting back to the profile.

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
