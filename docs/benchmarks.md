# Performance benchmarks

Date: 2026-10-24. Refresh wall time: 182.2 seconds.

Machine: Mac mini (M4). OS: 27.0.1.

Ranges show the minimum–maximum across unique rounds for each scenario; at least three rounds are required. Incomplete measurements remain visible. On-time share uses the recorded display rate with a 5% interval tolerance. The methods measure different events and are not ranked against one another.

## Builds and availability

| Target | Version | SHA-256 | Method | Status | Reason |
| --- | --- | --- | --- | --- | --- |
| chupathingy | v0.8.0g | 0370192fbcbf712bf597538086b2ec098988a8eeb3c10fb1e4f85c44a1b7f68a | Metal HUD present intervals | unavailable | host-not-quiet |
| halo-og | v0.5.1 | 28ee21c12c25d6cc53ecf3c9729b0a61ab155733d4e9a9990193c8b3a48060d6 | Metal HUD present intervals | unavailable | host-not-quiet |
| opence-macos | v1.0.2 | 15b78674d3a80eeb34e3be6140852b5dbff0605669ef74a8de784671cc0cee80 | Metal HUD present intervals | unavailable | host-not-quiet |
| opence-web | hosted entry document | 93277da132ad0801b993c01e1bad5b5a7c7a19caf8b24ecdd7aab971a7cbe5ba | Safari RAF callbacks (entry-document hash) | unavailable | host-not-quiet |
| visr | 8d26312ddb4ad2e97074f0c9ba42ce386241d55a | 72272775f0c12a98603b8f4b574a3c8be64842d8085da4110884fdd49451a195 | Metal HUD present intervals | unavailable | host-not-quiet |

## Mac native Metal HUD

Metal HUD per-frame intervals describe native presentation pacing. GPU duration and aggregate HUD FPS are excluded.

| Target | Scenario | Status | FPS | p50 (ms) | p95 (ms) | p99 (ms) | On time (%) | Worst (ms) | Frames | Duration (s) | Thermal | Resolution | Cap (fps) | Display (Hz) | Display rate basis |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| chupathingy | a10 | unavailable; incomplete (0/3 rounds); host-not-quiet | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |
| chupathingy | b30 | unavailable; incomplete (0/3 rounds); host-not-quiet | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |
| halo-og | a10 | unavailable; incomplete (0/3 rounds); host-not-quiet | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |
| halo-og | b30 | unavailable; incomplete (0/3 rounds); host-not-quiet | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |
| opence-macos | a10 | unavailable; incomplete (0/3 rounds); host-not-quiet | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |
| opence-macos | b30 | unavailable; incomplete (0/3 rounds); host-not-quiet | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |
| visr | a10 | unavailable; incomplete (0/3 rounds); host-not-quiet | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |
| visr | b30 | unavailable; incomplete (0/3 rounds); host-not-quiet | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |

## Safari requestAnimationFrame

Safari callbacks measure browser scheduling and may include repeated game images; they do not prove unique game frames.

| Target | Scenario | Status | FPS | p50 (ms) | p95 (ms) | p99 (ms) | On time (%) | Worst (ms) | Frames | Duration (s) | Thermal | Resolution | Cap (fps) | Display (Hz) | Display rate basis |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| opence-web | a10 | unavailable; incomplete (0/3 rounds); host-not-quiet | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |
| opence-web | b30 | unavailable; incomplete (0/3 rounds); host-not-quiet | — | — | — | — | — | — | — | — | unknown | unknown | unknown | unknown | unknown |

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
