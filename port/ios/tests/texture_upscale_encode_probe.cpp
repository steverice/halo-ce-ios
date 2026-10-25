/* Synthetic compression, quality boundaries, and generator record publication. */
#include "texture_upscale_encode.h"
#include "texture_record.h"
extern "C" {
#include "texture_cache.h"
#include "texture_upscale_generate.h"
void texture_upscale_reader_probe(const char *directory, uint32_t format, const uint8_t *record, size_t size);
}
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>


struct chain
{
    std::vector<std::vector<uint8_t>> storage;
    std::vector<texture_upscale_rgba_level> levels;
    chain(unsigned width, unsigned height, bool noise = false)
    {
        unsigned random = 0x273a5f09;
        for (;;)
        {
            storage.emplace_back(size_t(width) * height * 4);
            auto &pixels = storage.back();
            for (size_t i = 0; i < pixels.size(); i += 4)
            {
                for (unsigned c = 0; c < 3; ++c)
                {
                    random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                    pixels[i + c] = noise ? uint8_t(random) : uint8_t(40 + c * 60);
                }
                pixels[i + 3] = 255;
            }
            levels.push_back({ pixels.data(), width, height });
            if (width == 1 && height == 1) break;
            width = texture_record_half(width); height = texture_record_half(height);
        }
    }
};

static void gate_boundaries()
{
    uint8_t source[] = { 40, 60, 80, 255, 255, 255, 255, 0 };
    uint8_t decoded[] = { 43, 63, 83, 247, 0, 0, 0, 8 };
    texture_upscale_encode_metrics metrics {};
    assert(texture_upscale_encode_gate(source, decoded, 2, &metrics));
    assert(std::abs(metrics.rgb_psnr - 10 * std::log10(65025.0 / 9)) < 1e-10);
    assert(metrics.binary_alpha_max_error == 8);
    decoded[3] = 246;
    assert(!texture_upscale_encode_gate(source, decoded, 2, &metrics) && metrics.binary_alpha_max_error == 9);
    decoded[3] = 247; decoded[7] = 9;
    assert(!texture_upscale_encode_gate(source, decoded, 2, &metrics));
    decoded[7] = 8;
    for (unsigned c = 0; c < 3; ++c) decoded[c] = source[c] + 4;
    assert(!texture_upscale_encode_gate(source, decoded, 2, &metrics) && metrics.rgb_psnr < 38);
    memset(source, 0, sizeof(source)); memset(decoded, 0, sizeof(decoded));
    assert(texture_upscale_encode_gate(source, decoded, 2, &metrics) && std::isinf(metrics.rgb_psnr));
    assert(!texture_upscale_encode_gate(nullptr, decoded, 2, &metrics));
    /* Adjacent integral squared-error totals on opposite sides of 38 dB. */
    const size_t texels = 100000;
    std::vector<uint8_t> originals(texels * 4), actual(texels * 4, 3);
    for (size_t i = 0; i < texels; ++i) originals[i * 4 + 3] = actual[i * 4 + 3] = 255;
    size_t high = size_t(std::floor((65025.0 * std::pow(10.0, -3.8) - 9) * (texels * 3) / 7));
    for (size_t i = 0; i < high; ++i) actual[(i / 3) * 4 + i % 3] = 4;
    assert(texture_upscale_encode_gate(originals.data(), actual.data(), texels, &metrics) && metrics.rgb_psnr >= 38);
    actual[(high / 3) * 4 + high % 3] = 4;
    assert(!texture_upscale_encode_gate(originals.data(), actual.data(), texels, &metrics) && metrics.rgb_psnr < 38);
}

static void roundtrip(uint32_t format)
{
    /* Odd dimensions and sub-block tails must agree with the record reader. */
    chain input(258, 130);
    void *record = nullptr;
    size_t size = 0;
    texture_upscale_encode_metrics metrics {};
    assert(texture_upscale_encode_record(input.levels.data(), unsigned(input.levels.size()), 129, 65, 0x93, 0xabc,
        format, TEXTURE_UPSCALE_ENCODE_ALPHA_BINARY, 4, &record, &size, &metrics) == TEXTURE_UPSCALE_ENCODE_OK);
    assert(record && size && metrics.rgb_psnr >= 38 && metrics.binary_alpha_max_error <= 8);
    void *serial = nullptr;
    size_t serial_size = 0;
    texture_upscale_encode_metrics serial_metrics {};
    assert(texture_upscale_encode_record(input.levels.data(), unsigned(input.levels.size()), 129, 65, 0x93, 0xabc,
        format, TEXTURE_UPSCALE_ENCODE_ALPHA_BINARY, 1, &serial, &serial_size, &serial_metrics) == TEXTURE_UPSCALE_ENCODE_OK);
    assert(serial_size == size && !memcmp(serial, record, size));
    free(serial);
    texture_record_layout layout {};
    auto bytes = static_cast<uint8_t *>(record);
    assert(!texture_record_check(bytes, size, size, 0x93, 129, 65, format, &layout));
    assert(!texture_record_check_payload(&layout, bytes + layout.header_size));
    assert(layout.recipe_key == 0xabc && layout.levels == input.levels.size());
    char directory[] = "/tmp/upscale-encode-XXXXXX";
    assert(mkdtemp(directory));
    texture_cache_record entry { 0x93, 129, 65, TEXTURE_POLICY_S4G, TEXTURE_POLICY_COLOR, 0xabc, 0xdef, size };
    assert(!texture_cache_write_entry(directory, &entry, record, size));
    assert(!texture_cache_manifest_save(directory, &entry, 1));
    texture_cache_record *loaded = nullptr;
    size_t count = 0;
    assert(!texture_cache_manifest_load(directory, &loaded, &count) && count == 1);
    assert(loaded[0].recipe_key == entry.recipe_key && loaded[0].bytes == size);
    char path[512];
    snprintf(path, sizeof(path), "%s/0000000000000093-129x65.bin", directory);
    FILE *file = fopen(path, "rb"); assert(file);
    std::vector<uint8_t> disk(size);
    assert(fread(disk.data(), 1, size, file) == size && fgetc(file) == EOF); fclose(file);
    assert(!memcmp(disk.data(), bytes, size));
    assert(!texture_record_check(disk.data(), disk.size(), disk.size(), 0x93, 129, 65, format, &layout));
    assert(!texture_record_check_payload(&layout, disk.data() + layout.header_size));
    disk.back() ^= 1;
    assert(texture_record_check_payload(&layout, disk.data() + layout.header_size));
    free(loaded); free(record);
    assert(!texture_cache_delete_all(directory)); assert(!rmdir(directory));
    printf("format %u: record round trip, PSNR %.2f, preset %u, attempts %u\n", format, metrics.rgb_psnr, metrics.preset, metrics.attempts);
}

static void exclusions(uint32_t format)
{
    chain input(32, 32, true);
    void *record = reinterpret_cast<void *>(1);
    size_t size = 123;
    texture_upscale_encode_metrics metrics {};
    assert(texture_upscale_encode_record(input.levels.data(), unsigned(input.levels.size()), 16, 16, 1, 2, format,
        TEXTURE_UPSCALE_ENCODE_ALPHA_BINARY, 2, &record, &size, &metrics) == TEXTURE_UPSCALE_ENCODE_OK);
    assert(record && size && metrics.raw && metrics.attempts == 3 && metrics.rgb_psnr < 38);
    /* the noisy texture is kept uncompressed: a BGRA8 record any GPU's reader takes, its texels swizzled exactly */
    auto bytes = static_cast<uint8_t *>(record);
    texture_record_layout layout {};
    assert(!texture_record_check(bytes, size, size, 1, 16, 16, format, &layout));
    assert(layout.format == TEXTURE_RECORD_BGRA8 && !texture_record_check_payload(&layout, bytes + layout.header_size));
    for (unsigned level = 0; level < layout.levels; ++level)
    {
        const uint8_t *texels = bytes + layout.level_offset[level], *source = input.levels[level].pixels;
        for (size_t t = 0; t < size_t(input.levels[level].width) * input.levels[level].height; ++t)
            assert(texels[t * 4] == source[t * 4 + 2] && texels[t * 4 + 1] == source[t * 4 + 1] &&
                texels[t * 4 + 2] == source[t * 4] && texels[t * 4 + 3] == source[t * 4 + 3]);
    }
    free(record);
    record = reinterpret_cast<void *>(1); size = 123;
    ++input.levels[1].width;
    assert(texture_upscale_encode_record(input.levels.data(), unsigned(input.levels.size()), 16, 16, 1, 2, format,
        TEXTURE_UPSCALE_ENCODE_ALPHA_BINARY, 2, &record, &size, &metrics) == TEXTURE_UPSCALE_ENCODE_ERROR);
    assert(!record && !size);
    printf("format %u: noisy texture stored as BGRA8 after all three presets missed\n", format);
}

/* graded alpha is held to 38 dB over every texel, not to the binary endpoint test */
static void graded_alpha()
{
    uint8_t source[] = { 40, 60, 80, 255, 40, 60, 80, 0, 40, 60, 80, 128 };
    uint8_t decoded[] = { 40, 60, 80, 235, 40, 60, 80, 0, 40, 60, 80, 128 };
    texture_upscale_encode_metrics metrics {};
    assert(!texture_upscale_encode_gate_alpha(source, decoded, 3, TEXTURE_UPSCALE_ENCODE_ALPHA_BINARY, &metrics));
    assert(!texture_upscale_encode_gate_alpha(source, decoded, 3, TEXTURE_UPSCALE_ENCODE_ALPHA_GRADED, &metrics));
    assert(metrics.alpha_psnr < 38);
    decoded[3] = 252;
    assert(texture_upscale_encode_gate_alpha(source, decoded, 3, TEXTURE_UPSCALE_ENCODE_ALPHA_GRADED, &metrics));
    assert(metrics.alpha_psnr >= 38 && metrics.binary_alpha_max_error == 0);
    decoded[3] = 0;
    assert(texture_upscale_encode_gate_alpha(source, decoded, 3, TEXTURE_UPSCALE_ENCODE_ALPHA_NONE, &metrics));
    printf("graded and absent alpha gates\n");
}

static void dxt_tail(uint32_t format)
{
    chain input(16, 16);
    input.levels.resize(3); /* The source's 4x4 tail is preserved without adding 2x2 or 1x1. */
    void *record = nullptr;
    size_t size = 0;
    texture_upscale_encode_metrics metrics {};
    assert(texture_upscale_encode_record(input.levels.data(), unsigned(input.levels.size()), 8, 8, 1, 2, format, TEXTURE_UPSCALE_ENCODE_ALPHA_BINARY,
        2, &record, &size, &metrics) == TEXTURE_UPSCALE_ENCODE_OK);
    auto bytes = static_cast<uint8_t *>(record);
    texture_record_layout layout {};
    assert(!texture_record_check(bytes, size, size, 1, 8, 8, format, &layout));
    assert(layout.levels == 3 && layout.level_size[2] == 16);
    assert(!texture_record_check_payload(&layout, bytes + layout.header_size));
    free(record);
}

static void binary_alpha(uint32_t format)
{
    chain input(16, 16);
    auto &top = input.storage[0];
    for (unsigned y = 0; y < 16; ++y) for (unsigned x = 0; x < 16; ++x)
        top[(y * 16 + x) * 4 + 3] = ((x + y) & 1) ? 255 : 0;
    void *record = nullptr;
    size_t size = 0;
    texture_upscale_encode_metrics metrics {};
    assert(texture_upscale_encode_record(input.levels.data(), unsigned(input.levels.size()), 8, 8, 1, 2, format, TEXTURE_UPSCALE_ENCODE_ALPHA_BINARY,
        2, &record, &size, &metrics) == TEXTURE_UPSCALE_ENCODE_OK);
    assert(metrics.binary_alpha_max_error <= 8 && metrics.rgb_psnr >= 38);
    free(record);
}

/* A cutout: color varies across the edge where alpha switches between 0 and 255, which mode 6's shared indices
   cannot keep apart (the phase 2 device run excluded such textures with endpoint alpha off by 20 to 98 levels). */
static void cutout_edge(uint32_t format)
{
    chain input(32, 32);
    auto &top = input.storage[0];
    for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 32; ++x)
    {
        uint8_t *texel = top.data() + (y * 32 + x) * 4;
        texel[0] = uint8_t(x * 8); texel[1] = uint8_t(255 - y * 8); texel[2] = uint8_t((x + y) * 4);
        texel[3] = (x * x + y * y) < 400 ? 255 : 0;
    }
    void *record = nullptr;
    size_t size = 0;
    texture_upscale_encode_metrics metrics {};
    int result = texture_upscale_encode_record(input.levels.data(), unsigned(input.levels.size()), 16, 16, 1, 2, format, TEXTURE_UPSCALE_ENCODE_ALPHA_BINARY,
        2, &record, &size, &metrics);
    printf("cutout edge format %u: result %d, preset %u, PSNR %.2f, alpha error %u\n", format, result, metrics.preset,
        metrics.rgb_psnr, metrics.binary_alpha_max_error);
    assert(result == TEXTURE_UPSCALE_ENCODE_OK);
    assert(metrics.binary_alpha_max_error <= 8 && metrics.rgb_psnr >= 38);
    printf("cutout edge format %u: preset %u, PSNR %.2f, alpha error %u\n", format, metrics.preset, metrics.rgb_psnr,
        metrics.binary_alpha_max_error);
    free(record);
}

static void bc7_retry()
{
    chain input(16, 16);
    const uint8_t colors[4][3] = { { 20, 30, 40 }, { 80, 90, 100 }, { 160, 25, 20 }, { 220, 85, 80 } };
    auto &top = input.storage[0];
    for (unsigned y = 0; y < 16; ++y) for (unsigned x = 0; x < 16; ++x)
        memcpy(top.data() + (y * 16 + x) * 4, colors[((x % 4) / 2) * 2 + y % 2], 3);
    void *record = nullptr;
    size_t size = 0;
    texture_upscale_encode_metrics metrics {};
    assert(texture_upscale_encode_record(input.levels.data(), unsigned(input.levels.size()), 8, 8, 1, 2, TEXTURE_RECORD_BC7, TEXTURE_UPSCALE_ENCODE_ALPHA_BINARY,
        2, &record, &size, &metrics) == TEXTURE_UPSCALE_ENCODE_OK);
    assert(metrics.rgb_psnr >= 38);
    printf("BC7 four-color blocks accepted at preset %u, PSNR %.2f\n", metrics.preset, metrics.rgb_psnr);
    free(record);
}

static void generator_to_reader(uint32_t format)
{
    uint8_t pixels[4 * 4 * 4];
    for (unsigned i = 0; i < 16; ++i)
    {
        pixels[i * 4] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = 100;
        pixels[i * 4 + 3] = 255;
    }
    texture_upscale_rgba_level original { pixels, 4, 4 };
    texture_upscale_generate_input input {};
    input.decision.width = input.decision.height = 4;
    input.decision.kind = TEXTURE_POLICY_GRAPHIC;
    input.decision.result = TEXTURE_POLICY_BPF;
    input.decision.treatment = TEXTURE_POLICY_COLOR;
    input.decision.scale = 2;
    input.levels = &original; input.level_count = 1; input.alpha = TEXTURE_UPSCALE_ALPHA_NONE;
    char error[256];
    auto generator = texture_upscale_generate_create(error, sizeof(error));
    assert(generator);
    texture_upscale_generate_output output {};
    assert(!texture_upscale_generate(generator, &input, &output, error, sizeof(error)));
    assert(output.level_count == 2 && output.levels[0].width == 8 && output.levels[1].width == 4);
    /* The deterministic fake model and refinement services are linked from the
       orchestration probe; generation, serialization, storage, and reader are real. */
    assert(output.levels[0].pixels[0] == 120 && !memcmp(output.levels[1].pixels, pixels, sizeof(pixels)));
    void *record = nullptr;
    size_t size = 0;
    texture_upscale_encode_metrics metrics {};
    assert(texture_upscale_encode_record(output.levels, output.level_count, 4, 4, 0x71, 0x999, format, TEXTURE_UPSCALE_ENCODE_ALPHA_BINARY, 2,
        &record, &size, &metrics) == TEXTURE_UPSCALE_ENCODE_OK);
    auto bytes = static_cast<uint8_t *>(record);
    texture_record_layout layout {};
    assert(!texture_record_check(bytes, size, size, 0x71, 4, 4, format, &layout));
    assert(!texture_record_check_payload(&layout, bytes + layout.header_size));
    char directory[] = "/tmp/upscale-generator-reader-XXXXXX";
    assert(mkdtemp(directory));
    texture_cache_record entry { 0x71, 4, 4, output.result, input.decision.treatment, 0x999, 0x888, size };
    assert(!texture_cache_write_entry(directory, &entry, record, size));
    assert(!texture_cache_manifest_save(directory, &entry, 1));
    texture_upscale_reader_probe(directory, format, bytes, size);
    assert(!texture_cache_delete_all(directory)); assert(!rmdir(directory));
    free(record);
    texture_upscale_generate_free_output(&output);
    texture_upscale_generate_destroy(generator);
    printf("format %u: real generator -> compressed record -> manifest -> reader upload; damaged record refused\n", format);
}

int main()
{
    gate_boundaries();
    bc7_retry();
    graded_alpha();
    for (auto format : { TEXTURE_RECORD_BC7, TEXTURE_RECORD_ASTC4X4 })
    {
        roundtrip(format);
        dxt_tail(format);
        exclusions(format);
        binary_alpha(format);
        cutout_edge(format);
        generator_to_reader(format);
    }
    puts("PASS: generator compression gates, retry exclusion, and record/checksum/manifest round trips");
}
