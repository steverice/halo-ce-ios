# Arm ASTC encoder

Unmodified compressor library sources from [Arm astc-encoder](https://github.com/ARM-software/astc-encoder), pinned to revision `baff485b0ff36d2f95d28961605106502c653966`, under Apache-2.0. The command-line front end, image loaders, third-party image libraries, and tests are excluded. Apple builds use NEON and 4x4 blocks. The decompressor is retained to reject encoded textures outside the quality limits before storing them.
