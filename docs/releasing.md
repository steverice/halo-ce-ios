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

The signing helper creates a random-password temporary keychain, imports the existing distribution identity, validates profile ownership and expiration, and installs only the selected profiles. When `benchmark_upload` is `on`, each selected profile must also allow CloudKit, the configured container, and production `aps-environment`; a missing capability stops the job before signing. The release build keeps iCloud saves off. The `always()` cleanup step restores the previous keychain search list and deletes its keychain, installed profiles, and `.p12`, including after failures. No signing material is cached or uploaded as an artifact. GitHub-hosted runners are discarded after the job. Do not move this workflow to persistent runners without reviewing interruption and cleanup behavior.

## Validate before releasing

Dispatch with `mode=validate`, the exact reviewed `source_sha`, and the selected platforms. The workflow checks out that source, fixes `SOURCE_DATE_EPOCH` to its commit timestamp, and builds unsigned tester apps with guest PGO, full LTO, host PGO, movies enabled, and benchmark routes disabled. Signed data packs are disabled in this probe because the guest-specific recording feed cannot be selected until the guest identity is known. Probe apps are not distribution artifacts.

Download the `release-provenance` artifact. It contains `release-probe.json` and `ios/halo_guest.elf` for independent guest load-segment audits. These validation files are not attached to releases or uploaded to TestFlight. Its JSON records the source SHA, timestamp, workflow run and attempt, exact Xcode/SDKs, actual guest compiler version, profile hashes, compile-command hash, and embedded guest identity for each platform. Checks inspect the generated guest compile command, full LTO stage, resolved host compiler flags, tester/routes definitions, and actual app guest segments. Host PGO fallback is an error. Both platforms must embed the same guest.

Configure `RELEASE_BUILD_POLICY` as a JSON object with the eight required fields below. For new policies, you may add the optional `benchmark_upload` field. Older policies that omit it keep uploads off.

| Field | Reviewed value |
| --- | --- |
| `guest_sha256` | Common guest SHA-256 from the probe |
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

New release policies require device-keyed profile pins and a shader feed pin. For example, `profile_feed_sha256s` maps `ios/iPhone18,1/_` to that shard's envelope hash. The release verifier downloads every pinned shard and checks its exact target-bound compatibility; the app requests only its own shard. Shader verification checks `shader-lists-1` and complete map-local descriptor dependencies. See [signed data updates](data-packs.md) for publication and resource limits.

With `benchmark_upload=on`, the tester's existing consent screen can send a compressed benchmark report to the app's CloudKit public database. The same policy is bound to the unsigned probe, release manifest, and release build. Signing requires an existing manual App Store profile for the exact CloudKit container and production APS environment. The release never enables cloud saves or changes account capabilities.

Verify the recording package against this guest before publishing its feed through the separate data-pack procedure. Release configuration does not publish packs. A generic feed for an older guest does not satisfy this requirement. Changing source timestamps or compilers can change the guest even when gameplay code is unchanged; rerun validation after a source change.

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

Publish the probed guest's recording feed at a mutable HTTPS endpoint, such as `https://code.steverice.org/visr/packs/tester/recordings-<guest-sha256>.json`, from a separate data branch or hosting deployment. Keep that URL stable while replacing its signed envelope with higher-sequence compatible updates. A guest-specific filename selects compatibility; it does not freeze the feed contents. Never embed a commit-pinned raw GitHub URL or another immutable snapshot URL as a runtime feed: installed apps would never see later updates.

Pin the exact envelope bytes reviewed for the release in `profile_feed_sha256s`, `shader_feed_sha256`, and `recording_feed_sha256`. These hashes are checked during release validation; runtime updates continue to enforce signatures, compatibility and sequence. Configure the mutable URL and its current envelope hash in `RELEASE_BUILD_POLICY` before dispatching. Committing feed data to the application source on `main` changes its timestamp and guest identity, invalidating the preceding probe. Release policy rejects recognizable full commit-pinned GitHub feed URLs; custom HTTPS hosts still require review for mutability. An already installed app's embedded URL requires a replacement app build to change.

For Pages, create a reviewed data commit directly on the probed source with exactly one changed file: the existing mutable `docs/site/packs/tester/recordings.json` path or a guest-specific `recordings-<guest-sha256>.json` path. A mutable feed must use signed `benchmark-catalog-1:<guest-sha256>` compatibility; the overlay derives its guest from the verified manifest. Source and deployed old feeds must validate, and rollback checks apply only within the same compatibility domain. The guest-specific path remains supported. Push the data commit on a separate data branch, then dispatch the Pages workflow from unchanged `main` with that commit's full SHA as `recording_commit`. The workflow verifies the data commit's parent and sole regular-file change, validates the bounded signed envelope and native recording contract, overlays only the JSON into the site artifact, and executes no code from the data commit. After release preparation creates its metadata commit and tag, persist the identical feed file to `main` so ordinary Pages deployments retain it. This does not change deployment protection rules or the application's pinned source.


Selected platforms build serially in one job and one guest cache. Distribution commands explicitly require guest PGO, full LTO, host PGO, tester mode, routes disabled, movies enabled, render validation disabled, and the reviewed feed URLs. Each archive must match the probe guest, exact Xcode/SDK, bundle ID, and tester feed metadata before upload. The tag supplies `CFBundleShortVersionString`; the source commit's UTC timestamp supplies the stable unique `CFBundleVersion` for that release. Each numeric component omits leading zeros, matching App Store Connect's canonical form. Recovery queries also normalize component padding in older manifests while still requiring the exact app, platform, marketing version, and numeric build identity. `SOURCE_DATE_EPOCH` fixes the guest's compilation date and time across recovery runs. Each actual archived app must match the release version, build number, SDK platform, shared guest ELF load segments, and guest hash. The flow compares archived guest identities across all selected targets. A recovery build with a different toolchain or guest fails that comparison.

The existing packaging implementation uses manual signing with the supplied profile. When benchmark uploads are enabled, validation checks the CloudKit service, exact container, and production APS environment in both the signed archive and exported IPA. Export disables Xcode's automatic version/build-number management and sets `testFlightInternalTestingOnly`. Upload uses the existing App Store Connect API key. Archives, exported IPAs, dSYMs, signing material, and game data are not attached to the GitHub release. The actual archived bundle identifier selects the existing App Store Connect app; the script does not assume an app ID.

The script queries builds using app, platform, marketing version, and build number, then verifies all four fields in the returned relationships. It waits up to 30 minutes per target for that exact build to become `VALID`; `FAILED`, `INVALID`, an unexpected audience, or a timeout fails the job. It never selects the latest arbitrary build. A processed build can still require export compliance or tester-group configuration in App Store Connect. This workflow does not answer those questions or change tester access.

After processing, the script creates `en-US` beta build notes from the manifest summary, fills an existing empty `en-US` entry, or accepts identical existing text. It refuses to overwrite different existing text. Other locales remain unchanged. It reads back and checks the exact summary. GitHub publication happens only after every selected target has processed and retained those same notes; the GitHub release body remains the full entry with summary and details.

## Recovering a partial release

A draft GitHub release holds an upload recovery journal in an HTML comment while work is incomplete. The public release removes that journal and uses only the generated entry. The journal records actual archived guest and app identities, upload state, and processed build IDs. Do not edit the draft's notes or journal.

After a failure, dispatch again with the original `source_sha`, the same `platforms`, and `resume_tag` set to the existing tag. The preparation job verifies the release manifest, parent commit, main ancestry, and immutable notes and summary. It does not call the summary API, bump, create another tag, or change versions. A completed target is reconciled by its exact build and notes. A target that failed before an upload started can rebuild. HTTP reads retry bounded transient errors; writes are never blindly retried.

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
- [Anthropic Messages API](https://platform.claude.com/docs/en/api/messages)
- [Claude Sonnet 5.5](https://platform.claude.com/docs/en/models/sonnet-5-5/overview)
- [Current Xcode 27 ARM64 runner software](https://github.com/actions/runner-images/blob/main/images/macos/xcode-27-arm64-Readme.md)
- [Apple build filtering and processing states](https://developer.apple.com/documentation/appstoreconnectapi/get-v1-builds)
- [Apple beta build localization creation](https://developer.apple.com/documentation/appstoreconnectapi/post-v1-betabuildlocalizations)
- [Apple beta build localization updates](https://developer.apple.com/documentation/appstoreconnectapi/patch-v1-betabuildlocalizations-_id_)
- [LLVM compiler timestamp support](https://reviews.llvm.org/D136717)
