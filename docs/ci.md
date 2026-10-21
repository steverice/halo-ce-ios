# Continuous integration

Pull requests and pushes to `main` run the portable tooling suite on Ubuntu
24.04 with Python 3.11 and 3.14. Shared source and build changes also compile
a complete 32-bit x86 Linux release in an Arch container, then check its ELF
header and the recorded network format. The Apple workflow runs native
regressions and builds on ARM64 `macos-26` for changes outside Markdown,
`docs/` and `research/`. These workflows can also be started manually. New
pushes cancel older runs for the same branch.

The PR jobs use read-only repository permissions and do not require signing
identities, Apple accounts, cloud containers or repository secrets. Checkout
credentials are not persisted. They do not install or launch an app on a
physical device. Branch protection and required status checks are repository
settings; adding these files does not configure them.

## Test inventory

| Existing target | Runner and dependencies | Automation and coverage |
| --- | --- | --- |
| Top-level `tools/test_*.py` unit tests for build helpers, guest embedding, bridges, Mac runner orchestration, texture policy, profiles, settings, routes, replay formats and shader lists | Ubuntu, Python, pytest, jsonschema, C compiler, OpenSSL | PR gate in `Tooling / portable`; synthetic fixtures and mocked launch paths. Does not launch a simulator, game or network broker. |
| `test_bmp_files.py`, `test_cache_file_formats.py` | Ubuntu, clang | PR gate; compiles the real parsers and tests generated malformed/valid inputs. Retail map cases keep their existing missing-asset skips. |
| `test_data_pack.py`, `test_profile_newer.py` | Ubuntu, C compiler | PR gate; compiles shared validators and tests Ed25519 with temporary test keys. No production signing keys. |
| `test_linux_port.py` | Ubuntu, clang, 32-bit libc headers, `nm` | PR gate for tooling and header/link probes. Tests requiring a configured complete build or maps retain their explicit existing skips. |
| `test_cinematic_titles.py` | Ubuntu, clang, 32-bit C runtime, pinned pytest | Targeted PR and `main` push gate for the extracted `cinematic_render` function. The cinematic harness validation branch also runs it on push. Its stereo film-bar and prompt HUD checks include negative controls; the other extracted-source modules remain outside this gate. |
| `test_death_timing.py`, `test_light_storage.py` | Ubuntu, clang, lld, x86 32-bit ELF execution | PR gate as standalone programs, including ordinary/full-LTO storage links. These programs are not pytest test functions. |
| `ci_build.py linux release` | Ubuntu 24.04, Arch container with clang/lld, 32-bit glibc and SDL3 | PR and `main` push gate for changes to shared source, port, build tools or its workflow. Compiles and links the complete native Linux game, verifies an ELF32 x86 executable, and runs the recorded network format, signed lobby, Unicode line-break, orphaned-sound, co-op queue-reset and Monitor-scaling probes. No game data or game launch. |
| `settings_schema.py --check`, `profile_check.py`, `profile_catalog_check.py` | Ubuntu, Python 3.11+ | PR gate for generated schema/reference drift and shipped profile/catalog policy. `test_profile_check.py` and `test_route_check.py` also execute their assertions during pytest collection. |
| `tools/ios_test.py` | ARM64 macOS, Xcode, Homebrew LLVM/lld, SDL3, pkgconf | Existing PR gate in `iOS / build`, run once. Guest ABI, layout, memory, audio handoff, config/profile/data pack, network, timing, stereo math, capture/replay validators, sanitizers, XISO parser and Metal compilation probes. Uses synthetic inputs, not game maps. |
| `test_menu_cancel.py`, `test_network_light_state.py`, `test_network_shader_warmup.py`, `test_shader_list_warmup.py` | macOS, `xcrun`, SDL3 where needed | Run by `ios_test.py`; deliberately excluded from the Ubuntu target because they invoke Apple's compiler driver. |
| `bink_test.py --if-present` | macOS, Xcode | Existing native gate compiles the decoder. Movie decoding is unavailable without a legally supplied movie/disc image; successful compilation does not establish decoding coverage. |
| `ios_build.py --unsigned` and `--simulator`, with iOS/tvOS/visionOS selectors | ARM64 macOS, Xcode device and simulator SDKs, Homebrew build dependencies | Existing automatic builds and artifacts for all three platforms. iOS and tvOS fail the workflow on error. visionOS retains its pre-existing advisory `continue-on-error` until a successful hosted run is reviewed and the policy is changed. |
| `ios_build.py --mac` | ARM64 macOS, Xcode Mac Catalyst SDK | Added to the iOS build job; reuses its guest build and tools. Build only, no game launch. |
| `tools/macos_test.py` | ARM64 macOS, Xcode, SDL3, pkgconf | AppKit import publication and rollback, service thread guards and event preservation, and native/mobile window flags. Runs from `ios_test.py` and `iOS / macos`; synthetic inputs, no app launch. |
| `ios_build.py --native-mac --renderer metal` | ARM64 macOS, Xcode macOS SDK | `iOS / macos` builds the AppKit host with the shared Apple source cache. Build only, no game launch. |
| `ci_build.py linux debug/release`, `test_linux_port.py`, `tools/harness`, `test_light_storage.py` | Arch container on Ubuntu, multilib clang/lld/SDL3 | Existing manual `Upstream desktop` workflow. Its debug build and full extracted-source harness remain separate from the release compile gate; see baseline failures below. |
| `ci_build.py windows debug/release`, `test_light_storage.py` | Windows 2025, LLVM, Visual Studio x86 libraries/SDK, ninja, ccache | Existing manual `Upstream desktop` workflow. Full build is currently affected by known port compile failures; it is not represented as a passing PR gate. |
| Shader translator golden comparisons, `mac_run.py`, `replay_check.py`, renderer frame comparisons, multiplayer sessions | Controlled Apple hardware, GPU-specific baselines, legally held game assets, recordings and display session | Physical/private validation. Keep outside public Actions. Tests of their parsers and orchestration are in the portable/native suites; rendered output and real synchronization are not. |
| Device benchmarks, cloud-save integration, controller/head tracking, external displays, audio output, thermal/performance measurements | Exact physical device and suitable accounts/peripherals | Attended device validation. Hosted compile/probe success does not replace these checks. |

The portable target collects only the top level of `tools/`, adding new unit
test modules automatically. It explicitly excludes the four Apple probe
modules and the two standalone programs named above. It does not recursively
collect `tools/harness`.

## Run the same checks locally

```sh
uv run --no-project --with-requirements tools/requirements-ci.txt python tools/ci_test.py
# On Linux, also exercise the standalone native linker/death probes:
uv run --no-project --with-requirements tools/requirements-ci.txt python tools/ci_test.py --linux
python3 tools/ios_test.py
```

`tools/requirements-ci.in` defines test dependencies; `requirements-ci.txt`
pins their transitive versions. Refresh deliberately with:

```sh
uv pip compile --python-version 3.11 --no-header --output-file tools/requirements-ci.txt tools/requirements-ci.in
```

The workflow validator runs pinned `actionlint` over every workflow. The
portable jobs upload JUnit reports, including their skip reasons, for seven
days. Apple builds retain their existing unsigned IPA/checksum and simulator
ZIP artifacts for fourteen days. Manual upstream artifacts expire after
three days. Missing expected artifacts fail their upload steps.

## Apple toolchain and cost

The Apple workflow selects
`/Applications/Xcode_26.6.app/Contents/Developer` explicitly and checks the
required SDK paths before building. The [published macOS 26 ARM64 runner
inventory](https://github.com/actions/runner-images/blob/main/images/macos/macos-26-arm64-Readme.md)
lists Xcode 26.6 and iOS, tvOS and visionOS 26.5 device/simulator SDKs as of
October 16, 2026. It does not promise the locally used Xcode 27.2 beta. An
image change that removes the selected version fails clearly; the workflow
does not download a beta Xcode or sign into an Apple account. Native
`ios_test.py` also needs the visionOS SDK for its stereo shader compile.
If the Metal compiler is absent, the job installs Apple's documented
`MetalToolchain` component for the selected Xcode, then verifies it again.

The existing three Apple jobs each reuse setup for their device/simulator
builds. The iOS job also shares setup with native probes and the Mac build.
Only downloaded third-party sources are cached, keyed by architecture and
the downloader definitions. Build products, game assets and signing state
are not cached. The upstream desktop workflow retains its manual trigger
and compiler caches. There is no new scheduled build or private runner.

## Known gaps and first hosted run

A full recursive `pytest tools` run has a recorded baseline of 205 failures
across 14 extracted-source harness modules. The cinematic title fixture's
missing stereo/HUD declarations were repaired and its eight focused cases
passed with a native ABI on macOS. The required 32-bit Linux run remains
unverified until this workflow runs. The other 13 modules still need individual
diagnosis. Their failures are not skipped, marked expected failures or accepted
by the new PR job. Repair those harness dependencies before making recursive
collection a required PR check. The manual upstream workflow continues to run
the full harness and can still report those failures.

The first published run must verify both Ubuntu Python versions, the
standalone ELF probes, the Arch container's 32-bit headers and SDL3, the
complete Linux release link and ELF check, Xcode/SDK availability, all seven
Apple builds and their artifact paths. Local validation cannot prove GitHub
image behavior.
Review the visionOS result before proposing removal of its existing
advisory status. Do not treat a skipped missing-map/movie test as a measured
pass or infer real game rendering from a build.

Never upload game maps, disc images, extracted textures/movies, gameplay
captures or private recordings to public CI artifacts. Public jobs produce
synthetic fixtures and app binaries without game data. Signing, App Store
uploads and publication belong to a separately approved release workflow.
