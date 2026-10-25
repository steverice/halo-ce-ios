# Device profiles

A device profile is the set of graphics settings VISR uses on one Apple device: one file here for each model we have measured, and one for each class of device (every Apple TV, every Mac) for the models we have not. The app picks the files for the device it runs on at every launch.

A profile only ever sets graphics settings, the tracking-loss grace (`input.aim_loss_grace`, how long a hand-tracking dropout holds the aim) and the texture cache's idle time (`display.texture_idle_seconds`, how long an undrawn texture stays in memory), the clutter distance (`display.clutter_distance`, how far bodies, loose items and garbage are drawn) and the player capacity (`network.player_capacity`, the most players the device holds at 30 frames a second, which sets where a hosted game starts and what the server browser marks and never refuses a join), from a fixed list. Controls, sound, the rest of the network settings and everything else stay the player's on every device, and anything the player set themselves wins over the profile.

## How a value is chosen

Lowest first; each layer overrides the ones before it.

1. The game's built-in default.
2. The **class** profile: the one `kind = "class"` file whose platform matches and whose GPU family range holds the device's.
3. The **model** profile: an exact `kind = "model"` match, otherwise the strongest compatible source selected by the hardware catalog (for any device the catalog lists), otherwise an explicit `and_newer` rule (only for identifiers the catalog does not list). An inherited source brings its own class baseline (see below).
4. A **drop-in** file, `device-profile.toml` in the app's data folder, for trying a profile (below).
5. What the player chose: a value set in the menus, with the Settings app's switch, or by editing `config.toml`.
6. A developer's override: a `HALO_*` environment variable or a build's `--render-height`.

Layers 1 to 4 together are the setting's value on this device. `config.toml` shows it, and a small file beside it, `config.origins`, records which of `config.toml`'s values the game wrote as defaults and which somebody chose. A written default follows the profile when the profile changes; a chosen value stays. In the menus, DEFAULTS then OK hands a setting back to the profile.

## This device and everything newer

A measured model profile can explicitly cover later generations:

```toml
[match]
platform = "ios"
models = ["iPhone18,1", "iPhone18,2"]
and_newer = true
```

The rule covers listed devices and higher major identifiers in the same recognized product line (`iPhone`, `iPad`, `AppleTV` or `RealityDevice`). Minor numbers do not rank devices: `iPhone19,1` qualifies, but `iPhone18,3` does not. All source identities must belong to one product line and its platform. Generation numbers are bounded to 1 through 1000000. This is an explicit author decision; identifiers do not become catalog capability tiers.

On a Mac, use `chips = ["Apple M4 Pro"]`. Known tiers are base, Pro, Max and Ultra. A Pro rule covers the same generation's Max and Ultra, and later generations at Pro or above. A later base chip does not qualify. Unknown tiers retain the existing hardware/class fallback because their capability relative to Pro is unknown. Multiple source chips must share one tier; the newest generation sets the threshold.

Exact matches always win. Among qualifying rules, the newest source generation wins, then the highest Mac tier. Equal thresholds for one platform/product line are rejected by both snapshot validation and `tools/profile_check.py`. A rule precedes catalog inheritance and uses the receiving device's class baseline. The log names the source rule and target identity. Drop-ins and player choices keep their usual precedence.

The newest measured profile in each product line carries this rule: `iphone-17-pro.toml`, `ipad-pro-m1.toml` and `apple-tv-4k-3rd.toml`. Apps built before support for `and_newer` reject a downloaded snapshot containing the field, including `profiles-2` snapshots. Version 0.4.0 is not kept compatible with current feeds. The publisher still refuses the field in `profiles-1` snapshots.

A rule applies only to identifiers the hardware catalog does not list. A device the catalog lists takes the catalog's answer (inheritance, or its class profile when it has no tier or no compatible source), so a later identifier of a weaker model, such as the iPad mini 6 (`iPad14,1`), is not taken over by the newest iPad profile.

## Hardware fallback

An exact model or chip profile wins. Otherwise, the app uses `hardware/catalog.toml` to select the strongest compatible source tier at or below this device's tier. If A9 and A12 profiles are available, a cataloged A11 selects A9; adding an A10 profile makes A11 select A10 on its next launch. The app does not infer capability from a model identifier, release date, or chip-name suffix.

The catalog has separate orderings for iOS, tvOS, visionOS, and Mac. A source must fit the receiving device's physical memory, GPU family, render-pixel limits for gameplay/menu/cutscenes, and refresh target. Unknown or unordered hardware keeps its class fallback. Inheritance uses the source's class baseline and model values together, including scene values; the receiving class does not contribute extra settings. Drop-ins and player choices retain their precedence.

The catalog lists every iPhone, iPad and Apple TV model the app supports, from the iPhone 8, iPad (5th generation) and Apple TV HD through the current generation. Each entry records its basis and the source of its ordering. A tier orders GPU capability: the design generation, then the GPU core count where a design is shared. Hardware whose ordering against its neighbors cannot be defended has no tier and keeps its class profile; none is left out today. Inheritance for hardware without a measurement of its own is an explicit hardware assessment, not a benchmark of that model.

iOS has two tier ranges, iPhones from 100 and iPads from 1000, so an iPhone never inherits an iPad's profile. Each source's envelope carries a memory floor, the pixels its measured device drew, and a refresh limit. A device on a display faster than the source was measured at is outside the envelope, because iPhone, iPad and Apple TV builds cannot cap gameplay at a lower frame rate (`display.frame_rate` is Mac-only there; the system paces frames), so the settings would run at a rate nobody measured them at. Such a device keeps the next lower compatible source, or the class floor. Physical runtime checks can reject a catalog candidate.

**The class floor.** A device weaker than every measured profile of its line, or outside every envelope, takes its class profile, whose values are the weakest measured device's settings: for iPhone and iPad, the iPad Pro 12.9-inch (1st generation, A9X) at 720 lines with menus and cutscenes at the display's size; for Apple TV, the Apple TV HD's values (`apple-tv-hd.toml`). Because every model profile sits on its class, each states the values the floor would otherwise change.

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

`display.upscaled_textures` is the one setting where a player's choice does not win over the profile: the textures are drawn upscaled only where the profile and the player's switch both say so, and a device whose profile sets false never makes them.

`debug.settings_dump = true` in `config.toml` lists every profile setting, defaults included. The log is `ios-runtime.log` in the app's data folder.

## Editor schemas and generated references

Put `#:schema ../port/settings/profile.schema.json` at the top of a profile for TOML editors that support JSON Schema. The template includes it. This schema requires model identities for contributed profiles. A local drop-in may omit them; check that form with `python3 tools/profile_check.py device-profile.toml`. For `config.toml`, point the editor to [`config.schema.json`](../port/settings/config.schema.json); the [configuration reference](../port/linux/README.md#settings) lists every option from the C setting table. The [hardware catalog schema](../port/settings/catalog.schema.json) covers catalog metadata; cross-file and evidence checks still require `tools/profile_check.py`.

Run `python3 tools/settings_schema.py` after changing the C setting definitions. It refreshes the three schemas and the reference tables in this file and `port/linux/README.md`. `--check` checks their freshness; `--markdown config` and `--markdown profile` print either table. Config schemas check names and types. Profile schemas also check the intentionally narrower profile limits. Build-dependent defaults list Apple and desktop values.

## Targets

A device's profile aims for the highest tier of this ladder that the device holds comfortably. Each tier is a frame rate, a render-line count and an enemy multiplier (`debug.enemy_multiplier`, the replay's extra enemies):

| Tier | Frame rate | Render lines | Enemies |
|---|---|---|---|
| 0, the floor | 30 | 540 | 1x |
| 1 | 60 | 720 | 1x |
| 2 | 60 | 900 | 1x |
| 3, the target | 60 | 1080 | 1x |
| 4 | 60 | 1080 | 1.5x |
| 5 | 120 | 1080 | 1.5x |
| 6 | 120 | 1440 | 2x |
| 7 | 120 | 2160 | 2x |

Tier 3 is the ideal. Above it, each tier must also hold at more enemies than real play has, so the profile keeps headroom: a device that holds 120 frames a second at 1440 lines with twice the enemies should not run 4K and drop below 60 in a busy game.

**Profile resolution.** The profile resolution is the highest the device holds with p90 frame time within the frame budget after warm-up; dynamic resolution, when it exists, covers the rest. The benchmark measures p90 directly with the same nearest-rank method as p95 and p99, at fixed resolution. Older reports without p90 need their per-frame measurements or a new run; p90 is not estimated from other percentiles.

**Holding a frame-rate target.** In every benchmark window at the tier's frame rate, render lines and enemy multiplier, after the device has warmed up, at least 99.5% of frames present on time and late frames are few. On time means shown within one refresh of the tier's frame rate; a late frame is one shown after more than 1.5 refreshes. GPU time is for diagnosis and for comparing the cost of settings, not the pass/fail test: on pipelined Metal, `gpu_ms` includes waits for the next drawable, so a device that holds its rate can show a GPU time near the whole frame budget. Each level's window is the stretch that level stresses most (open terrain with vehicles, swarms of small enemies, effects and reflections, large fights between factions). Fanless devices and phones throttle, so a cold run does not count.

**Settings before tiers.** Before a device drops a tier, the profile lowers the settings that cost the most for the least visible difference (shadow detail, anisotropy, level of detail, anti-aliasing), in the order the measured cost of each setting gives. Frame rate and render lines change only when those settings cannot close the gap.

**What the display allows.** The display caps the ladder. 120 frames a second needs a 120 Hz display (ProMotion, or a 120 Hz external display). Render lines stop at the display's native vertical resolution, and 2160 lines need a 4K output, so the Apple TV HD tops out at tier 3. A device that reaches its display's cap takes the highest tier the display allows.

**Other scenes.** Cutscenes run at 30 frames a second and menus at the display's rate. Each multiplayer map and player count is qualified separately at the profile's tier, and a multiplayer miss does not lower the campaign tier.

**Vision Pro** has its own ladder. Holding the display's rate comes first there, for comfort, and resolution second.

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

`[external.display]` supplies output values while an iPhone or iPad drives a wired display. It may set only the `PROFILE_EXTERNAL` keys in `port/linux/src/port_profile_keys.h`: `display.output_height`, `display.render_height`, `display.render_scale` and `display.anti_aliasing`. `display.output_height` is a profile-only output cap; zero leaves the mode height uncapped. It is valid here and in a tvOS profile's own `[display]`, where it caps the built-in output: the game draws into a drawable that many lines tall and the system scales it to the TV without changing the TV's mode. A launch's `HALO_OUTPUT_HEIGHT` overrides it on an Apple TV, and the benchmark report's `output` block records the panel size, `drawable_width`, `drawable_height` and `output_cap_height`.

`[external.scene.menu.display]` and `[external.scene.cutscene.display]` can further set the keys also marked `PROFILE_SCENE`. Precedence is the built-in profile value, the built-in scene value, the external value, then the external scene value. The highest class, model or drop-in layer that supplies each context value wins. Player choices and environment overrides apply on every output. External values never enter `config.toml` or `config.origins`.

Evidence names include the context: `[evidence."external.display.render_height"]` or `[evidence."external.scene.cutscene.display.render_scale"]`. Measured external evidence also needs `output = "external"` and a source identifying the external-output report, scene and date. The marker declares the source's output context; review the report's `output.kind` before marking a value measured.

## Profile settings reference

<!-- BEGIN GENERATED SETTINGS profile -->

### display

| Setting | Type and accepted values | Default (Apple) | Contexts | Description |
| --- | --- | --- | --- | --- |
| `display.renderer` | string: `gl`, `metal` | `"metal"` | gameplay | What draws the game: "metal" (default) Metal, or "gl" OpenGL ES, for debugging only: it lacks the Metal renderer's performance work and measures no GPU time. |
| `display.render_height` | integer, 0 to 4320 | `0` | gameplay, ⓢ scene, external | Internal rendering height in physical pixels: 0 (default) uses native display resolution. |
| `display.output_height` | integer, 0 to 4320 | Profile only | tvOS built-in, external | Output height cap in pixels, external or tvOS built-in; zero leaves it uncapped. |
| `display.render_scale` | real, 0.25 to 1 | `1.0` | gameplay, ⓢ scene, external | Render at this fraction of the display's pixels each way (0.25 to 1.0; 0.67 draws under half the pixels), scaled back up by display.upscaler. display.render_height, when set, caps the result. |
| `display.upscaler` | string: `bilinear`, `metalfx` | `"bilinear"` | gameplay | How a picture rendered below the display's size is scaled up: "bilinear" (default), or "metalfx" (Metal renderer, on a device: MetalFX's spatial scaler, sharper edges and text for a fraction of a millisecond). |
| `display.anisotropic_filtering` | integer, 1 to 16 | `16` | gameplay | Anisotropic filtering of the game's textures, 1 (off, as on the Xbox) to 16: sharper floors and terrain at a distance, for a few percent of GPU time. |
| `display.dynamic_light_limit` | integer, 0 to 128 | `0` | gameplay | Device-profile cap on dynamic lights illuminating level surfaces, 0 to 128. |
| `display.specular_light_limit` | integer, 0 to 128 | `0` | gameplay | Device-profile cap on specular lights among the level's selected dynamic lights, 0 to 128. |
| `display.shadow_map_size` | integer, 128 to 1024 | `512` | gameplay | The size of the textures characters' and vehicles' shadows are drawn into, 128 (as on the Xbox) to 2048: larger is smoother. |
| `display.effect_resolution` | string: `xbox`, `half`, `full` | `"half"` | gameplay | The resolution of the 320x240 target the reflections in mirror floors and the active camouflage are drawn into: "xbox" keeps the Xbox's 320x240, "half" is half the screen's resolution each way, and "full" the screen's resolution (sharper; up to a whole second render of the scene's pixels where a mirror shows). |
| `display.model_lod` | string: `max`, `auto` | `"max"` | gameplay | Which geometry detail level models draw: "max" (default) always the highest they have, so they never pop between levels or drop out with distance; "auto" picks by on-screen size, as the Xbox did. |
| `display.lod_scale` | real, 0.5 to 4 | `1.0` | gameplay | HEAD mode and the side-by-side view: a multiplier on the pixel size the game picks model detail and particle distance by; 1.0 is the Xbox's (0.5 to 4). |
| `display.compressed_textures` | boolean | `true` | gameplay | With the Metal renderer, keep the game's DXT textures compressed on the GPU where it supports them (BC1-3): a quarter to an eighth of the memory, and faster loads. |
| `display.high_res_hud` | boolean | `true` | gameplay | Draw the HUD (meters, counters, panels, motion sensor, reticles, waypoints, scopes) from the high-res assets (8x the maps' bitmaps); false draws the maps' own bitmaps. |
| `display.high_res_text` | boolean | `true` | gameplay | Draw the menus' and HUD's text with the fonts in port/assets/fonts (Overpass) at the resolution the game draws at, and the menus' titles from port/assets/titles; false draws the maps' bitmap fonts and titles. |
| `display.upscaled_textures` | boolean | `true` | gameplay | Draw upscaled textures (made on this device from your copy of the game) where they exist; false draws the maps' own. |
| `display.anti_aliasing` | string: `off`, `fxaa`, `smaa` | `"off"` | gameplay, external | Smoothing of jagged edges, which the Xbox did not have: "off", or "fxaa" or "smaa", a pass over the 3D view once it is drawn, at the size it is drawn at (the HUD and menus are drawn after it and stay sharp). |
| `display.foveation` | boolean | `true` | gameplay | Apple Vision Pro, display.stereo = "head": render through the Compositor's foveation, sharper at the middle of each view than at its edges, at display.render_quality. |
| `display.render_quality` | real, 0.1 to 1 | `0.6` | gameplay | With display.foveation: the Compositor's render quality, 0 to 1 (the headset's lowest to its highest): how many pixels the middle of each view gets. |
| `display.frame_repeat` | integer, 0 to 3 | `0` | gameplay | Apple Vision Pro, display.stereo = "head" or "screen": how many extra display refreshes each frame stays up, 0 to 3: 0 draws a frame every refresh (90 a second), 1 every other (45 a second) for when 90 costs too much. |
| `display.hud_resolution` | real, 0.5 to 1 | `1.0` | gameplay | HEAD mode and the side-by-side view: the pixels the HUD, the crosshair and the menus are drawn with, as a fraction of the view's along each axis, 0.5 to 1. |
| `display.frame_pacing` | string: `off`, `tick`, `refresh` | `"off"` | gameplay | How the Metal renderer times frames: "off" (default) shows each frame as soon as it's drawn; "tick" shows every frame for the same whole number of display refreshes, at a rate that is a multiple of the game's 30 ticks a second (90 or 30 at 90 Hz, 60 or 30 at 60 Hz), dropping to the next rate when frames take too long; "refresh" allows any whole number of refreshes (45 at 90 Hz). |
| `display.frame_rate` | string: `display`, `unlimited`, quoted 30 to 1000 | `"display"` | gameplay, ⓢ scene | The most frames a second: "display" (default) draws one frame for each refresh of the display; a number ("60", "240") holds that rate; "unlimited" draws as fast as it can. |
| `display.async_shaders` | string: `auto`, `on`, `off` | `"auto"` | gameplay | How the Metal renderer compiles shaders: "auto" (default) on worker threads, skipping a draw for the frames until its pipeline is ready, so the game never waits on the compiler (on Apple Vision Pro a long wait ends the app); a run that must draw the same frames every time (fixed timestep, an input replay or benchmark, debug.exit_after, screenshots) compiles each at its first draw instead. "on" or "off" forces either. |
| `display.interpolation` | boolean | `true` | gameplay | Draw a frame for every display refresh, blending between the game's 30 ticks a second; false keeps the original 30 frames a second. |
### input

| Setting | Type and accepted values | Default (Apple) | Contexts | Description |
| --- | --- | --- | --- | --- |
| `input.aim_loss_grace` | real, 0 to 5 | `0.5` | gameplay | With input.aim = "controller": seconds of lost hand tracking during which the last hand aim is held, in local and network play, before the aim eases to the head over 0.2 seconds (0 to 5). |
### display

| Setting | Type and accepted values | Default (Apple) | Contexts | Description |
| --- | --- | --- | --- | --- |
| `display.texture_idle_seconds` | real, 10 to 600 | `60.0` | gameplay | Seconds a texture goes undrawn before the cache drops it, 10 to 600. |
| `display.clutter_distance` | real, 0 to 200 | `0.0` | gameplay | Beyond this many world units from the camera, dead bodies, weapons and equipment on the ground and garbage objects are not drawn; 0 (default) draws them all, 0 to 200. |
### network

| Setting | Type and accepted values | Default (Apple) | Contexts | Description |
| --- | --- | --- | --- | --- |
| `network.player_capacity` | integer, 2 to 128 | `0` | gameplay | The most players this device holds at 30 frames a second at the lowest settings, 2 to 128; 0 (default) names no limit. |
### debug

| Setting | Type and accepted values | Default (Apple) | Contexts | Description |
| --- | --- | --- | --- | --- |
| `debug.dynamic_resolution_shadow` | boolean | `false` | gameplay | With the Metal renderer, log and record the dynamic resolution planner's choices without changing the rendered resolution. |

<!-- END GENERATED SETTINGS profile -->
