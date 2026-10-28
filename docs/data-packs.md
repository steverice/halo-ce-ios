# Signed data updates

VISR keeps bundled device profiles, shader lists, and benchmark data. Signed updates are checked in the background. The launch menu keeps Play available while profiles and benchmark recordings are verified and downloaded. Benchmark stays visible with a progress indicator until its profile and recordings are ready; a failed check displays a short reason and retries when the network changes or the menu reappears. Valid cached data can make Benchmark available during a refresh. The menu freezes its selected data when starting Play or Benchmark, so later downloads apply on the next launch. Shader downloads do not gate either choice. Downloads cannot change a running session. An interrupted first launch quarantines its trial snapshot; the next launch uses the previous healthy generation or bundled data. A completed menu or world frame marks a trial healthy. Loading frames do not.

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

A shard's signed kind is `profiles`; its compatibility is `device-profiles-N:PLATFORM/MODEL/CHIP-KEY`, where `N` is the app's profile feed generation. The publisher and app construct and validate keys through the same native code. A shard for a different device or generation cannot activate in this device's cache.

Create one envelope from the repository profile sources:

```sh
python3 tools/data_pack.py --kind profiles --channel tester \
  --sequence 1 --compatibility device-profiles-2:ios/iPhone18,1/_ \
  --input profiles --signing-key /private/path/signing-seed \
  --output iphone-profile.json
```

Serve that envelope under `device-profiles-2/ios/iPhone18,1/_.json`. Each shard contains the target platform's class partitions, selected model, only the hardware catalog records and source profiles needed for conservative inheritance, and the profile set's revision. Runtime memory, GPU, pixel, and refresh checks still decide whether an inherited profile is suitable. User overrides keep their existing precedence. Debug profile selection uses the bundled profile set.

An optional `capacity.toml` reserves schema 1 with optional `validated_maximum_players` from 2 through 128 and an `evidence` string of at most 2048 bytes. A supplied maximum requires evidence. Omit unknown capacity. This metadata does not change profile matching or display a warning.

### Profile feed generations

A downloaded shard replaces all of the app's bundled profiles for that launch, and each app's validator refuses a whole shard that contains a key, range, choice or file it does not know. Apps that validate differently therefore read separate directories:

| Generation | Directory | Apps |
| --- | --- | --- |
| `device-profiles-1` | `profiles/` | 0.2.2 to 0.5.1 |
| `device-profiles-2` | `device-profiles-2/` | 0.6.0 and later |

The generation is `HOST_PROFILE_FEED_GENERATION` in `port/ios/host/host_profile.h`, and `PROFILE_FEED_GENERATION` in `tools/data_pack.py`. Bump it, and give the new generation its own directory, whenever an app would accept a shard that an earlier app of the same generation refuses: a profile key added or removed, a range or choice changed, a new heading, catalog field or file. `tools/test_profile_feed.py` fails when `port/linux/src/port_profile_keys.h` changes without a bump. Bump it also when a release needs profile updates of its own, separate from earlier releases. A device that has no shard in its generation's directory uses its bundled profiles; it never falls back to an older generation.

Before signing a device shard, `tools/data_pack.py` builds the profile validator of every release tag that reads the shard's generation and refuses the shard if any of them would. The `profiles/` directory is frozen: its shards stay as they were published, and a `device-profiles-1` shard signed from current sources is refused, because 0.5.0 and earlier refuse `network.player_capacity`. To update an older generation, sign from its release tag's sources.

### Profile revisions

`profiles/hardware/revision.toml` holds the profile set's revision and the SHA-256 of the sources it was stamped from: every `profiles/*.toml` except `TEMPLATE.toml`, and `hardware/catalog.toml`. After changing any of them, run:

```sh
python3 tools/profile_revision.py --bump
```

`python3 tools/profile_revision.py` checks that the file matches the sources; `tools/ios_test.py`, release validation and the publisher run the check. Two branches that both bump edit the same lines, so merging them conflicts until the revision is bumped again; each revision on `main` names one set of sources. Releases and shards come from `main`, so their revisions are comparable.

The app bundles the file and the publisher copies it into every generation-2 shard. The app uses a shard only when its revision is higher than the bundled one, or equal with the same source hash; otherwise it uses its bundled profiles. This applies to a downloaded shard and to one already cached, so an app update with newer profiles is never masked by a shard signed before them. A served shard older than the bundled profiles counts as published for the launch menu, as an absent shard does, and the benchmark uses the bundled profiles. Such a shard is downloaded again at each check until a newer one is published. A bundle without a valid revision uses no downloaded profiles.

To roll a profile back, revert it on `main`, bump the revision and sign again: an app that took a newer revision refuses a lower one, and Pages staging refuses to replace a shard with one of a lower revision.

## Additive shader lists

A shader feed uses kind `shaders` and compatibility `shader-lists-2`, independently of the replay guest hash. The source and bundled map-specific `.txt` lists stay plain text. The publisher emits a small signed index and compressed per-map downloads, with complete vertex and pixel dependencies for every pipeline in that map. Prepare additions from correctly attributed captures; do not assign mixed UI or multiplayer pipelines to another map.

```sh
python3 tools/data_pack.py --kind shaders --channel tester \
  --sequence 1 --compatibility shader-lists-2 \
  --input /private/path/validated-shader-additions \
  --signing-key /private/path/signing-seed --output shaders.json
```

The native validator checks descriptor shapes and ranges, 252-byte pixel keys and their FNV-1a hashes, vertex variants and packed masks, attribute formats, four-bit border masks, legacy alpha defaults, and map-local pipeline dependencies. Conflicting shader definitions are rejected. Each snapshot allows at most 32 maps, 4 MiB per decoded file, 16 MiB total decoded files, and a 64 KiB index envelope. Compressed downloads allow at most 4 MiB per map and 16 MiB total. Each map permits at most 8192 descriptors. The bundled and downloaded union permits at most 16384 descriptors per map.

Apps released with the previous 8 MiB aggregate limit reject larger snapshots and keep their bundled shader lists.

The index retains the signed envelope's manifest, signature, and payload fields. Its payload is `{"files":{"a10.txt":"RAW_SHA256:RAW_BYTES:COMPRESSED_SHA256:COMPRESSED_BYTES"}}`. Hashes use lowercase SHA-256 and sizes use positive decimal integers without leading zeros. A map downloads from `shader-maps/COMPRESSED_SHA256.zlib` alongside the feed. The zlib stream must end exactly at its declared raw length, without trailing bytes. The signed index authenticates both representations. Asset URLs cannot select another host or directory.

The publisher writes compressed assets before atomically replacing its local index and leaves existing asset files in that output directory. Generate each Pages publication in a fresh staging directory and include exactly the indexed assets and updated publication receipt in the same commit. A Pages deployment replaces the site snapshot; a client using a previous index may briefly receive a missing-asset response. That failure keeps the healthy snapshot and retries the current feed on the next check. Local publisher write ordering does not guarantee asset retention across deployments. Unchanged maps keep the same content address. Clients recheck retained raw files against the index and download only missing or changed maps. Corrupt retained files are downloaded again. All downloads run in the existing background update check; map loading and frame rendering do not fetch data. Partial downloads never activate. A complete set must pass the same native dependency validator before it becomes a pending snapshot for the next launch.

Offline publication tools can still validate `shader-lists-1` envelopes. Clients using the per-map format require a `shader-lists-2` feed at the existing shader URL.

At launch the host freezes a canonical, deduplicated union of bundled records and downloaded additions. Bundled lists always remain. Existing local shader captures are appended by the game as before. Invalid or missing additions fall back to bundled lists.

## Recording catalogs

Recording snapshots contain `benchmark-catalog.json` and complete `.input`, `.actions`, `.seeds`, and `.route` sets, with optional `.checks`. The catalog supplies case order, map, measurement window, replay references, and multiplayer parameters. Use the exact compatibility identifier in the destination app's report, `data_updates.recordings.compatibility`:

```sh
python3 tools/data_pack.py --kind recordings --channel tester \
  --sequence 1 --compatibility benchmark-catalog-1:GUEST_SHA256 \
  --input /private/path/validated-recordings \
  --signing-key /private/path/signing-seed --output recordings.json
```

The tester site serves one feed per release, `recordings-GUEST_SHA256.json`, for 0.6.0 and later, plus the plain `recordings.json` that development builds read by default. Feeds for earlier releases are retired (`RETIRED_FEEDS` in `tools/pages_asset_overlay.py` removes a retired feed from the live site, which otherwise keeps every asset the repository lacks). A 0.5.x build whose feed is gone gets a 404, shows "Benchmark recordings are unavailable" and rechecks with backoff only while its menu is showing.

A running benchmark keeps a copied, hashed recording snapshot. Resume uses those same bytes. A changed catalog, profile, guest hash, or baseline setting starts a fresh session. Format validation does not establish replay fidelity; verify actual movement or combat during the measurement window.

## Latest release record

A feed of kind `latest` (compatibility `latest-release-1`) tells a build which VISR version is newest on its platform. It sits beside the recording feed, at `latest.json` in the same directory, and the app derives its URL from the recording feed URL. The envelope holds one file, `latest-release.json`, at most 4 KiB:

```json
{"schema": 1, "platforms": {"ios": "0.6.0", "tvos": "0.6.0", "visionos": "0.6.0", "macos": "0.6.0"}}
```

A platform may be absent. Versions are plain `x.y.z`. The record is two levels deep because the pack JSON readers refuse deeper nesting.

The launch menu checks it with the recording check (one conditional request) and keeps the last valid record. When the recording feed answers 404 for this build, no cached catalog exists, and the record names a newer version for this platform, Benchmark stays off and the menu says "Benchmarking is no longer supported on this version. Update to VISR X.Y.Z." Without a newer record, or with a cached catalog, nothing changes. `host_data_updates_latest_newer()` returns the newer version for a future update note.

After a release can be installed, prepare and sign the record:

```sh
python3 tools/latest_release.py --version 0.6.0 --platforms ios,tvos,macos --output latest-release-input
python3 tools/data_pack.py --kind latest --channel tester --sequence SEQUENCE \
  --compatibility latest-release-1 --input latest-release-input \
  --signing-key /private/path/signing-seed --output latest.json
```

`latest_release.py` reads the live record, sets the released platforms and never lowers a version. Use the Unix time as the sequence. Commit the envelope as `docs/site/packs/tester/latest.json` with the updated `publication.json`; release policy does not pin it. The release job prints the first command after publishing.

## Publication

Keep the random 32-byte signing seed outside the repository and distribution artifacts. `tools/data_pack.py` signs locally and does not publish. Serve each envelope directly over HTTPS after review. Publish a higher sequence to restore older shader or recording payload bytes; clients reject repeated and decreasing sequences. Device profiles roll back through a new revision instead ("Profile revisions"). Sign separately for each channel. Changing the pinned public key requires an app update.

Release policy pins every reviewed profile shard's exact envelope hash, plus the shader and recording envelope hashes. The publication map is used by release verification; clients do not download it. The checked-in `docs/site/packs/tester/` tree in `main` is the source of truth for the served signed feeds. Keep its signed envelopes, `publication.json` hash receipt, and `publication-format.txt` marker together in the same commit. The Pages workflow validates every checked-in path, hash, signature, sequence, and size before copying the site into its deployment artifact. It does not fetch live feeds or accept asset commits from another ref. Signed profile and shader data updates do not require an app build while their compatibility identifiers and pinned verification key remain unchanged. Publishing feeds and uploading a build remain separate actions. See [release publication](releasing.md#builds-and-testflight-notes).

Reports identify the profile, shader, and recording generations. Per-run `settings.upscaler_requested` records the requested scaler, and `upscaler` records observed scene/final scaling paths and counts. `metalfx` means the renderer encoded that path; an unavailable MetalFX request that uses bilinear scaling is reported as `bilinear`. Equal render and output dimensions yield `none`; unobserved or varying paths yield `unknown` or `mixed`. These are renderer observations, not proof that a physical display showed each frame.

Each run also stores `graphics.first` and `graphics.last`. Their `values` contain typed `display.*` configuration resolved by the game after device profiles, chosen values, environment overrides, and scene/external context. A small explicit set of performance debug flags accompanies them. Snapshot schema 2 adds `applied_overrides` for the benchmark's transient render lines, shadow size, anisotropy, and model detail, separate from persistent configuration. `renderer_state` records a shadow target size only after a GPU target was obtained, the maximum anisotropy placed in a mipmapped sampler, and the model detail policy in force. An unobserved target or sampler is `null`.

`renderer_state.texture_replacement_selections` counts successful selections of a loaded replacement texture during measured windows. A positive count identifies the debug replacement directory as the source; zero means no selection was observed in those windows, not that the device has no assets or that none were loaded earlier. The production upscaled-texture cache currently has no renderer load path, so `upscaled_texture_assets` reports `unavailable` even when `display.upscaled_textures` is true. Configuration is captured at measurement boundaries and on resolver or context changes; the final snapshot also records renderer observations made during measurement. `graphics.changed` marks changes to persistent values, scene/output context, or applied overrides. New renderer observations alone do not set it. Paths, account settings, network settings, and player data are excluded. The separate upscaler, render-size, and output fields remain the evidence for their respective rendering paths.

## Shader observation curation

`tools/shader_observations.py` validates gzip `ShaderObservation` exports before placing dependency-complete lists in a private review catalog. Supply `--asset`, `--metadata`, the approved `--guest-sha256` and `--output`. The metadata export contains `recordName`, `payloadSHA256`, `descriptorCount` and the fields duplicated from the asset; CloudKit creator metadata is not needed. Schema 1 uses descriptor ABI `shader-lists-2` and a shader-list identity of `shader-lists-2:SEQUENCE:SHA256` (`bundled` replaces the sequence for bundled lists).

The tool bounds compressed and decoded bytes, rejects duplicate JSON keys, verifies record and payload hashes, compares duplicated fields and runs the publisher's native admission validator. It rejects incomplete pipelines and unrelated dependencies. Dependencies accompany a contributed miss; they do not count as separate misses. Staging metadata records only the full map fingerprint, map key, descriptor ABI, renderer and SHA-256 hashes of contributed miss lines and the staged list. Rejected records leave a sanitized reason in `quarantine/`.

The default stock catalog comes from the native attribution table of verified retail fingerprints. `--official` can supply a private JSON object mapping independently verified official full fingerprints to stock aliases. A client-supplied alias alone never proves a stock map. Verified stock files go to `stock/`; other maps go to `community/` under `m_` plus the first 60 fingerprint hex digits. Full fingerprints stay in metadata, and conflicting prefixes are quarantined. Keep catalogs for different descriptor ABIs separate. Run one curation process per catalog.

Review stock candidates before running `tools/shader_lists.py merge` on the approved stock folder. Keep community files in their separate reviewed catalog. Do not automatically promote benchmark dependency candidates or merge the entire community catalog into the bundle. Signed `shader-lists-2` publication still permits at most 32 maps and 16 MiB total; select a bounded subset from the private catalog. Larger community distribution requires a future feed revision.
