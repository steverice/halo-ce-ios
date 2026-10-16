# Updating profiles and benchmark recordings

An installed app can fetch signed data snapshots without replacing its binary. Profiles and recordings have separate feeds and sequence numbers. Updates are checked in the background on every launch, with a short timeout and no retry; a failed check only logs one line and leaves the current snapshot in use, and an unchanged feed answers a conditional request with no download. Downloads are staged after validation and selected at the next launch. A failed launch returns to the last healthy snapshot or the bundled data. A snapshot becomes healthy after a main-menu frame is presented.

Device and simulator builds download from the project's feeds on its site, `https://code.steverice.org/visr/packs/CHANNEL/profiles-2.json` and `.../recordings.json`, and verify against the project's pinned Ed25519 public key (`DATA_PACK_PUBLIC_KEY` in `tools/ios_build.py`). `--tester` selects the `tester` channel; other builds select `stable`. Mac builds (`--mac`, the runner) download nothing unless a key or feed is named. A build of your own can name its own key and feeds, or turn downloads off:

```sh
python3 tools/ios_build.py --unsigned --tester \
  --data-pack-public-key PUBLIC_KEY_HEX \
  --profile-feed-url https://example.com/tester/profiles-2.json \
  --recording-feed-url https://example.com/tester/recordings.json

python3 tools/ios_build.py --unsigned --data-packs off
```

The channel, pack kind and compatibility identifier must match the signed manifest. Redirects are rejected, so each URL must serve the envelope directly. The updater does not upload benchmark reports.

## Publishing a snapshot

Keep a randomly generated 32-byte Ed25519 signing seed in a private file with owner-only permissions. The public key is safe to embed in the app; the seed must stay outside the repository and distribution artifacts. `tools/data_pack.py` signs locally and does not publish anything.

```sh
python3 tools/data_pack.py --kind profiles --channel tester \
  --sequence 1 --compatibility profiles-2 --input profiles \
  --signing-key /private/path/signing-seed --output profiles-2.json
```

The `profiles-2` compatibility identifier admits external-display sections; the publisher refuses external sections in a `profiles-1` snapshot. Apps fetch `profiles-2.json` and refuse `profiles-1` snapshots, and the project's feeds serve only `profiles-2`: apps built before it keep their bundled profiles.

Each profile snapshot contains the complete profile set and `hardware/catalog.toml`. The publisher and app reject incomplete class coverage, overlapping exact device identities, invalid catalog references, unknown settings and out-of-range graphics values. User overrides keep their existing precedence.

Recording snapshots contain complete `.input`, `.actions`, `.seeds` and `.route` sets, with optional `.checks`. Use the compatibility identifier from the destination app's benchmark report, `data_updates.recordings.compatibility` (the guest image hash is also `app.image`):

```sh
python3 tools/data_pack.py --kind recordings --channel tester \
  --sequence 1 --compatibility benchmark-replay-1:GUEST_SHA256 \
  --input /private/path/validated-recordings \
  --signing-key /private/path/signing-seed --output recordings.json
```

The exact guest-code hash is required. A recording header alone does not identify replay semantics: engine changes can assign meaning to previously unused action bits without changing that header. Format validation establishes that the data is readable; replay verification must also establish that the route and measurement window work on the target engine and devices.

Serve each resulting envelope at its configured HTTPS URL after testing it: for the project's feeds, `docs/site/packs/CHANNEL/profiles-2.json` or `recordings.json`, which the Pages workflow publishes when `main` changes under `docs/site/`. Publish a higher sequence to roll back to older payload bytes; clients reject repeated and decreasing sequences. Promote the same validated snapshot to `stable` by signing it for that channel. Keep the signing seed backed up securely—changing a pinned public key requires another app build.

## Benchmark sessions

A running benchmark uses a copied, hashed recording snapshot. An unfinished session resumes with those same bytes even if another recording update becomes active. A changed profile snapshot, guest hash or benchmark settings starts a fresh session. Reports identify the profile pack and the recording pack actually used by the session.

Downloaded data never contains executable code, console commands, saved games or game assets. A feed cannot update replay engine code or add new configuration keys; those changes require an app build.
