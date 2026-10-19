# Native macOS host

The AppKit target uses the macOS SDK, SDL3's Cocoa window and Metal view, and the same compiled guest and Metal renderer as the other Apple targets. It requires an Apple silicon Mac running macOS 14 or later. Catalyst remains available through `tools/ios_build.py --mac`.

```sh
python3 tools/macos_build.py --renderer metal
```

The development product is `build/macos/app-native/host/Release/VISR.app`. It is unsigned. This target does not provision, upload, or enable CloudKit. It does not bundle benchmark recordings. Signed profile and recording feeds are off by default; explicit data-pack inputs use the existing verification path.

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
