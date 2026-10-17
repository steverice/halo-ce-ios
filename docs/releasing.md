# Releasing VISR

The **Release** workflow generates `CHANGELOG.md` with Commitizen, commits the release manifest, creates a version tag, uploads internal TestFlight builds, and publishes a GitHub release without attached assets. The generated release entry is the exact GitHub release body and the `en-US` TestFlight **What to Test** text. No game data is included.

Run releases through `workflow_dispatch` on `main`. The default `validate` mode builds unsigned provenance only. It does not create release metadata, tags, uploads, or GitHub releases. Select `release` only after reviewing that provenance and configuring the matching build policy. Do not run `cz bump` locally, edit versions, or edit `CHANGELOG.md`. Commitizen reads the version from `visr-vX.Y.Z` tags. Its parser accepts the repository's existing optional gitmoji prefixes and all documented commit types, including `perf`, `build`, `revert`, and `merge`. Features increment minor, fixes and performance changes increment patch, and breaking changes increment major. Other types appear in the changelog without forcing a version increment. Plain upstream subjects outside the conventional format do not become changelog entries. Existing upstream `v*-ios` tags do not establish a VISR release boundary.

## One-time configuration

Create a GitHub environment named `release`. Configure required reviewers, prevent self-review if appropriate, and restrict deployments to `main`. Merely naming the environment in YAML does not enable approval protection. Enable Actions read/write repository permissions and arrange for the release bot to push the generated changelog commit to `main` under the repository's branch rules. If branch protection requires PRs or checks for every bot commit, grant a narrowly scoped bot exception or adapt the preparation step to the required review process before enabling releases. The workflow fails if a push is rejected; it does not bypass branch rules.

Environment approvals authorize that dispatch's version commit, tag push, TestFlight uploads, notes updates, and GitHub publication. The preflight, preparation, and signing jobs use this environment, so GitHub may request approval before each protected stage. The unsigned probe does not receive environment credentials. Normal PR CI does not receive these credentials.

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

Use an App Store Connect API key with permission to upload builds and edit beta build localizations for this app. Limit app access where the account supports it. An Admin key is not required by this flow. `GITHUB_TOKEN` supplies repository contents access; no personal GitHub token is required if the repository permits that token to create the release commit and tag. If the repository requires a dedicated bot token, configure and review its scope before changing the workflow.

The signing helper creates a random-password temporary keychain, imports the existing distribution identity, validates profile ownership and expiration, and installs only the selected profiles. The `always()` cleanup step restores the previous keychain search list and deletes its keychain, installed profiles, `.p12`, and API key, including after failures. No signing material is cached or uploaded as an artifact. GitHub-hosted runners are discarded after the job. Do not move this workflow to persistent runners without reviewing interruption and cleanup behavior.

## Validate before releasing

Dispatch with `mode=validate`, the exact reviewed `source_sha`, and the selected platforms. The workflow checks out that source, fixes `SOURCE_DATE_EPOCH` to its commit timestamp, and builds unsigned tester apps with guest PGO, full LTO, host PGO, movies enabled, and benchmark routes disabled. Signed data packs are disabled in this probe because the guest-specific recording feed cannot be selected until the guest identity is known. Probe apps are not distribution artifacts.

Download the `release-provenance` artifact. It contains `release-probe.json` and `ios/halo_guest.elf` for independent guest load-segment audits. These validation files are not attached to releases or uploaded to TestFlight. Its JSON records the source SHA, timestamp, workflow run and attempt, exact Xcode/SDKs, actual guest compiler version, profile hashes, compile-command hash, and embedded guest identity for each platform. Checks inspect the generated guest compile command, full LTO stage, resolved host compiler flags, tester/routes definitions, and actual app guest segments. Host PGO fallback is an error. Both platforms must embed the same guest.

Configure `RELEASE_BUILD_POLICY` as a JSON object with exactly these fields:

| Field | Reviewed value |
| --- | --- |
| `guest_sha256` | Common guest SHA-256 from the probe |
| `guest_compiler_version` | Exact first compiler-version line from the probe |
| `guest_profile_sha256` | Guest profile SHA-256 from the probe |
| `host_profile_sha256` | Host profile SHA-256 from the probe |
| `profile_feed_url` | Mutable HTTPS endpoint for the signed tester profile feed |
| `profile_feed_sha256` | SHA-256 of the exact envelope reviewed for this release |
| `recording_feed_url` | Mutable HTTPS endpoint for the signed tester recording feed for this guest |
| `recording_feed_sha256` | SHA-256 of the exact envelope reviewed for this release |

Verify the recording package against this guest before publishing its feed through the separate data-pack procedure. Release configuration does not publish packs. A generic feed for an older guest does not satisfy this requirement. Changing source timestamps or compilers can change the guest even when gameplay code is unchanged; rerun validation after a source change.

In `release` mode, the workflow repeats the unsigned probe, then validates existing signing material and API access before preparation can push anything. It checks the reviewed profile/compiler/guest pins, tester channel, Ed25519 signatures, payload hashes, engine compatibility, and guest-specific recording compatibility. Feeds must match their reviewed envelope hashes and must not redirect. A digest binds the preflight source, platform selection, and build policy to preparation, so changing an environment value between approval stages stops preparation.

## Dispatch inputs

| Input | Value |
| --- | --- |
| `mode` | `validate` (default) produces unsigned provenance only; `release` enables the protected publication stages |
| `source_sha` | Reviewed, full 40-character `main` commit SHA; for recovery, use the original source SHA from `.release/current.json` |
| `platforms` | Unique comma-separated subset of `ios,tvos,visionos`; defaults to `ios,tvos` |
| `increment` | `auto`, `patch`, `minor`, or `major`; default `auto` |
| `resume_tag` | Empty for a new release; the existing `visr-vX.Y.Z` tag for recovery |
| `bootstrap_since` | First managed release only: explicitly reviewed full ancestor SHA defining the initial changelog range |
| `bootstrap_version` | First managed release only: current TestFlight marketing version; defaults to `0.1.2` and cannot be lower |

The first managed release needs an explicit `bootstrap_since` because earlier uploaded TestFlight builds were not managed by this workflow. Review that boundary against the previously distributed build. The workflow creates a temporary local SCM baseline for Commitizen and pushes only the new release tag. It does not retag, rewrite, or publish a historical version.

Review the entire initial range before approving it. A generated entry over Apple's 4,000-character limit stops preparation before any push or upload. The workflow does not summarize or truncate entries. For the first managed release, choose an accurate reviewed baseline that describes the distributed changes. Later oversized entries require an explicit release policy change; moving the boundary to hide changes is not recovery.

The release job rejects a stale source SHA, unsupported platform, altered recovery input, unrecognized managed tag, or inconsistent generated notes. A global concurrency group serializes releases. The preparation job atomically pushes the changelog/manifest commit and annotated tag, so a concurrent `main` update rejects the push together with the tag. Existing tags are never forced or deleted. The generated manifest records the source commit, version, build number, platforms, note hash, bootstrap boundary, compiler timestamp, and reviewed build policy.

## Builds and TestFlight notes

Publish the probed guest's recording feed at a mutable HTTPS endpoint, such as `https://code.steverice.org/visr/packs/tester/recordings-<guest-sha256>.json`, from a separate data branch or hosting deployment. Keep that URL stable while replacing its signed envelope with higher-sequence compatible updates. A guest-specific filename selects compatibility; it does not freeze the feed contents. Never embed a commit-pinned raw GitHub URL or another immutable snapshot URL as a runtime feed: installed apps would never see later updates.

Pin the exact envelope bytes reviewed for the release in `profile_feed_sha256` and `recording_feed_sha256`. These hashes are checked during release validation; runtime updates continue to enforce signatures, compatibility and sequence. Configure the mutable URL and its current envelope hash in `RELEASE_BUILD_POLICY` before dispatching. Committing feed data to the application source on `main` changes its timestamp and guest identity, invalidating the preceding probe. Release policy rejects recognizable full commit-pinned GitHub feed URLs; custom HTTPS hosts still require review for mutability. An already installed app's embedded URL requires a replacement app build to change.

For Pages, create a reviewed data commit directly on the probed source with exactly one changed file, `docs/site/packs/tester/recordings-<guest-sha256>.json`, and push it on a separate data branch. Dispatch the Pages workflow from the unchanged `main`, passing that commit's full SHA as `recording_commit`. The workflow keeps its main checkout, verifies the data commit's parent and sole regular-file change, validates the bounded signed envelope and native recording contract, and rejects sequence rollback against source and deployed feeds. It overlays only the JSON into the site artifact and executes no code from the data commit. After release preparation creates its metadata commit and tag, persist the identical feed file to `main` so ordinary Pages deployments retain it. This does not change deployment protection rules or the application's pinned source.


Selected platforms build serially in one job and one guest cache. Distribution commands explicitly require guest PGO, full LTO, host PGO, tester mode, routes disabled, movies enabled, render validation disabled, and the reviewed feed URLs. Each archive must match the probe guest, exact Xcode/SDK, bundle ID, and tester feed metadata before upload. The tag supplies `CFBundleShortVersionString`; the source commit's UTC timestamp supplies the stable unique `CFBundleVersion` for that release. Each numeric component omits leading zeros, matching App Store Connect's canonical form. Recovery queries also normalize component padding in older manifests while still requiring the exact app, platform, marketing version, and numeric build identity. `SOURCE_DATE_EPOCH` fixes the guest's compilation date and time across recovery runs. Each actual archived app must match the release version, build number, SDK platform, shared guest ELF load segments, and guest hash. The flow compares archived guest identities across all selected targets. A recovery build with a different toolchain or guest fails that comparison.

The existing packaging implementation uses manual signing with the supplied profile. Export disables Xcode's automatic version/build-number management and sets `testFlightInternalTestingOnly`. Upload uses the existing App Store Connect API key. Archives, exported IPAs, dSYMs, signing material, and game data are not attached to the GitHub release. The actual archived bundle identifier selects the existing App Store Connect app; the script does not assume an app ID.

The script queries builds using app, platform, marketing version, and build number, then verifies all four fields in the returned relationships. It waits up to 30 minutes per target for that exact build to become `VALID`; `FAILED`, `INVALID`, an unexpected audience, or a timeout fails the job. It never selects the latest arbitrary build. A processed build can still require export compliance or tester-group configuration in App Store Connect. This workflow does not answer those questions or change tester access.

After processing, the script creates `en-US` beta build notes, fills an existing empty `en-US` entry, or accepts identical existing text. It refuses to overwrite different existing text. Other locales remain unchanged. It reads back and checks the exact text. GitHub publication happens only after every selected target has processed and retained those same notes.

## Recovering a partial release

A draft GitHub release holds an upload recovery journal in an HTML comment while work is incomplete. The public release removes that journal and uses only the generated entry. The journal records actual archived guest and app identities, upload state, and processed build IDs. Do not edit the draft's notes or journal.

After a failure, dispatch again with the original `source_sha`, the same `platforms`, and `resume_tag` set to the existing tag. The preparation job verifies the release manifest, parent commit, main ancestry, and immutable notes. It does not bump, create another tag, or change versions. A completed target is reconciled by its exact build and notes. A target that failed before an upload started can rebuild. HTTP reads retry bounded transient errors; writes are never blindly retried.

The application stays checked out at the release tag in `GITHUB_WORKSPACE`. Publication uses a separate, clean controller worktree pinned to the dispatch's exact `GITHUB_SHA`, so a reviewed release-tool correction can recover an existing tag. The controller passes the application directory explicitly; its build script, signing helpers, profiles, guest and signed-feed validation contracts still come from the tag. Archive validation reads `VISR.xcarchive/Products/Applications/VISR.app`, since Xcode can consume the ordinary build-product symlink during archiving. Both checkouts must remain clean, and recovery never overlays updated tools into the application source.

Before starting an upload, the script persists its `uploading` checkpoint. If the upload times out or the runner disappears, recovery only polls that exact build. It does not repeat an uncertain upload or invent a new build number. If App Store Connect never receives that build, inspect the failed upload and draft journal before an operator decides how to proceed. An unresolved ambiguous upload remains a failure requiring manual reconciliation. Preserve the tag and draft; do not delete them and start another release to conceal the failure.

A release that is already published with identical notes and no attached assets is a successful no-op on recovery. A matching tag with edited notes or mismatched inputs fails.

## Runner requirements and limits

The workflow uses GitHub's `xcode-27` Apple silicon preview runner and pins `/Applications/Xcode_27.2_beta_2.app/Contents/Developer`, Xcode build `27B5028f`, and SDK 27.2. The [published inventory](https://github.com/actions/runner-images/blob/main/images/macos/xcode-27-arm64-Readme.md) lists that build and the required device SDKs. Its preview status can affect availability. The `actionlint` label registry names only that published preview label, which is newer than its bundled hosted inventory. Every build verifies the installed toolchain rather than assuming the inventory or following the runner's default Xcode.

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
- [Current Xcode 27 ARM64 runner software](https://github.com/actions/runner-images/blob/main/images/macos/xcode-27-arm64-Readme.md)
- [Apple build filtering and processing states](https://developer.apple.com/documentation/appstoreconnectapi/get-v1-builds)
- [Apple beta build localization creation](https://developer.apple.com/documentation/appstoreconnectapi/post-v1-betabuildlocalizations)
- [Apple beta build localization updates](https://developer.apple.com/documentation/appstoreconnectapi/patch-v1-betabuildlocalizations-_id_)
- [LLVM compiler timestamp support](https://reviews.llvm.org/D136717)
