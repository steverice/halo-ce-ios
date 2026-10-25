#include "texture_upscale_encode.h"
#include "texture_record.h"
#include "astc_encode.h"
#include "astcenc.h"
#include "bc7enc.h"
#include "bc7decomp.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
/* A normal map's biased RGB as a unit vector. */
static void unit_normal(const uint8_t *rgb, double *n)
{
    double length = 0;
    for (unsigned c = 0; c < 3; ++c) { n[c] = rgb[c] / 127.5 - 1; length += n[c] * n[c]; }
    length = std::max(std::sqrt(length), 1e-6);
    for (unsigned c = 0; c < 3; ++c) n[c] /= length;
}
struct metrics_accumulator
{
    unsigned alpha_mode = TEXTURE_UPSCALE_ENCODE_ALPHA_BINARY;
    bool normals = false;
    double squared_error = 0, alpha_squared_error = 0, angles = 0;
    uint64_t channels = 0, alphas = 0, angle_count = 0;
    unsigned alpha_error = 0;
    void add(const uint8_t *source, const uint8_t *decoded, size_t texels)
    {
        for (size_t i = 0; i < texels; ++i)
        {
            unsigned alpha = source[i * 4 + 3];
            if (alpha) for (unsigned c = 0; c < 3; ++c)
            {
                int delta = int(source[i * 4 + c]) - decoded[i * 4 + c];
                squared_error += delta * delta;
                ++channels;
            }
            if (alpha && normals)
            {
                double a[3], b[3];
                unit_normal(source + i * 4, a); unit_normal(decoded + i * 4, b);
                angles += std::acos(std::min(1.0, std::max(-1.0, a[0] * b[0] + a[1] * b[1] + a[2] * b[2]))) * (180 / M_PI);
                ++angle_count;
            }
            if (alpha_mode == TEXTURE_UPSCALE_ENCODE_ALPHA_BINARY && (alpha == 0 || alpha == 255))
                alpha_error = std::max(alpha_error, unsigned(std::abs(int(alpha) - decoded[i * 4 + 3])));
            if (alpha_mode == TEXTURE_UPSCALE_ENCODE_ALPHA_GRADED)
            {
                int delta = int(alpha) - decoded[i * 4 + 3];
                alpha_squared_error += delta * delta;
                ++alphas;
            }
        }
    }
    static double psnr(double squared, uint64_t count)
    {
        return !count || !squared ? std::numeric_limits<double>::infinity() : 10 * std::log10(65025.0 * count / squared);
    }
    bool finish(texture_upscale_encode_metrics *metrics) const
    {
        const double limit = 65025.0 / std::pow(10.0, 3.8);
        metrics->rgb_psnr = psnr(squared_error, channels);
        metrics->alpha_psnr = psnr(alpha_squared_error, alphas);
        metrics->binary_alpha_max_error = alpha_error;
        metrics->mean_angle = normals ? (angle_count ? angles / angle_count : 0) : -1;
        /* A normal map keeps its directions instead of its RGB levels (TEXTURE_UPSCALE_ENCODE_NORMAL_ANGLE). */
        bool rgb = normals ? metrics->mean_angle <= TEXTURE_UPSCALE_ENCODE_NORMAL_ANGLE :
            (!channels || squared_error / channels <= limit);
        return rgb && alpha_error <= 8 && (!alphas || alpha_squared_error / alphas <= limit);
    }
};

static void put32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (i * 8));
}
static void put64(uint8_t *bytes, uint64_t value)
{
    put32(bytes, uint32_t(value)); put32(bytes + 4, uint32_t(value >> 32));
}

static bool encode_bc7(const texture_upscale_rgba_level &level, uint8_t *output, unsigned preset, unsigned threads)
{
    static std::once_flag once;
    std::call_once(once, [] { bc7enc_compress_block_init(); });
    /* Modes 1 and 6 for opaque blocks, 5, 6 and 7 where alpha varies (mode 6 alone shares its indices between color
       and alpha, which smears a cutout's edge); each preset searches more partitions and endpoints. */
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    bc7enc_compress_block_params_init_linear_weights(&params);
    params.m_max_partitions = preset == 0 ? 16 : BC7ENC_MAX_PARTITIONS;
    params.m_uber_level = preset == 2 ? BC7ENC_MAX_UBER_LEVEL : preset;
    params.m_mode17_partition_estimation_filterbank = preset != 2;
    unsigned blocks_x = (level.width + 3) / 4, blocks_y = (level.height + 3) / 4;
    threads = std::min(std::min(threads ? threads : 4, 4u), blocks_y);
    std::atomic<unsigned> next_row { 0 };
    std::atomic<bool> failed { false };
    auto work = [&]() {
        while (!failed)
        {
            unsigned by = next_row.fetch_add(1);
            if (by >= blocks_y) break;
            for (unsigned bx = 0; bx < blocks_x; ++bx)
            {
                alignas(8) uint8_t pixels[64];
                for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x)
                {
                    unsigned sx = std::min(bx * 4 + x, level.width - 1), sy = std::min(by * 4 + y, level.height - 1);
                    memcpy(pixels + (y * 4 + x) * 4, level.pixels + (size_t(sy) * level.width + sx) * 4, 4);
                }
                bc7enc_compress_block(output + (size_t(by) * blocks_x + bx) * 16, pixels, &params);
            }
        }
    };
    std::vector<std::thread> workers;
    try
    {
        for (unsigned i = 1; i < threads; ++i) workers.emplace_back(work);
        work();
    }
    catch (...)
    {
        failed = true;
    }
    for (auto &worker : workers) worker.join();
    return !failed;
}

/* Decode into bounded tiles instead of keeping another uncompressed top level.
   The BC decoder is independent of the compressor, including its mode selection. */
static int check_encoded(const texture_upscale_rgba_level &level, const uint8_t *blocks, uint32_t format,
    unsigned alpha_mode, texture_upscale_encode_metrics *metrics)
{
    metrics_accumulator accumulator;
    accumulator.alpha_mode = alpha_mode & ~unsigned(TEXTURE_UPSCALE_ENCODE_NORMALS);
    accumulator.normals = alpha_mode & TEXTURE_UPSCALE_ENCODE_NORMALS;
    unsigned blocks_x = (level.width + 3) / 4;
    if (format == TEXTURE_RECORD_BC7)
    {
        for (unsigned by = 0; by < (level.height + 3) / 4; ++by)
        for (unsigned bx = 0; bx < blocks_x; ++bx)
        {
            alignas(8) uint8_t pixels[64];
            if (!detexDecompressBlockBPTC(blocks + (size_t(by) * blocks_x + bx) * 16, 0xffffffffu, 0, pixels)) return -1;
            unsigned width = std::min(4u, level.width - bx * 4), height = std::min(4u, level.height - by * 4);
            for (unsigned y = 0; y < height; ++y)
                accumulator.add(level.pixels + (size_t(by * 4 + y) * level.width + bx * 4) * 4, pixels + y * 16, width);
        }
    }
    else
    {
        astcenc_config config;
        astcenc_context *context = nullptr;
        if (astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY, &config) != ASTCENC_SUCCESS ||
            astcenc_context_alloc(&config, 1, &context, nullptr) != ASTCENC_SUCCESS) return -1;
        struct context_free { void operator()(astcenc_context *p) const { astcenc_context_free(p); } };
        std::unique_ptr<astcenc_context, context_free> decoder(context);
        const astcenc_swizzle swizzle { ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A };
        uint8_t pixels[128 * 128 * 4], tile[128 * 128];
        for (unsigned top = 0; top < level.height; top += 128)
        for (unsigned left = 0; left < level.width; left += 128)
        {
            unsigned width = std::min(128u, level.width - left), height = std::min(128u, level.height - top);
            unsigned tbx = (width + 3) / 4, tby = (height + 3) / 4;
            for (unsigned by = 0; by < tby; ++by)
                memcpy(tile + size_t(by) * tbx * 16, blocks + (size_t(top / 4 + by) * blocks_x + left / 4) * 16, size_t(tbx) * 16);
            void *slice = pixels;
            astcenc_image image { width, height, 1, ASTCENC_TYPE_U8, &slice };
            if (astcenc_decompress_image(context, tile, size_t(tbx) * tby * 16, &image, &swizzle, 0) != ASTCENC_SUCCESS) return -1;
            for (unsigned y = 0; y < height; ++y)
                accumulator.add(level.pixels + (size_t(top + y) * level.width + left) * 4, pixels + size_t(y) * width * 4, width);
        }
    }
    return accumulator.finish(metrics) ? 1 : 0;
}

static bool encode_level(const texture_upscale_rgba_level &level, uint8_t *output, uint32_t format, unsigned preset, unsigned threads)
{
    return format == TEXTURE_RECORD_BC7 ? encode_bc7(level, output, preset, threads) :
        astc_encode_rgba_level(level.pixels, level.width, level.height, output, preset, threads) == ASTC_ENCODE_OK;
}
}

static texture_upscale_encode_result raw_record(const texture_upscale_rgba_level *levels, unsigned count,
    uint32_t source_width, uint32_t source_height, uint64_t hash, uint64_t recipe_key, void **record, size_t *size)
{
    uint32_t header_size = TEXTURE_RECORD_FIXED_SIZE + 16 * count;
    size_t bytes = header_size;
    for (unsigned i = 0; i < count; ++i)
        bytes += size_t(texture_record_format_level_bytes(TEXTURE_RECORD_BGRA8, levels[i].width, levels[i].height));
    uint8_t *data = static_cast<uint8_t *>(calloc(1, bytes));
    if (!data) return TEXTURE_UPSCALE_ENCODE_ERROR;
    size_t offset = header_size;
    for (unsigned i = 0; i < count; ++i)
    {
        size_t texels = size_t(levels[i].width) * levels[i].height;
        uint8_t *out = data + offset;
        for (size_t t = 0; t < texels; ++t)
        {
            const uint8_t *in = levels[i].pixels + t * 4;
            out[t * 4] = in[2]; out[t * 4 + 1] = in[1]; out[t * 4 + 2] = in[0]; out[t * 4 + 3] = in[3];
        }
        size_t length = size_t(texture_record_format_level_bytes(TEXTURE_RECORD_BGRA8, levels[i].width, levels[i].height));
        put64(data + TEXTURE_RECORD_FIXED_SIZE + i * 16, offset);
        put64(data + TEXTURE_RECORD_FIXED_SIZE + i * 16 + 8, length);
        offset += length;
    }
    memcpy(data, TEXTURE_RECORD_MAGIC, 4);
    put32(data + 4, TEXTURE_RECORD_VERSION); put32(data + 8, TEXTURE_RECORD_BGRA8);
    put32(data + 12, source_width); put32(data + 16, source_height);
    put32(data + 20, levels[0].width); put32(data + 24, levels[0].height); put32(data + 28, count);
    put64(data + 32, hash); put64(data + 40, recipe_key);
    put64(data + 48, texture_record_checksum(data + header_size, bytes - header_size)); put32(data + 56, header_size);
    *record = data; *size = bytes;
    return TEXTURE_UPSCALE_ENCODE_OK;
}

int texture_upscale_encode_gate(const uint8_t *source, const uint8_t *decoded, size_t texels, texture_upscale_encode_metrics *metrics)
{
    return texture_upscale_encode_gate_alpha(source, decoded, texels, TEXTURE_UPSCALE_ENCODE_ALPHA_BINARY, metrics);
}

int texture_upscale_encode_gate_alpha(const uint8_t *source, const uint8_t *decoded, size_t texels, unsigned alpha_mode,
    texture_upscale_encode_metrics *metrics)
{
    if (!source || !decoded || !metrics || !texels || texels > size_t(8192) * 8192 ||
        (alpha_mode & ~unsigned(TEXTURE_UPSCALE_ENCODE_NORMALS)) > TEXTURE_UPSCALE_ENCODE_ALPHA_GRADED) return 0;
    metrics_accumulator accumulator;
    accumulator.alpha_mode = alpha_mode & ~unsigned(TEXTURE_UPSCALE_ENCODE_NORMALS);
    accumulator.normals = alpha_mode & TEXTURE_UPSCALE_ENCODE_NORMALS;
    accumulator.add(source, decoded, texels);
    return accumulator.finish(metrics);
}

const char *texture_upscale_encoder_name(uint32_t format)
{
    /* v2: bc7enc (modes 1, 5, 6, 7) in place of bc7enc16; v3 and astc v2: the per-alpha gate and BGRA8 exceptions;
       v4 and astc v3: bump maps gated on their normals' mean angle */
    if (format == TEXTURE_RECORD_BC7) return "bc7-v4";
    if (format == TEXTURE_RECORD_ASTC4X4) return "astc4x4-fast-gated-v3";
    return nullptr;
}

texture_upscale_encode_result texture_upscale_encode_record(const texture_upscale_rgba_level *levels, unsigned count,
    uint32_t source_width, uint32_t source_height, uint64_t hash, uint64_t recipe_key,
    uint32_t format, unsigned alpha_mode, unsigned threads, void **record, size_t *size,
    texture_upscale_encode_metrics *metrics)
{
    if (record) *record = nullptr;
    if (size) *size = 0;
    if (metrics) *metrics = {};
    if (!record || !size || !metrics || !levels || !count || count > TEXTURE_RECORD_MAXIMUM_LEVELS ||
        !source_width || !source_height || !texture_upscale_encoder_name(format) ||
        (alpha_mode & ~unsigned(TEXTURE_UPSCALE_ENCODE_NORMALS)) > TEXTURE_UPSCALE_ENCODE_ALPHA_GRADED)
        return TEXTURE_UPSCALE_ENCODE_ERROR;
    unsigned factor;
    for (factor = 1; factor <= 8; factor *= 2)
        if (source_width <= TEXTURE_RECORD_MAXIMUM_DIMENSION / factor && source_height <= TEXTURE_RECORD_MAXIMUM_DIMENSION / factor &&
            levels[0].width == source_width * factor && levels[0].height == source_height * factor) break;
    if (factor > 8) return TEXTURE_UPSCALE_ENCODE_ERROR;
    uint32_t width = levels[0].width, height = levels[0].height;
    size_t bytes = TEXTURE_RECORD_FIXED_SIZE + 16 * count;
    for (unsigned i = 0; i < count; ++i)
    {
        if (!levels[i].pixels || levels[i].width != width || levels[i].height != height ||
            (width == 1 && height == 1 && i + 1 != count)) return TEXTURE_UPSCALE_ENCODE_ERROR;
        bytes += size_t(texture_record_level_bytes(width, height));
        width = texture_record_half(width); height = texture_record_half(height);
    }
    struct allocation_free { void operator()(uint8_t *p) const { free(p); } };
    std::unique_ptr<uint8_t, allocation_free> data(static_cast<uint8_t *>(calloc(1, bytes)));
    if (!data) return TEXTURE_UPSCALE_ENCODE_ERROR;
    uint32_t header_size = TEXTURE_RECORD_FIXED_SIZE + 16 * count;
    uint8_t *payload = data.get() + header_size;
    unsigned preset = 0;
    try
    {
        for (; preset < 3; ++preset)
        {
            metrics->attempts = preset + 1; metrics->preset = preset;
            if (!encode_level(levels[0], payload, format, preset, threads)) return TEXTURE_UPSCALE_ENCODE_ERROR;
            int accepted = check_encoded(levels[0], payload, format, alpha_mode, metrics);
            if (accepted < 0) return TEXTURE_UPSCALE_ENCODE_ERROR;
            if (accepted) break;
        }
        if (preset == 3)
        {
            /* No block encoding keeps this texture within the gate: store it uncompressed, at four times the bytes,
               rather than loosen the gate or keep the original. */
            texture_upscale_encode_metrics missed = *metrics;
            texture_upscale_encode_result raw = raw_record(levels, count, source_width, source_height, hash, recipe_key,
                record, size);
            *metrics = missed;
            metrics->raw = raw == TEXTURE_UPSCALE_ENCODE_OK;
            return raw;
        }
        size_t offset = header_size;
        for (unsigned i = 0; i < count; ++i)
        {
            size_t length = size_t(texture_record_level_bytes(levels[i].width, levels[i].height));
            if (i && !encode_level(levels[i], data.get() + offset, format, preset, threads)) return TEXTURE_UPSCALE_ENCODE_ERROR;
            put64(data.get() + TEXTURE_RECORD_FIXED_SIZE + i * 16, offset);
            put64(data.get() + TEXTURE_RECORD_FIXED_SIZE + i * 16 + 8, length);
            offset += length;
        }
    }
    catch (...)
    {
        return TEXTURE_UPSCALE_ENCODE_ERROR;
    }
    memcpy(data.get(), TEXTURE_RECORD_MAGIC, 4);
    put32(data.get() + 4, TEXTURE_RECORD_VERSION); put32(data.get() + 8, format);
    put32(data.get() + 12, source_width); put32(data.get() + 16, source_height);
    put32(data.get() + 20, levels[0].width); put32(data.get() + 24, levels[0].height); put32(data.get() + 28, count);
    put64(data.get() + 32, hash); put64(data.get() + 40, recipe_key);
    put64(data.get() + 48, texture_record_checksum(payload, bytes - header_size)); put32(data.get() + 56, header_size);
    *record = data.release(); *size = bytes;
    return TEXTURE_UPSCALE_ENCODE_OK;
}
