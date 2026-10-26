/* The real sampler construction, lookup caches and bind_stages body are
inserted by tools/test_metal_border.py. Only the external Metal resource and
encoder boundaries are stand-ins; no GPU device or command buffer is made. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "gpu.h"
#include "metal_state_cache.h"
#include <assert.h>
#include <stdio.h>

@interface ProbeTexture : NSObject
@property MTLPixelFormat pixelFormat;
@property MTLTextureType textureType;
@end
@implementation ProbeTexture
@end

@interface MetalTexture : NSObject
{
@public
    struct gpu_texture_description description;
    id<MTLTexture> texture;
    unsigned long used;
    id<MTLTexture> view_parent;
}
@end
@implementation MetalTexture
@end

@interface ProbeSampler : NSObject
@property MTLSamplerDescriptor *descriptor;
@end
@implementation ProbeSampler
@end

@interface ProbeDevice : NSObject
@property unsigned creations;
@property BOOL failCreation;
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor;
@end
@implementation ProbeDevice
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor
{
    self.creations++;
    if (self.failCreation) return nil;
    ProbeSampler *sampler = [ProbeSampler new];
    sampler.descriptor = [descriptor copy];
    return (id<MTLSamplerState>)sampler;
}
@end

@interface ProbeEncoder : NSObject
{
@public
    id<MTLTexture> textures[GPU_STAGE_COUNT];
    id<MTLSamplerState> samplers[GPU_STAGE_COUNT * 2];
    unsigned textureCalls, samplerCalls, colorCalls;
    float colors[GPU_STAGE_COUNT][4];
}
- (void)setFragmentTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index;
- (void)setFragmentSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index;
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index;
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages;
@end
@implementation ProbeEncoder
- (void)setFragmentTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index
{ textures[index] = texture; textureCalls++; }
- (void)setFragmentSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index
{ samplers[index] = sampler; samplerCalls++; }
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index
{ assert(index == 3 && length == sizeof(colors)); memcpy(colors, bytes, length); colorCalls++; }
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{ (void)buffer; (void)offset; (void)index; assert(!"unexpected foveation buffer"); }
- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages
{ (void)resource; (void)usage; (void)stages; assert(!"unexpected view of another texture"); }
@end

static ProbeDevice *device;
static ProbeEncoder *encoder;
static BOOL border_colors;
static NSMutableDictionary<NSData *, id<MTLSamplerState>> *samplers;
static struct metal_state_cache state_cache;
static id<MTLTexture> empty_textures[4];
static id<MTLSamplerState> empty_sampler;
static MetalTexture *records[16];
static MetalTexture *texture_record(gpu_texture handle) { return handle < 16 ? records[handle] : nil; }
static void use_texture(MetalTexture *record) { if (record) record->used++; }
static BOOL foveation_sizes(MetalTexture *record, MTLSize *screen, MTLSize *physical)
{ (void)record; (void)screen; (void)physical; return NO; }
static id<MTLBuffer> foveation_parameters(MetalTexture *record) { (void)record; return nil; }

/* PRODUCTION_SAMPLER_AND_BINDING */

static MetalTexture *resource(MTLPixelFormat format, MTLTextureType type, BOOL storage)
{
    MetalTexture *record = [MetalTexture new];
    record->description.type = GPU_TEXTURE_2D;
    record->description.format = GPU_FORMAT_BGRA8;
    if (storage)
    {
        ProbeTexture *texture = [ProbeTexture new];
        texture.pixelFormat = format;
        texture.textureType = type;
        record->texture = (id<MTLTexture>)texture;
    }
    return record;
}

static void expect_mode(unsigned stage, MTLSamplerAddressMode s, MTLSamplerAddressMode t)
{
    ProbeSampler *sampler = (ProbeSampler *)encoder->samplers[stage];
    assert(sampler && "sampler creation must succeed; a nil binding is not proof");
    MTLSamplerDescriptor *descriptor = sampler.descriptor;
    if (descriptor.sAddressMode != s || descriptor.tAddressMode != t)
    {
        fprintf(stderr, "stage %u: address modes %lu/%lu, expected %lu/%lu\n", stage,
            (unsigned long)descriptor.sAddressMode, (unsigned long)descriptor.tAddressMode,
            (unsigned long)s, (unsigned long)t);
        abort();
    }
}

static struct gpu_draw base_draw(void)
{
    struct gpu_draw draw = { 0 };
    for (int stage = 0; stage < GPU_STAGE_COUNT; stage++)
    {
        draw.stages[stage].type = GPU_TEXTURE_2D;
        draw.stages[stage].texture = 1;
        draw.stages[stage].sampler.address_u = GPU_ADDRESS_BORDER;
        draw.stages[stage].sampler.address_v = GPU_ADDRESS_BORDER;
        draw.stages[stage].sampler.address_w = GPU_ADDRESS_CLAMP_TO_EDGE;
        draw.stages[stage].sampler.min_filter = GPU_FILTER_LINEAR;
        draw.stages[stage].sampler.mag_filter = GPU_FILTER_LINEAR;
    }
    return draw;
}

static void check_resource_transitions(MTLSamplerAddressMode handled)
{
    struct gpu_draw draw = base_draw();
    bind_stages(&draw, 0);
    expect_mode(0, handled, handled);
    id<MTLSamplerState> first = encoder->samplers[0];
    unsigned calls = encoder->samplerCalls, textures = encoder->textureCalls;
    unsigned long used = records[1]->used;
    bind_stages(&draw, 0);
    assert(encoder->samplerCalls == calls && encoder->textureCalls == textures);
    assert(records[1]->used == used + GPU_STAGE_COUNT);
    draw.stages[0].texture = 2;
    bind_stages(&draw, 0);
    assert(encoder->samplers[0] == first && encoder->samplerCalls == calls);
    assert(encoder->textureCalls == textures + 1);

    const gpu_texture ineligible[] = {3, 4, 5, 6, 7, 15};
    for (unsigned i = 0; i < sizeof(ineligible)/sizeof(ineligible[0]); i++)
    {
        draw.stages[0].texture = ineligible[i];
        bind_stages(&draw, 0);
        MTLSamplerAddressMode expected = border_colors ? handled : MTLSamplerAddressModeClampToEdge;
        expect_mode(0, expected, expected);
        assert(encoder->samplers[0] != first || border_colors);
        draw.stages[0].texture = 1;
        bind_stages(&draw, 0);
        expect_mode(0, handled, handled);
        assert(encoder->samplers[0] == first);
    }
    /* A previously unuploaded record gains storage under the same handle. */
    draw.stages[0].texture = 4;
    bind_stages(&draw, 0);
    records[4]->texture = records[2]->texture;
    bind_stages(&draw, 0);
    expect_mode(0, handled, handled);
    assert(encoder->samplers[0] == first);
    /* A destroyed record's reused handle must not retain eligibility. */
    records[4] = resource(MTLPixelFormatDepth32Float, MTLTextureType2D, YES);
    bind_stages(&draw, 0);
    expect_mode(0, border_colors ? handled : MTLSamplerAddressModeClampToEdge,
        border_colors ? handled : MTLSamplerAddressModeClampToEdge);
    for (unsigned type = GPU_TEXTURE_3D; type <= GPU_TEXTURE_CUBE; type++)
    {
        draw.stages[0].texture = 1;
        draw.stages[0].type = type;
        bind_stages(&draw, 0);
        expect_mode(0, border_colors ? handled : MTLSamplerAddressModeClampToEdge,
            border_colors ? handled : MTLSamplerAddressModeClampToEdge);
    }
    draw.stages[0].type = 0;
    bind_stages(&draw, 0);
    assert(encoder->samplers[0] == empty_sampler);
}

static void check_axes_colors_and_dictionary(MTLSamplerAddressMode handled)
{
    struct gpu_draw draw = base_draw();
    draw.stages[0].sampler.min_filter = GPU_FILTER_ANISOTROPIC;
    draw.stages[0].sampler.mag_filter = GPU_FILTER_POINT;
    draw.stages[0].sampler.mip_filter = GPU_FILTER_LINEAR;
    draw.stages[0].sampler.max_mip_level = 2;
    draw.stages[0].sampler.max_anisotropy = 32;
    bind_stages(&draw, 0);
    expect_mode(0, handled, handled);
    MTLSamplerDescriptor *filtered = ((ProbeSampler *)encoder->samplers[0]).descriptor;
    assert(filtered.minFilter == MTLSamplerMinMagFilterLinear);
    assert(filtered.magFilter == MTLSamplerMinMagFilterNearest);
    assert(filtered.mipFilter == MTLSamplerMipFilterLinear);
    assert(filtered.lodMinClamp == 2 && filtered.maxAnisotropy == 16);
    const uint32_t colors[] = {0xff000000, 0xffffffff};
    for (int i = 0; i < 2; i++)
    {
        draw.stages[0].sampler.border_color = colors[i];
        bind_stages(&draw, exact_borders(&draw));
        expect_mode(0, border_colors ? handled : MTLSamplerAddressModeClampToEdge,
            border_colors ? handled : MTLSamplerAddressModeClampToEdge);
        if (border_colors)
        {
            MTLSamplerDescriptor *descriptor = ((ProbeSampler *)encoder->samplers[0]).descriptor;
            assert(descriptor.borderColor == (i ? MTLSamplerBorderColorOpaqueWhite : MTLSamplerBorderColorOpaqueBlack));
        }
    }
    draw = base_draw();
    const uint8_t modes[][2] = {
        {GPU_ADDRESS_BORDER,GPU_ADDRESS_WRAP}, {GPU_ADDRESS_WRAP,GPU_ADDRESS_BORDER},
        {GPU_ADDRESS_BORDER,GPU_ADDRESS_MIRROR}, {GPU_ADDRESS_BORDER,GPU_ADDRESS_CLAMP},
        {GPU_ADDRESS_BORDER,GPU_ADDRESS_BORDER}, {GPU_ADDRESS_CLAMP,GPU_ADDRESS_CLAMP},
    };
    const MTLSamplerAddressMode expected[][2] = {
        {handled,MTLSamplerAddressModeRepeat}, {MTLSamplerAddressModeRepeat,handled},
        {handled,MTLSamplerAddressModeMirrorRepeat}, {handled,MTLSamplerAddressModeClampToEdge},
        {handled,handled}, {MTLSamplerAddressModeClampToEdge,MTLSamplerAddressModeClampToEdge},
    };
    for (unsigned i = 0; i < sizeof(modes)/sizeof(modes[0]); i++)
    {
        draw.stages[0].sampler.address_u = modes[i][0];
        draw.stages[0].sampler.address_v = modes[i][1];
        draw.stages[0].sampler.address_w = GPU_ADDRESS_BORDER;
        bind_stages(&draw, 0);
        expect_mode(0, expected[i][0], expected[i][1]);
    }
    draw = base_draw();
    for (int stage = 0; stage < GPU_STAGE_COUNT; stage++)
        draw.stages[stage].texture = stage & 1 ? 3 : 1;
    bind_stages(&draw, 0);
    id<MTLSamplerState> saved[GPU_STAGE_COUNT];
    for (int stage = 0; stage < GPU_STAGE_COUNT; stage++)
    {
        saved[stage] = encoder->samplers[stage];
        MTLSamplerAddressMode expected = stage & 1 && !border_colors ? MTLSamplerAddressModeClampToEdge : handled;
        expect_mode(stage, expected, expected);
    }
    @autoreleasepool
    {
        for (int stage = 0; stage < GPU_STAGE_COUNT; stage++) draw.stages[stage].sampler.border_color = 0x46000000;
        bind_stages(&draw, exact_borders(&draw));
        for (int stage = 0; stage < GPU_STAGE_COUNT; stage++)
            expect_mode(stage, border_colors ? handled : MTLSamplerAddressModeClampToEdge,
                border_colors ? handled : MTLSamplerAddressModeClampToEdge);
        if (border_colors)
        {
            assert(exact_borders(&draw) == 15 && encoder->colorCalls);
            ProbeSampler *white = (ProbeSampler *)encoder->samplers[4];
            assert(white.descriptor.borderColor == MTLSamplerBorderColorOpaqueWhite);
            assert(encoder->colors[0][3] == 70.0f/255.0f);
        }
        else assert(exact_borders(&draw) == 0 && encoder->colorCalls == 0);
    }
    /* Evict every front-cache slot; revisit through the dictionary after the pool. */
    memset(&sampler_cache, 0, sizeof(sampler_cache));
    for (int stage = 0; stage < GPU_STAGE_COUNT; stage++) draw.stages[stage].sampler.border_color = 0;
    bind_stages(&draw, 0);
    for (int stage = 0; stage < GPU_STAGE_COUNT; stage++) assert(encoder->samplers[stage] == saved[stage]);

    unsigned calls = encoder->samplerCalls;
    metal_state_reset(&state_cache);
    bind_stages(&draw, 0);
    assert(encoder->samplerCalls == calls + GPU_STAGE_COUNT);
    state_cache.enabled = 0;
    calls = encoder->samplerCalls;
    bind_stages(&draw, 0);
    bind_stages(&draw, 0);
    assert(encoder->samplerCalls == calls + GPU_STAGE_COUNT * 2);
    /* Sampler creation failure stays nil and retries; it must not masquerade as EDGE. */
    device.failCreation = YES;
    draw.stages[0].sampler.max_mip_level = 987;
    calls = device.creations;
    bind_stages(&draw, 0);
    assert(encoder->samplers[0] == nil);
    bind_stages(&draw, 0);
    assert(encoder->samplers[0] == nil && device.creations == calls + 2);
}

int main(int argc, const char **argv)
{
    @autoreleasepool
    {
        assert(argc == 2);
        border_colors = !strcmp(argv[1], "native");
        device = [ProbeDevice new]; encoder = [ProbeEncoder new];
        samplers = [NSMutableDictionary dictionary];
        empty_sampler = (id<MTLSamplerState>)[ProbeSampler new];
        for (int i = 0; i < 4; i++) empty_textures[i] = (id<MTLTexture>)[ProbeTexture new];
        metal_state_initialize(&state_cache, 1);
        records[1] = resource(MTLPixelFormatBGRA8Unorm, MTLTextureType2D, YES);
        records[1]->description.usage = GPU_USAGE_RENDER_TARGET;
        records[2] = resource(MTLPixelFormatBGRA8Unorm, MTLTextureType2D, YES);
        records[3] = resource(MTLPixelFormatDepth32Float, MTLTextureType2D, YES);
        records[4] = resource(MTLPixelFormatBGRA8Unorm, MTLTextureType2D, NO);
        records[5] = resource(MTLPixelFormatBGRA8Unorm, MTLTextureType2D, YES);
        records[5]->description.foveated_eye = 1;
        records[6] = resource(MTLPixelFormatBGRA8Unorm, MTLTextureType3D, YES);
        records[7] = resource(MTLPixelFormatR8Unorm, MTLTextureType2D, YES);
        MTLSamplerAddressMode handled = border_colors ? MTLSamplerAddressModeClampToBorderColor : MTLSamplerAddressModeClampToZero;
        check_resource_transitions(handled);
        check_axes_colors_and_dictionary(handled);
        printf("Metal border binding: %s resource, format, cache and encoder transitions passed\n", argv[1]);
    }
    return 0;
}
