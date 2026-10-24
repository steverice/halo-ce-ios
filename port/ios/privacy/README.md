# Privacy manifests

Builds made with shader uploads enabled automatically send missing shader descriptions and device model information to improve loading. Builds from source do not send these reports unless built with `--shader-upload`. Reports exclude gameplay and personal file names. Apple's upload service attaches an account-specific identifier.

The app bundle carries one `PrivacyInfo.xcprivacy`, chosen by upload capability:

- `plain/`: builds without benchmark or shader uploads declare no collected data.
- `shader/`: shader-only upload builds declare Product Interaction and Other Diagnostic Data linked to the user, for Analytics, without tracking.
- `tester/`: benchmark-capable builds declare Performance Data, Product Interaction and Other Diagnostic Data linked to the user, for Analytics, without tracking.

Map fingerprints reveal which maps were loaded. CloudKit attaches a creator user record ID even though the report payload omits account information.

Shader upload capability is independent of benchmark uploads. Local builds default to neither. Builds with shader uploads enabled collect and deliver reports automatically. There is no player preference to disable them. Scripted runs, runners and replays suppress uploads. Previously uploaded records remain subject to server retention. Local sent hashes remain to prevent repeat contributions; bounded retention of these hashes can permit resends after eviction.

Shader reports contain canonical missing compilation descriptors and their supporting dependencies, a full map-file fingerprint and format, renderer, coarse device model and platform, app version and build, exact guest code hash, descriptor ABI and selected shader-list identity. A random per-batch nonce separates otherwise identical reports. Reports exclude map display names, file paths, usernames, account IDs, session IDs, timestamps, gameplay, settings, shader source and game assets. The nonce identifies an immutable batch, not a player or session.

Raw CloudKit records must be purged after 30 days. Offline curation retains sanitized descriptors and full fingerprints, strips CloudKit creator metadata and quarantines rejected records. Publication requires review and signing; public writes are untrusted.

All three manifests declare the same required-reason APIs, which the game and its libraries use in every build: file timestamps of files inside the app's own container (`C617.1`), elapsed time measured inside the app (`35F9.1`), free disk space checked before writing (`E174.1`) and the app's own user defaults (`CA92.1`).
