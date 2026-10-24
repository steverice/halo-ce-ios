# Native macOS host

The AppKit target uses the macOS SDK, SDL3's Cocoa window and Metal view, and the same compiled guest and Metal renderer as the other Apple targets. It requires an Apple silicon Mac running macOS 14 or later. Catalyst remains available through `tools/ios_build.py --mac`.

```sh
python3 tools/macos_build.py --renderer metal
```

`--local-routes` additionally lets the build run the unsigned `benchmark/local-routes` catalog that `tools/replay_check.py` writes (replay checks and whole-take window review); it bundles no recordings.

The development product is `build/macos/app-native/host/Release/VISR.app`. The build signs it ad hoc for local development and verifies the completed bundle. By default it does not use an Apple signing identity or provisioning profile. `--cloud-saves --team YOUR_TEAM_ID` instead uses Apple signing and the shared iCloud save engine; the profile must allow the selected container. `--unsigned --cloud-saves` checks compilation without signing or account access. The player must turn on **iCloud Saves** in Network Setup and relaunch before sync starts. This target does not upload an App Store package. It does not bundle benchmark recordings. Signed profile, recording, and shader feeds are off by default; explicit data-pack inputs use the existing verification path. The native target does not include the interactive benchmark panel or CPU/GPU timing graph.

On first launch, choose an original Xbox PAL or NTSC-US ISO or XISO. The importer validates and stages the maps before replacing incomplete data. The original disc image stays unchanged. Native Mac starts in a resizable window. Set `display.fullscreen = true` or `display.mode = "fullscreen"` for an explicit fullscreen launch; `display.mode = "windowed"` selects the window. `HALO_DISPLAY_MODE` and `HALO_FULLSCREEN` provide launch overrides. Keyboard, mouse, controllers, and audio use SDL. Command-Q and closing the game window use the game's quit path.

Local saves, configuration, recordings, and imported data go under Application Support/`org.steverice.visr.native`. `HALO_DATA_ROOT` overrides that location for a development run and must be absolute. The app does not move or overwrite Catalyst data.

The following probe tests real Foundation file publication and rollback with a synthetic readiness validator. It does not launch an application or extract a real disc image:

```sh
xcrun --sdk macosx clang -fobjc-arc -mmacosx-version-min=14.0 \
  -DHALO_NATIVE_MAC=1 -DHALO_IOS=1 \
  -I port/ios/host -I port/runtime -I port/runtime/include \
  tools/macos_import_test.m -framework AppKit -framework UniformTypeIdentifiers \
  -o /tmp/visr-macos-import-test
/tmp/visr-macos-import-test
```
