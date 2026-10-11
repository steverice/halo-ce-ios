# Upstream sync, 2026-10-11

Based on local main `2d1930a32`, on branch `upstream-sync-2026-10-11`.
OpenCE checked through `e0b581b157999e90382668dbc5578528e85701cc`; original iOS through `b8a7266a6`.

Networking is now **26**, including the actual native/browser signaling and ICE-lite/DTLS/SCTP transport. Mbed TLS runs in the Apple host, with generated pointer bridges for the ILP32 guest. Native peers still use UDP.

Also imported co-op elevator prediction, five-or-more-player majority respawns, stale player handle cleanup, vehicle home/name safety, CE map-list isolation, HUD digits and new-profile Settings behavior. Oct8/9 hardening omitted by the focused network25 sync now bounds map strings, tag names, map headers and wide formatting; joins accept punctuation in map names. Audio stream starvation recovery, cancellation and replaced looping voice retirement are imported; held reverb stays excluded. High-resolution HUD PNG decoding uses a loop per row filter.

Original iOS `59d4fa139` viewport clear is already implemented in our `d3d8_device.c` (viewport-clipped rectangles sent through `gpu_clear`), so no duplicate patch is needed. Decomp's five new main commits and cseries movement remain routed through bnunu; bnunu's three commits are empty-header/BSS cleanup, not behavior fixes and not imported.

## Deferred work

Voice chat needs Apple microphone permission, input-device capture imports, Opus build support and HUD/settings integration. Voting needs scoreboard pointer interactions adapted to headset/controller input and Server Setup UI. Both are optional protocol messages and are not required for version26 compatibility. They remain explicit future ports.

Viewmodel FOV, legs/shield effects, replacement reticles, menu layout/input changes, desktop GL batching/transparent sorting, frame throttling and profiling need their own Apple/Metal/stereo integration and visual/device checks. They are held for port work, not declared incompatible. Existing reverb and `metal-render-options` holds remain. Android runtime/touch, desktop packaging and the web application do not become part of VISR.

## Conflict resolutions

- New-profile Settings: add only `new campaign decision` to the custom function names; do not add `port profile settings save`, whose pause-settings implementation is not present.
- Map-format helpers: keep the newest checked-format helpers from `40bb5ea4a` when applying its older HUD-message prerequisite.
- Environment switches: place false-value handling in our shared `config_set_from_environment`, preserving Apple reload behavior.
- Starved audio: keep our three-argument `voice_gains` and omit the held reverb parameters.

## Validation

- Full `ninja ios_guest`: passed after all code imports.
- Actual-source elevator, stale-player, audio flush/transition/lifecycle probes: **98 passed**, including negative controls, compiled natively on macOS. This validates behavior; the guest build validates ILP32 compilation. The upstream harness's default `-m32` cannot link on Apple Silicon, so a temporary compiler wrapper removed that flag for these fake-world tests.
- Native WebRTC with Chromium: **320/320** messages round-trip without loss; with 10% UDP loss the channel stays open and **705/960** datagrams return (loss is expected).
- Actual-C network light regression passed.
- iOS/visionOS application builds, `ios_test.py` and Metal parity checks: pending.

## a10 intro music

`17b63c8ac` fixes a streamed voice that permanently stops after exhausting its queued packets during a hitch; `56db20924` fixes a stale cursor after flushing. They plausibly address the dropout and are imported, but the headset cutscene still needs retesting. OpenCE issue/PR search also finds #184 cancellation and #187 looping retirement; no report establishes this exact a10 cut fixed.

