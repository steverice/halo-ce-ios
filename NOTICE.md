# Notices

VISR is licensed under Apache-2.0 (see [LICENSE](LICENSE)). It includes code
that originated in CC0 1.0 projects (punpckhdq/halo, bnunu/halo-1, OpenCE,
NicholasDominici/halo-ce-ios and pfista/halo-og, listed below), whose original
code remains available from those projects under CC0 1.0. VISR's commits before
the change that added this notice were published under CC0 1.0. The CC0 1.0
text is kept at [LICENSES/CC0-1.0.txt](LICENSES/CC0-1.0.txt) for reference.

Halo, its game content and its trademarks are the property of Microsoft. VISR
contains no game assets, grants no rights in Microsoft's property, and requires
players to supply their own copy of the game.

The components below keep their own licenses. Each one's full license text sits
next to its source in this repository, and the Apple builds copy the notices of
what they link into the app's `Licenses` folder (`tools/ios_build.py`).

## Upstream projects

| Project | License | What VISR takes from it |
| --- | --- | --- |
| [punpckhdq/halo](https://github.com/punpckhdq/halo) | CC0 1.0 | The decompiled game in `source/` |
| [bnunu/halo-1](https://github.com/bnunu/halo-1) | CC0 1.0 | Further decompilation in `source/` |
| [OpenCE](https://github.com/OpenCommunityEdition/OpenCE) (formerly cybersecurity/halo-ce-universal) | CC0 1.0 | The ports in `port/`, `tools/` and `pgo/`, the high-resolution HUD, menus and titles in `port/assets/` |
| [NicholasDominici/halo-ce-ios](https://github.com/NicholasDominici/halo-ce-ios) | CC0 1.0 | The iOS port in `port/ios/` and `port/runtime/` |
| [pfista/halo-og](https://github.com/pfista/halo-og) | CC0 1.0 | Metal renderer fixes |

## Vendored libraries

| Component | License | Location | In the Apple apps |
| --- | --- | --- | --- |
| [xxHash](https://github.com/Cyan4973/xxHash), texture keys and checksums | BSD 2-Clause | `port/third_party/xxhash/LICENSE` | Yes |
| [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo)'s bc7enc, BC7 compressor | MIT | `port/third_party/bc7enc/LICENSE` | Yes |
| [bc7enc16](https://github.com/richgel999/bc7enc16)'s BC7 decoder, for the quality check | ISC | `port/third_party/bc7enc16/LICENSE` | Yes |
| [Arm astc-encoder](https://github.com/ARM-software/astc-encoder), compressor and quality-check decoder | Apache-2.0 | `port/third_party/astcenc/LICENSE.txt` | Yes |
| [Arm astc-encoder](https://github.com/ARM-software/astc-encoder), reference decoder | Apache-2.0 | `port/third_party/astcenc-reference/LICENSE.txt` | No, tests only |
| [Kim2091/PBRify_Remix](https://github.com/Kim2091/PBRify_Remix), S4g per-channel Core ML conversion of 4x-PBRify_UpscalerSPANV4 | CC0 1.0 | `port/ios/Licenses/PBRify-CC0-1.0.txt` | Yes, iOS and visionOS |
| [FFmpeg](https://ffmpeg.org/) 9.0.2, its Bink demuxer and decoders only | LGPL-2.1-or-later | `port/third_party/ffmpeg-bink/COPYING.LGPLv2.1`, `port/third_party/ffmpeg-bink/` | Yes, unless built with `--bink off` |
| [Expat](https://libexpat.github.io/) | MIT | `port/third_party/expat/COPYING` | Yes |
| [KCP](https://github.com/skywind3000/kcp) | MIT | `port/third_party/kcp/LICENSE` | Yes |
| [Monocypher](https://monocypher.org/) | BSD-2-Clause or CC0 1.0 | `port/third_party/monocypher/LICENCE.md` | Yes |
| [stb_truetype](https://github.com/nothings/stb) | MIT or public domain (Unlicense) | `port/third_party/stb/LICENSE` | Yes |
| [tomlc17](https://github.com/cktan/tomlc17) | MIT | `port/third_party/tomlc17/LICENSE` | Yes |
| [zlib](https://zlib.net/) 1.3.2 | zlib | `port/third_party/zlib/LICENSE` | Yes |
| [musl](https://musl.libc.org/) math functions | MIT | `port/third_party/musl-math/COPYRIGHT` | No, Linux build only (the Apple builds download musl itself) |
| [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/) | Apache-2.0 or GPL-2.0-or-later (we use Apache-2.0) | `port/third_party/mbedtls/LICENSE` | No, desktop builds only |
| [miniupnpc](https://miniupnp.tuxfamily.org/) | BSD-3-Clause | `port/third_party/miniupnpc/LICENSE` | No, desktop builds only |
| zlib, as the game shipped it | zlib | `source/memory/zlib/` | Part of the decompiled source tree |
| libtiff, as the game shipped it | libtiff (Sam Leffler and Silicon Graphics, notice in each file) | `source/bitmaps/libtiff/` | Part of the decompiled source tree |

## FFmpeg and the LGPL

The Apple apps play the game's intro and credits movies with FFmpeg's Bink
demuxer and decoders. This software uses libraries from the
[FFmpeg](https://ffmpeg.org/) project under the
[LGPLv2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html). FFmpeg's
files in `port/third_party/ffmpeg-bink/` are not under CC0: they stay under
the GNU Lesser General Public License, version 2.1 or later, and each keeps
its own notice.

- The source is the unmodified release `n9.0.2`
  (`port/third_party/ffmpeg-bink/README.md` lists the files, the configure
  command and how they were chosen). VISR does not own FFmpeg.
- The configuration is LGPL only: no `--enable-gpl`, `--enable-version3` or
  `--enable-nonfree`, and no external libraries.
- FFmpeg is linked statically into the app's executable. The complete source
  of the app, including the FFmpeg code it is built with, is at
  <https://github.com/steverice/visr>.
- To rebuild the app with a modified FFmpeg, change the files in
  `port/third_party/ffmpeg-bink/`, then build from the repository root with
  `python3 tools/ios_build.py --unsigned --ipa dist/VISR-iOS-unsigned.ipa`
  (other targets and signing options are in `port/ios/README.md`).
- An app that contains FFmpeg carries the license text as
  `Licenses/FFmpeg-LGPL-2.1.txt`.
- `tools/ios_build.py --bink off` (CMake `HALO_BINK=OFF`) builds an app with
  no FFmpeg code and no FFmpeg license file. It skips the movies.

## Downloaded at build time

`tools/ios_build.py` downloads these and does not commit them:

| Component | License |
| --- | --- |
| [SDL](https://libsdl.org/) 3.4.16 | zlib |
| [musl](https://musl.libc.org/) 1.2.5 | MIT, with the component notices in its `COPYRIGHT` |
| Khronos OpenGL ES and EGL headers | MIT and Apache-2.0, as their registries state in each header |

## Fonts

| Font | License | Location |
| --- | --- | --- |
| [Overpass](https://overpassfont.org/) | SIL Open Font License 1.1 | `port/assets/fonts/Overpass-OFL.txt` |
| OpenCE (Newtown, respaced) | SIL Open Font License 1.1 | `port/assets/fonts/OpenCE-OFL.txt` |
| Newtown by Roger White | Public domain | `port/assets/fonts/Newtown-LICENSE.txt` |
| Orbitron (launch menu, site font and Top Shelf wordmark outlines) | SIL Open Font License 1.1 | `docs/site/fonts/OFL-Orbitron.txt` |

## Game content and trademarks

Halo, Halo: Combat Evolved, Master Chief and the related names and logos are
trademarks of Microsoft Corporation. Halo: Combat Evolved © Microsoft
Corporation. VISR is not affiliated with or endorsed by Microsoft, Halo Studios
or Bungie.

The repository does not contain the game's maps, sounds, textures or videos.
The HUD, menu and title pictures in `port/assets/` are high-resolution redraws
made for OpenCE, not files from the game. The app icon's helmet, visor and brow
outlines are traced from a frame of the game (see `port/ios/ICON.md`). The menu
layouts and strings in `port/assets/menus/` are generated from the game's tags.
None of the licenses above, CC0 and Apache-2.0 included, grants any rights to Microsoft's
game, its assets or its trademarks.
