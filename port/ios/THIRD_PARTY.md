# Credits and third-party notices

This iOS port builds on [cybersecurity/halo-ce-universal](https://github.com/cybersecurity/halo-ce-universal),
the native ports in [bnunu/halo-1](https://github.com/bnunu/halo-1), and the
decompilation work in [punpckhdq/halo](https://github.com/punpckhdq/halo).
VISR is licensed under [Apache-2.0](../../LICENSE). It includes code that originated in CC0 1.0 projects, credited in [NOTICE.md](../../NOTICE.md).

The app includes SDL 3.4.16 (zlib license), portions of musl 1.2.5 (see its
COPYRIGHT for the MIT license and component notices), KCP (MIT), tomlc17
(MIT), stb_truetype (MIT), Expat (MIT), Monocypher (2-clause BSD or CC0) and
zlib (zlib license), Mbed TLS 3.6.7 (Apache License 2.0), and the Overpass and OpenCE fonts (SIL Open Font License),
the Newtown font (public domain), and SMAA (MIT;
`port/third_party/smaa`).
Streamer mode's built-in word table derives from Surge AI's profanity list
(MIT); its provenance and license are in `Licenses/Streamer-Words-MIT.txt`.
The English word list used to review terms is under the Unlicense; its notice
is in `Licenses/English-Words-Unlicense.txt`.
Builds that play the game's intro and credits movies (the default; not those
made with `--bink off`) use libraries from the FFmpeg project (9.0.2, its
Bink demuxer and decoders) under the LGPLv2.1, statically linked. The
complete source of this app, including the FFmpeg code in
`port/third_party/ffmpeg-bink`, is at https://github.com/steverice/visr. To
rebuild the app with a modified FFmpeg, change those files and run
`python3 tools/ios_build.py --unsigned --ipa dist/VISR-iOS-unsigned.ipa` from
the repository root. The license is in `Licenses/FFmpeg-LGPL-2.1.txt`. Their complete notices are copied into the built app's `Licenses`
directory. Khronos headers retain the notices supplied by their registries.
Other vendored components retain their original notices in the source tree.

The texture generator uses bc7enc16 by Richard Geldreich, Jr. (MIT) and
the BC7 decoder by Harm Hanemaaijer (ISC). Their notices are bundled in
`Licenses/BC7-MIT-ISC.txt` and retained in `port/third_party/bc7enc16/LICENSE`.

The texture upscale model, S4g, contains the CC0 1.0 weights of
[Kim2091's 4x-PBRify_UpscalerSPANV4](https://github.com/Kim2091/PBRify_Remix),
converted to a float16 Core ML package with independent per-channel inference.
Its license is bundled as `Licenses/PBRify-CC0-1.0.txt`.

Halo, Master Chief, game artwork, and original game assets belong to their
respective rights holders. The project license does not grant rights to those
materials. The VISR icon is vector art whose helmet and visor outlines are
traced from the game's Master Chief helmet; see [ICON.md](ICON.md). No game maps, disc images, Microsoft signing material, or
Xbox SDK are distributed in the app. This is an unofficial community port,
unaffiliated with Microsoft, Bungie, or Halo Studios.
