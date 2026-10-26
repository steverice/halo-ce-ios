# Performance benchmarks

We try to make these comparisons fair, but our numbers or our setup for other projects may be wrong or out of date. Corrections are welcome: [open an issue](https://github.com/steverice/visr/issues).

Last measured: 2026-10-26.

Ranges show the minimum–maximum across unique rounds for each scenario; at least three rounds are required. Incomplete measurements remain visible. On-time share uses the recorded display rate with a 5% interval tolerance; it is omitted for uncapped rows and does not establish a display tier. The methods measure different events and are not ranked against one another.

Background CPU is sampled for five seconds immediately before each native sampling window. Runs are rejected when average outside-process CPU rises more than half a core above that baseline. Steady background work remains active and may affect these estimates. Each round allows up to three attempts; rejected attempts do not enter the ranges.

Opening runs use a10 and b30 with no input: 15 seconds of warm-up after player readiness, a five-second background CPU baseline, then 60 seconds of sampling. Opening cinematics are included. Native apps use 1280×960 drawables on a 60 Hz display with interpolation. Capped rows request a 60 fps cap and keep each app's other graphics defaults. These runs do not establish combat performance or identical image quality. Player readiness does not align cinematic clocks, so opening sequences can have phase offsets between apps.

Uncapped runs request `display.frame_rate = "unlimited"` in VISR, `display.max_fps = -1` in OpenCE-macOS, and `display.frame_limit = 0` in CoreCE, with `display.vsync = false` and interpolation enabled in each. The Conditions column identifies each mode and extra delay after warm-up (+0 s is the opening window). The 60 Hz display still limits visible refreshes; presentation intervals do not establish simulation speed or unique displayed frames. Above-refresh trace rates have unverified physical meaning and are not verified submission throughput. The same elapsed window does not prove identical gameplay. Each launch uses fresh isolated data/save folders; OS and driver caches are not reset or equalized.

The matched-stock preset disables antialiasing, replacement HUD/text, and first-person legs in each native app, and each uses direct camera and 128-pixel shadow maps. VISR also uses automatic model LOD, the Xbox-size effect buffer, 1× anisotropic filtering, no replacement textures, and no first-person body. OpenCE-macOS uses native resolution scaling, stock world/weapon FOV, and disables optional per-pixel lighting, weapon shields, and shield glow. CoreCE uses the same options as OpenCE-macOS and `model_detail = "stock"`. Other defaults remain unchanged. VISR uses native Metal and compressed GPU textures; OpenCE-macOS uses ANGLE over Metal. These runs could not verify the other ports' internal filtering, LOD, effect-buffer, or texture-storage policy, so identical image quality remains unverified. Weapon framing and texture filtering still differ visibly. Capped rows keep each app's graphics defaults and are not quality-matched to the matched-stock rows.

Configuration references: [VISR settings](../port/ios/README.md), [OpenCE-macOS v1.0.2 settings](https://github.com/channelramble/OpenCE-macOS/blob/v1.0.2/port/linux/README.md#settings), [CoreCE graphics settings](https://github.com/mastercheese-dev/CoreCE/blob/cccbf6fcc/README.md#graphics).

The later b30 window adds 90 seconds after warm-up, reaching the beach encounter at about 110 seconds after player readiness once the CPU baseline is included. It samples 60 seconds with a stationary player and no input. Start/end screenshots qualify the camera location, but AI and combat state can vary between apps and rounds; this is not a deterministic combat replay.

OpenCE web runs import the disc image into each fresh Safari automation session, start the level with the documented `?map=<level>&play`, wait 6 seconds for loading, then warm up for 15 seconds and sample requestAnimationFrame callbacks for 60 seconds with no input. The page viewport is 640×480 points, so the game canvas is 1280×960 pixels on the same 60 Hz display as the native rows. The page does not report player readiness, so its sampling window can be offset from the native windows within the opening sequences. Browser callbacks are not the same event as native on-glass intervals.

CoreCE rows come from separate matrices run after the VISR and OpenCE-macOS matrices on the same hosts and dates, so they do not share round rotation with them. CoreCE reads the same maps from a copied folder, because its Mac host does not accept a linked maps folder.

Pre-release main builds are VISR sources measured before a release was tagged. They are kept for history and are not the released product.

## Measured builds

| Port | Version | Build | Source | Kind | Artifact | Artifact SHA-256 | Bundle storage | Machine | OS | Browser | Date | Method | Conditions | Status | Collection (s) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| VISR | main | 20261023.203835 | 8d26312dd | pre-release main build | ad hoc Mac build of main | 72272775f0c12a98603b8f4b574a3c8be64842d8085da4110884fdd49451a195 | internal | Mac mini (M4, 16 GB) | macOS 27.0.1 | — | 2026-10-24 | metalperftrace on-glass intervals | capped; +0 s; defaults; warm-up 15 s | ok | 1098.4 |
| VISR | main | 20261023.203835 | 8d26312dd | pre-release main build | ad hoc Mac build of main | 72272775f0c12a98603b8f4b574a3c8be64842d8085da4110884fdd49451a195 | internal | Mac mini (M4, 16 GB) | macOS 27.0.1 | — | 2026-10-24 | metalperftrace on-glass intervals | uncapped; +0 s; matched-stock; warm-up 15 s | ok | 1297.5 |
| VISR | main | 20261023.203835 | 8d26312dd | pre-release main build | ad hoc Mac build of main | 72272775f0c12a98603b8f4b574a3c8be64842d8085da4110884fdd49451a195 | internal | Mac mini (M4, 16 GB) | macOS 27.0.1 | — | 2026-10-24 | metalperftrace on-glass intervals | uncapped; +90 s; matched-stock; warm-up 15 s | ok | 1308.6 |
| VISR | 0.5.1 | 20261026.31927 | 632034e99 | release | ad hoc Mac build of tag visr-v0.5.1 with the release build number | ce8c047692d8df1c8d1cac367f461a5ae3251281fed396c33c752462d591dc00 | internal | Mac mini (M4, 16 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | capped; +0 s; defaults; warm-up 15 s | ok | 1109.3 |
| VISR | 0.5.1 | 20261026.31927 | 632034e99 | release | ad hoc Mac build of tag visr-v0.5.1 with the release build number | ce8c047692d8df1c8d1cac367f461a5ae3251281fed396c33c752462d591dc00 | internal | Mac mini (M4, 16 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | uncapped; +0 s; matched-stock; warm-up 15 s | ok | 1187.3 |
| VISR | 0.5.1 | 20261026.31927 | 632034e99 | release | ad hoc Mac build of tag visr-v0.5.1 with the release build number | ce8c047692d8df1c8d1cac367f461a5ae3251281fed396c33c752462d591dc00 | internal | Mac mini (M4, 16 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | uncapped; +90 s; matched-stock; warm-up 15 s | ok | 1126.2 |
| VISR | 0.5.1 | 20261026.31927 | 632034e99 | release | ad hoc Mac build of tag visr-v0.5.1 with the release build number | ec790fac4536fd011b4c7774dfa1308855b474e169e49735f9b4ba06092848e4 | internal | Mac mini (M6, 24 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | capped; +0 s; defaults; warm-up 15 s | ok | 1057.0 |
| VISR | 0.5.1 | 20261026.31927 | 632034e99 | release | ad hoc Mac build of tag visr-v0.5.1 with the release build number | ec790fac4536fd011b4c7774dfa1308855b474e169e49735f9b4ba06092848e4 | internal | Mac mini (M6, 24 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | uncapped; +0 s; matched-stock; warm-up 15 s | ok | 1175.2 |
| VISR | 0.5.1 | 20261026.31927 | 632034e99 | release | ad hoc Mac build of tag visr-v0.5.1 with the release build number | ec790fac4536fd011b4c7774dfa1308855b474e169e49735f9b4ba06092848e4 | internal | Mac mini (M6, 24 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | uncapped; +90 s; matched-stock; warm-up 15 s | ok | 1096.1 |
| OpenCE-macOS | v1.0.2 | 2 | 80a4d0383 | release | release Halo.dmg | 15b78674d3a80eeb34e3be6140852b5dbff0605669ef74a8de784671cc0cee80 | internal | Mac mini (M4, 16 GB) | macOS 27.0.1 | — | 2026-10-24 | metalperftrace on-glass intervals | capped; +0 s; defaults; warm-up 15 s | ok | 1098.4 |
| OpenCE-macOS | v1.0.2 | 2 | 80a4d0383 | release | release Halo.dmg | 15b78674d3a80eeb34e3be6140852b5dbff0605669ef74a8de784671cc0cee80 | external USB | Mac mini (M4, 16 GB) | macOS 27.0.1 | — | 2026-10-24 | metalperftrace on-glass intervals | uncapped; +0 s; matched-stock; warm-up 15 s | ok | 1297.5 |
| OpenCE-macOS | v1.0.2 | 2 | 80a4d0383 | release | release Halo.dmg | 15b78674d3a80eeb34e3be6140852b5dbff0605669ef74a8de784671cc0cee80 | external USB | Mac mini (M4, 16 GB) | macOS 27.0.1 | — | 2026-10-24 | metalperftrace on-glass intervals | uncapped; +90 s; matched-stock; warm-up 15 s | ok | 1308.6 |
| OpenCE-macOS | v1.0.2 | 2 | 80a4d0383 | release | release Halo.dmg | 15b78674d3a80eeb34e3be6140852b5dbff0605669ef74a8de784671cc0cee80 | internal | Mac mini (M4, 16 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | capped; +0 s; defaults; warm-up 15 s | ok | 1109.3 |
| OpenCE-macOS | v1.0.2 | 2 | 80a4d0383 | release | release Halo.dmg | 15b78674d3a80eeb34e3be6140852b5dbff0605669ef74a8de784671cc0cee80 | internal | Mac mini (M4, 16 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | uncapped; +0 s; matched-stock; warm-up 15 s | ok | 1187.3 |
| OpenCE-macOS | v1.0.2 | 2 | 80a4d0383 | release | release Halo.dmg | 15b78674d3a80eeb34e3be6140852b5dbff0605669ef74a8de784671cc0cee80 | internal | Mac mini (M4, 16 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | uncapped; +90 s; matched-stock; warm-up 15 s | ok | 1126.2 |
| OpenCE-macOS | v1.0.2 | 2 | 80a4d0383 | release | release Halo.dmg | 15b78674d3a80eeb34e3be6140852b5dbff0605669ef74a8de784671cc0cee80 | internal | Mac mini (M6, 24 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | capped; +0 s; defaults; warm-up 15 s | ok | 1057.0 |
| OpenCE-macOS | v1.0.2 | 2 | 80a4d0383 | release | release Halo.dmg | 15b78674d3a80eeb34e3be6140852b5dbff0605669ef74a8de784671cc0cee80 | internal | Mac mini (M6, 24 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | uncapped; +0 s; matched-stock; warm-up 15 s | ok | 1175.2 |
| OpenCE-macOS | v1.0.2 | 2 | 80a4d0383 | release | release Halo.dmg | 15b78674d3a80eeb34e3be6140852b5dbff0605669ef74a8de784671cc0cee80 | internal | Mac mini (M6, 24 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | uncapped; +90 s; matched-stock; warm-up 15 s | ok | 1096.1 |
| OpenCE web | unknown | unknown | — | hosted web build | hosted entry document | — | — | Mac mini (M4, 16 GB) | macOS 27.0.1 | — | 2026-10-24 | Safari RAF callbacks (entry-document hash) | capped; +0 s; defaults; warm-up 15 s | unavailable; safari-remote-automation-disabled | — |
| OpenCE web | unknown | unknown | — | hosted web build | hosted entry document | — | — | Mac mini (M4, 16 GB) | macOS 27.0.1 (26A434) | Safari 27.0.1 (22625.1.29.11.28) | 2026-10-26 | Safari RAF callbacks (entry-document hash) | capped; +0 s; defaults; warm-up 15 s | unavailable; safari-remote-automation-disabled | — |
| OpenCE web | build-280 | 280 | 0a46688c3 | hosted web build (release channel) | hosted entry document | 10f8e892343fac1b094921324ec9fe4440dbc889f8b52ca8b012909c34330c26 | — | Mac mini (M6, 24 GB) | macOS 27.0.1 (26A434) | Safari 27.0.1 | 2026-10-26 | Safari RAF callbacks (entry-document hash) | capped; +0 s; defaults; warm-up 15 s | ok | 525.7 |
| CoreCE | 0.3.0 | 44 | cccbf6fcc | release | release CoreCE-macos-arm64.dmg | b7bdf892ceb2d16ca70bac4bb4de971cbbee96623e25e181f2d3e96dc651d307 | internal | Mac mini (M4, 16 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | capped; +0 s; defaults; warm-up 15 s | ok | 547.0 |
| CoreCE | 0.3.0 | 44 | cccbf6fcc | release | release CoreCE-macos-arm64.dmg | b7bdf892ceb2d16ca70bac4bb4de971cbbee96623e25e181f2d3e96dc651d307 | internal | Mac mini (M4, 16 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | uncapped; +0 s; matched-stock; warm-up 15 s | ok | 568.9 |
| CoreCE | 0.3.0 | 44 | cccbf6fcc | release | release CoreCE-macos-arm64.dmg | b7bdf892ceb2d16ca70bac4bb4de971cbbee96623e25e181f2d3e96dc651d307 | internal | Mac mini (M4, 16 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | uncapped; +90 s; matched-stock; warm-up 15 s | ok | 546.8 |
| CoreCE | 0.3.0 | 44 | cccbf6fcc | release | release CoreCE-macos-arm64.dmg | b7bdf892ceb2d16ca70bac4bb4de971cbbee96623e25e181f2d3e96dc651d307 | internal | Mac mini (M6, 24 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | capped; +0 s; defaults; warm-up 15 s | ok | 526.6 |
| CoreCE | 0.3.0 | 44 | cccbf6fcc | release | release CoreCE-macos-arm64.dmg | b7bdf892ceb2d16ca70bac4bb4de971cbbee96623e25e181f2d3e96dc651d307 | internal | Mac mini (M6, 24 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | uncapped; +0 s; matched-stock; warm-up 15 s | ok | 548.6 |
| CoreCE | 0.3.0 | 44 | cccbf6fcc | release | release CoreCE-macos-arm64.dmg | b7bdf892ceb2d16ca70bac4bb4de971cbbee96623e25e181f2d3e96dc651d307 | internal | Mac mini (M6, 24 GB) | macOS 27.0.1 (26A434) | — | 2026-10-26 | metalperftrace on-glass intervals | uncapped; +90 s; matched-stock; warm-up 15 s | ok | 537.8 |
| Chupathingy | v0.8.0g | 0.8.0g | — | release | release chupathingyce-macos-release.zip | 0370192fbcbf712bf597538086b2ec098988a8eeb3c10fb1e4f85c44a1b7f68a | — | Mac mini (M4, 16 GB) | macOS 27.0.1 | — | 2026-10-24 | Metal HUD present intervals | capped; +0 s; defaults; warm-up 15 s | unavailable; console-unavailable | — |
| Halo OG | v0.5.1 | unknown | — | release | release Halo-OG-macos-arm64.dmg | 28ee21c12c25d6cc53ecf3c9729b0a61ab155733d4e9a9990193c8b3a48060d6 | — | Mac mini (M4, 16 GB) | macOS 27.0.1 | — | 2026-10-24 | Metal HUD present intervals | capped; +0 s; defaults; warm-up 15 s | unavailable; console-unavailable | — |

## Side by side

Rows with the same machine, method, scenario, conditions and date are directly comparable. The same build measured on different dates can differ by several percent as the host's background state changes.

| Machine | Method | Scenario | Conditions | Build | Date | Rounds | Avg FPS | p50 (ms) | p99 (ms) | Worst (ms) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | a10 | capped; +0 s; defaults; warm-up 15 s | VISR main (pre-release, build 20261023.203835, source 8d26312dd) | 2026-10-24 | 3 | 59.95–59.97 | 16.66–16.66 | 19.64–19.86 | 34.09–39.64 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | a10 | capped; +0 s; defaults; warm-up 15 s | VISR 0.5.1 (build 20261026.31927, source 632034e99) | 2026-10-26 | 3 | 59.63–59.88 | 16.66–16.69 | 19.18–20.31 | 46.69–60.90 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | a10 | capped; +0 s; defaults; warm-up 15 s | OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | 2026-10-24 | 3 | 59.33–59.98 | 16.65–16.66 | 19.41–19.59 | 31.52–484.67 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | a10 | capped; +0 s; defaults; warm-up 15 s | OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | 2026-10-26 | 3 | 59.65–59.73 | 16.67–16.70 | 19.49–19.88 | 55.88–58.13 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | a10 | capped; +0 s; defaults; warm-up 15 s | CoreCE 0.3.0 (build 44, source cccbf6fcc) | 2026-10-26 | 3 | 58.91–59.83 | 16.65–16.65 | 20.16–20.65 | 45.82–403.24 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | b30 | capped; +0 s; defaults; warm-up 15 s | VISR main (pre-release, build 20261023.203835, source 8d26312dd) | 2026-10-24 | 3 | 59.85–59.88 | 16.66–16.67 | 19.52–19.58 | 116.40–119.43 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | b30 | capped; +0 s; defaults; warm-up 15 s | VISR 0.5.1 (build 20261026.31927, source 632034e99) | 2026-10-26 | 3 | 59.05–59.33 | 16.68–16.69 | 19.86–20.62 | 284.15–302.04 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | b30 | capped; +0 s; defaults; warm-up 15 s | OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | 2026-10-24 | 3 | 59.02–59.92 | 16.65–16.66 | 19.24–19.79 | 97.10–165.96 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | b30 | capped; +0 s; defaults; warm-up 15 s | OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | 2026-10-26 | 3 | 59.25–59.32 | 16.66–16.68 | 19.73–19.77 | 266.45–267.50 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | b30 | capped; +0 s; defaults; warm-up 15 s | CoreCE 0.3.0 (build 44, source cccbf6fcc) | 2026-10-26 | 3 | 58.47–59.50 | 16.63–16.65 | 20.12–20.28 | 301.18–321.75 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | a10 | uncapped; +0 s; matched-stock; warm-up 15 s | VISR main (pre-release, build 20261023.203835, source 8d26312dd) | 2026-10-24 | 3 | 529.74–538.87 | 1.37–1.39 | 5.74–6.04 | 22.98–23.99 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | a10 | uncapped; +0 s; matched-stock; warm-up 15 s | VISR 0.5.1 (build 20261026.31927, source 632034e99) | 2026-10-26 | 3 | 489.02–493.28 | 1.61–1.64 | 5.84–5.99 | 24.29–38.66 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | a10 | uncapped; +0 s; matched-stock; warm-up 15 s | OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | 2026-10-24 | 3 | 487.75–490.48 | 1.31–1.35 | 6.36–6.48 | 44.61–526.92 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | a10 | uncapped; +0 s; matched-stock; warm-up 15 s | OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | 2026-10-26 | 3 | 438.03–439.14 | 1.43–1.44 | 6.91–7.15 | 47.60–47.90 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | a10 | uncapped; +0 s; matched-stock; warm-up 15 s | CoreCE 0.3.0 (build 44, source cccbf6fcc) | 2026-10-26 | 3 | 298.17–301.56 | 2.87–2.95 | 7.29–7.91 | 26.41–28.12 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | b30 | uncapped; +0 s; matched-stock; warm-up 15 s | VISR main (pre-release, build 20261023.203835, source 8d26312dd) | 2026-10-24 | 3 | 417.71–421.77 | 1.97–2.01 | 6.41–6.53 | 291.75–304.36 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | b30 | uncapped; +0 s; matched-stock; warm-up 15 s | VISR 0.5.1 (build 20261026.31927, source 632034e99) | 2026-10-26 | 3 | 419.11–419.64 | 1.70–1.73 | 6.25–6.32 | 294.67–579.73 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | b30 | uncapped; +0 s; matched-stock; warm-up 15 s | OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | 2026-10-24 | 3 | 341.62–349.68 | 2.47–2.50 | 7.46–7.92 | 294.04–302.16 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | b30 | uncapped; +0 s; matched-stock; warm-up 15 s | OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | 2026-10-26 | 3 | 325.35–328.51 | 2.56–2.56 | 8.01–8.19 | 283.55–304.88 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | b30 | uncapped; +0 s; matched-stock; warm-up 15 s | CoreCE 0.3.0 (build 44, source cccbf6fcc) | 2026-10-26 | 3 | 175.24–179.65 | 5.33–5.49 | 9.48–11.53 | 297.70–303.09 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | b30 | uncapped; +90 s; matched-stock; warm-up 15 s | VISR main (pre-release, build 20261023.203835, source 8d26312dd) | 2026-10-24 | 3 | 404.42–421.47 | 1.92–2.00 | 6.24–7.02 | 297.26–596.70 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | b30 | uncapped; +90 s; matched-stock; warm-up 15 s | VISR 0.5.1 (build 20261026.31927, source 632034e99) | 2026-10-26 | 3 | 420.59–427.26 | 1.76–1.80 | 6.04–6.25 | 24.17–25.34 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | b30 | uncapped; +90 s; matched-stock; warm-up 15 s | OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | 2026-10-24 | 3 | 328.88–335.75 | 2.84–2.91 | 6.63–7.00 | 23.08–296.08 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | b30 | uncapped; +90 s; matched-stock; warm-up 15 s | OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | 2026-10-26 | 3 | 319.99–329.92 | 2.62–2.88 | 7.09–7.50 | 293.58–295.26 |
| Mac mini (M4, 16 GB) | metalperftrace on-glass intervals | b30 | uncapped; +90 s; matched-stock; warm-up 15 s | CoreCE 0.3.0 (build 44, source cccbf6fcc) | 2026-10-26 | 3 | 142.59–146.67 | 6.52–6.73 | 10.79–11.31 | 124.82–304.36 |
| Mac mini (M6, 24 GB) | Safari RAF callbacks (entry-document hash) | a10 | capped; +0 s; defaults; warm-up 15 s | OpenCE web build-280 (build 280, source 0a46688c3, network version 39) | 2026-10-26 | 3 | 59.96–59.96 | 16.68–16.72 | 18.50–18.56 | 19.38–20.46 |
| Mac mini (M6, 24 GB) | Safari RAF callbacks (entry-document hash) | b30 | capped; +0 s; defaults; warm-up 15 s | OpenCE web build-280 (build 280, source 0a46688c3, network version 39) | 2026-10-26 | 3 | 59.96–59.96 | 16.68–16.70 | 18.38–18.52 | 19.00–19.88 |
| Mac mini (M6, 24 GB) | metalperftrace on-glass intervals | a10 | capped; +0 s; defaults; warm-up 15 s | VISR 0.5.1 (build 20261026.31927, source 632034e99) | 2026-10-26 | 3 | 59.93–59.95 | 16.65–16.66 | 18.50–19.13 | 31.33–34.42 |
| Mac mini (M6, 24 GB) | metalperftrace on-glass intervals | a10 | capped; +0 s; defaults; warm-up 15 s | OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | 2026-10-26 | 3 | 59.38–59.93 | 16.67–16.68 | 18.59–18.93 | 35.16–458.64 |
| Mac mini (M6, 24 GB) | metalperftrace on-glass intervals | a10 | capped; +0 s; defaults; warm-up 15 s | CoreCE 0.3.0 (build 44, source cccbf6fcc) | 2026-10-26 | 3 | 59.28–59.95 | 16.66–16.68 | 18.56–18.68 | 32.84–402.66 |
| Mac mini (M6, 24 GB) | metalperftrace on-glass intervals | b30 | capped; +0 s; defaults; warm-up 15 s | VISR 0.5.1 (build 20261026.31927, source 632034e99) | 2026-10-26 | 3 | 59.35–59.88 | 16.65–16.67 | 18.53–18.91 | 51.72–567.81 |
| Mac mini (M6, 24 GB) | metalperftrace on-glass intervals | b30 | capped; +0 s; defaults; warm-up 15 s | OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | 2026-10-26 | 3 | 59.30–59.91 | 16.68–16.68 | 18.40–18.56 | 66.99–114.75 |
| Mac mini (M6, 24 GB) | metalperftrace on-glass intervals | b30 | capped; +0 s; defaults; warm-up 15 s | CoreCE 0.3.0 (build 44, source cccbf6fcc) | 2026-10-26 | 3 | 59.08–59.86 | 16.66–16.66 | 19.12–19.23 | 100.70–168.60 |
| Mac mini (M6, 24 GB) | metalperftrace on-glass intervals | a10 | uncapped; +0 s; matched-stock; warm-up 15 s | VISR 0.5.1 (build 20261026.31927, source 632034e99) | 2026-10-26 | 3 | 584.04–740.88 | 0.65–0.75 | 5.22–5.33 | 18.25–123.05 |
| Mac mini (M6, 24 GB) | metalperftrace on-glass intervals | a10 | uncapped; +0 s; matched-stock; warm-up 15 s | OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | 2026-10-26 | 3 | 579.06–992.72 | 0.86–1.16 | 4.31–5.19 | 24.68–28.05 |
| Mac mini (M6, 24 GB) | metalperftrace on-glass intervals | a10 | uncapped; +0 s; matched-stock; warm-up 15 s | CoreCE 0.3.0 (build 44, source cccbf6fcc) | 2026-10-26 | 3 | 548.89–559.03 | 1.60–1.64 | 3.95–4.05 | 21.84–22.46 |
| Mac mini (M6, 24 GB) | metalperftrace on-glass intervals | b30 | uncapped; +0 s; matched-stock; warm-up 15 s | VISR 0.5.1 (build 20261026.31927, source 632034e99) | 2026-10-26 | 3 | 733.58–754.03 | 0.90–0.98 | 4.72–5.04 | 21.24–119.57 |
| Mac mini (M6, 24 GB) | metalperftrace on-glass intervals | b30 | uncapped; +0 s; matched-stock; warm-up 15 s | OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | 2026-10-26 | 3 | 572.63–604.83 | 1.62–1.65 | 3.41–4.50 | 21.70–22.76 |
| Mac mini (M6, 24 GB) | metalperftrace on-glass intervals | b30 | uncapped; +0 s; matched-stock; warm-up 15 s | CoreCE 0.3.0 (build 44, source cccbf6fcc) | 2026-10-26 | 3 | 259.81–263.01 | 3.86–3.87 | 5.59–5.92 | 22.22–22.86 |
| Mac mini (M6, 24 GB) | metalperftrace on-glass intervals | b30 | uncapped; +90 s; matched-stock; warm-up 15 s | VISR 0.5.1 (build 20261026.31927, source 632034e99) | 2026-10-26 | 3 | 549.79–614.88 | 1.04–1.07 | 5.13–5.16 | 20.18–22.18 |
| Mac mini (M6, 24 GB) | metalperftrace on-glass intervals | b30 | uncapped; +90 s; matched-stock; warm-up 15 s | OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | 2026-10-26 | 3 | 547.51–558.48 | 1.72–1.75 | 3.10–3.80 | 21.62–42.36 |
| Mac mini (M6, 24 GB) | metalperftrace on-glass intervals | b30 | uncapped; +90 s; matched-stock; warm-up 15 s | CoreCE 0.3.0 (build 44, source cccbf6fcc) | 2026-10-26 | 3 | 192.56–228.35 | 4.37–5.10 | 6.18–7.05 | 21.95–126.83 |

## VISR

Measured versions: VISR 0.5.1 (build 20261026.31927, source 632034e99); VISR main (pre-release, build 20261023.203835, source 8d26312dd).

Apple's metalperftrace records per-frame on-glass drawable intervals. CPU/GPU durations and aggregate FPS are excluded. Repeated image content is not detected.

| Build | Machine | Date | Scenario | Conditions | Status | Avg FPS | p50 (ms) | p95 (ms) | p99 (ms) | On time (%) | Worst (ms) | Frames | Duration (s) | Thermal | Resolution | Cap (fps) | Display (Hz) | Display rate basis |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| VISR main (pre-release, build 20261023.203835, source 8d26312dd) | Mac mini (M4, 16 GB) | 2026-10-24 | a10 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 59.95–59.97 | 16.66–16.66 | 18.14–18.45 | 19.64–19.86 | 88.58–89.97 | 34.09–39.64 | 3598–3599 | 60.00–60.02 | nominal → nominal | 1280×960 | 60 | 60 | operator-qualified |
| VISR main (pre-release, build 20261023.203835, source 8d26312dd) | Mac mini (M4, 16 GB) | 2026-10-24 | b30 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 59.85–59.88 | 16.66–16.67 | 18.11–18.30 | 19.52–19.58 | 87.89–88.84 | 116.40–119.43 | 3591–3594 | 60.00–60.02 | nominal → nominal | 1280×960 | 60 | 60 | operator-qualified |
| VISR main (pre-release, build 20261023.203835, source 8d26312dd) | Mac mini (M4, 16 GB) | 2026-10-24 | a10 | uncapped; +0 s; matched-stock; warm-up 15 s | ok; 3 rounds | 529.74–538.87 | 1.37–1.39 | 4.41–4.49 | 5.74–6.04 | — | 22.98–23.99 | 31784–32337 | 60.00–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| VISR main (pre-release, build 20261023.203835, source 8d26312dd) | Mac mini (M4, 16 GB) | 2026-10-24 | b30 | uncapped; +0 s; matched-stock; warm-up 15 s | ok; 3 rounds | 417.71–421.77 | 1.97–2.01 | 4.95–5.02 | 6.41–6.53 | — | 291.75–304.36 | 25067–25308 | 60.00–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| VISR main (pre-release, build 20261023.203835, source 8d26312dd) | Mac mini (M4, 16 GB) | 2026-10-24 | b30 | uncapped; +90 s; matched-stock; warm-up 15 s | ok; 3 rounds | 404.42–421.47 | 1.92–2.00 | 4.60–4.91 | 6.24–7.02 | — | 297.26–596.70 | 24266–25291 | 60.00–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| VISR 0.5.1 (build 20261026.31927, source 632034e99) | Mac mini (M4, 16 GB) | 2026-10-26 | a10 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 59.63–59.88 | 16.66–16.69 | 17.98–18.35 | 19.18–20.31 | 88.10–90.12 | 46.69–60.90 | 3579–3594 | 60.02–60.02 | nominal → nominal | 1280×960 | 60 | 60 | operator-qualified |
| VISR 0.5.1 (build 20261026.31927, source 632034e99) | Mac mini (M4, 16 GB) | 2026-10-26 | b30 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 59.05–59.33 | 16.68–16.69 | 18.13–18.14 | 19.86–20.62 | 87.08–88.97 | 284.15–302.04 | 3544–3561 | 60.02–60.02 | nominal → nominal | 1280×960 | 60 | 60 | operator-qualified |
| VISR 0.5.1 (build 20261026.31927, source 632034e99) | Mac mini (M4, 16 GB) | 2026-10-26 | a10 | uncapped; +0 s; matched-stock; warm-up 15 s | ok; 3 rounds | 489.02–493.28 | 1.61–1.64 | 4.61–4.63 | 5.84–5.99 | — | 24.29–38.66 | 29349–29601 | 60.01–60.02 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| VISR 0.5.1 (build 20261026.31927, source 632034e99) | Mac mini (M4, 16 GB) | 2026-10-26 | b30 | uncapped; +0 s; matched-stock; warm-up 15 s | ok; 3 rounds | 419.11–419.64 | 1.70–1.73 | 5.16–5.22 | 6.25–6.32 | — | 294.67–579.73 | 25152–25182 | 60.01–60.02 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| VISR 0.5.1 (build 20261026.31927, source 632034e99) | Mac mini (M4, 16 GB) | 2026-10-26 | b30 | uncapped; +90 s; matched-stock; warm-up 15 s | ok; 3 rounds | 420.59–427.26 | 1.76–1.80 | 4.84–5.17 | 6.04–6.25 | — | 24.17–25.34 | 25238–25641 | 60.01–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| VISR 0.5.1 (build 20261026.31927, source 632034e99) | Mac mini (M6, 24 GB) | 2026-10-26 | a10 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 59.93–59.95 | 16.65–16.66 | 17.60–18.14 | 18.50–19.13 | 86.27–93.94 | 31.33–34.42 | 3596–3598 | 60.00–60.02 | nominal → nominal | 1280×960 | 60 | 60 | operator-qualified |
| VISR 0.5.1 (build 20261026.31927, source 632034e99) | Mac mini (M6, 24 GB) | 2026-10-26 | b30 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 59.35–59.88 | 16.65–16.67 | 17.79–18.04 | 18.53–18.91 | 86.89–89.61 | 51.72–567.81 | 3561–3594 | 60.00–60.02 | nominal → nominal | 1280×960 | 60 | 60 | operator-qualified |
| VISR 0.5.1 (build 20261026.31927, source 632034e99) | Mac mini (M6, 24 GB) | 2026-10-26 | a10 | uncapped; +0 s; matched-stock; warm-up 15 s | ok; 3 rounds | 584.04–740.88 | 0.65–0.75 | 4.56–4.68 | 5.22–5.33 | — | 18.25–123.05 | 35044–44456 | 60.00–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| VISR 0.5.1 (build 20261026.31927, source 632034e99) | Mac mini (M6, 24 GB) | 2026-10-26 | b30 | uncapped; +0 s; matched-stock; warm-up 15 s | ok; 3 rounds | 733.58–754.03 | 0.90–0.98 | 4.18–4.41 | 4.72–5.04 | — | 21.24–119.57 | 44017–45253 | 60.00–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| VISR 0.5.1 (build 20261026.31927, source 632034e99) | Mac mini (M6, 24 GB) | 2026-10-26 | b30 | uncapped; +90 s; matched-stock; warm-up 15 s | ok; 3 rounds | 549.79–614.88 | 1.04–1.07 | 4.58–4.80 | 5.13–5.16 | — | 20.18–22.18 | 32994–36896 | 60.01–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |

## OpenCE-macOS

Measured versions: OpenCE-macOS v1.0.2 (build 2, source 80a4d0383).

Apple's metalperftrace records per-frame on-glass drawable intervals. CPU/GPU durations and aggregate FPS are excluded. Repeated image content is not detected.

| Build | Machine | Date | Scenario | Conditions | Status | Avg FPS | p50 (ms) | p95 (ms) | p99 (ms) | On time (%) | Worst (ms) | Frames | Duration (s) | Thermal | Resolution | Cap (fps) | Display (Hz) | Display rate basis |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | Mac mini (M4, 16 GB) | 2026-10-24 | a10 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 59.33–59.98 | 16.65–16.66 | 18.23–18.48 | 19.41–19.59 | 85.13–89.97 | 31.52–484.67 | 3560–3599 | 60.00–60.00 | nominal → nominal | 1280×960 | 60 | 60 | operator-qualified |
| OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | Mac mini (M4, 16 GB) | 2026-10-24 | b30 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 59.02–59.92 | 16.65–16.66 | 18.16–18.31 | 19.24–19.79 | 87.88–89.74 | 97.10–165.96 | 3541–3595 | 60.00–60.02 | nominal → nominal | 1280×960 | 60 | 60 | operator-qualified |
| OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | Mac mini (M4, 16 GB) | 2026-10-24 | a10 | uncapped; +0 s; matched-stock; warm-up 15 s | ok; 3 rounds | 487.75–490.48 | 1.31–1.35 | 4.95–4.98 | 6.36–6.48 | — | 44.61–526.92 | 29269–29432 | 60.00–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | Mac mini (M4, 16 GB) | 2026-10-24 | b30 | uncapped; +0 s; matched-stock; warm-up 15 s | ok; 3 rounds | 341.62–349.68 | 2.47–2.50 | 5.68–5.75 | 7.46–7.92 | — | 294.04–302.16 | 20500–20985 | 60.01–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | Mac mini (M4, 16 GB) | 2026-10-24 | b30 | uncapped; +90 s; matched-stock; warm-up 15 s | ok; 3 rounds | 328.88–335.75 | 2.84–2.91 | 4.02–4.40 | 6.63–7.00 | — | 23.08–296.08 | 19733–20146 | 60.00–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | Mac mini (M4, 16 GB) | 2026-10-26 | a10 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 59.65–59.73 | 16.67–16.70 | 18.16–18.29 | 19.49–19.88 | 85.08–87.25 | 55.88–58.13 | 3579–3585 | 60.00–60.02 | nominal → nominal | 1280×960 | 60 | 60 | operator-qualified |
| OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | Mac mini (M4, 16 GB) | 2026-10-26 | b30 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 59.25–59.32 | 16.66–16.68 | 18.14–18.19 | 19.73–19.77 | 85.84–86.29 | 266.45–267.50 | 3556–3560 | 60.00–60.02 | nominal → nominal | 1280×960 | 60 | 60 | operator-qualified |
| OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | Mac mini (M4, 16 GB) | 2026-10-26 | a10 | uncapped; +0 s; matched-stock; warm-up 15 s | ok; 3 rounds | 438.03–439.14 | 1.43–1.44 | 5.47–5.55 | 6.91–7.15 | — | 47.60–47.90 | 26287–26353 | 60.01–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | Mac mini (M4, 16 GB) | 2026-10-26 | b30 | uncapped; +0 s; matched-stock; warm-up 15 s | ok; 3 rounds | 325.35–328.51 | 2.56–2.56 | 6.20–6.30 | 8.01–8.19 | — | 283.55–304.88 | 19524–19713 | 60.01–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | Mac mini (M4, 16 GB) | 2026-10-26 | b30 | uncapped; +90 s; matched-stock; warm-up 15 s | ok; 3 rounds | 319.99–329.92 | 2.62–2.88 | 5.53–6.00 | 7.09–7.50 | — | 293.58–295.26 | 19202–19796 | 60.00–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | Mac mini (M6, 24 GB) | 2026-10-26 | a10 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 59.38–59.93 | 16.67–16.68 | 17.66–18.03 | 18.59–18.93 | 89.10–92.96 | 35.16–458.64 | 3563–3596 | 60.00–60.01 | nominal → nominal | 1280×960 | 60 | 60 | operator-qualified |
| OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | Mac mini (M6, 24 GB) | 2026-10-26 | b30 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 59.30–59.91 | 16.68–16.68 | 17.68–17.75 | 18.40–18.56 | 90.54–91.57 | 66.99–114.75 | 3559–3596 | 60.02–60.02 | nominal → nominal | 1280×960 | 60 | 60 | operator-qualified |
| OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | Mac mini (M6, 24 GB) | 2026-10-26 | a10 | uncapped; +0 s; matched-stock; warm-up 15 s | ok; 3 rounds | 579.06–992.72 | 0.86–1.16 | 3.10–4.58 | 4.31–5.19 | — | 24.68–28.05 | 34752–59573 | 60.01–60.02 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | Mac mini (M6, 24 GB) | 2026-10-26 | b30 | uncapped; +0 s; matched-stock; warm-up 15 s | ok; 3 rounds | 572.63–604.83 | 1.62–1.65 | 2.68–3.09 | 3.41–4.50 | — | 21.70–22.76 | 34366–36299 | 60.01–60.02 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| OpenCE-macOS v1.0.2 (build 2, source 80a4d0383) | Mac mini (M6, 24 GB) | 2026-10-26 | b30 | uncapped; +90 s; matched-stock; warm-up 15 s | ok; 3 rounds | 547.51–558.48 | 1.72–1.75 | 2.25–2.38 | 3.10–3.80 | — | 21.62–42.36 | 32855–33514 | 60.01–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |

## OpenCE web

Measured versions: OpenCE web build-280 (build 280, source 0a46688c3, network version 39); OpenCE web unknown version (build unknown).

Safari callbacks measure browser scheduling and may include repeated game images; they do not prove unique game frames.

| Build | Machine | Date | Scenario | Conditions | Status | Avg FPS | p50 (ms) | p95 (ms) | p99 (ms) | On time (%) | Worst (ms) | Frames | Duration (s) | Thermal | Resolution | Cap (fps) | Display (Hz) | Display rate basis |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| OpenCE web unknown version (build unknown) | Mac mini (M4, 16 GB) | 2026-10-24 | a10 | capped; +0 s; defaults; warm-up 15 s | unavailable; incomplete (0/3 rounds); safari-remote-automation-disabled | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |
| OpenCE web unknown version (build unknown) | Mac mini (M4, 16 GB) | 2026-10-24 | b30 | capped; +0 s; defaults; warm-up 15 s | unavailable; incomplete (0/3 rounds); safari-remote-automation-disabled | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |
| OpenCE web unknown version (build unknown) | Mac mini (M4, 16 GB) | 2026-10-26 | a10 | capped; +0 s; defaults; warm-up 15 s | unavailable; incomplete (0/3 rounds); safari-remote-automation-disabled | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |
| OpenCE web unknown version (build unknown) | Mac mini (M4, 16 GB) | 2026-10-26 | b30 | capped; +0 s; defaults; warm-up 15 s | unavailable; incomplete (0/3 rounds); safari-remote-automation-disabled | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |
| OpenCE web build-280 (build 280, source 0a46688c3, network version 39) | Mac mini (M6, 24 GB) | 2026-10-26 | a10 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 59.96–59.96 | 16.68–16.72 | 17.84–17.98 | 18.50–18.56 | 87.49–91.02 | 19.38–20.46 | 3597–3598 | 59.99–60.00 | nominal | 1280×960 | browser | 60 | operator-qualified |
| OpenCE web build-280 (build 280, source 0a46688c3, network version 39) | Mac mini (M6, 24 GB) | 2026-10-26 | b30 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 59.96–59.96 | 16.68–16.70 | 17.76–17.84 | 18.38–18.52 | 88.02–90.63 | 19.00–19.88 | 3597–3598 | 59.99–60.00 | nominal | 1280×960 | browser | 60 | operator-qualified |

## CoreCE

Measured versions: CoreCE 0.3.0 (build 44, source cccbf6fcc).

Apple's metalperftrace records per-frame on-glass drawable intervals. CPU/GPU durations and aggregate FPS are excluded. Repeated image content is not detected.

| Build | Machine | Date | Scenario | Conditions | Status | Avg FPS | p50 (ms) | p95 (ms) | p99 (ms) | On time (%) | Worst (ms) | Frames | Duration (s) | Thermal | Resolution | Cap (fps) | Display (Hz) | Display rate basis |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| CoreCE 0.3.0 (build 44, source cccbf6fcc) | Mac mini (M4, 16 GB) | 2026-10-26 | a10 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 58.91–59.83 | 16.65–16.65 | 18.43–18.57 | 20.16–20.65 | 85.54–87.70 | 45.82–403.24 | 3535–3591 | 60.00–60.02 | nominal → nominal | 1280×960 | 60 | 60 | operator-qualified |
| CoreCE 0.3.0 (build 44, source cccbf6fcc) | Mac mini (M4, 16 GB) | 2026-10-26 | b30 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 58.47–59.50 | 16.63–16.65 | 18.40–18.51 | 20.12–20.28 | 83.76–86.56 | 301.18–321.75 | 3509–3571 | 60.02–60.02 | nominal → nominal | 1280×960 | 60 | 60 | operator-qualified |
| CoreCE 0.3.0 (build 44, source cccbf6fcc) | Mac mini (M4, 16 GB) | 2026-10-26 | a10 | uncapped; +0 s; matched-stock; warm-up 15 s | ok; 3 rounds | 298.17–301.56 | 2.87–2.95 | 5.14–6.13 | 7.29–7.91 | — | 26.41–28.12 | 17894–18096 | 60.01–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| CoreCE 0.3.0 (build 44, source cccbf6fcc) | Mac mini (M4, 16 GB) | 2026-10-26 | b30 | uncapped; +0 s; matched-stock; warm-up 15 s | ok; 3 rounds | 175.24–179.65 | 5.33–5.49 | 8.25–8.54 | 9.48–11.53 | — | 297.70–303.09 | 10515–10780 | 60.00–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| CoreCE 0.3.0 (build 44, source cccbf6fcc) | Mac mini (M4, 16 GB) | 2026-10-26 | b30 | uncapped; +90 s; matched-stock; warm-up 15 s | ok; 3 rounds | 142.59–146.67 | 6.52–6.73 | 9.11–9.72 | 10.79–11.31 | — | 124.82–304.36 | 8556–8801 | 60.01–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| CoreCE 0.3.0 (build 44, source cccbf6fcc) | Mac mini (M6, 24 GB) | 2026-10-26 | a10 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 59.28–59.95 | 16.66–16.68 | 17.80–17.86 | 18.56–18.68 | 90.99–91.21 | 32.84–402.66 | 3557–3597 | 60.00–60.00 | nominal → nominal | 1280×960 | 60 | 60 | operator-qualified |
| CoreCE 0.3.0 (build 44, source cccbf6fcc) | Mac mini (M6, 24 GB) | 2026-10-26 | b30 | capped; +0 s; defaults; warm-up 15 s | ok; 3 rounds | 59.08–59.86 | 16.66–16.66 | 18.00–18.45 | 19.12–19.23 | 86.61–89.28 | 100.70–168.60 | 3545–3593 | 60.00–60.02 | nominal → nominal | 1280×960 | 60 | 60 | operator-qualified |
| CoreCE 0.3.0 (build 44, source cccbf6fcc) | Mac mini (M6, 24 GB) | 2026-10-26 | a10 | uncapped; +0 s; matched-stock; warm-up 15 s | ok; 3 rounds | 548.89–559.03 | 1.60–1.64 | 3.40–3.46 | 3.95–4.05 | — | 21.84–22.46 | 32934–33544 | 60.00–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| CoreCE 0.3.0 (build 44, source cccbf6fcc) | Mac mini (M6, 24 GB) | 2026-10-26 | b30 | uncapped; +0 s; matched-stock; warm-up 15 s | ok; 3 rounds | 259.81–263.01 | 3.86–3.87 | 4.90–5.04 | 5.59–5.92 | — | 22.22–22.86 | 15592–15784 | 60.01–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |
| CoreCE 0.3.0 (build 44, source cccbf6fcc) | Mac mini (M6, 24 GB) | 2026-10-26 | b30 | uncapped; +90 s; matched-stock; warm-up 15 s | ok; 3 rounds | 192.56–228.35 | 4.37–5.10 | 5.28–5.94 | 6.18–7.05 | — | 21.95–126.83 | 11553–13702 | 60.00–60.01 | nominal → nominal | 1280×960 | unlimited | 60 | operator-qualified |

## Chupathingy

Measured versions: Chupathingy v0.8.0g (build 0.8.0g).

Metal HUD per-frame intervals describe native presentation pacing. GPU duration and aggregate HUD FPS are excluded.

| Build | Machine | Date | Scenario | Conditions | Status | Avg FPS | p50 (ms) | p95 (ms) | p99 (ms) | On time (%) | Worst (ms) | Frames | Duration (s) | Thermal | Resolution | Cap (fps) | Display (Hz) | Display rate basis |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Chupathingy v0.8.0g (build 0.8.0g) | Mac mini (M4, 16 GB) | 2026-10-24 | a10 | capped; +0 s; defaults; warm-up 15 s | unavailable; incomplete (0/3 rounds); console-unavailable | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |
| Chupathingy v0.8.0g (build 0.8.0g) | Mac mini (M4, 16 GB) | 2026-10-24 | b30 | capped; +0 s; defaults; warm-up 15 s | unavailable; incomplete (0/3 rounds); console-unavailable | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |

## Halo OG

Measured versions: Halo OG v0.5.1 (build unknown).

Metal HUD per-frame intervals describe native presentation pacing. GPU duration and aggregate HUD FPS are excluded.

| Build | Machine | Date | Scenario | Conditions | Status | Avg FPS | p50 (ms) | p95 (ms) | p99 (ms) | On time (%) | Worst (ms) | Frames | Duration (s) | Thermal | Resolution | Cap (fps) | Display (Hz) | Display rate basis |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Halo OG v0.5.1 (build unknown) | Mac mini (M4, 16 GB) | 2026-10-24 | a10 | capped; +0 s; defaults; warm-up 15 s | unavailable; incomplete (0/3 rounds); console-unavailable | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |
| Halo OG v0.5.1 (build unknown) | Mac mini (M4, 16 GB) | 2026-10-24 | b30 | capped; +0 s; defaults; warm-up 15 s | unavailable; incomplete (0/3 rounds); console-unavailable | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |

## VISR offscreen sweep

Offscreen throughput uses the report's timing basis and is separate from displayed native intervals and Safari callbacks.

| Device | Build | Workload | Resolution | Configuration | Settings hash | Trials | FPS | Timing basis | Thermal | Report |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Apple TV HD | fe18f775b | a10-heroic | 1280×720 | tiers/tier-l720x1 | d8a75b017331 | 3 | 20.27–20.49 | offscreen\_uncapped | 0 | 9069b3dd9493 |
| Apple TV HD | fe18f775b | a10-heroic | 1600×900 | tiers/tier-l900x1 | 82ef2cf09b32 | 3 | 15.02–15.03 | offscreen\_uncapped | 0 | 9069b3dd9493 |
| Apple TV HD | fe18f775b | a10-heroic | 1920×1080 | tiers/tier-l1080x1 | 640733b6b241 | 3 | 12.46–12.50 | offscreen\_uncapped | 0 | 9069b3dd9493 |
| Apple TV HD | fe18f775b | a10-heroic | 960×540 | knobs/all-cheapest | 2fc8505b75a9 | 2 | 30.41–30.71 | offscreen\_uncapped | 0 | f2008300d5ad |
| Apple TV HD | fe18f775b | a10-heroic | 960×540 | knobs/anisotropic-filtering-2 | c59517a3e1d2 | 2 | 27.89–28.21 | offscreen\_uncapped | 0 | f642df06a7ae |
| Apple TV HD | fe18f775b | a10-heroic | 960×540 | knobs/anisotropic-filtering-4 | 2ff3734037a6 | 1 | 26.79–26.79 | offscreen\_uncapped | 0 | 22d5a96ad8db |
| Apple TV HD | fe18f775b | a10-heroic | 960×540 | knobs/anisotropic-filtering-8 | 014cd0ec43ff | 1 | 25.55–25.55 | offscreen\_uncapped | 0 | 22d5a96ad8db |
| Apple TV HD | fe18f775b | a10-heroic | 960×540 | knobs/anti-aliasing-fxaa | 0ad503b18cb6 | 2 | 23.39–23.50 | offscreen\_uncapped | 0 | f642df06a7ae |
| Apple TV HD | fe18f775b | a10-heroic | 960×540 | knobs/anti-aliasing-smaa | d3eca251f642 | 2 | 23.28–23.51 | offscreen\_uncapped | 0 | f2008300d5ad |
| Apple TV HD | fe18f775b | a10-heroic | 960×540 | knobs/base | 7813faad0744 | 1 | 24.84–24.84 | offscreen\_uncapped | 0 | 22d5a96ad8db |
| Apple TV HD | fe18f775b | a10-heroic | 960×540 | knobs/clutter-lowest-lod-on | fc1dcfaece1b | 2 | 25.06–25.12 | offscreen\_uncapped | 0 | f2008300d5ad |
| Apple TV HD | fe18f775b | a10-heroic | 960×540 | knobs/effect-resolution-half | 350fdf370443 | 2 | 24.90–24.93 | offscreen\_uncapped | 0 | f2008300d5ad |
| Apple TV HD | fe18f775b | a10-heroic | 960×540 | knobs/effect-resolution-xbox | cd5931e56d84 | 2 | 25.00–25.15 | offscreen\_uncapped | 0 | f2008300d5ad |
| Apple TV HD | fe18f775b | a10-heroic | 960×540 | knobs/model-lod-auto | 1e366c9087dc | 2 | 25.43–25.71 | offscreen\_uncapped | 0 | f642df06a7ae |
| Apple TV HD | fe18f775b | a10-heroic | 960×540 | knobs/shadow-map-size-128 | 4b3ec95c4899 | 1 | 25.95–25.95 | offscreen\_uncapped | 0 | 22d5a96ad8db |
| Apple TV HD | fe18f775b | a10-heroic | 960×540 | knobs/shadow-map-size-256 | 0afec7915385 | 1 | 25.49–25.49 | offscreen\_uncapped | 0 | 22d5a96ad8db |
| Apple TV HD | fe18f775b | a10-heroic | 960×540 | tiers/tier-l540x1 | 7813faad0744 | 3 | 24.73–24.91 | offscreen\_uncapped | 0 | 9069b3dd9493 |
| Apple TV HD | fe18f775b | d40-maw-fight | 1280×720 | tiers/tier-l720x1 | d8a75b017331 | 3 | 20.88–21.02 | offscreen\_uncapped | 0 | b50ca01e41f0 |
| Apple TV HD | fe18f775b | d40-maw-fight | 1600×900 | tiers/tier-l900x1 | 82ef2cf09b32 | 3 | 18.22–18.26 | offscreen\_uncapped | 0 | b50ca01e41f0 |
| Apple TV HD | fe18f775b | d40-maw-fight | 1920×1080 | tiers/tier-l1080x1 | 640733b6b241 | 3 | 13.79–13.89 | offscreen\_uncapped | 0 | b50ca01e41f0 |
| Apple TV HD | fe18f775b | d40-maw-fight | 960×540 | knobs/all-cheapest | 2fc8505b75a9 | 1 | 29.53–29.53 | offscreen\_uncapped | 0 | 8552e2da798a |
| Apple TV HD | fe18f775b | d40-maw-fight | 960×540 | knobs/anisotropic-filtering-2 | c59517a3e1d2 | 1 | 26.69–26.69 | offscreen\_uncapped | 0 | 5acc70b816b0 |
| Apple TV HD | fe18f775b | d40-maw-fight | 960×540 | knobs/anisotropic-filtering-4 | 2ff3734037a6 | 1 | 25.50–25.50 | offscreen\_uncapped | 0 | 5acc70b816b0 |
| Apple TV HD | fe18f775b | d40-maw-fight | 960×540 | knobs/anisotropic-filtering-8 | 014cd0ec43ff | 1 | 25.13–25.13 | offscreen\_uncapped | 0 | 5acc70b816b0 |
| Apple TV HD | fe18f775b | d40-maw-fight | 960×540 | knobs/anti-aliasing-fxaa | 0ad503b18cb6 | 1 | 22.75–22.75 | offscreen\_uncapped | 0 | 8552e2da798a |
| Apple TV HD | fe18f775b | d40-maw-fight | 960×540 | knobs/anti-aliasing-smaa | d3eca251f642 | 1 | 22.73–22.73 | offscreen\_uncapped | 0 | 8552e2da798a |
| Apple TV HD | fe18f775b | d40-maw-fight | 960×540 | knobs/base | 7813faad0744 | 1 | 24.77–24.77 | offscreen\_uncapped | 0 | 5acc70b816b0 |
| Apple TV HD | fe18f775b | d40-maw-fight | 960×540 | knobs/clutter-lowest-lod-on | fc1dcfaece1b | 1 | 25.02–25.02 | offscreen\_uncapped | 0 | 8552e2da798a |
| Apple TV HD | fe18f775b | d40-maw-fight | 960×540 | knobs/effect-resolution-half | 350fdf370443 | 1 | 24.97–24.97 | offscreen\_uncapped | 0 | 8552e2da798a |
| Apple TV HD | fe18f775b | d40-maw-fight | 960×540 | knobs/effect-resolution-xbox | cd5931e56d84 | 1 | 24.49–24.49 | offscreen\_uncapped | 0 | 8552e2da798a |
| Apple TV HD | fe18f775b | d40-maw-fight | 960×540 | knobs/model-lod-auto | 1e366c9087dc | 1 | 25.40–25.40 | offscreen\_uncapped | 0 | 5acc70b816b0 |
| Apple TV HD | fe18f775b | d40-maw-fight | 960×540 | knobs/shadow-map-size-128 | 4b3ec95c4899 | 1 | 26.10–26.10 | offscreen\_uncapped | 0 | 5acc70b816b0 |
| Apple TV HD | fe18f775b | d40-maw-fight | 960×540 | knobs/shadow-map-size-256 | 0afec7915385 | 1 | 25.85–25.85 | offscreen\_uncapped | 0 | 5acc70b816b0 |
| Apple TV HD | fe18f775b | d40-maw-fight | 960×540 | tiers/tier-l540x1 | 7813faad0744 | 3 | 24.78–24.82 | offscreen\_uncapped | 0 | b50ca01e41f0 |
| iPad Air (4th generation) | fe18f775b | a10-heroic | 1295×900 | knobs/all-cheapest | 8f88cd82a6b4 | 3 | 114.75–124.77 | offscreen\_uncapped | 0 | 5369adf6f446 |
| iPad Air (4th generation) | fe18f775b | a10-heroic | 1295×900 | knobs/anisotropic-filtering-2 | b04656ae088a | 3 | 117.60–122.28 | offscreen\_uncapped | 0 | 4637619dd7ca |
| iPad Air (4th generation) | fe18f775b | a10-heroic | 1295×900 | knobs/anisotropic-filtering-4 | ee4efe9c0bbf | 3 | 118.05–122.10 | offscreen\_uncapped | 0 | 9c0fd937f382 |
| iPad Air (4th generation) | fe18f775b | a10-heroic | 1295×900 | knobs/anisotropic-filtering-8 | 3f82ddf732a0 | 3 | 116.66–119.79 | offscreen\_uncapped | 0 | 9c0fd937f382 |
| iPad Air (4th generation) | fe18f775b | a10-heroic | 1295×900 | knobs/anti-aliasing-fxaa | 751fe66b2589 | 3 | 110.80–114.47 | offscreen\_uncapped | 0, 1 | 4637619dd7ca |
| iPad Air (4th generation) | fe18f775b | a10-heroic | 1295×900 | knobs/anti-aliasing-smaa | 277c60d5d590 | 3 | 111.14–112.99 | offscreen\_uncapped | 0, 1 | fbb72d3f95cf |
| iPad Air (4th generation) | fe18f775b | a10-heroic | 1295×900 | knobs/base | 66834c1d805f | 3 | 116.26–120.57 | offscreen\_uncapped | 0 | 9c0fd937f382 |
| iPad Air (4th generation) | fe18f775b | a10-heroic | 1295×900 | knobs/clutter-lowest-lod-on | 790b54d87215 | 3 | 116.51–122.41 | offscreen\_uncapped | 0, 1 | fbb72d3f95cf |
| iPad Air (4th generation) | fe18f775b | a10-heroic | 1295×900 | knobs/effect-resolution-half | 9b2946585a04 | 3 | 116.92–120.03 | offscreen\_uncapped | 0, 1 | 5369adf6f446 |
| iPad Air (4th generation) | fe18f775b | a10-heroic | 1295×900 | knobs/effect-resolution-xbox | 32fad638bd75 | 3 | 110.56–118.11 | offscreen\_uncapped | 0 | 5369adf6f446 |
| iPad Air (4th generation) | fe18f775b | a10-heroic | 1295×900 | knobs/model-lod-auto | 3ba71c4b331f | 3 | 116.09–119.56 | offscreen\_uncapped | 0, 1 | 4637619dd7ca |
| iPad Air (4th generation) | fe18f775b | a10-heroic | 1295×900 | knobs/shadow-map-size-128 | be77efbf5e0c | 3 | 117.19–119.79 | offscreen\_uncapped | 0 | 9c0fd937f382 |
| iPad Air (4th generation) | fe18f775b | a10-heroic | 1295×900 | knobs/shadow-map-size-256 | 6f2f29835687 | 3 | 115.35–122.78 | offscreen\_uncapped | 0 | 9c0fd937f382 |
| iPad Air (4th generation) | fe18f775b | a10-heroic | 1295×900 | tiers/tier-l900x1 | 66834c1d805f | 3 | 117.68–118.62 | offscreen\_uncapped | 0, 1 | 78475456757e |
| iPad Air (4th generation) | fe18f775b | a10-heroic | 1554×1080 | tiers/tier-l1080x1 | 88df170709f1 | 3 | 114.30–116.87 | offscreen\_uncapped | 0, 1 | 78475456757e |
| iPad Air (4th generation) | fe18f775b | a10-heroic-x1-5 | 1295×900 | tiers/tier-l900x1p5 | 66834c1d805f | 3 | 98.17–100.10 | offscreen\_uncapped | 0, 1 | 78475456757e |
| iPad Air (4th generation) | fe18f775b | a10-heroic-x1-5 | 1554×1080 | tiers/tier-l1080x1p5 | 88df170709f1 | 3 | 95.46–97.78 | offscreen\_uncapped | 0, 1 | 78475456757e |
| iPad Air (4th generation) | fe18f775b | d40-maw-fight | 1295×900 | knobs/all-cheapest | 8f88cd82a6b4 | 3 | 127.21–128.55 | offscreen\_uncapped | 0, 1 | 5136f5b77a33 |
| iPad Air (4th generation) | fe18f775b | d40-maw-fight | 1295×900 | knobs/anisotropic-filtering-2 | b04656ae088a | 3 | 122.82–124.50 | offscreen\_uncapped | 0 | 52901dd9378f |
| iPad Air (4th generation) | fe18f775b | d40-maw-fight | 1295×900 | knobs/anisotropic-filtering-4 | ee4efe9c0bbf | 3 | 121.48–123.26 | offscreen\_uncapped | 0 | 74b9c606ef82 |
| iPad Air (4th generation) | fe18f775b | d40-maw-fight | 1295×900 | knobs/anisotropic-filtering-8 | 3f82ddf732a0 | 3 | 121.47–123.70 | offscreen\_uncapped | 0 | 74b9c606ef82 |
| iPad Air (4th generation) | fe18f775b | d40-maw-fight | 1295×900 | knobs/anti-aliasing-fxaa | 751fe66b2589 | 3 | 116.51–118.42 | offscreen\_uncapped | 0 | 2e503fb66293 |
| iPad Air (4th generation) | fe18f775b | d40-maw-fight | 1295×900 | knobs/anti-aliasing-smaa | 277c60d5d590 | 3 | 114.49–115.83 | offscreen\_uncapped | 0, 1 | 5136f5b77a33 |
| iPad Air (4th generation) | fe18f775b | d40-maw-fight | 1295×900 | knobs/base | 66834c1d805f | 3 | 121.87–123.50 | offscreen\_uncapped | 0 | 0d01a6eb668f |
| iPad Air (4th generation) | fe18f775b | d40-maw-fight | 1295×900 | knobs/clutter-lowest-lod-on | 790b54d87215 | 3 | 122.34–123.89 | offscreen\_uncapped | 0, 1 | 5136f5b77a33 |
| iPad Air (4th generation) | fe18f775b | d40-maw-fight | 1295×900 | knobs/effect-resolution-half | 9b2946585a04 | 3 | 121.18–122.47 | offscreen\_uncapped | 0, 1 | 5136f5b77a33 |
| iPad Air (4th generation) | fe18f775b | d40-maw-fight | 1295×900 | knobs/effect-resolution-xbox | 32fad638bd75 | 3 | 121.99–123.71 | offscreen\_uncapped | 0, 1 | 5136f5b77a33 |
| iPad Air (4th generation) | fe18f775b | d40-maw-fight | 1295×900 | knobs/model-lod-auto | 3ba71c4b331f | 3 | 121.29–122.00 | offscreen\_uncapped | 0 | 52901dd9378f |
| iPad Air (4th generation) | fe18f775b | d40-maw-fight | 1295×900 | knobs/shadow-map-size-128 | be77efbf5e0c | 3 | 122.20–125.51 | offscreen\_uncapped | 0 | 0d01a6eb668f |
| iPad Air (4th generation) | fe18f775b | d40-maw-fight | 1295×900 | knobs/shadow-map-size-256 | 6f2f29835687 | 3 | 122.04–123.57 | offscreen\_uncapped | 0 | 0d01a6eb668f |
| iPad Air (4th generation) | fe18f775b | d40-maw-fight | 1295×900 | tiers/tier-l900x1 | 66834c1d805f | 3 | 121.72–124.07 | offscreen\_uncapped | 0 | 2fb6cc948b18 |
| iPad Air (4th generation) | fe18f775b | d40-maw-fight | 1554×1080 | tiers/tier-l1080x1 | 88df170709f1 | 3 | 117.74–120.97 | offscreen\_uncapped | 0 | 2fb6cc948b18 |
| iPad Air (4th generation) | fe18f775b | d40-x1-5 | 1295×900 | tiers/tier-l900x1p5 | 66834c1d805f | 3 | 107.65–109.02 | offscreen\_uncapped | 0 | 78475456757e |
| iPad Air (4th generation) | fe18f775b | d40-x1-5 | 1554×1080 | tiers/tier-l1080x1p5 | 88df170709f1 | 3 | 104.63–106.51 | offscreen\_uncapped | 0, 1 | 78475456757e |
| iPad Air (4th generation) | fe18f775b | mp-bloodgulch-64 | 777×540 | capacity/cap-min540 | b21701ad263b | 1 | 25.50–25.50 | offscreen\_uncapped | 1 | bcc2d90a45c9 |
| iPad Air (4th generation) | fe18f775b | mp-bloodgulch-64 | 777×540 | capacity/cap-min540-collfilter | 21ffb439593f | 1 | 25.31–25.31 | offscreen\_uncapped | 1 | bcc2d90a45c9 |
| iPad Air (4th generation) | fe18f775b | mp-sidewinder-128 | 777×540 | capacity/cap-min540 | b21701ad263b | 1 | 36.52–36.52 | offscreen\_uncapped | 1 | bcc2d90a45c9 |
| iPad Air (4th generation) | fe18f775b | mp16-chillout | 777×540 | capacity/cap-min540 | b21701ad263b | 1 | 52.97–52.97 | offscreen\_uncapped | 0 | bcc2d90a45c9 |
| iPad Air (4th generation) | fe18f775b | mp32-bloodgulch | 777×540 | capacity/cap-min540 | b21701ad263b | 1 | 38.20–38.20 | offscreen\_uncapped | 1 | bcc2d90a45c9 |
| iPad Pro (M1) | fe18f775b | a10-heroic | 1201×900 | tiers/tier-l900x1 | 66834c1d805f | 3 | 149.44–151.31 | offscreen\_uncapped | 0 | 857c4893c59e |
| iPad Pro (M1) | fe18f775b | a10-heroic | 1441×1080 | knobs/all-cheapest | 9eb041d40260 | 3 | 148.31–150.06 | offscreen\_uncapped | 0 | 295d77869d10 |
| iPad Pro (M1) | fe18f775b | a10-heroic | 1441×1080 | knobs/anisotropic-filtering-2 | 5f60aaddcb2c | 3 | 143.41–145.73 | offscreen\_uncapped | 0 | 4d0766312426 |
| iPad Pro (M1) | fe18f775b | a10-heroic | 1441×1080 | knobs/anisotropic-filtering-4 | a7d45e182252 | 3 | 140.28–145.05 | offscreen\_uncapped | 0 | f6a519711a23 |
| iPad Pro (M1) | fe18f775b | a10-heroic | 1441×1080 | knobs/anisotropic-filtering-8 | 3cde8a82fb8b | 3 | 137.53–140.20 | offscreen\_uncapped | 0 | f6a519711a23 |
| iPad Pro (M1) | fe18f775b | a10-heroic | 1441×1080 | knobs/anti-aliasing-fxaa | 2b4e415c1a64 | 3 | 122.90–125.76 | offscreen\_uncapped | 0 | 4d0766312426 |
| iPad Pro (M1) | fe18f775b | a10-heroic | 1441×1080 | knobs/anti-aliasing-smaa | 834b3cdb78ea | 3 | 123.36–127.28 | offscreen\_uncapped | 0 | b4c3959a487d |
| iPad Pro (M1) | fe18f775b | a10-heroic | 1441×1080 | knobs/base | 88df170709f1 | 3 | 135.91–138.98 | offscreen\_uncapped | 0 | f6a519711a23 |
| iPad Pro (M1) | fe18f775b | a10-heroic | 1441×1080 | knobs/clutter-lowest-lod-on | aa52788af8b8 | 3 | 135.92–139.18 | offscreen\_uncapped | 0 | b4c3959a487d |
| iPad Pro (M1) | fe18f775b | a10-heroic | 1441×1080 | knobs/effect-resolution-half | a96dc1413753 | 3 | 135.61–137.93 | offscreen\_uncapped | 0 | 295d77869d10 |
| iPad Pro (M1) | fe18f775b | a10-heroic | 1441×1080 | knobs/effect-resolution-xbox | b86f98f22af3 | 3 | 135.94–137.77 | offscreen\_uncapped | 0 | 295d77869d10 |
| iPad Pro (M1) | fe18f775b | a10-heroic | 1441×1080 | knobs/model-lod-auto | ac9fedcda502 | 3 | 138.07–140.28 | offscreen\_uncapped | 0 | 4d0766312426 |
| iPad Pro (M1) | fe18f775b | a10-heroic | 1441×1080 | knobs/shadow-map-size-128 | 1edf713156d2 | 3 | 138.79–141.51 | offscreen\_uncapped | 0 | f6a519711a23 |
| iPad Pro (M1) | fe18f775b | a10-heroic | 1441×1080 | knobs/shadow-map-size-256 | c71903603c95 | 3 | 138.25–139.76 | offscreen\_uncapped | 0 | f6a519711a23 |
| iPad Pro (M1) | fe18f775b | a10-heroic | 1441×1080 | tiers/tier-l1080x1 | 88df170709f1 | 3 | 136.20–139.19 | offscreen\_uncapped | 0 | 857c4893c59e |
| iPad Pro (M1) | fe18f775b | a10-heroic-x1-5 | 1201×900 | tiers/tier-l900x1p5 | 66834c1d805f | 3 | 113.90–114.37 | offscreen\_uncapped | 0 | 857c4893c59e |
| iPad Pro (M1) | fe18f775b | a10-heroic-x1-5 | 1441×1080 | tiers/tier-l1080x1p5 | 88df170709f1 | 3 | 106.55–107.52 | offscreen\_uncapped | 0 | 771f805ce948 |
| iPad Pro (M1) | fe18f775b | a10-heroic-x2 | 1201×900 | tiers/tier-l900x2 | 66834c1d805f | 3 | 117.41–119.57 | offscreen\_uncapped | 0 | 771f805ce948 |
| iPad Pro (M1) | fe18f775b | a10-heroic-x2 | 1441×1080 | tiers/tier-l1080x2 | 88df170709f1 | 3 | 110.76–113.13 | offscreen\_uncapped | 0 | 5f9524f50b0c |
| iPad Pro (M1) | fe18f775b | d40-maw-fight | 1201×900 | tiers/tier-l900x1 | 66834c1d805f | 3 | 153.04–156.43 | offscreen\_uncapped | 0 | 28ed761858a7 |
| iPad Pro (M1) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/all-cheapest | 9eb041d40260 | 3 | 149.93–152.48 | offscreen\_uncapped | 0 | 60d38e18372c |
| iPad Pro (M1) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/anisotropic-filtering-2 | 5f60aaddcb2c | 3 | 143.63–145.36 | offscreen\_uncapped | 0 | 4f9fa4aa1684 |
| iPad Pro (M1) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/anisotropic-filtering-4 | a7d45e182252 | 3 | 141.28–142.27 | offscreen\_uncapped | 0 | 4f9fa4aa1684 |
| iPad Pro (M1) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/anisotropic-filtering-8 | 3cde8a82fb8b | 3 | 139.44–142.94 | offscreen\_uncapped | 0 | 4f9fa4aa1684 |
| iPad Pro (M1) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/anti-aliasing-fxaa | 2b4e415c1a64 | 3 | 129.50–131.25 | offscreen\_uncapped | 0 | d264b6f115a5 |
| iPad Pro (M1) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/anti-aliasing-smaa | 834b3cdb78ea | 3 | 126.49–126.82 | offscreen\_uncapped | 0 | 60d38e18372c |
| iPad Pro (M1) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/base | 88df170709f1 | 3 | 138.62–138.89 | offscreen\_uncapped | 0 | 4f9fa4aa1684 |
| iPad Pro (M1) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/clutter-lowest-lod-on | aa52788af8b8 | 3 | 140.06–142.74 | offscreen\_uncapped | 0 | 60d38e18372c |
| iPad Pro (M1) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/effect-resolution-half | a96dc1413753 | 3 | 139.59–140.94 | offscreen\_uncapped | 0 | 0e3f0cf89357 |
| iPad Pro (M1) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/effect-resolution-xbox | b86f98f22af3 | 3 | 139.03–141.01 | offscreen\_uncapped | 0 | 60d38e18372c |
| iPad Pro (M1) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/model-lod-auto | ac9fedcda502 | 3 | 140.27–141.45 | offscreen\_uncapped | 0 | 9bb40de485d7 |
| iPad Pro (M1) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/shadow-map-size-128 | 1edf713156d2 | 3 | 142.98–145.71 | offscreen\_uncapped | 0 | 4f9fa4aa1684 |
| iPad Pro (M1) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/shadow-map-size-256 | c71903603c95 | 3 | 143.36–144.29 | offscreen\_uncapped | 0 | 4f9fa4aa1684 |
| iPad Pro (M1) | fe18f775b | d40-maw-fight | 1441×1080 | tiers/tier-l1080x1 | 88df170709f1 | 3 | 139.49–141.92 | offscreen\_uncapped | 0 | 28ed761858a7 |
| iPad Pro (M1) | fe18f775b | d40-x1-5 | 1201×900 | tiers/tier-l900x1p5 | 66834c1d805f | 3 | 139.70–140.75 | offscreen\_uncapped | 0 | d3abaf493edd |
| iPad Pro (M1) | fe18f775b | d40-x1-5 | 1441×1080 | tiers/tier-l1080x1p5 | 88df170709f1 | 3 | 125.35–126.32 | offscreen\_uncapped | 0 | 857c4893c59e |
| iPad Pro (M1) | fe18f775b | d40-x2 | 1201×900 | tiers/tier-l900x2 | 66834c1d805f | 3 | 128.37–129.85 | offscreen\_uncapped | 0 | 857c4893c59e |
| iPad Pro (M1) | fe18f775b | d40-x2 | 1441×1080 | tiers/tier-l1080x2 | 88df170709f1 | 3 | 119.26–119.70 | offscreen\_uncapped | 0 | 857c4893c59e |
| iPad Pro (M1) | fe18f775b | mp-bloodgulch-64 | 720×540 | capacity/cap-min540 | b21701ad263b | 2 | 25.81–26.38 | offscreen\_uncapped | 0 | 1fded446ede8 |
| iPad Pro (M1) | fe18f775b | mp-bloodgulch-64 | 720×540 | capacity/cap-min540-collfilter | 21ffb439593f | 2 | 25.03–26.23 | offscreen\_uncapped | 0 | 1fded446ede8 |
| iPad Pro (M1) | fe18f775b | mp-sidewinder-128 | 720×540 | capacity/cap-min540 | b21701ad263b | 2 | 44.53–44.89 | offscreen\_uncapped | 0 | 1fded446ede8 |
| iPad Pro (M1) | fe18f775b | mp-sidewinder-128 | 720×540 | capacity/cap-min540-collfilter | 21ffb439593f | 2 | 44.05–44.62 | offscreen\_uncapped | 0 | 1fded446ede8 |
| iPad Pro (M1) | fe18f775b | mp16-chillout | 720×540 | capacity/cap-min540 | b21701ad263b | 2 | 50.25–53.94 | offscreen\_uncapped | 0 | 1fded446ede8 |
| iPad Pro (M1) | fe18f775b | mp32-bloodgulch | 720×540 | capacity/cap-min540 | b21701ad263b | 2 | 35.95–38.33 | offscreen\_uncapped | 0 | 1fded446ede8 |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | a50-x1-5 | 1200×900 | ext-new/ext-l900x1p5 | 66834c1d805f | 1 | 49.22–49.22 | offscreen\_uncapped | 0 | d44bc21a980e |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | a50-x1-5 | 1440×1080 | ext-new/ext-l1080x1p5 | 88df170709f1 | 1 | 40.24–40.24 | offscreen\_uncapped | 0 | d44bc21a980e |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | a50-x1-5 | 720×540 | ext-new/ext-l540x1p5 | 02a24c65ee8b | 1 | 72.13–72.13 | offscreen\_uncapped | 0 | d44bc21a980e |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | a50-x1-5 | 960×720 | ext-new/ext-l720x1p5 | 771fcc949602 | 1 | 60.94–60.94 | offscreen\_uncapped | 0 | d44bc21a980e |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 1200×900 | tiers/tier-l900x1 | 66834c1d805f | 3 | 43.41–43.50 | offscreen\_uncapped | 0 | d7663b87d17a |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 1440×1080 | tiers/tier-l1080x1 | 88df170709f1 | 3 | 37.29–37.61 | offscreen\_uncapped | 0 | d7663b87d17a |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 720×540 | tiers/tier-l540x1 | 02a24c65ee8b | 3 | 59.32–59.56 | offscreen\_uncapped | 0 | d7663b87d17a |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 960×720 | knobs/all-cheapest | 70602919de3f | 3 | 54.88–54.98 | offscreen\_uncapped | 0 | da62e467b0c7 |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 960×720 | knobs/anisotropic-filtering-2 | 261c6492380c | 3 | 52.47–52.51 | offscreen\_uncapped | 0 | 91ab2f2b47b3 |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 960×720 | knobs/anisotropic-filtering-4 | 893b9fbbe11e | 3 | 51.79–51.98 | offscreen\_uncapped | 0 | 30d598922149 |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 960×720 | knobs/anisotropic-filtering-8 | 3c42eac64058 | 3 | 51.19–51.46 | offscreen\_uncapped | 0 | 30d598922149 |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 960×720 | knobs/anti-aliasing-fxaa | 6a7daf32a19b | 3 | 44.55–44.64 | offscreen\_uncapped | 0 | 91ab2f2b47b3 |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 960×720 | knobs/anti-aliasing-smaa | 3df2daf57b5c | 3 | 46.01–46.19 | offscreen\_uncapped | 0 | da62e467b0c7 |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 960×720 | knobs/base | 771fcc949602 | 3 | 50.95–51.17 | offscreen\_uncapped | 0 | 30d598922149 |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 960×720 | knobs/clutter-lowest-lod-on | 2ce4cfd04ad8 | 3 | 51.44–51.68 | offscreen\_uncapped | 0 | da62e467b0c7 |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 960×720 | knobs/effect-resolution-half | 7bf20dbb8d5f | 3 | 50.98–51.11 | offscreen\_uncapped | 0 | da62e467b0c7 |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 960×720 | knobs/effect-resolution-xbox | cf02a0fb3c49 | 3 | 50.83–51.11 | offscreen\_uncapped | 0 | da62e467b0c7 |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 960×720 | knobs/model-lod-auto | c55cc81b3004 | 3 | 51.79–51.98 | offscreen\_uncapped | 0 | 91ab2f2b47b3 |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 960×720 | knobs/shadow-map-size-128 | d1cf68db663b | 3 | 52.89–53.16 | offscreen\_uncapped | 0 | 30d598922149 |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 960×720 | knobs/shadow-map-size-256 | dc9494b46f0b | 3 | 52.88–53.18 | offscreen\_uncapped | 0 | 30d598922149 |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 960×720 | tiers/tier-l720x1 | 771fcc949602 | 3 | 51.00–51.04 | offscreen\_uncapped | 0 | d7663b87d17a |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-x1-5 | 1200×900 | ext-new/ext-l900x1p5 | 66834c1d805f | 1 | 41.57–41.57 | offscreen\_uncapped | 0 | d44bc21a980e |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-x1-5 | 1440×1080 | ext-new/ext-l1080x1p5 | 88df170709f1 | 1 | 36.00–36.00 | offscreen\_uncapped | 0 | d44bc21a980e |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-x1-5 | 720×540 | ext-new/ext-l540x1p5 | 02a24c65ee8b | 1 | 54.72–54.72 | offscreen\_uncapped | 0 | d44bc21a980e |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-x1-5 | 960×720 | ext-new/ext-l720x1p5 | 771fcc949602 | 1 | 48.07–48.07 | offscreen\_uncapped | 0 | d44bc21a980e |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | mp16-chillout | 720×540 | capacity/cap-min540 | b21701ad263b | 2 | 15.35–15.48 | offscreen\_uncapped | 0 | 45f36fb318e7 |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | mp32-bloodgulch | 720×540 | capacity/cap-min540 | b21701ad263b | 2 | 12.70–13.43 | offscreen\_uncapped | 0 | 45f36fb318e7 |
| iPad Pro 12.9-inch (2nd generation) | fe18f775b | a10-heroic | 1201×900 | tiers/tier-l900x1 | 66834c1d805f | 3 | 71.97–73.15 | offscreen\_uncapped | 0 | 2c23a9486636 |
| iPad Pro 12.9-inch (2nd generation) | fe18f775b | a10-heroic | 1441×1080 | tiers/tier-l1080x1 | 88df170709f1 | 3 | 62.93–63.50 | offscreen\_uncapped | 0 | 2c23a9486636 |
| iPad Pro 12.9-inch (2nd generation) | fe18f775b | a10-heroic-x1-5 | 1201×900 | tiers/tier-l900x1p5 | 66834c1d805f | 3 | 56.74–57.28 | offscreen\_uncapped | 0 | 2c23a9486636 |
| iPad Pro 12.9-inch (2nd generation) | fe18f775b | a10-heroic-x1-5 | 1441×1080 | tiers/tier-l1080x1p5 | 88df170709f1 | 3 | 48.65–49.34 | offscreen\_uncapped | 0 | 2c23a9486636 |
| iPad Pro 12.9-inch (2nd generation) | fe18f775b | d40-maw-fight | 1201×900 | knobs/anisotropic-filtering-2 | b04656ae088a | 1 | 77.66–77.66 | offscreen\_uncapped | 0 | 24338cd75eac |
| iPad Pro 12.9-inch (2nd generation) | fe18f775b | d40-maw-fight | 1201×900 | knobs/anisotropic-filtering-4 | ee4efe9c0bbf | 1 | 76.91–76.91 | offscreen\_uncapped | 0 | 24338cd75eac |
| iPad Pro 12.9-inch (2nd generation) | fe18f775b | d40-maw-fight | 1201×900 | knobs/anisotropic-filtering-8 | 3f82ddf732a0 | 1 | 75.54–75.54 | offscreen\_uncapped | 0 | 24338cd75eac |
| iPad Pro 12.9-inch (2nd generation) | fe18f775b | d40-maw-fight | 1201×900 | knobs/anti-aliasing-fxaa | 751fe66b2589 | 1 | 67.22–67.22 | offscreen\_uncapped | 0 | 24338cd75eac |
| iPad Pro 12.9-inch (2nd generation) | fe18f775b | d40-maw-fight | 1201×900 | knobs/base | 66834c1d805f | 1 | 75.35–75.35 | offscreen\_uncapped | 0 | 24338cd75eac |
| iPad Pro 12.9-inch (2nd generation) | fe18f775b | d40-maw-fight | 1201×900 | knobs/model-lod-auto | 3ba71c4b331f | 1 | 75.86–75.86 | offscreen\_uncapped | 0 | 24338cd75eac |
| iPad Pro 12.9-inch (2nd generation) | fe18f775b | d40-maw-fight | 1201×900 | knobs/shadow-map-size-128 | be77efbf5e0c | 1 | 79.71–79.71 | offscreen\_uncapped | 0 | 24338cd75eac |
| iPad Pro 12.9-inch (2nd generation) | fe18f775b | d40-maw-fight | 1201×900 | knobs/shadow-map-size-256 | 6f2f29835687 | 1 | 78.41–78.41 | offscreen\_uncapped | 0 | 24338cd75eac |
| iPad Pro 12.9-inch (2nd generation) | fe18f775b | d40-maw-fight | 1201×900 | tiers/tier-l900x1 | 66834c1d805f | 4 | 73.80–74.24 | offscreen\_uncapped | 0 | 35e7e1848e04 |
| iPad Pro 12.9-inch (2nd generation) | fe18f775b | d40-maw-fight | 1441×1080 | tiers/tier-l1080x1 | 88df170709f1 | 4 | 65.85–65.91 | offscreen\_uncapped | 0 | 35e7e1848e04 |
| iPad Pro 12.9-inch (2nd generation) | fe18f775b | d40-x1-5 | 1201×900 | tiers/tier-l900x1p5 | 66834c1d805f | 3 | 70.22–70.55 | offscreen\_uncapped | 0 | 2c23a9486636 |
| iPad Pro 12.9-inch (2nd generation) | fe18f775b | d40-x1-5 | 1441×1080 | tiers/tier-l1080x1p5 | 88df170709f1 | 3 | 63.04–63.47 | offscreen\_uncapped | 0 | 2c23a9486636 |
| iPhone 12 Pro | fe18f775b | a10-heroic | 1558×720 | knobs/all-cheapest | 70602919de3f | 3 | 86.26–86.51 | offscreen\_uncapped | 0 | 58faa1f96f7c |
| iPhone 12 Pro | fe18f775b | a10-heroic | 1558×720 | knobs/anisotropic-filtering-2 | 261c6492380c | 3 | 76.99–77.29 | offscreen\_uncapped | 0 | 54a65607cf3d |
| iPhone 12 Pro | fe18f775b | a10-heroic | 1558×720 | knobs/anisotropic-filtering-4 | 893b9fbbe11e | 3 | 73.60–73.74 | offscreen\_uncapped | 0 | 365dd110454d |
| iPhone 12 Pro | fe18f775b | a10-heroic | 1558×720 | knobs/anisotropic-filtering-8 | 3c42eac64058 | 3 | 69.96–71.26 | offscreen\_uncapped | 0 | 365dd110454d |
| iPhone 12 Pro | fe18f775b | a10-heroic | 1558×720 | knobs/anti-aliasing-fxaa | 6a7daf32a19b | 3 | 60.87–61.02 | offscreen\_uncapped | 0 | 54a65607cf3d |
| iPhone 12 Pro | fe18f775b | a10-heroic | 1558×720 | knobs/anti-aliasing-smaa | 3df2daf57b5c | 3 | 59.72–60.35 | offscreen\_uncapped | 0, 1 | 276758d7e878 |
| iPhone 12 Pro | fe18f775b | a10-heroic | 1558×720 | knobs/base | 771fcc949602 | 3 | 69.53–70.26 | offscreen\_uncapped | 0 | 365dd110454d |
| iPhone 12 Pro | fe18f775b | a10-heroic | 1558×720 | knobs/clutter-lowest-lod-on | 2ce4cfd04ad8 | 3 | 70.27–70.46 | offscreen\_uncapped | 0, 1 | 276758d7e878 |
| iPhone 12 Pro | fe18f775b | a10-heroic | 1558×720 | knobs/effect-resolution-half | 7bf20dbb8d5f | 3 | 69.23–69.84 | offscreen\_uncapped | 0 | 58faa1f96f7c |
| iPhone 12 Pro | fe18f775b | a10-heroic | 1558×720 | knobs/effect-resolution-xbox | cf02a0fb3c49 | 3 | 69.39–69.81 | offscreen\_uncapped | 0 | 58faa1f96f7c |
| iPhone 12 Pro | fe18f775b | a10-heroic | 1558×720 | knobs/model-lod-auto | c55cc81b3004 | 3 | 70.55–71.20 | offscreen\_uncapped | 0 | 54a65607cf3d |
| iPhone 12 Pro | fe18f775b | a10-heroic | 1558×720 | knobs/shadow-map-size-128 | d1cf68db663b | 3 | 74.98–75.16 | offscreen\_uncapped | 0 | 365dd110454d |
| iPhone 12 Pro | fe18f775b | a10-heroic | 1558×720 | knobs/shadow-map-size-256 | dc9494b46f0b | 3 | 73.89–74.18 | offscreen\_uncapped | 0 | 365dd110454d |
| iPhone 12 Pro | fe18f775b | a10-heroic | 1558×720 | tiers/tier-l720x1 | 771fcc949602 | 3 | 69.52–70.54 | offscreen\_uncapped | 0 | c341198e2254 |
| iPhone 12 Pro | fe18f775b | a10-heroic | 1948×900 | tiers/tier-l900x1 | 66834c1d805f | 3 | 56.21–56.56 | offscreen\_uncapped | 0 | c341198e2254 |
| iPhone 12 Pro | fe18f775b | a10-heroic | 2337×1080 | tiers/tier-l1080x1 | 88df170709f1 | 3 | 48.92–49.14 | offscreen\_uncapped | 0 | c341198e2254 |
| iPhone 12 Pro | fe18f775b | d40-maw-fight | 1558×720 | knobs/all-cheapest | 70602919de3f | 3 | 87.76–88.32 | offscreen\_uncapped | 0, 1 | d859c209c85d |
| iPhone 12 Pro | fe18f775b | d40-maw-fight | 1558×720 | knobs/anisotropic-filtering-2 | 261c6492380c | 3 | 75.33–75.54 | offscreen\_uncapped | 0, 1 | 51d00506a910 |
| iPhone 12 Pro | fe18f775b | d40-maw-fight | 1558×720 | knobs/anisotropic-filtering-4 | 893b9fbbe11e | 3 | 73.19–73.51 | offscreen\_uncapped | 0, 1 | 68db0ff28b70 |
| iPhone 12 Pro | fe18f775b | d40-maw-fight | 1558×720 | knobs/anisotropic-filtering-8 | 3c42eac64058 | 3 | 71.32–71.53 | offscreen\_uncapped | 0, 1 | 68db0ff28b70 |
| iPhone 12 Pro | fe18f775b | d40-maw-fight | 1558×720 | knobs/anti-aliasing-fxaa | 6a7daf32a19b | 3 | 61.47–61.82 | offscreen\_uncapped | 0, 1 | 51d00506a910 |
| iPhone 12 Pro | fe18f775b | d40-maw-fight | 1558×720 | knobs/anti-aliasing-smaa | 3df2daf57b5c | 3 | 60.73–61.06 | offscreen\_uncapped | 0 | d859c209c85d |
| iPhone 12 Pro | fe18f775b | d40-maw-fight | 1558×720 | knobs/base | 771fcc949602 | 3 | 70.09–70.42 | offscreen\_uncapped | 0 | aae6fc2f8038 |
| iPhone 12 Pro | fe18f775b | d40-maw-fight | 1558×720 | knobs/clutter-lowest-lod-on | 2ce4cfd04ad8 | 3 | 70.90–71.27 | offscreen\_uncapped | 0 | d859c209c85d |
| iPhone 12 Pro | fe18f775b | d40-maw-fight | 1558×720 | knobs/effect-resolution-half | 7bf20dbb8d5f | 3 | 70.43–70.59 | offscreen\_uncapped | 0, 1 | d859c209c85d |
| iPhone 12 Pro | fe18f775b | d40-maw-fight | 1558×720 | knobs/effect-resolution-xbox | cf02a0fb3c49 | 3 | 70.22–70.80 | offscreen\_uncapped | 0, 1 | d859c209c85d |
| iPhone 12 Pro | fe18f775b | d40-maw-fight | 1558×720 | knobs/model-lod-auto | c55cc81b3004 | 3 | 71.18–71.76 | offscreen\_uncapped | 0, 1 | 51d00506a910 |
| iPhone 12 Pro | fe18f775b | d40-maw-fight | 1558×720 | knobs/shadow-map-size-128 | d1cf68db663b | 3 | 78.77–78.93 | offscreen\_uncapped | 0 | c49e8e2cfcbf |
| iPhone 12 Pro | fe18f775b | d40-maw-fight | 1558×720 | knobs/shadow-map-size-256 | dc9494b46f0b | 3 | 77.02–77.19 | offscreen\_uncapped | 0 | c49e8e2cfcbf |
| iPhone 12 Pro | fe18f775b | d40-maw-fight | 1558×720 | tiers/tier-l720x1 | 771fcc949602 | 3 | 70.15–70.48 | offscreen\_uncapped | 0 | f5fa3d833780 |
| iPhone 12 Pro | fe18f775b | d40-maw-fight | 1948×900 | tiers/tier-l900x1 | 66834c1d805f | 3 | 57.62–57.95 | offscreen\_uncapped | 0 | f5fa3d833780 |
| iPhone 12 Pro | fe18f775b | d40-maw-fight | 2337×1080 | tiers/tier-l1080x1 | 88df170709f1 | 3 | 50.30–50.87 | offscreen\_uncapped | 0 | f5fa3d833780 |
| iPhone 17 Pro | fe18f775b | a10-heroic | 1957×900 | tiers/tier-l900x1 | 66834c1d805f | 3 | 171.85–187.10 | offscreen\_uncapped | 0, 1 | d2111658df97 |
| iPhone 17 Pro | fe18f775b | a10-heroic | 2348×1080 | knobs/all-cheapest | 9eb041d40260 | 3 | 184.99–269.03 | offscreen\_uncapped | 0, 1 | 1f7046cd9c0a |
| iPhone 17 Pro | fe18f775b | a10-heroic | 2348×1080 | knobs/anisotropic-filtering-2 | 5f60aaddcb2c | 3 | 158.34–165.65 | offscreen\_uncapped | 0, 1 | b272bae69bd0 |
| iPhone 17 Pro | fe18f775b | a10-heroic | 2348×1080 | knobs/anisotropic-filtering-4 | a7d45e182252 | 3 | 189.50–239.59 | offscreen\_uncapped | 0, 1 | 5a02d6a23286 |
| iPhone 17 Pro | fe18f775b | a10-heroic | 2348×1080 | knobs/anisotropic-filtering-8 | 3cde8a82fb8b | 3 | 173.57–240.74 | offscreen\_uncapped | 0, 1 | 5a02d6a23286 |
| iPhone 17 Pro | fe18f775b | a10-heroic | 2348×1080 | knobs/anti-aliasing-fxaa | 2b4e415c1a64 | 3 | 157.19–176.48 | offscreen\_uncapped | 0, 1 | b272bae69bd0 |
| iPhone 17 Pro | fe18f775b | a10-heroic | 2348×1080 | knobs/anti-aliasing-smaa | 834b3cdb78ea | 3 | 147.93–182.21 | offscreen\_uncapped | 1 | dc2fa319dde6 |
| iPhone 17 Pro | fe18f775b | a10-heroic | 2348×1080 | knobs/base | 88df170709f1 | 3 | 171.48–245.72 | offscreen\_uncapped | 0, 1 | 5a02d6a23286 |
| iPhone 17 Pro | fe18f775b | a10-heroic | 2348×1080 | knobs/clutter-lowest-lod-on | aa52788af8b8 | 3 | 162.01–186.03 | offscreen\_uncapped | 1 | dc2fa319dde6 |
| iPhone 17 Pro | fe18f775b | a10-heroic | 2348×1080 | knobs/effect-resolution-half | a96dc1413753 | 3 | 166.33–241.55 | offscreen\_uncapped | 0, 1 | 1f7046cd9c0a |
| iPhone 17 Pro | fe18f775b | a10-heroic | 2348×1080 | knobs/effect-resolution-xbox | b86f98f22af3 | 3 | 177.64–228.36 | offscreen\_uncapped | 0, 1 | 1f7046cd9c0a |
| iPhone 17 Pro | fe18f775b | a10-heroic | 2348×1080 | knobs/model-lod-auto | ac9fedcda502 | 3 | 154.08–166.72 | offscreen\_uncapped | 0, 1 | b272bae69bd0 |
| iPhone 17 Pro | fe18f775b | a10-heroic | 2348×1080 | knobs/shadow-map-size-128 | 1edf713156d2 | 3 | 169.58–234.61 | offscreen\_uncapped | 0, 1 | 5a02d6a23286 |
| iPhone 17 Pro | fe18f775b | a10-heroic | 2348×1080 | knobs/shadow-map-size-256 | c71903603c95 | 3 | 168.16–232.43 | offscreen\_uncapped | 0, 1 | 5a02d6a23286 |
| iPhone 17 Pro | fe18f775b | a10-heroic | 2348×1080 | tiers/tier-l1080x1 | 88df170709f1 | 3 | 161.77–177.89 | offscreen\_uncapped | 0, 1 | d2111658df97 |
| iPhone 17 Pro | fe18f775b | a10-heroic-x1-5 | 1957×900 | tiers/tier-l900x1p5 | 66834c1d805f | 3 | 125.85–130.67 | offscreen\_uncapped | 0, 1 | d2111658df97 |
| iPhone 17 Pro | fe18f775b | a10-heroic-x1-5 | 2348×1080 | tiers/tier-l1080x1p5 | 88df170709f1 | 3 | 132.23–137.09 | offscreen\_uncapped | 0, 1 | 5373cd7a10a7 |
| iPhone 17 Pro | fe18f775b | a10-heroic-x2 | 1957×900 | tiers/tier-l900x2 | 66834c1d805f | 3 | 134.25–139.27 | offscreen\_uncapped | 0, 1 | 5373cd7a10a7 |
| iPhone 17 Pro | fe18f775b | a10-heroic-x2 | 2348×1080 | tiers/tier-l1080x2 | 88df170709f1 | 3 | 134.57–142.99 | offscreen\_uncapped | 0, 1 | 664002fa46ac |
| iPhone 17 Pro | fe18f775b | d40-maw-fight | 1957×900 | tiers/tier-l900x1 | 66834c1d805f | 3 | 180.69–189.35 | offscreen\_uncapped | 1 | 47cb22ad9201 |
| iPhone 17 Pro | fe18f775b | d40-maw-fight | 2348×1080 | knobs/all-cheapest | 9eb041d40260 | 3 | 196.60–208.90 | offscreen\_uncapped | 1 | 1a6fb0efa7a1 |
| iPhone 17 Pro | fe18f775b | d40-maw-fight | 2348×1080 | knobs/anisotropic-filtering-2 | 5f60aaddcb2c | 3 | 176.83–195.18 | offscreen\_uncapped | 1 | f5e3118f74bb |
| iPhone 17 Pro | fe18f775b | d40-maw-fight | 2348×1080 | knobs/anisotropic-filtering-4 | a7d45e182252 | 3 | 196.95–197.70 | offscreen\_uncapped | 0, 1 | a251155e0e42 |
| iPhone 17 Pro | fe18f775b | d40-maw-fight | 2348×1080 | knobs/anisotropic-filtering-8 | 3cde8a82fb8b | 3 | 182.60–243.44 | offscreen\_uncapped | 0, 1 | a251155e0e42 |
| iPhone 17 Pro | fe18f775b | d40-maw-fight | 2348×1080 | knobs/anti-aliasing-fxaa | 2b4e415c1a64 | 3 | 172.00–186.20 | offscreen\_uncapped | 1 | f5e3118f74bb |
| iPhone 17 Pro | fe18f775b | d40-maw-fight | 2348×1080 | knobs/anti-aliasing-smaa | 834b3cdb78ea | 3 | 170.85–179.83 | offscreen\_uncapped | 0, 1 | 1a6fb0efa7a1 |
| iPhone 17 Pro | fe18f775b | d40-maw-fight | 2348×1080 | knobs/base | 88df170709f1 | 3 | 192.01–246.81 | offscreen\_uncapped | 0 | b697a09e8633 |
| iPhone 17 Pro | fe18f775b | d40-maw-fight | 2348×1080 | knobs/clutter-lowest-lod-on | aa52788af8b8 | 3 | 179.29–197.33 | offscreen\_uncapped | 1 | 1a6fb0efa7a1 |
| iPhone 17 Pro | fe18f775b | d40-maw-fight | 2348×1080 | knobs/effect-resolution-half | a96dc1413753 | 3 | 183.35–189.49 | offscreen\_uncapped | 1 | 1a6fb0efa7a1 |
| iPhone 17 Pro | fe18f775b | d40-maw-fight | 2348×1080 | knobs/effect-resolution-xbox | b86f98f22af3 | 3 | 180.90–194.65 | offscreen\_uncapped | 1 | 1a6fb0efa7a1 |
| iPhone 17 Pro | fe18f775b | d40-maw-fight | 2348×1080 | knobs/model-lod-auto | ac9fedcda502 | 3 | 191.28–197.50 | offscreen\_uncapped | 1 | f5e3118f74bb |
| iPhone 17 Pro | fe18f775b | d40-maw-fight | 2348×1080 | knobs/shadow-map-size-128 | 1edf713156d2 | 3 | 195.86–263.92 | offscreen\_uncapped | 0 | b697a09e8633 |
| iPhone 17 Pro | fe18f775b | d40-maw-fight | 2348×1080 | knobs/shadow-map-size-256 | c71903603c95 | 3 | 192.50–255.75 | offscreen\_uncapped | 0 | b697a09e8633 |
| iPhone 17 Pro | fe18f775b | d40-maw-fight | 2348×1080 | tiers/tier-l1080x1 | 88df170709f1 | 3 | 180.87–200.27 | offscreen\_uncapped | 1 | 47cb22ad9201 |
| iPhone 17 Pro | fe18f775b | d40-x1-5 | 1957×900 | tiers/tier-l900x1p5 | 66834c1d805f | 3 | 127.97–203.50 | offscreen\_uncapped | 0, 1 | 45ea3736328e |
| iPhone 17 Pro | fe18f775b | d40-x1-5 | 2348×1080 | tiers/tier-l1080x1p5 | 88df170709f1 | 3 | 125.69–184.19 | offscreen\_uncapped | 0, 1 | d2111658df97 |
| iPhone 17 Pro | fe18f775b | d40-x2 | 1957×900 | tiers/tier-l900x2 | 66834c1d805f | 3 | 131.00–192.05 | offscreen\_uncapped | 0, 1 | d2111658df97 |
| iPhone 17 Pro | fe18f775b | d40-x2 | 2348×1080 | tiers/tier-l1080x2 | 88df170709f1 | 3 | 135.89–153.16 | offscreen\_uncapped | 0, 1 | d2111658df97 |
| iPhone 17 Pro | fe18f775b | mp-bloodgulch-64 | 1174×540 | capacity/cap-min540 | b21701ad263b | 2 | 31.65–32.55 | offscreen\_uncapped | 0, 1 | 0b729dc09219 |
| iPhone 17 Pro | fe18f775b | mp-bloodgulch-64 | 1174×540 | capacity/cap-min540-collfilter | 21ffb439593f | 2 | 30.96–33.17 | offscreen\_uncapped | 0, 1 | 0b729dc09219 |
| iPhone 17 Pro | fe18f775b | mp-sidewinder-128 | 1174×540 | capacity/cap-min540 | b21701ad263b | 2 | 59.22–62.40 | offscreen\_uncapped | 1 | 0b729dc09219 |
| iPhone 17 Pro | fe18f775b | mp-sidewinder-128 | 1174×540 | capacity/cap-min540-collfilter | 21ffb439593f | 2 | 59.72–60.59 | offscreen\_uncapped | 0, 1 | 0b729dc09219 |
| iPhone 17 Pro | fe18f775b | mp16-chillout | 1174×540 | capacity/cap-min540 | b21701ad263b | 2 | 59.33–66.26 | offscreen\_uncapped | 0, 1 | 0b729dc09219 |
| iPhone 17 Pro | fe18f775b | mp32-bloodgulch | 1174×540 | capacity/cap-min540 | b21701ad263b | 2 | 41.54–43.86 | offscreen\_uncapped | 0, 1 | 0b729dc09219 |

## VISR submission timing

These self-reported submission timings measure submission throughput, not displayed pacing. They are separate from offscreen throughput, native presentation intervals, and Safari callbacks.

| Device | Build | Workload | Resolution | Configuration | Settings hash | Trials | FPS | Timing basis | Thermal | Report |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Apple TV HD | fe18f775b | a10-heroic | 960×540 | reconcile/recon-min540-off | 2fc8505b75a9 | 2 | 51.56–51.63 | submission\_timing | 0 | 24a42d6fb0cc |
| Apple TV HD | fe18f775b | a10-heroic | 960×540 | reconcile/recon-prof540-off | 7813faad0744 | 2 | 39.58–39.60 | submission\_timing | 0 | 24a42d6fb0cc |
| Apple TV HD | fe18f775b | a10-heroic | 960×540 | reconcile/recon-prof540-on | 7813faad0744 | 2 | 39.79–40.02 | submission\_timing | 0 | 24a42d6fb0cc |
| Apple TV HD | fe18f775b | d40-maw-fight | 960×540 | reconcile/recon-min540-off | 2fc8505b75a9 | 2 | 45.24–45.45 | submission\_timing | 0 | 6bc45462ea92 |
| Apple TV HD | fe18f775b | d40-maw-fight | 960×540 | reconcile/recon-prof540-off | 7813faad0744 | 2 | 38.56–38.62 | submission\_timing | 0 | 6bc45462ea92 |
| Apple TV HD | fe18f775b | d40-maw-fight | 960×540 | reconcile/recon-prof540-on | 7813faad0744 | 2 | 37.83–38.02 | submission\_timing | 0 | 6bc45462ea92 |
| MacBook Air (M3) | fe18f775b | a10-heroic | 1441×1080 | knobs/all-cheapest | 9eb041d40260 | 2 | 119.16–119.73 | submission\_timing | 0 | 74be4ee3b844 |
| MacBook Air (M3) | fe18f775b | a10-heroic | 1441×1080 | knobs/anisotropic-filtering-2 | 5f60aaddcb2c | 1 | 59.88–59.88 | submission\_timing | 0 | 74e2131bf54d |
| MacBook Air (M3) | fe18f775b | a10-heroic | 1441×1080 | knobs/anisotropic-filtering-4 | a7d45e182252 | 1 | 60.00–60.00 | submission\_timing | 0 | 3b24498d700a |
| MacBook Air (M3) | fe18f775b | a10-heroic | 1441×1080 | knobs/anisotropic-filtering-8 | 3cde8a82fb8b | 1 | 60.00–60.00 | submission\_timing | 0 | 3b24498d700a |
| MacBook Air (M3) | fe18f775b | a10-heroic | 1441×1080 | knobs/anti-aliasing-fxaa | 2b4e415c1a64 | 1 | 60.00–60.00 | submission\_timing | 0 | 74e2131bf54d |
| MacBook Air (M3) | fe18f775b | a10-heroic | 1441×1080 | knobs/anti-aliasing-smaa | 834b3cdb78ea | 2 | 119.56–119.69 | submission\_timing | 0 | c685ce71e532 |
| MacBook Air (M3) | fe18f775b | a10-heroic | 1441×1080 | knobs/base | 88df170709f1 | 1 | 119.19–119.19 | submission\_timing | 0 | 3b24498d700a |
| MacBook Air (M3) | fe18f775b | a10-heroic | 1441×1080 | knobs/clutter-lowest-lod-on | aa52788af8b8 | 2 | 119.56–119.65 | submission\_timing | 0 | c685ce71e532 |
| MacBook Air (M3) | fe18f775b | a10-heroic | 1441×1080 | knobs/effect-resolution-half | a96dc1413753 | 2 | 119.43–119.60 | submission\_timing | 0 | 74be4ee3b844 |
| MacBook Air (M3) | fe18f775b | a10-heroic | 1441×1080 | knobs/effect-resolution-xbox | b86f98f22af3 | 2 | 119.55–119.64 | submission\_timing | 0 | 74be4ee3b844 |
| MacBook Air (M3) | fe18f775b | a10-heroic | 1441×1080 | knobs/model-lod-auto | ac9fedcda502 | 1 | 59.98–59.98 | submission\_timing | 0 | 74e2131bf54d |
| MacBook Air (M3) | fe18f775b | a10-heroic | 1441×1080 | knobs/shadow-map-size-128 | 1edf713156d2 | 1 | 60.00–60.00 | submission\_timing | 0 | 3b24498d700a |
| MacBook Air (M3) | fe18f775b | a10-heroic | 1441×1080 | knobs/shadow-map-size-256 | c71903603c95 | 1 | 87.12–87.12 | submission\_timing | 0 | 3b24498d700a |
| MacBook Air (M3) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/anisotropic-filtering-2 | 5f60aaddcb2c | 1 | 119.65–119.65 | submission\_timing | 0 | a840fd74c349 |
| MacBook Air (M3) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/anisotropic-filtering-4 | a7d45e182252 | 1 | 119.66–119.66 | submission\_timing | 0 | 3b24498d700a |
| MacBook Air (M3) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/anisotropic-filtering-8 | 3cde8a82fb8b | 1 | 119.72–119.72 | submission\_timing | 0 | 3b24498d700a |
| MacBook Air (M3) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/anti-aliasing-fxaa | 2b4e415c1a64 | 1 | 119.64–119.64 | submission\_timing | 0 | a840fd74c349 |
| MacBook Air (M3) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/anti-aliasing-smaa | 834b3cdb78ea | 1 | 119.71–119.71 | submission\_timing | 0 | a840fd74c349 |
| MacBook Air (M3) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/base | 88df170709f1 | 1 | 119.69–119.69 | submission\_timing | 0 | 3b24498d700a |
| MacBook Air (M3) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/clutter-lowest-lod-on | aa52788af8b8 | 1 | 119.71–119.71 | submission\_timing | 0 | a840fd74c349 |
| MacBook Air (M3) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/model-lod-auto | ac9fedcda502 | 1 | 119.71–119.71 | submission\_timing | 0 | a840fd74c349 |
| MacBook Air (M3) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/shadow-map-size-128 | 1edf713156d2 | 1 | 119.65–119.65 | submission\_timing | 0 | 3b24498d700a |
| MacBook Air (M3) | fe18f775b | d40-maw-fight | 1441×1080 | knobs/shadow-map-size-256 | c71903603c95 | 1 | 119.59–119.59 | submission\_timing | 0 | 3b24498d700a |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | a10-heroic | 960×720 | onscreen/onscreen-base | 15a275b09bd7 | 1 | 28.28–28.28 | submission\_timing | 0 | ab9d1b3b7eea |
| iPad Pro 12.9-inch (1st generation) | fe18f775b | d40-maw-fight | 960×720 | onscreen/onscreen-base | 15a275b09bd7 | 1 | 28.28–28.28 | submission\_timing | 0 | aa8496877c96 |
