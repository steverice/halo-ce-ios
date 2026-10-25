# bc7enc

`bc7enc.cpp` and `bc7enc.h` from Richard Geldreich, Jr.'s [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo), revision `b9438627eef73a1157e84201b6fa6eb2ffd6d9f0`, unmodified, under the MIT license or public domain (`LICENSE`). It encodes BC7 modes 1 and 6 for opaque blocks and modes 5, 6 and 7 for blocks with alpha. The texture generator (`port/ios/host/texture_upscale_encode.cpp`) uses it; the quality check decodes with the independent decoder in `port/third_party/bc7enc16/bc7decomp.c`.
