# Releasing VISR

The **Release** workflow generates `CHANGELOG.md` with Commitizen, summarizes the new entry, commits the changelog and release manifest, creates a version tag, uploads internal TestFlight builds, and publishes a GitHub release without attached assets. The changelog and GitHub release contain the summary followed by the complete generated entry. The manifest records the full-entry hash and the exact summary text, hash, and model. TestFlight **What to Test** uses the pinned summary. No game data is included.

Run releases through `workflow_dispatch` on `main`. The default `validate` mode builds unsigned provenance only. It does not create release metadata, tags, uploads, or GitHub releases. The `auth` mode checks Claude workload identity only; it does not build, generate a summary, create tags, or publish. Select `release` only after reviewing provenance and configuring the matching build policy. Do not run `cz bump` locally, edit versions, or edit `CHANGELOG.md`. Commitizen reads the version from `visr-vX.Y.Z` tags. Its parser accepts the repository's existing optional gitmoji prefixes and all documented commit types, including `perf`, `build`, `revert`, and `merge`. Features increment minor, fixes and performance changes increment patch, and breaking changes increment major. Other types appear in the changelog without forcing a version increment. Plain upstream subjects outside the conventional format do not become changelog entries. Existing upstream `v*-ios` tags do not establish a VISR release boundary.

## One-time configuration

Create a GitHub environment named `release`. Configure required reviewers, prevent self-review if appropriate, and restrict deployments to `main`. Merely naming the environment in YAML does not enable approval protection. Enable Actions read/write repository permissions and arrange for the release bot to push the generated changelog commit to `main` under the repository's branch rules. If branch protection requires PRs or checks for every bot commit, grant a narrowly scoped bot exception or adapt the preparation step to the required review process before enabling releases. The workflow fails if a push is rejected; it does not bypass branch rules.

Environment approvals authorize that dispatch's version commit, tag push, TestFlight uploads, notes updates, and GitHub publication. The unsigned probe, preflight, preparation, and signing jobs use this environment, so GitHub may request approval before each protected stage. The probe receives the nonsecret release policy variable but does not receive signing credentials or request an Anthropic token. Normal PR CI does not receive release credentials.

Set these environment values. Supply only preexisting credentials and profiles; the workflow does not register devices, create certificates or profiles, enable capabilities, create App Store records, invite testers, or submit external beta/App Store review.

| Name | Kind | Content |
| --- | --- | --- |
| `APPLE_BUNDLE_ID` | Variable | Existing app bundle identifier shared by the selected platform records |
| `RELEASE_BUILD_POLICY` | Variable | Reviewed JSON pins from the unsigned provenance and the compatible signed tester feeds; see below |
| `APPLE_TEAM_ID` | Secret | Team that owns the existing distribution identity and app |
| `APPLE_DISTRIBUTION_P12_BASE64` | Secret | Base64 Apple Distribution certificate with its private key |
| `APPLE_DISTRIBUTION_P12_PASSWORD` | Secret | Password of that `.p12` export |
| `APPLE_PROFILE_IOS_BASE64` | Secret | Base64 existing App Store distribution provisioning profile for iOS |
| `APPLE_PROFILE_TVOS_BASE64` | Secret | Corresponding existing tvOS profile, when selected |
| `APPLE_PROFILE_VISIONOS_BASE64` | Secret | Corresponding existing visionOS profile, when selected |
| `ASC_KEY_ID` | Secret | Existing App Store Connect team API key ID |
| `ASC_ISSUER_ID` | Secret | Issuer ID of that key |
| `ASC_PRIVATE_KEY` | Secret | Complete PEM `.p8` private key for that key |
| `ANTHROPIC_FEDERATION_RULE_ID` | Variable | Existing Claude WIF rule ID with a workspace scope |
| `ANTHROPIC_ORGANIZATION_ID` | Variable | Anthropic organization ID for that rule |
| `ANTHROPIC_SERVICE_ACCOUNT_ID` | Variable | Existing Claude service account ID for that rule |
| `ANTHROPIC_WORKSPACE_ID` | Variable | Existing Claude workspace ID for that rule |
| `RELEASE_SUMMARY_MODEL` | Variable (optional) | Anthropic Claude model; defaults to `claude-sonnet-5-5` |

The Claude federation rule must trust only the release environment on `main` for this workflow: subject `repo:steverice/visr:environment:release`, repository `steverice/visr`, ref `refs/heads/main`, and workflow `steverice/visr/.github/workflows/release.yml@refs/heads/main`. Use GitHub's OIDC issuer and a workspace scope: `workspace:inference` is the narrower option, and `workspace:developer` also permits the Messages API. The release code accepts only those two scopes and reports the actual returned scope; it rejects `org:admin` and unknown scopes. Store only the four nonsecret IDs above as release-environment variables. The `auth` mode checks one token exchange without making a Messages API inference request. The `prepare` job requests a fresh exchange only for a new release summary; recovery reuses the pinned summary and does not request a token. The assertion and access token stay in process memory, and request or response bodies are never logged. The existing `ANTHROPIC_API_KEY` secret is no longer read; leave it unused until the hosted identity check and a protected release run verify the new path. If token exchange or summary generation fails, preparation stops before committing release metadata or creating a release tag. Configure `RELEASE_SUMMARY_MODEL` only when selecting another model that supports the same Messages API adaptive-thinking and low-effort request.

Use an App Store Connect API key with permission to upload builds and edit beta build localizations for this app. Limit app access where the account supports it. An Admin key is not required by this flow. `GITHUB_TOKEN` supplies repository contents access; no personal GitHub token is required if the repository permits that token to create the release commit and tag. If the repository requires a dedicated bot token, configure and review its scope before changing the workflow.

The signing helper creates a random-password temporary keychain, imports the existing distribution identity, validates profile ownership and expiration, and installs only the selected profiles. When `benchmark_upload` or `cloud_saves` is `on`, each selected profile must also allow CloudKit, the configured container, and production `aps-environment`; a missing capability stops the job before signing. Cloud saves stay inactive until the player enables iCloud Saves in settings. The `always()` cleanup step restores the previous keychain search list and deletes its keychain, installed profiles, and `.p12`, including after failures. No signing material is cached or uploaded as an artifact. GitHub-hosted runners are discarded after the job. Do not move this workflow to persistent runners without reviewing interruption and cleanup behavior.

## Validate before releasing

Dispatch with `mode=validate`, the exact reviewed `source_sha`, and the selected platforms. The workflow checks out that source, records its actual commit timestamp as `source_date_epoch`, and builds unsigned tester apps with guest PGO, full LTO, host PGO, movies enabled, and benchmark routes disabled. Signed data packs are disabled in this probe because the guest-specific recording feed cannot be selected until the guest identity is known. Probe apps are not distribution artifacts.

Download the `release-provenance` artifact. It contains `release-probe.json` and `ios/halo_guest.elf` for independent guest load-segment audits. These validation files are not attached to releases or uploaded to TestFlight. Its JSON records the source SHA, timestamp, workflow run and attempt, exact Xcode/SDKs, actual guest compiler version, profile hashes, compile-command hash, and embedded guest identity for each platform. Checks inspect the generated guest compile command, full LTO stage, resolved host compiler flags, tester/routes definitions, and actual app guest segments. Host PGO fallback is an error. Both platforms must embed the same guest.

Configure `RELEASE_BUILD_POLICY` as a JSON object with the required fields below. For new policies, you may add the optional `benchmark_upload` and `cloud_saves` fields. Older policies that omit them keep both features off.

| Field | Reviewed value |
| --- | --- |
| `guest_sha256` | Common guest SHA-256 from the probe |
| `guest_source_date_epoch` | Explicit guest compile epoch in canonical decimal seconds, 1..253402300799 |
| `guest_compiler_version` | Exact first compiler-version line from the probe |
| `guest_profile_sha256` | Guest profile SHA-256 from the probe |
| `host_profile_sha256` | Host profile SHA-256 from the probe |
| `profile_feed_url` | Mutable HTTPS base directory for device-keyed signed tester profiles |
| `profile_feed_sha256s` | Object mapping each canonical `platform/model/chip-key` to the SHA-256 of its reviewed envelope |
| `shader_feed_url` | Mutable HTTPS endpoint for signed additive shader lists |
| `shader_feed_sha256` | SHA-256 of the reviewed shader envelope |
| `recording_feed_url` | Mutable HTTPS endpoint for the signed tester recording feed for this guest |
| `recording_feed_sha256` | SHA-256 of the exact envelope reviewed for this release |
| `benchmark_upload` | Optional string, either `on` or `off`; omitted means `off` |
| `cloud_saves` | Optional string, either `on` or `off`; omitted means `off` |
| `recording_feed_required` | Optional string, either `on` or `off`; omitted means `on`. With `off`, omit `recording_feed_sha256`: release validation skips the recording feed's envelope pin and guest compatibility check, and the summary receives no catalog facts. `recording_feed_url` stays required and is still built into the app, so recordings published later reach installed builds |

New releases also require `guest_source_date_epoch`. The immutable manifest records this separately from the actual source commit timestamp and build number. Only Apple guest compile/LTO commands receive the pinned reproducibility epoch, so Ninja detects an epoch change and rebuilds stale bitcode. Host builds retain the actual source epoch. A data-only main commit can therefore preserve an already qualified guest, but the successor validation must still prove its actual ELF, compiler and profile hashes. The optional workflow `guest_source_date_epoch` input is accepted only in unsigned `validate` mode to test a candidate without changing production policy. Release preflight requires report and policy epochs to agree. Old immutable manifests lacking both the guest field and policy pin use their own recorded source epoch; ambient time never replaces it.

New release policies require device-keyed profile pins and a shader feed pin. For example, `profile_feed_sha256s` maps `ios/iPhone18,1/_` to that shard's envelope hash. The release verifier downloads every pinned shard and checks its exact target-bound compatibility; the app requests only its own shard. Shader verification checks `shader-lists-1` and complete map-local descriptor dependencies. See [signed data updates](data-packs.md) for publication and resource limits.

With `cloud_saves=on`, the unsigned probe and release build include `--cloud-saves --cloud-container iCloud.org.steverice.visr`. Validation rejects a compiled feature that differs from the reviewed policy. The feature is off in player settings by default on iPhone, iPad, Vision Pro, Apple TV, and Mac; enabling it takes effect on the next launch. Before inviting testers to enable it, deploy the `SaveSlot` schema to Production in CloudKit Console. The native AppKit development target also supports `--native-mac --cloud-saves --cloud-container iCloud.org.steverice.visr --team YOUR_TEAM_ID`; it needs a valid signed Mac profile and does not support App Store packaging yet. Use `--unsigned` in place of `--team` for a compile-only check that does not request provisioning.

With `benchmark_upload=on`, finishing the tester benchmark automatically queues and uploads its compressed report to the app's CloudKit public database. The same policy is bound to the unsigned probe, release manifest, and release build. Signing requires an existing manual App Store profile for the exact CloudKit container and production APS environment. The release does not change account capabilities.

Verify the recording package against this guest before publishing its feed through the separate data-pack procedure. Release configuration does not publish packs. A generic feed for an older guest does not satisfy this requirement. Changing guest compilation inputs, compilers or the pinned guest epoch can change the guest. Revalidate the actual successor source before reusing a qualified guest; a timestamp or manifest edit alone does not establish compatibility.

In `release` mode, the workflow repeats the unsigned probe, then validates existing signing material and API access before preparation can push anything. It checks the reviewed profile/compiler/guest pins, tester channel, Ed25519 signatures, payload hashes, engine compatibility, and guest-specific recording compatibility. Feeds must match their reviewed envelope hashes and must not redirect. A digest binds the preflight source, platform selection, and build policy to preparation, so changing an environment value between approval stages stops preparation.

## Dispatch inputs

| Input | Value |
| --- | --- |
| `mode` | `validate` (default) produces unsigned provenance only; `auth` checks Claude WIF without building or publishing; `release` enables the protected release stages |
| `source_sha` | Reviewed, full 40-character `main` commit SHA for `validate` and `release`; not needed for `auth`; for recovery, use the original source SHA from `.release/current.json` |
| `platforms` | Unique comma-separated subset of `ios,tvos,visionos`; defaults to `ios,tvos` |
| `increment` | `auto`, `patch`, `minor`, or `major`; default `auto` |
| `resume_tag` | Empty for a new release; the existing `visr-vX.Y.Z` tag for recovery |
| `bootstrap_since` | First managed release only: explicitly reviewed full ancestor SHA defining the initial changelog range |
| `bootstrap_version` | First managed release only: current TestFlight marketing version; defaults to `0.1.2` and cannot be lower |

The first managed release needs an explicit `bootstrap_since` because earlier uploaded TestFlight builds were not managed by this workflow. Review that boundary against the previously distributed build. The workflow creates a temporary local SCM baseline for Commitizen and pushes only the new release tag. It does not retag, rewrite, or publish a historical version.

Review the entire initial range before approving it. The full generated entry remains in `CHANGELOG.md` and the GitHub release; a separate model-generated summary is placed at the top of the entry and used as TestFlight notes. The summary receives only case IDs, kinds, maps, and multiplayer player counts from the exact signed, guest-compatible recording feed already verified during preflight. Older changelog prototypes cannot override those current catalog facts. Summaries must fit Apple's 4,000-character limit; the workflow rejects oversized output instead of truncating it. The full changelog entry and summary text and hashes are pinned in the manifest, so recovery reuses the original summary without another API call. Older manifests without summary fields remain recoverable by using their full entry as TestFlight notes only when it fits Apple's limit. For the first managed release, choose an accurate reviewed baseline that describes the distributed changes. Moving the boundary to hide changes is not recovery.

The release job rejects a stale source SHA, unsupported platform, altered recovery input, unrecognized managed tag, or inconsistent generated notes. A global concurrency group serializes releases. The preparation job atomically pushes the changelog/manifest commit and annotated tag, so a concurrent `main` update rejects the push together with the tag. Existing tags are never forced or deleted. The generated manifest records the source commit, version, build number, platforms, note hash, bootstrap boundary, compiler timestamp, and reviewed build policy.

## Builds and TestFlight notes

Publish the probed guest's recording feed at the existing mutable `https://code.steverice.org/visr/packs/tester/recordings.json` endpoint from a separate data branch or hosting deployment. Keep that URL stable while replacing its signed envelope. Same-domain updates need a higher sequence; a new guest starts a new signed compatibility domain. Never embed a commit-pinned raw GitHub URL or another immutable snapshot URL as a runtime feed: installed apps would never see later updates.

Pin the exact envelope bytes reviewed for the release in `profile_feed_sha256s`, `shader_feed_sha256`, and `recording_feed_sha256`. These hashes are checked during release validation; runtime updates continue to enforce signatures, compatibility and sequence. Configure the mutable URL and its current envelope hash in `RELEASE_BUILD_POLICY` before dispatching. Committing feed data to the application source on `main` changes its timestamp and guest identity, invalidating the preceding probe. Release policy rejects recognizable full commit-pinned GitHub feed URLs; custom HTTPS hosts still require review for mutability. An already installed app's embedded URL requires a replacement app build to change.

For Pages, create a reviewed data commit directly on the frozen application source. It may change only regular, nonexecutable signed feeds under `docs/site/packs/tester/`: `recordings.json`, `shaders.json`, and `profiles/<platform>/<model>/<chip-key>.json`. Push it on a separate data branch, then dispatch Pages from unchanged `main` with its full SHA as `asset_commit`. The workflow verifies the exact parent, allowed paths, signatures, path-bound kind and compatibility, native payload contracts and same-domain sequence increases. It executes no code from the data commit. No feed data is committed to application `main` to perform this publication.

Every Pages deployment, including ordinary source deployments, preserves previously deployed signed asset feeds. The publisher reads `packs/tester/publication.json`, fetches its bounded allowed paths, verifies the exact envelope hashes and signatures, and stages the complete result before replacing the site artifact. Invalid receipts, unavailable referenced assets, altered envelopes and rollback attempts stop the deployment. The initial deployment without a receipt preserves the existing signed `recordings.json`; a generated `publication-format.txt` marker distinguishes that initial transition. Later deployments require both valid metadata files; either missing or malformed stops publication. Profile, shader and recording feeds can be updated independently. The generated receipt contains only paths and hashes. The app downloads neither publisher metadata file. Normal source deployments retain the live mutable compatibility domain; only an explicitly reviewed data candidate can change it.

The older `recording_commit` input still accepts exactly one recording-feed file directly on the workflow source, including an existing guest-specific recording path. It keeps its original validation and gains the same preservation of other deployed signed assets. Do not provide both inputs. These steps do not change deployment protection rules or the application's frozen source.

Selected platforms build serially in one job and one guest cache. Distribution commands explicitly require guest PGO, full LTO, host PGO, tester mode, routes disabled, movies enabled, render validation disabled, and the reviewed feed URLs. Each archive must match the probe guest, exact Xcode/SDK, bundle ID, and tester feed metadata before upload. The tag supplies `CFBundleShortVersionString`; the source commit's UTC timestamp supplies the stable unique `CFBundleVersion` for that release. Each numeric component omits leading zeros, matching App Store Connect's canonical form. Recovery queries also normalize component padding in older manifests while still requiring the exact app, platform, marketing version, and numeric build identity. `SOURCE_DATE_EPOCH` fixes the guest's compilation date and time across recovery runs. Each actual archived app must match the release version, build number, SDK platform, shared guest ELF load segments, and guest hash. The flow compares archived guest identities across all selected targets. A recovery build with a different toolchain or guest fails that comparison.

The existing packaging implementation uses manual signing with the supplied profile. When benchmark uploads or cloud saves are enabled, validation checks the CloudKit service, exact container, and production APS environment in both the signed archive and exported IPA. Export disables Xcode's automatic version/build-number management and sets `testFlightInternalTestingOnly`. Upload uses the existing App Store Connect API key. Archives, exported IPAs, dSYMs, signing material, and game data are not attached to the GitHub release. The actual archived bundle identifier selects the existing App Store Connect app; the script does not assume an app ID.

The script queries builds using app, platform, marketing version, and build number, then verifies all four fields in the returned relationships. It waits up to 30 minutes per target for that exact build to become `VALID`; `FAILED`, `INVALID`, an unexpected audience, or a timeout fails the job. It never selects the latest arbitrary build. The archive and exported IPA must contain the reviewed encryption exemption described below. A processed build can still require tester-group configuration in App Store Connect; the workflow does not change tester access.

After processing, the script creates `en-US` beta build notes from the manifest summary, fills an existing empty `en-US` entry, or accepts identical existing text. It refuses to overwrite different existing text. Other locales remain unchanged. It reads back and checks the exact summary. GitHub publication happens only after every selected target has processed and retained those same notes; the GitHub release body remains the full entry with summary and details.

## Recovering a partial release

A draft GitHub release holds an upload recovery journal in an HTML comment while work is incomplete. The public release removes that journal and uses only the generated entry. The journal records actual archived guest and app identities, upload state, and processed build IDs. Do not edit the draft's notes or journal.

After a failure, dispatch again with the original `source_sha`, the same `platforms`, and `resume_tag` set to the existing tag. The preparation job verifies the release manifest, parent commit, main ancestry, and immutable notes and summary. It does not call the summary API, bump, create another tag, or change versions. A completed target is reconciled by its exact build and notes. A target that failed before an upload started can rebuild. HTTP reads retry bounded transient errors; writes are never blindly retried.

The application stays checked out at the release tag in `GITHUB_WORKSPACE`. Publication uses a separate, clean controller worktree pinned to the dispatch's exact `GITHUB_SHA`, so a reviewed release-tool correction can recover an existing tag. The controller passes the application directory explicitly; its build script, signing helpers, profiles, guest and signed-feed validation contracts still come from the tag. Archive validation reads `VISR.xcarchive/Products/Applications/VISR.app`, since Xcode can consume the ordinary build-product symlink during archiving. Both checkouts must remain clean, and recovery never overlays updated tools into the application source.

Before starting an upload, the script persists its `uploading` checkpoint. If the upload times out or the runner disappears, recovery only polls that exact build. It does not repeat an uncertain upload or invent a new build number. If App Store Connect never receives that build, inspect the failed upload and draft journal before an operator decides how to proceed. An unresolved ambiguous upload remains a failure requiring manual reconciliation. Preserve the tag and draft; do not delete them and start another release to conceal the failure.

A release that is already published with identical notes and no attached assets is a successful no-op on recovery. A matching tag with edited notes or mismatched inputs fails.

## Runner requirements and limits

The workflow uses GitHub's `xcode-27` Apple silicon preview runner and selects the installed Xcode whose build is listed in `tools/xcode_builds.py` (the 27.2 RC `27B48`, or the 27.2 betas `27B5038j` and `27B5028f`; the runner fleet serves images with each, and App Store Connect accepts uploads only from the RC) with `tools/xcode_select.py`, which logs every installed Xcode, and requires SDK 27.2. The [published inventory](https://github.com/actions/runner-images/blob/main/images/macos/xcode-27-arm64-Readme.md) lists that build and the required device SDKs. Its preview status can affect availability. The `actionlint` label registry names only that published preview label, which is newer than its bundled hosted inventory. Every build verifies the installed toolchain rather than assuming the inventory or following the runner's default Xcode.

Homebrew can update the guest compiler independently. The unsigned probe records its actual version; the distribution build must match the reviewed compiler and guest identity. An image update, missing SDK, unreadable PGO profile, or differing guest stops the flow. SDK 27 includes the external-display scene accessory path. Successful unsigned validation does not establish Apple's current beta SDK acceptance or signing/upload access; the protected release still must complete those checks.

The native Mac/Catalyst app is not a supported TestFlight release target in this workflow. Its App Store packaging, entitlements, metadata, and signing require separate work.

## Local verification

These checks use fixtures and mocked HTTP calls; they do not release, tag, push, upload, or query an account:

```sh
uv run --python 3.11 --with pytest --with-requirements tools/release-requirements.txt python -m pytest tools/test_release.py tools/test_release_validation.py tools/test_ios_build.py
uv run --python 3.11 --with ruff ruff check tools/release.py tools/release_signing.py tools/release_validation.py tools/test_release.py tools/test_release_validation.py
uv run --python 3.11 --with ruff ruff format --check tools/release.py tools/release_signing.py tools/release_validation.py tools/test_release.py tools/test_release_validation.py
```

The workflow and signing path still require a protected, approved CI run with real credentials to establish end-to-end publishing success. Local tests do not validate current certificate/profile access, Apple's processing service, or actual hosted-runner signing.

## References

- [Commitizen configuration customization](https://commitizen-tools.github.io/commitizen/customization/config_file/)
- [Commitizen SCM version provider](https://commitizen-tools.github.io/commitizen/config/version_provider/)
- [GitHub signing credentials on macOS runners](https://docs.github.com/en/actions/how-tos/deploy/deploy-to-third-party-platforms/sign-xcode-applications)
- [GitHub release API](https://docs.github.com/en/rest/releases/releases)
- [Anthropic Messages API](https://platform.claude.com/docs/en/api/messages)
- [Claude Sonnet 5.5](https://platform.claude.com/docs/en/models/sonnet-5-5/overview)
- [Current Xcode 27 ARM64 runner software](https://github.com/actions/runner-images/blob/main/images/macos/xcode-27-arm64-Readme.md)
- [Apple build filtering and processing states](https://developer.apple.com/documentation/appstoreconnectapi/get-v1-builds)
- [Apple beta build localization creation](https://developer.apple.com/documentation/appstoreconnectapi/post-v1-betabuildlocalizations)
- [Apple beta build localization updates](https://developer.apple.com/documentation/appstoreconnectapi/patch-v1-betabuildlocalizations-_id_)
- [LLVM compiler timestamp support](https://reviews.llvm.org/D136717)


## Encryption declaration

All four Apple app plist templates set the Boolean `ITSAppUsesNonExemptEncryption` to `false`. This declares that the current app is exempt from providing encryption documentation; it does not claim that the app contains no cryptography. The release controller checks the actual archive and exported IPA for that exact Boolean before upload and rejects an unexpected `ITSEncryptionExportComplianceCode`.

The current declaration assumes standard encryption and no distribution in France. [Apple's documentation table](https://developer.apple.com/help/app-store-connect/reference/app-information/export-compliance-documentation-for-encryption/) requires a French declaration for standard algorithms outside Apple's operating system only when distributing in France. [Apple's submission instructions](https://developer.apple.com/help/app-store-connect/manage-app-information/determine-and-upload-app-encryption-documentation/) direct apps exempt from providing documentation to record that result in `Info.plist`. The [key reference](https://developer.apple.com/documentation/bundleresources/information-property-list/itsappusesnonexemptencryption) defines `false` for no encryption or exempt encryption, including linked third-party libraries. No Apple approval code is needed for this no-documentation route.

The Apple builds include app-provided encryption: `port/linux/src/p2p_crypto.c` uses ChaCha20-Poly1305 and X25519 for private multiplayer signaling and tunnel packets; Monocypher seals password-protected join tokens. `port/ios/CMakeLists.txt` also links Mbed TLS and `posix_dtls.c` for WebRTC DTLS. These are separate from the Ed25519 signatures and SHA-256 hashes used to authenticate downloaded data packs and from `NSURLSession` HTTPS, which uses Apple's networking implementation. The declaration does not rely on treating all cryptography as OS-provided or authentication-only.

Reassess the declaration before adding proprietary encryption, changing cryptographic functionality or dependencies, or distributing in France. If documentation becomes required, submit it through App Store Connect and use the [Apple-issued export compliance code](https://developer.apple.com/documentation/bundleresources/information-property-list/itsencryptionexportcompliancecode) with `ITSAppUsesNonExemptEncryption=true`; update the release checks as part of that reviewed change. Do not invent an approval code. This configuration does not change App Store territories, certify an existing uploaded build, or authorize an upload.
