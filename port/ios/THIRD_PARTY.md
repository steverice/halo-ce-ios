# Credits and third-party notices

This iOS port builds on [cybersecurity/halo-ce-universal](https://github.com/cybersecurity/halo-ce-universal),
the native ports in [bnunu/halo-1](https://github.com/bnunu/halo-1), and the
decompilation work in [punpckhdq/halo](https://github.com/punpckhdq/halo).
The inherited project license is [CC0 1.0](../../LICENSE.md).

The app includes SDL 3.4.16 (zlib license), portions of musl 1.2.5 (see its
COPYRIGHT for the MIT license and component notices), KCP (MIT), tomlc17
(MIT), stb_truetype (MIT), Expat (MIT), Monocypher (2-clause BSD or CC0) and
zlib (zlib license), Mbed TLS 3.6.7 (Apache License 2.0), and the Overpass and OpenCE fonts (SIL Open Font License),
the Newtown font (public domain), and SMAA (MIT;
`port/third_party/smaa`). The native XISO parser adapts upstream
extraction code following
extract-xiso (modified BSD; see `port/third_party/extract-xiso/LICENSE.TXT`).
This product includes software developed by in <in@fishtank.com>.
Builds that play the game's intro and credits movies (the default; not those
made with `--bink off`) use libraries from the FFmpeg project (9.0.2, its
Bink demuxer and decoders) under the LGPLv2.1, statically linked. FFmpeg's
source, unmodified, is in `port/third_party/ffmpeg-bink` of this app's source
repository, and the app can be rebuilt from that repository with a modified
FFmpeg; the license is in `Licenses/FFmpeg-LGPL-2.1.txt`. Their complete notices are copied into the built app's `Licenses`
directory. Khronos headers retain the notices supplied by their registries.
Other vendored components retain their original notices in the source tree.

Halo, Master Chief, game artwork, and original game assets belong to their
respective rights holders. The project license does not grant rights to those
materials. The VISR icon is vector art whose helmet and visor outlines are
traced from the game's Master Chief helmet; see [ICON.md](ICON.md). No game maps, disc images, Microsoft signing material, or
Xbox SDK are distributed in the app. This is an unofficial community port,
unaffiliated with Microsoft, Bungie, or Halo Studios.
