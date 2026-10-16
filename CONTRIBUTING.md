# Contributing to VISR

Thanks for helping. Bug reports, testing on devices we have not tried, and
pull requests are all welcome.

## Ground rules

- **Never commit game data.** That means maps, disc images, sounds, textures,
  videos, screenshots or frame captures of the game, and anything extracted
  from them, such as upscaled textures or crops. `.gitignore` already blocks
  `*.iso`, `*.xiso`, `assets/` and other common cases. Pull requests that add
  game content will be closed.
- **Never commit signing material**: certificates, `.p12` files, provisioning
  profiles, keychains or API keys.
- **Keep it free.** VISR is not sold, and contributions must not add
  payments, ads or tracking.
- Contributions are dedicated to the public domain under
  [CC0 1.0](LICENSE.md), the same as the rest of the code. By opening a pull
  request, you agree that you have the right to do that for what you submit.
  If you bring in a third-party library, keep its license file next to it and
  add it to [NOTICE.md](NOTICE.md).

## Building

You need an Apple silicon Mac with Xcode, Python 3 and Homebrew:

```sh
brew install cmake ninja llvm lld sdl3 pkgconf
python3 tools/ios_build.py --simulator
```

The [build and install guide](port/ios/README.md) covers device builds,
signing, Apple TV, Apple Vision Pro and the Mac. To test with real game data,
you need your own disc image, imported into the app as the README describes.

## Testing

Run the portable tooling suite with Python 3.11 or newer before you open a
pull request:

```sh
uv run --no-project --with-requirements tools/requirements-ci.txt python tools/ci_test.py
```

Run native Apple regressions separately on an Apple silicon Mac with Xcode:

```sh
python3 tools/ios_test.py
```

The [CI guide](docs/ci.md) lists the automated checks, dependencies and known
coverage gaps. visionOS builds retain their existing advisory status. The
[release guide](docs/releasing.md) covers the separate, manually approved
release workflow and its required repository protection settings.

If your change touches rendering, gameplay or the importer, also run it on a
device or in the simulator and say in the pull request what you tried: the
device, the OS version, the renderer (`display.renderer` in `config.toml`) and
the level. `tools/mac_run.py` can run a build on an Apple silicon Mac and save
screenshots for comparison; the guide shows how.

The upstream Linux and Windows builds still build from this tree
([README.upstream.md](README.upstream.md)). If you change shared code in
`port/linux` or `source/`, check that `python3 tools/ci_build.py linux
release` still builds if you can.

## Code style

- Match the file you are editing. The decompiled game in `source/` follows the
  original's naming and layout, so changes there should look like the code
  around them, and port-specific changes in `source/` are marked with a
  `port:` comment.
- C in `source/` and `port/linux` is indented with tabs, as upstream does.
  The Objective-C and Swift in `port/ios/host` and the Python in `tools/` use
  spaces.
- Explain why in comments, not what. Name the files and functions a reader
  should look at.
- Keep upstream code easy to merge: prefer adding a small hook over
  rewriting an upstream file.

## Commits

We use [Conventional Commits](https://www.conventionalcommits.org/), each led by its [gitmoji](https://gitmoji.dev/):

```
🐛 fix(ios): keep the import's progress bar on screen after rotation — `ImportView` ...
```

- A gitmoji for the type: ✨ `feat`, 🐛 `fix`, ⚡️ `perf`, ♻️ `refactor`, 📝 `docs`,
  ✅ `test`, 📦️ `build`, 👷 `ci`, 🔧 `chore`, ⏪️ `revert`, 🔀 for merges
- A type (`feat`, `fix`, `perf`, `refactor`, `docs`, `test`, `build`, `ci`,
  `chore` or `revert`) and a scope such as `ios`, `metal`, `msl`, `visionos`,
  `tools` or `mac_run`
- A subject line that says what changed and, after a dash, why
- Code names in backticks
- One commit per logical change
- Commits taken from upstream keep their original messages; pull requests sent
  upstream follow that project's own style

## Reporting bugs

Open an [issue](https://github.com/steverice/visr/issues) with the bug report
template. Include your device, OS version, the app's version, your disc's
region (NTSC-US or PAL), and what you were doing. Attach `ios-runtime.log` and
`debug.txt` from the VISR folder in Files after you check them for anything
personal. Do not attach game files.

Security problems go through [SECURITY.md](SECURITY.md), not public issues.

## Conduct

Everyone who takes part agrees to the [code of conduct](CODE_OF_CONDUCT.md).
