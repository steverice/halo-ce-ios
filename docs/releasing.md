# Releasing VISR

The **Release** workflow generates `CHANGELOG.md` with Commitizen, commits the release manifest, creates a version tag, uploads internal TestFlight builds, and publishes a GitHub release without attached assets. The generated release entry is the exact GitHub release body and the `en-US` TestFlight **What to Test** text. No game data is included.

Run releases through `workflow_dispatch` on `main`. Do not run `cz bump` locally, edit versions, or edit `CHANGELOG.md`. Commitizen reads the version from `visr-vX.Y.Z` tags. Its parser accepts the repository's existing optional gitmoji prefixes and all documented commit types, including `perf`, `build`, `revert`, and `merge`. Features increment minor, fixes and performance changes increment patch, and breaking changes increment major. Other types appear in the changelog without forcing a version increment. Plain upstream subjects outside the conventional format do not become changelog entries. Existing upstream `v*-ios` tags do not establish a VISR release boundary.

## One-time configuration

Create a GitHub environment named `release`. Configure required reviewers, prevent self-review if appropriate, and restrict deployments to `main`. Merely naming the environment in YAML does not enable approval protection. Enable Actions read/write repository permissions and arrange for the release bot to push the generated changelog commit to `main` under the repository's branch rules. If branch protection requires PRs or checks for every bot commit, grant a narrowly scoped bot exception or adapt the preparation step to the required review process before enabling releases. The workflow fails if a push is rejected; it does not bypass branch rules.

Environment approvals authorize that dispatch's version commit, tag push, TestFlight uploads, notes updates, and GitHub publication. Both jobs use this environment, so GitHub may request approval again before the signing job. Normal PR CI does not receive these credentials.

Set these environment values. Supply only preexisting credentials and profiles; the workflow does not register devices, create certificates or profiles, enable capabilities, create App Store records, invite testers, or submit external beta/App Store review.

| Name | Kind | Content |
| --- | --- | --- |
| `APPLE_BUNDLE_ID` | Variable | Existing app bundle identifier shared by the selected platform records |
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

## Dispatch inputs

| Input | Value |
| --- | --- |
| `source_sha` | Reviewed, full 40-character `main` commit SHA; for recovery, use the original source SHA from `.release/current.json` |
| `platforms` | Unique comma-separated subset of `ios,tvos,visionos`; defaults to `ios,tvos` |
| `increment` | `auto`, `patch`, `minor`, or `major`; default `auto` |
| `resume_tag` | Empty for a new release; the existing `visr-vX.Y.Z` tag for recovery |
| `bootstrap_since` | First managed release only: explicitly reviewed full ancestor SHA defining the initial changelog range |
| `bootstrap_version` | First managed release only: current TestFlight marketing version; defaults to `0.1.2` and cannot be lower |

The first managed release needs an explicit `bootstrap_since` because earlier uploaded TestFlight builds were not managed by this workflow. Review that boundary against the previously distributed build. The workflow creates a temporary local SCM baseline for Commitizen and pushes only the new release tag. It does not retag, rewrite, or publish a historical version.

Review the entire initial range before approving it. A generated entry over Apple's 4,000-character limit stops preparation before any push or upload. The workflow does not summarize or truncate entries. For the first managed release, choose an accurate reviewed baseline that describes the distributed changes. Later oversized entries require an explicit release policy change; moving the boundary to hide changes is not recovery.

The release job rejects a stale source SHA, unsupported platform, altered recovery input, unrecognized managed tag, or inconsistent generated notes. A global concurrency group serializes releases. The preparation job atomically pushes the changelog/manifest commit and annotated tag, so a concurrent `main` update rejects the push together with the tag. Existing tags are never forced or deleted. The generated manifest records the source commit, version, build number, platforms, note hash, bootstrap boundary, and compiler timestamp.

## Builds and TestFlight notes

Selected platforms build serially in one job and one guest cache. The tag supplies `CFBundleShortVersionString`; the source commit's UTC timestamp supplies the stable unique `CFBundleVersion` for that release. `SOURCE_DATE_EPOCH` fixes the guest's compilation date and time across recovery runs. Each actual archived app must match the release version, build number, SDK platform, shared guest ELF load segments, and guest hash. The flow compares archived guest identities across all selected targets. A recovery build with a different toolchain or guest fails that comparison.

The existing packaging implementation uses manual signing with the supplied profile. Export disables Xcode's automatic version/build-number management and sets `testFlightInternalTestingOnly`. Upload uses the existing App Store Connect API key. Archives, exported IPAs, dSYMs, signing material, and game data are not attached to the GitHub release. The actual archived bundle identifier selects the existing App Store Connect app; the script does not assume an app ID.

The script queries builds using app, platform, marketing version, and build number, then verifies all four fields in the returned relationships. It waits up to 30 minutes per target for that exact build to become `VALID`; `FAILED`, `INVALID`, an unexpected audience, or a timeout fails the job. It never selects the latest arbitrary build. A processed build can still require export compliance or tester-group configuration in App Store Connect. This workflow does not answer those questions or change tester access.

After processing, the script creates `en-US` beta build notes, fills an existing empty `en-US` entry, or accepts identical existing text. It refuses to overwrite different existing text. Other locales remain unchanged. It reads back and checks the exact text. GitHub publication happens only after every selected target has processed and retained those same notes.

## Recovering a partial release

A draft GitHub release holds an upload recovery journal in an HTML comment while work is incomplete. The public release removes that journal and uses only the generated entry. The journal records actual archived guest and app identities, upload state, and processed build IDs. Do not edit the draft's notes or journal.

After a failure, dispatch again with the original `source_sha`, the same `platforms`, and `resume_tag` set to the existing tag. The preparation job verifies the release manifest, parent commit, main ancestry, and immutable notes. It does not bump, create another tag, or change versions. A completed target is reconciled by its exact build and notes. A target that failed before an upload started can rebuild. HTTP reads retry bounded transient errors; writes are never blindly retried.

Before starting an upload, the script persists its `uploading` checkpoint. If the upload times out or the runner disappears, recovery only polls that exact build. It does not repeat an uncertain upload or invent a new build number. If App Store Connect never receives that build, inspect the failed upload and draft journal before an operator decides how to proceed. An unresolved ambiguous upload remains a failure requiring manual reconciliation. Preserve the tag and draft; do not delete them and start another release to conceal the failure.

A release that is already published with identical notes and no attached assets is a successful no-op on recovery. A matching tag with edited notes or mismatched inputs fails.

## Runner requirements and limits

The workflow pins the macOS 26 Apple silicon hosted runner and Xcode 26.6. The runner must contain the iOS, tvOS, and visionOS SDKs for the selected platforms; only select visionOS after its App Store metadata, profile, and SDK are available. A hosted image update can change the compiler or installed SDKs, and recovery rejects a differing guest identity. Record the toolchain in the job log. Xcode 26 builds do not include APIs guarded for SDK 27, including the current external-display scene accessory path. A release requiring those features needs a reviewed runner/Xcode change once an eligible SDK is available. Beta SDK acceptance by TestFlight is a separate Apple policy and should be checked before changing Xcode.

The native Mac/Catalyst app is not a supported TestFlight release target in this workflow. Its App Store packaging, entitlements, metadata, and signing require separate work.

## Local verification

These checks use fixtures and mocked HTTP calls; they do not release, tag, push, upload, or query an account:

```sh
uv run --python 3.11 --with pytest python -m pytest tools/test_release.py tools/test_ios_build.py
uv run --python 3.11 --with ruff ruff check tools/release.py tools/release_signing.py tools/test_release.py
uv run --python 3.11 --with ruff ruff format --check tools/release.py tools/release_signing.py tools/test_release.py
```

The workflow and signing path still require a protected, approved CI run with real credentials to establish end-to-end publishing success. Local tests do not validate current certificate/profile access, Apple's processing service, or actual hosted-runner signing.

## References

- [Commitizen configuration customization](https://commitizen-tools.github.io/commitizen/customization/config_file/)
- [Commitizen SCM version provider](https://commitizen-tools.github.io/commitizen/config/version_provider/)
- [GitHub signing credentials on macOS runners](https://docs.github.com/en/actions/how-tos/deploy/deploy-to-third-party-platforms/sign-xcode-applications)
- [GitHub release API](https://docs.github.com/en/rest/releases/releases)
- [Current macOS 26 ARM64 runner software](https://github.com/actions/runner-images/blob/main/images/macos/macos-26-arm64-Readme.md)
- [Apple build filtering and processing states](https://developer.apple.com/documentation/appstoreconnectapi/get-v1-builds)
- [Apple beta build localization creation](https://developer.apple.com/documentation/appstoreconnectapi/post-v1-betabuildlocalizations)
- [Apple beta build localization updates](https://developer.apple.com/documentation/appstoreconnectapi/patch-v1-betabuildlocalizations-_id_)
- [LLVM compiler timestamp support](https://reviews.llvm.org/D136717)
