# AGENTS.md

Instructions for coding agents working in this repository. The versions for
people are [README.md](README.md) and [CONTRIBUTING.md](CONTRIBUTING.md).

VISR is a native port of Halo: Combat Evolved to Apple Vision Pro, iPhone,
iPad, Apple TV and the Mac, built from the Halo: CE decompilation and rendered
with Metal. The upstream Linux and Windows ports still build from this tree.

## Layout

- `source/`: the decompiled game, from the upstream decompilation projects.
  Port changes here are marked with a `port:` comment.
- `port/runtime/`: the portable runtime for the game's 32-bit pointers on ARM64.
- `port/ios/`: the Apple app (UIKit, input, audio, the loader, the disc
  importer, the Metal renderer, the visionOS scenes). `port/ios/README.md` is
  the build and install guide and explains how the 32-bit arena works.
- `port/linux/`: the platform layer, the OpenGL renderer and the Xbox
  compatibility layer shared with the upstream desktop ports.
- `tools/`: build, packaging and test scripts. `tools/ios_build.py` builds
  every Apple target; `tools/ios_test.py` runs the regressions;
  `tools/mac_run.py` runs a build on an Apple silicon Mac and saves
  screenshots; `tools/ci_build.py` builds the upstream Linux and Windows ports.
- `docs/`: design notes and the project site (`docs/site`).
- `ROADMAP.md`: shipped and planned features, with the `config.toml` setting
  behind each.

## Build

An Apple silicon Mac with Xcode, Python 3 and Homebrew:

```sh
brew install cmake ninja llvm lld sdl3 pkgconf

python3 tools/ios_build.py --simulator                      # iOS simulator, no signing
python3 tools/ios_build.py --visionos --simulator           # visionOS simulator
python3 tools/ios_build.py --tvos --unsigned --ipa dist/VISR-tvOS-unsigned.ipa
python3 tools/ios_build.py --mac                            # the Mac app
python3 tools/ios_build.py --visionos --team TEAM_ID --bundle-id BUNDLE_ID   # signed device build
```

The build script downloads SDL, musl and the Khronos headers itself. Builds are
slow; build only the targets a change touches, and pass `--jobs` when sharing a
machine.

## Test

Run before every commit that changes code:

```sh
python3 tools/ios_test.py

python3 -m pytest tools/test_ios_build.py tools/test_ios_bridges.py \
  tools/test_ios_embed_guest.py tools/test_mac_run.py
```

A change to rendering, gameplay or the importer also needs a run in the
simulator or on a device, or a `tools/mac_run.py` run with screenshots. Name
the device, OS version, renderer (`display.renderer`) and level you tried. A
change to shared code in `port/linux` or `source/` should still pass
`python3 tools/ci_build.py linux release`.

CI (`.github/workflows/ios.yml`) runs `ios_test.py` and the unsigned and
simulator builds for iOS, tvOS and visionOS; `build.yml` builds the upstream
Linux and Windows ports.

## Code style

- Match the file you are editing. Code in `source/` keeps the original's
  naming and layout.
- C in `source/` and `port/linux` uses tabs. Objective-C and Swift in
  `port/ios/host` and Python in `tools/` use spaces.
- Comments explain why, not what, and name the files and functions a reader
  should look at.
- Keep upstream code easy to merge: add a small hook rather than rewriting an
  upstream file.
- Write new text (code, comments, docs, commit messages) in American English.
  Do not change spelling in existing upstream code or text.
- Use long-form flags (`--verbose`, not `-v`) when scripts or docs call a CLI,
  unless the flag has no long form.

## Commits

Conventional Commits, each led by its gitmoji:

```
🐛 fix(ios): keep the import's progress bar on screen after rotation — `ImportView` ...
```

- Gitmoji and type: ✨ `feat`, 🐛 `fix`, ⚡️ `perf`, ♻️ `refactor`, 📝 `docs`,
  ✅ `test`, 📦️ `build`, 👷 `ci`, 🔧 `chore`, ⏪️ `revert`, 🔀 for merges.
- A scope such as `ios`, `metal`, `msl`, `visionos`, `tools` or `mac_run`.
- The subject says what changed and, after a dash, why. No hard line breaks
  inside a paragraph.
- Code names in backticks.
- One commit per logical change.
- Commits taken from upstream keep their original messages. Pull requests sent
  upstream follow that project's style (plain imperative sentences for OpenCE).
- Never rewrite history with `git filter-repo`. It strips upstream's commit
  signatures and changes upstream's hashes, which breaks later merges from
  upstream.

## Never commit

- Game data: maps, disc images, sounds, textures, videos, screenshots or frame
  captures of the game, or anything extracted from them.
- Signing material: certificates, `.p12` and `.p8` files, provisioning
  profiles, keychains, API keys, team IDs or device identifiers.
- Personal details of whoever runs the agent: names of their machines, local
  paths outside the repository, network addresses, email addresses.
- Payments, ads or tracking. VISR is free.

New code is CC0 1.0 like the rest of the repository. A third-party library
keeps its license file next to it and is listed in `NOTICE.md`.
