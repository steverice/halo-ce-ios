# Build and install VISR on iPhone, iPad, Apple TV and Apple Vision Pro

This directory builds a native ARM64 iOS app around the existing game's ILP32
runtime. It uses SDL3, OpenGL ES 3 or Metal, UIKit touch controls, and the user's original
Xbox map files. The game files are separate from the application and are never
included in source control.

The Home Screen name is **VISR**. The portable ILP32 runtime lives in
`port/runtime`; UIKit, Darwin, touch, audio, and the native loader live here.
The Android app, Gradle/NDK targets, Java activities, and Android host services
have been removed from this branch.

The icon is original vector art drawn by `tools/visr_icon.py`, which renders
every size into `Assets.xcassets`, `Assets-tvOS.xcassets` and
`Assets-visionOS.xcassets`. See [icon notes](ICON.md).

## Build

Use an Apple Silicon Mac with Xcode, its iOS SDK, Python 3, CMake, Ninja, LLVM
and LLD. The validated toolchain is Xcode 27 and Homebrew LLVM/LLD 23.1.2.
The deployment target is iOS 16, but older OS versions have not been validated.
Install dependencies using `brew install cmake ninja llvm lld sdl3 pkgconf`.
Open Xcode once to accept its license and install the iOS platform. Ensure
`xcode-select -p` points into full Xcode, not only Command Line Tools.
For signed device builds, add your Apple account in Xcode Settings > Accounts.
Your provisioning profile must cover the device and chosen bundle identifier.
See [Apple's device setup guide](https://developer.apple.com/documentation/xcode/running-your-app-on-simulated-or-physical-devices).

From the repository root:

```sh
# Native ARM64 simulator app (no signing).
python3 tools/ios_build.py --simulator

# Device app, signed with your Apple development team.
python3 tools/ios_build.py --team YOUR_TEAM_ID --bundle-id com.yourname.haloce

# Unsigned device IPA (no Apple account needed to build).
python3 tools/ios_build.py --unsigned --ipa dist/VISR-iOS-unsigned.ipa
```

The script fetches pinned Khronos headers, SDL release-3.4.16 and musl 1.2.5,
compiles the guest, embeds it into signed application text, and builds the host.
Specify `--llvm` and `--lld` for non-default toolchain locations.
Game assertions remain enabled. An Xcode `Release` host configuration does not
disable the game's assertions. PGO and LTO are disabled for this initial port.

Outputs:

- `build/ios/app-device/Release-iphoneos/VISR.app`
- `build/ios/app-simulator/Release-iphonesimulator/VISR.app`
- `build/ios/app-unsigned/Release-iphoneos/VISR.app`
- `dist/VISR-iOS-unsigned.ipa` and its `.sha256` checksum when requested

The default bundle identifier is `org.steverice.visr`. Set `--bundle-id` to one
covered by your signing profile. `--ipa PATH` can also package a signed build;
only unsigned builds are suitable for this project's public release workflow.
Code signing and provisioning must succeed
before installation. No jailbreak, JIT entitlement, or writable executable
memory is used.

## Install and add game data

The cache validator accepts these exact Xbox v5 cache builds on iOS:

- PAL: `01.01.14.2342`
- NTSC-US: `01.10.12.2276` (experimental compatibility)

### Import on the device

1. Sign/install the IPA with your own account, then open **VISR**.
2. Tap **Choose XISO** and select your `.iso` or `.xiso` in Files (On My
   iPhone/iPad, iCloud Drive, or another Files provider). Compressed ZIP/7z
   archives and PC/MCC disc images are not supported.
3. The app validates the Xbox filesystem, cache version/build, and complete
   campaign map set before copying. A progress bar shows extraction; Cancel
   safely stops it. Once finished, the game starts automatically.

The image is opened through the system document picker with coordinated,
security-scoped access. A cloud image may need to download before extraction.
Leave enough local storage for its maps as well as any local XISO copy.
Nothing is fetched from a game-download service, and the source image is never
modified or deleted. After import, subsequent launches use the extracted maps.

You can also copy **one** `.iso` or `.xiso` directly into VISR's Documents
folder using Finder's Files tab or Files > On My iPhone/iPad > VISR, then
launch the app. It detects and imports that image when game data is missing.
If several images are present, choose one with the picker. After a successful
import, deleting the extra XISO copy from the app folder can reclaim storage.

Imports run in a private staging directory. Invalid or cancelled imports do
not replace existing maps. Interrupted imports are cleaned up on next launch;
existing maps displaced during the final move are restored if needed. Saves
and profiles stay in `Documents/save` throughout.

### Install a source build

List devices with `xcrun devicectl list devices`. Enable Developer Mode when
iOS requests it, pair/trust the Mac, and keep the device unlocked during
installation. Replace `DEVICE_UDID` and the example bundle identifier with
your device and the ID used for signing.

```sh
xcrun devicectl device install app --device DEVICE_UDID \
  build/ios/app-device/Release-iphoneos/VISR.app
xcrun devicectl device process launch --device DEVICE_UDID com.yourname.haloce
```

Downloaded unsigned IPAs must first be signed with your own Apple account.
For packaged-IPA signing instructions, see the official
[AltStore Classic setup guide](https://faq.altstore.io/) or
[Sideloadly FAQ](https://sideloadly.io/faq). These signing tools have not been
validated as part of this port; the documented Xcode route was used for the
physical-device test. Follow your signing tool's refresh instructions before
the provisioning profile expires. Keep the same account and bundle identifier
when updating to preserve the app's data.

### Optional manual extraction

The Mac helper remains available for inspecting your XISO or preparing maps
manually. It preserves original bytes and records build IDs, sizes and SHA-256
hashes. Existing output files are never replaced.

```sh
python3 tools/ios_extract_assets.py '/path/to/Halo.xiso.iso'
python3 tools/ios_extract_assets.py '/path/to/Halo.xiso.iso' --output assets
xcrun devicectl device copy to --device DEVICE_UDID \
  --domain-type appDataContainer --domain-identifier com.yourname.haloce \
  --source assets/maps --destination Documents/maps
```

Keep `maps` directly inside Documents. `config.toml`, `debug.txt`, and
`ios-runtime.log` are available there for diagnostics. Back up `Documents/save`
before uninstalling or changing bundle IDs.

### Upscaled textures in the Settings app

On iPhone, iPad and Apple Vision Pro, the app's page in the system Settings app
(`Settings.bundle`) has a Textures group. The "Upscale textures" switch turns
the upscaled textures on or off (`display.upscaled_textures` in `config.toml`,
written when the app starts or returns to the foreground after you change the
switch); it applies at the next level load. "Upscaled textures" shows how much
storage the upscaled textures take (`Documents/texture-cache`), and the
"Delete upscaled textures" switch deletes them. iOS's own storage screen can
only delete the whole app, imported game included. The app acts on the switch
at launch and whenever it returns to the foreground, deletes only the cache's
own files, and turns the switch back off. It also refreshes the size whenever
it enters the background, so Settings shows what the session built; the textures are made again when a
level needs them. Apple TV has no per-app Settings page. The code is in
`host/host_texture_settings.m`.

## Controls

### Resolution

Native physical display resolution is the default, with a Retina drawable and
matching internal color/depth targets. The game retains its original logical
layout coordinates, so the HUD and touch controls keep their size.

For a lower GPU workload, edit the existing `[display]` section of
`Documents/config.toml` and relaunch:

```toml
render_height = 0 # Native (default); 1080, 720, or 480 render fewer pixels
screen_width = 0  # Fit the display; 640 selects original 4:3
```

The renderer preserves the selected aspect ratio and caps the render size to
the drawable and GPU texture limit. Existing configuration files without
`render_height` automatically use native resolution. Anti-aliasing and an
in-app graphics settings menu are not implemented yet.

Each device starts from its **device profile**: the graphics settings chosen
for its model, or for its class of device when the model has none
([`profiles/`](../../profiles/README.md)). A value you set in `config.toml`,
in the menus or in the Settings app wins over the profile; `config.origins`,
beside `config.toml`, records which values are yours. The log's `device:`,
`device profile:` and `settings:` lines say what the app chose and why.

With the Metal renderer (`renderer = "metal"`; always on Apple Vision Pro),
these `[display]` settings also apply:

```toml
render_scale = 1.0           # Render this fraction of the display's pixels each way (0.25-1.0)
upscaler = "bilinear"        # "metalfx": scale a smaller render up with MetalFX (on a device)
compressed_textures = true   # Keep the game's DXT textures compressed on the GPU
anisotropic_filtering = 16   # 1 (as on the Xbox) to 16
shadow_map_size = 512        # 128 (as on the Xbox) to 2048
effect_resolution = true     # Active camouflage at the screen's resolution, not 320x240
frame_pacing = "off"         # Experimental: "tick" or "refresh" hold every frame for whole refreshes
frame_rate = "display"       # Mac app: a cap ("60", "240") or "unlimited"; see below
frames_in_flight = ""        # Metal: frames on their way to the display at once, "1" to "3" or "system" (empty: "2"); see below
frame_spacing = true         # Metal: each frame stays up for its share of the display's time; see below
frame_source = ""            # Metal: "clock" (default) or "system" (iOS 17, tvOS 17, macOS 14); see below
async_shaders = "auto"       # Compile shaders off the game thread, skipping a draw until it's ready ("off": wait)
direct_camera = true         # On foot, the view follows the stick every frame
```

Models always draw their highest geometry detail level, so they never pop
between detail levels or drop out with distance. For the Xbox's choice by
on-screen size, set `model_lod = "auto"` in `[display]` (default `"max"`); the
console's `rasterizer_debug_model_lod` overrides either.

`frame_rate` is for the Mac app. `"display"` (the default) draws one frame for
each refresh of the display. A number, such as `"60"` or `"240"`, holds that
many frames a second, and `"unlimited"` draws as fast as the Mac can. Above the
display's refresh rate, and with `"unlimited"`, frames stop waiting for the
display: the picture can tear, the Mac uses more power and runs warmer, and a
frame the display is not ready for is drawn and not shown.
The game runs at 30 ticks a second whatever is drawn, and with
`interpolation = false` it draws 30 frames a second at most. With the OpenGL ES
renderer only caps under the display's rate hold. On iPhone, iPad, Apple TV
and Vision Pro the system paces frames and the setting is ignored.

A frame pacer decides how frames reach the display, on the Mac, an iPhone, an
iPad and an Apple TV. It is a module of its own (`host/host_frame_pacer.h`;
its rules, in `host/frame_pacer.h`, have no clock or graphics in them and are
tested by `tests/frame_pacer_probe.c`), and the renderer only tells it when a
frame begins, is presented and was shown. It is compiled in by the build
option `HALO_FRAME_PACING` (`tools/ios_build.py --frame-pacing on|off`), on by
default everywhere except a Vision Pro build: there the Compositor paces
frames and the build has none of this.

- `frames_in_flight` (`"1"` to `"3"`, or `"system"`; default `"2"`) is how many
  frames may be on their way to the display at once. A frame is not started
  on its way until fewer than that many are waiting to be shown. `"1"` gives
  the shortest delay between the game and the screen, and costs frames
  whenever a frame's CPU and GPU work together take longer than one refresh.
  `"system"` leaves the queue to the system, which allows up to three.
- `frame_spacing` (default `true`) keeps each frame on screen for its share
  of the display's time before the next may replace it: one refresh, or with
  a `frame_rate` cap under the display's rate, the cap's (two refreshes for
  `"60"` on a 120 Hz display).
- `frame_source` (`"clock"` or `"system"`; default `"clock"`) is where frames
  get their place on the display. `"clock"` asks for one when a frame is ready
  and applies the two settings above. `"system"` takes one from the system
  for each refresh (`CAMetalDisplayLink`: iOS 17, tvOS 17, macOS 14 and
  later), which then keeps the frames in flight itself; it cannot draw faster
  than the display or hold a cap, so it is used only with `frame_rate =
  "display"`.
- The game's own 60 Hz clock, which paces frames when `interpolation = false`,
  follows the display's refreshes instead of a timer of its own.

None of it applies when frames do not wait for the display (`frame_rate =
"unlimited"`, or a cap above the display's rate): a minimum time on screen
would itself be a cap there. `debug.present_times = true` logs when each
frame reached the display and how long after its work began.

`render_scale = 0.67` with `upscaler = "metalfx"` draws under half the
pixels and scales them back up with sharper edges than a plain stretch; the
HUD is scaled with the rest. The simulators have no MetalFX.

### Touch and controller input

The left stick moves and the right stick aims. The four arrows navigate menus.
A selects/jumps; B returns/melees; X reloads/uses; Y changes weapons. Separate
buttons provide fire, grenade, crouch, zoom, flashlight, grenade selection,
and pause. Hold buttons for held actions. “Hide controls” leaves a small toggle
so a connected hardware controller can be used with an unobstructed picture.
The first hardware controller shares player one with the on-screen controls.
Developer console messages, frame counters, profiling text and the menu's build label are omitted
from the game picture. Diagnostic log files remain available in Documents.

The app supports landscape only on iPhone and iPad, including XISO import and
the Files picker. On iPadOS 26 and later it also requests the interface
orientation lock for the full-screen scene; portrait is excluded from the
app's orientation masks. iPadOS controls windowed multitasking and may
letterbox the landscape app when a full-screen orientation lock is unavailable.
Internet invite hosting and clipboard joining default to off on iOS. Local/network multiplayer is
not yet validated. Bink intro videos remain unsupported by the upstream port.

With internet play on (`network.online = true` in `config.toml`), opening a
`halo://join/...` invite link (tapped where it shows as a link, or pasted into
Safari's address bar; chat apps such as Discord may not make it tappable; on the
Mac, `open 'halo://join/...'` in Terminal) opens the app and joins the invite,
as pasting it into Join Game > Direct Link does. The app registers the `halo` URL scheme on iPhone, iPad, Apple Vision Pro, Apple TV and
the Mac. The link is taken in any state of the game: internet play reaches the
invite's host in the background, and the host's game then appears in Direct
Link's list. A link that opens the app cold waits until internet play starts.
Only a join link's 64 hex digits are used; any other `halo:` link is ignored
with a line in `ios-runtime.log`, as is a link while internet play is off.

## How the port works

The game relies on 32-bit pointers in its data structures. Apple's current
ARM64 iOS binaries require their first 4 GB of address space to remain unmapped,
so the game's 32-bit pointers are represented as offsets into an aligned
native arena. This is native compiled code, not an Android emulator.

`tools/ios_asm_convert.py` lifts compiled guest memory accesses and indirect
branches into an aligned 4 GB arena using reserved registers x15 and x27.
Guest pointer values and structure layouts stay 32-bit. PC-relative addresses
are normalized back to guest offsets. A small signed assembly entry switches
to an arena stack; host bridges translate pointers at the ABI boundary.

Guest code is embedded in the app's signed `__TEXT` and aliased into the arena
read/execute with `vm_remap`. Guest data is separately writable. The host
translates Linux/musl calls to Darwin and handles Apple's 16 KB pages. Explicit
Xbox 4 KB read-only regions protect only complete native pages, preserving
writable neighboring buffers. Texture dirty tracking works at 16 KB granularity
and serializes protection changes against concurrent streaming writers.

The generated bridge resolves all guest imports before entering the game.
The portable runtime and assembly conversion tools were derived from the
upstream ARM64 port, with the iOS host and address model implemented here.
SDL/UIKit calls stay on the UI thread; guest workers and audio callbacks run on
arena stacks with the same translation convention.
The audio worker stages mixed PCM for the SDL callback to submit on its own
thread. This avoids taking SDL's stream lock from a worker while the callback
holds that lock and waits for the worker.

## Regression checks

```sh
brew install sdl3 pkgconf  # Native macOS SDL library for the audio regression.
python3 tools/ios_test.py
```

This executes the translated guest on signed native pages and checks 32-bit
structure layout, global/stack access, function pointers, atomic operations,
and zero-extension of addresses. A separate stress test covers concurrent
texture page tracking, fresh zeroed mappings, and the map inflater's writable
4 KB tail beside a read-only buffer.
The SDL regression checks 100 callbacks and 134,144 exact PCM samples,
including reused guest stack buffers and a request larger than 64 KB.
The native XISO importer is tested under AddressSanitizer and UBSan with
synthetic disc images: exact byte preservation, supported builds, invalid
headers, missing maps, truncated extents, cycles, unsafe names, duplicate
files, cancellation/retry, existing files, and malformed directory mutations.

These tests do not replace a device campaign test. See [the validation record](VALIDATION.md)
for observed device/simulator behavior and remaining limitations.

### Benchmarks

`debug.input_record = "name"` records controller 1 while a map plays, to
`name.input` in Documents. `debug.input_replay = "name"` plays a recording back
at the same moments of the game, and with `debug.benchmark = true` the game
times every frame of the replay, writes `benchmark-name-<UTC time>.txt` to
Documents (frame-time percentiles, frames over each refresh budget, and every
frame's time) and quits when the replay ends. The Metal renderer also logs,
every 600 frames, how many refreshes frames stayed on screen, each frame's CPU
and GPU time, and its render passes' memory traffic (`stderr.log`).

## Simulator

Build with `--simulator`, then select and boot an ARM64 iPhone or iPad simulator
in Xcode. With exactly one simulator booted:

```sh
xcrun simctl install booted build/ios/app-simulator/Release-iphonesimulator/VISR.app
HALO_SIM_DATA=$(xcrun simctl get_app_container booted org.steverice.visr data)
cp -R assets/maps "$HALO_SIM_DATA/Documents/maps"
xcrun simctl launch booted org.steverice.visr
```

Use an explicit simulator ID instead of `booted` when more than one is running.
To exercise the in-app import instead, copy an XISO into the simulator app's
Documents folder and launch without a maps folder.
The manual copy command assumes `Documents/maps` does not yet exist; avoid nesting a
second `maps` directory. Simulator graphics are slow; validate performance on
a physical device.

### Running on a Mac

`tools/mac_run.py` runs the device build on an Apple-silicon Mac as a "Designed
for iPad" app, renders with the GPU, and collects the results:

```sh
python3 tools/ios_build.py --team TEAM
python3 tools/mac_run.py run --team TEAM --xiso ~/Downloads/halo.iso --exit-after 60 \
  --screenshot-every 300 --dump-shaders --out results/menu
python3 tools/mac_run.py compare results/menu results/menu-again
```

The first run imports the XISO into the app's container. `--init 'map_name a10'`
loads a level without input. The runner wraps the CMake-built executable in a
small `xcodegen` project because macOS kills the app the CMake project signs. It
runs without a debugger because `memory_watch.c`'s deliberate page faults would
stop one; to debug, use `process handle SIGSEGV SIGBUS --stop false --pass true`
in LLDB.

`run --simulator UDID` runs a simulator build in that simulator instead (the
visionOS one by default; `--app` picks another), with the same settings and
result folders. `--maps` copies an already imported `maps` folder into a new
container, which is quicker than importing the XISO:

```sh
python3 tools/ios_build.py --visionos --simulator
python3 tools/mac_run.py run --simulator UDID --maps path/to/Documents/maps --exit-after 40 \
  --screenshot-every 300 --set display.screen_width=852 --set display.render_height=1080 --out results/vision-menu
```

## Apple Vision Pro

`tools/ios_build.py --visionos` builds the app for visionOS 2.0 and later
(`--simulator`, `--unsigned` and `--team` work as for iPhone). The game runs in
an ordinary window and draws with Metal only: visionOS has no OpenGL ES, so the
build leaves the GL backend out and the app always sets `display.renderer` to
`"metal"`. The game renders at the window's own resolution (2556×1440 at its
default size), follows the window when it's resized, and is letterboxed at 16:9;
`--render-height` fixes a lower render height instead.

- **Controls.** A game controller plays. Looking at the window and pinching
  does nothing in the game; a "Connect a game controller" note shows while
  none is connected. A keyboard's arrows, Return and Escape work the menus.
- **Game data.** As on iPhone: the setup screen offers the Files picker, and
  an XISO placed in the app's Documents folder is imported.
- **Closing the window pauses the game**, sound included, and reopening Halo
  resumes it. If visionOS later reclaims the closed window, the game quits; it
  saves at checkpoints.
- **Memory.** The game reserves a 4 GB address range for its 32-bit memory at
  launch. If a Vision Pro refuses it, launching shows "Could not reserve the
  game's 4 GB memory arena"; rebuild with `--extended-virtual-addressing`
  (needs a paid developer team), which signs with that entitlement. The build
  always signs with `increased-memory-limit`.

On a Vision Pro, after installing a `--team` build:

1. Launch the app. `ios-runtime.log` in the app's Documents folder (Files)
   should show `guest arena at …`, not the arena error, then `visionOS window
   attached`.
2. Import the XISO. `stderr.log`, if you create an empty one in Documents
   before launching, shows `Metal on …` and the `GPU capabilities` line.
3. Pair a controller and play the menu and the first level. Check the frame
   rate, the window's size, and what pinching does (`ios-runtime.log` logs the
   first finger and mouse events it sees).

## Troubleshooting

- **Signing fails:** check the Apple account in Xcode, team ID, unique bundle
  identifier, device registration, and profile expiration. The script permits
  Xcode provisioning updates; it never supplies somebody else's certificate.
- **App fails to launch:** confirm it was signed for your device, trust the
  developer if requested, and enable Developer Mode. An unsigned IPA will not launch.
- **Menu never appears or maps fail validation:** check `Documents/maps/ui.map`,
  confirm exact original Xbox build IDs above, and inspect `ios-runtime.log`
  and `debug.txt`. A directory named `Documents/maps/maps` is incorrect.
- **Changing your bundle ID:** iOS treats this as a separate app/container.
  Keep the same ID when updating and back up `Documents/save` before uninstalling.
- **Audio issues:** share device/OS and output route along with the runtime
  log. The regression suite checks PCM handoff; it does not test every route.
