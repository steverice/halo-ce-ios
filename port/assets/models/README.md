# Texture upscale model

`S4g.mlpackage` is the float16 Core ML conversion of
[Kim2091/PBRify_Remix's 4x-PBRify_UpscalerSPANV4](https://github.com/Kim2091/PBRify_Remix).
The weights are released under CC0 1.0; the license is in
[`port/ios/Licenses/PBRify-CC0-1.0.txt`](../../ios/Licenses/PBRify-CC0-1.0.txt).

The package accepts NCHW floats as `image` and returns a 4x `upscaled` tensor.
Each input channel is replicated to three SPAN channels independently, then its
three output channels are averaged. Its one input shape is 288 by 288 texels,
the generator's tiles: a 256 by 256 texture core and 16 texels of real wrap or
clamp context on each side. (The Mac package enumerates every pair of 160, 288,
544 and 1056 with the same weights; on the Neural Engine each enumerated shape
costs memory, even unused.) It uses Core ML's
CPU and Neural Engine compute units.

The source weights have SHA-256
`d1f1fa1af7a5ab23fcf143e75aac4951e6c52d56bf77d9ccd4d004a4831d0461`.
