# Signed data updates

VISR keeps bundled device profiles, shader lists, and benchmark data. Signed updates are selected at launch and checked asynchronously for the next launch. Downloads cannot change a running session. An interrupted first launch quarantines its trial snapshot; the next launch uses the previous healthy generation or bundled data. A completed menu or world frame marks a trial healthy. Loading frames do not.

Device and simulator builds use the project's pinned Ed25519 public key and the `tester` channel with `--tester`, or `stable` otherwise. Mac runner builds download nothing unless a key or feed is explicitly named. Disable updates with `--data-packs off`.

```sh
python3 tools/ios_build.py --unsigned --tester \
  --data-pack-public-key PUBLIC_KEY_HEX \
  --profile-feed-url https://example.com/packs/tester/profiles \
  --recording-feed-url https://example.com/packs/tester/recordings.json \
  --shader-feed-url https://example.com/packs/tester/shaders.json
```

Every envelope uses schema 1. Its signed manifest binds the kind, channel, sequence, compatibility identifier, engine range, payload hash, and notes. The payload contains named base64 files. The app rejects redirects, malformed or duplicate JSON keys, invalid signatures, unexpected names, incompatible manifests, repeated sequences, and oversized downloads. Each kind and device compatibility key has separate cached state. Feed failures leave the current data in place and appear in `ios-runtime.log`.

## Device profile shards

`--profile-feed-url` names an HTTPS directory. The app requests only its own `<platform>/<model>/<chip-key>.json`, without downloading a global catalog or profile pack. For example:

```text
ios/iPhone18,1/_.json
mac/Mac14,5/Apple-M2-Max.json
```

Platforms are `ios`, `tvos`, `visionos`, and `mac`. Model identifiers preserve their case and contain only bounded ASCII letters, digits, commas, periods, and hyphens. A Mac key includes its Apple M-series chip and optional Pro, Max, or Ultra tier; spaces become hyphens. Other platforms use `_`. Invalid hardware identification and unavailable shards retain bundled profiles.

A shard's signed kind is `profiles`; its compatibility is `device-profiles-1:PLATFORM/MODEL/CHIP-KEY`. The publisher and app construct and validate keys through the same native code. A shard for a different device cannot activate in this device's cache.

Create one envelope from the repository profile sources:

```sh
python3 tools/data_pack.py --kind profiles --channel tester \
  --sequence 1 --compatibility device-profiles-1:ios/iPhone18,1/_ \
  --input profiles --signing-key /private/path/signing-seed \
  --output iphone-profile.json
```

Serve that envelope under `profiles/ios/iPhone18,1/_.json`. Each shard contains the target platform's class partitions, selected model, and only the hardware catalog records and source profiles needed for conservative inheritance. Runtime memory, GPU, pixel, and refresh checks still decide whether an inherited profile is suitable. User overrides keep their existing precedence. Debug profile selection uses the bundled profile set.

An optional `capacity.toml` reserves schema 1 with optional `validated_maximum_players` from 2 through 128 and an `evidence` string of at most 2048 bytes. A supplied maximum requires evidence. Omit unknown capacity. This metadata does not change profile matching or display a warning.

## Additive shader lists

A shader envelope uses kind `shaders` and compatibility `shader-lists-1`. This identifier describes the shader descriptor ABI independently of the replay guest hash. Its files are map-specific `.txt` lists, with complete vertex and pixel dependencies for every pipeline in that map. Prepare additions from correctly attributed captures; do not assign mixed UI or multiplayer pipelines to another map.

```sh
python3 tools/data_pack.py --kind shaders --channel tester \
  --sequence 1 --compatibility shader-lists-1 \
  --input /private/path/validated-shader-additions \
  --signing-key /private/path/signing-seed --output shaders.json
```

The native validator checks descriptor shapes and ranges, 252-byte pixel keys and their FNV-1a hashes, vertex variants and packed masks, attribute formats, four-bit border masks, legacy alpha defaults, and map-local pipeline dependencies. Conflicting shader definitions are rejected. Each snapshot allows at most 32 maps, 4 MiB per file, 8 MiB total decoded files, and a 16 MiB envelope; each map permits at most 8192 descriptors. The bundled and downloaded union permits at most 16384 descriptors per map.

At launch the host freezes a canonical, deduplicated union of bundled records and downloaded additions. Bundled lists always remain. Existing local shader captures are appended by the game as before. Invalid or missing additions fall back to bundled lists.

## Recording catalogs

Recording snapshots contain `benchmark-catalog.json` and complete `.input`, `.actions`, `.seeds`, and `.route` sets, with optional `.checks`. The catalog supplies case order, map, measurement window, replay references, and multiplayer parameters. Use the exact compatibility identifier in the destination app's report, `data_updates.recordings.compatibility`:

```sh
python3 tools/data_pack.py --kind recordings --channel tester \
  --sequence 1 --compatibility benchmark-catalog-1:GUEST_SHA256 \
  --input /private/path/validated-recordings \
  --signing-key /private/path/signing-seed --output recordings.json
```

A running benchmark keeps a copied, hashed recording snapshot. Resume uses those same bytes. A changed catalog, profile, guest hash, or baseline setting starts a fresh session. Format validation does not establish replay fidelity; verify actual movement or combat during the measurement window.

## Publication

Keep the random 32-byte signing seed outside the repository and distribution artifacts. `tools/data_pack.py` signs locally and does not publish. Serve each envelope directly over HTTPS after review. Publish a higher sequence to restore older payload bytes; clients reject repeated and decreasing sequences. Sign separately for each channel. Changing the pinned public key requires an app update.

Release policy pins every reviewed profile shard's exact envelope hash, plus the shader and recording envelope hashes. The publication map is used by release verification; clients do not download it. The checked-in `docs/site/packs/tester/` tree in `main` is the source of truth for the served signed feeds. Keep its signed envelopes, `publication.json` hash receipt, and `publication-format.txt` marker together in the same commit. The Pages workflow validates every checked-in path, hash, signature, sequence, and size before copying the site into its deployment artifact. It does not fetch live feeds or accept asset commits from another ref. Signed profile and shader data updates do not require an app build while their compatibility identifiers and pinned verification key remain unchanged. Publishing feeds and uploading a build remain separate actions. See [release publication](releasing.md#builds-and-testflight-notes).

Reports identify the profile, shader, and recording generations. Per-run `settings.upscaler_requested` records the requested scaler, and `upscaler` records observed scene/final scaling paths and counts. `metalfx` means the renderer encoded that path; an unavailable MetalFX request that uses bilinear scaling is reported as `bilinear`. Equal render and output dimensions yield `none`; unobserved or varying paths yield `unknown` or `mixed`. These are renderer observations, not proof that a physical display showed each frame.

Each run also stores `graphics.first` and `graphics.last`: complete typed `display.*` configuration snapshots resolved by the game after device profiles, chosen values, environment overrides, and scene/external context. A small explicit set of performance debug flags accompanies them. Configuration is captured at measurement boundaries and on resolver or context changes, rather than serialized every frame. `graphics.changed` marks observed value/context changes; unavailable or rejected captures are explicit. Paths, account settings, network settings, and player data are excluded. Configuration describes requested choices; the separate upscaler, render-size, and output observations describe the renderer's behavior.
