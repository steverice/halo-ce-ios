/* bp and bpf as Metal compute passes (texture_refine.h). Every filter is the NumPy reference's: Lanczos 3 down by 2
with output texel j between input texels 2j and 2j+1 (pipeline.downsample2), Lanczos 3 or Mitchell up on matching
texel centers (consistency._upsample_axis), a Gaussian of radius ceil(3 sigma), taps wrapped or clamped. */
#import <Metal/Metal.h>
#include "texture_refine.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MSL(...) #__VA_ARGS__
static const char *const refine_source = "#include <metal_stdlib>\nusing namespace metal;\n" MSL(
struct resample_params { uint in_w, in_h, out_w, out_h, channels, axis, up, down, taps, wrap; int first_tap; };
struct elementwise_params { uint count, channels; float low, high; uint clamp_output; };
struct normal_params { uint width, height; float factor; };

kernel void resample(device const float *src [[buffer(0)]], device float *dst [[buffer(1)]],
	constant resample_params &p [[buffer(2)]], device const float *weights [[buffer(3)]],
	uint2 g [[thread_position_in_grid]])
{
	if (g.x >= p.out_w || g.y >= p.out_h) return;
	uint o = p.axis == 0 ? g.x : g.y;
	int size = int(p.axis == 0 ? p.in_w : p.in_h);
	uint j = o / p.up, phase = o % p.up;
	for (uint c = 0; c < p.channels; c++) {
		float acc = 0;
		for (uint k = 0; k < p.taps; k++) {
			int s = int(j * p.down) + p.first_tap + int(k);
			s = p.wrap ? ((s % size) + size) % size : clamp(s, 0, size - 1);
			uint x = p.axis == 0 ? uint(s) : g.x, y = p.axis == 0 ? g.y : uint(s);
			acc += weights[phase * p.taps + k] * src[(y * p.in_w + x) * p.channels + c];
		}
		dst[(g.y * p.out_w + g.x) * p.channels + c] = acc;
	}
}
kernel void subtract(device const float *a [[buffer(0)]], device const float *b [[buffer(1)]],
	device float *out [[buffer(2)]], constant elementwise_params &p [[buffer(3)]], uint i [[thread_position_in_grid]])
{
	if (i < p.count) out[i] = a[i] - b[i];
}
kernel void add_to(device float *a [[buffer(0)]], device const float *b [[buffer(1)]],
	constant elementwise_params &p [[buffer(2)]], uint i [[thread_position_in_grid]])
{
	if (i < p.count) { float v = a[i] + b[i]; a[i] = p.clamp_output ? clamp(v, 0.0f, 1.0f) : v; }
}
kernel void clamp_unit(device float *a [[buffer(0)]], constant elementwise_params &p [[buffer(1)]],
	uint i [[thread_position_in_grid]])
{
	if (i < p.count) a[i] = clamp(a[i], 0.0f, 1.0f);
}
kernel void max_abs_difference(device const float *a [[buffer(0)]], device const float *b [[buffer(1)]],
	device float *out [[buffer(2)]], constant elementwise_params &p [[buffer(3)]], uint i [[thread_position_in_grid]])
{
	if (i >= p.count) return;
	float m = 0;
	for (uint c = 0; c < p.channels; c++) m = max(m, abs(a[i * p.channels + c] - b[i * p.channels + c]));
	out[i] = m;
}
kernel void smoothstep_mask(device float *m [[buffer(0)]], constant elementwise_params &p [[buffer(1)]],
	uint i [[thread_position_in_grid]])
{
	if (i >= p.count) return;
	float t = clamp((m[i] - p.low) / max(p.high - p.low, 1e-9f), 0.0f, 1.0f);
	m[i] = t * t * (3 - 2 * t);
}
kernel void blend(device float *top [[buffer(0)]], device const float *plain [[buffer(1)]],
	device const float *soft [[buffer(2)]], constant elementwise_params &p [[buffer(3)]], uint i [[thread_position_in_grid]])
{
	if (i >= p.count) return;
	float s = clamp(soft[i], 0.0f, 1.0f);
	for (uint c = 0; c < p.channels; c++) {
		uint k = i * p.channels + c;
		top[k] = clamp(top[k] * (1 - s) + clamp(plain[k], 0.0f, 1.0f) * s, 0.0f, 1.0f);
	}
}
kernel void derive_normals(device const float *h [[buffer(0)]], device float *n [[buffer(1)]],
	constant normal_params &p [[buffer(2)]], uint2 g [[thread_position_in_grid]])
{
	if (g.x >= p.width || g.y >= p.height) return;
	uint w = p.width, hh = p.height;
	float c = h[g.y * w + g.x] * p.factor;
	float left = h[g.y * w + (g.x + w - 1) % w] * p.factor, right = h[g.y * w + (g.x + 1) % w] * p.factor;
	float up = h[((g.y + hh - 1) % hh) * w + g.x] * p.factor, down = h[((g.y + 1) % hh) * w + g.x] * p.factor;
	bool peak_x = c > left && c > right, toward_left = !peak_x && left > right;
	float xi = toward_left ? -1.0f : 1.0f, xk = peak_x ? 0.0f : (toward_left ? left - c : right - c);
	bool peak_y = c > up && c > down, toward_up = !peak_y && up > down;
	float yj = toward_up ? -1.0f : 1.0f, yk = peak_y ? 0.0f : (toward_up ? up - c : down - c);
	float3 v = float3(-xk * yj, -xi * yk, xi * yj);
	v *= sign(v.z);
	v = normalize(v);
	uint k = (g.y * w + g.x) * 3;
	n[k] = v.x; n[k + 1] = v.y; n[k + 2] = v.z;
}
kernel void normal_xy_difference(device const float *a [[buffer(0)]], device const float *b [[buffer(1)]],
	device float *out [[buffer(2)]], constant elementwise_params &p [[buffer(3)]], uint i [[thread_position_in_grid]])
{
	if (i < p.count) out[i] = max(abs(a[i * 3] - b[i * 3]), abs(a[i * 3 + 1] - b[i * 3 + 1])) * 0.5f;
}
kernel void blend_normals(device float *n [[buffer(0)]], device const float *target [[buffer(1)]],
	device const float *soft [[buffer(2)]], constant elementwise_params &p [[buffer(3)]], uint i [[thread_position_in_grid]])
{
	if (i >= p.count) return;
	float s = clamp(soft[i], 0.0f, 1.0f);
	float2 xy = float2(n[i * 3], n[i * 3 + 1]) * (1 - s) + float2(target[i * 3], target[i * 3 + 1]) * s;
	float3 v = float3(xy, sqrt(max(1 - dot(xy, xy), 0.0f)));
	v /= max(length(v), 1e-6f);
	n[i * 3] = v.x; n[i * 3 + 1] = v.y; n[i * 3 + 2] = v.z;
}
);

struct resample_params { uint32_t in_w, in_h, out_w, out_h, channels, axis, up, down, taps, wrap; int32_t first_tap; };
struct elementwise_params { uint32_t count, channels; float low, high; uint32_t clamp_output; };
struct normal_params { uint32_t width, height; float factor; };
/* weights: kept alive by the context's `keep` array (or a local strong reference for the Gaussian) */
struct filter { __unsafe_unretained id<MTLBuffer> weights; uint32_t taps, up, down; int32_t first_tap; };

/* the opaque struct texture_refine_context is this object, bridged */
@interface TextureRefineContext : NSObject
{
@public
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	id<MTLComputePipelineState> resample, subtract, add_to, clamp_unit, max_abs_difference, smoothstep_mask, blend,
		derive_normals, normal_xy_difference, blend_normals;
	NSMutableArray *keep;
	/* an allocation failed during this call: the encoding helpers stop dispatching and hand back `placeholder` (never
	   bound to a dispatch) so no nil reaches an array literal; the call then ends encoding and returns -1 */
	BOOL failed;
	id<MTLBuffer> placeholder;
	struct filter down2, up_lanczos[9], up_mitchell[9];   /* up_*[scale] for scale 2, 4 and 8 */
}
@end
@implementation TextureRefineContext
@end

static TextureRefineContext *unwrap(struct texture_refine_context *handle)
{
	return (__bridge TextureRefineContext *)(void *)handle;
}

static double sinc(double x) { return x == 0 ? 1 : sin(M_PI * x) / (M_PI * x); }
static double lanczos3(double x) { x = fabs(x); return x < 3 ? sinc(x) * sinc(x / 3) : 0; }
static double mitchell(double x)
{
	const double b = 1.0 / 3, c = 1.0 / 3;
	x = fabs(x);
	if (x < 1)
		return ((12 - 9 * b - 6 * c) * x * x * x + (-18 + 12 * b + 6 * c) * x * x + (6 - 2 * b)) / 6;
	if (x < 2)
		return ((-b - 6 * c) * x * x * x + (6 * b + 30 * c) * x * x + (-12 * b - 48 * c) * x + (8 * b + 24 * c)) / 6;
	return 0;
}

static struct filter make_filter(TextureRefineContext *context, double (*f)(double), int support, uint32_t up,
	int downsample)
{
	id<MTLBuffer> strong;
	struct filter out = { nil, 0, up, downsample ? 2 : 1, 0 };
	float weights[8 * 16];
	uint32_t phase, k;

	if (downsample)   /* pipeline.downsample2: taps -2s+1 .. 2s, weights f((t - 0.5) / 2) */
	{
		double sum = 0;

		out.taps = 4 * support; out.first_tap = -2 * support + 1; out.up = 1;
		for (k = 0; k < out.taps; k++)
			sum += weights[k] = (float)f(((int)k + out.first_tap - 0.5) / 2);
		for (k = 0; k < out.taps; k++)
			weights[k] = (float)(weights[k] / sum);
	}
	else              /* consistency._upsample_axis: taps -s .. s, per phase f(t - frac) */
	{
		out.taps = 2 * support + 1; out.first_tap = -support;
		for (phase = 0; phase < up; phase++)
		{
			double frac = (phase + 0.5) / up - 0.5, sum = 0;

			for (k = 0; k < out.taps; k++)
				sum += weights[phase * out.taps + k] = (float)f((int)k + out.first_tap - frac);
			for (k = 0; k < out.taps; k++)
				weights[phase * out.taps + k] = (float)(weights[phase * out.taps + k] / sum);
		}
	}
	strong = [context->device newBufferWithBytes:weights length:sizeof(float) * out.taps * out.up
		options:MTLResourceStorageModeShared];
	if (strong)
		[context->keep addObject:strong];
	out.weights = strong;   /* nil: texture_refine_create fails */
	return out;
}

struct texture_refine_context *texture_refine_create(void *device_pointer, char *error, size_t error_size)
{
	TextureRefineContext *context = [TextureRefineContext new];
	id<MTLDevice> device = device_pointer ? (__bridge id<MTLDevice>)device_pointer : MTLCreateSystemDefaultDevice();
	NSError *problem = nil;
	id<MTLLibrary> library;

	if (!device)
	{
		snprintf(error, error_size, "no Metal device");
		return NULL;
	}
	library = [device newLibraryWithSource:@(refine_source) options:nil error:&problem];
	if (!library)
	{
		snprintf(error, error_size, "texture_refine kernels: %s", problem.description.UTF8String);
		return NULL;
	}
	context->device = device;
	context->queue = [device newCommandQueue];
	context->keep = [NSMutableArray new];
#define PIPELINE(name) context->name = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@#name] error:&problem]
	PIPELINE(resample); PIPELINE(subtract); PIPELINE(add_to); PIPELINE(clamp_unit); PIPELINE(max_abs_difference);
	PIPELINE(smoothstep_mask);
	PIPELINE(blend); PIPELINE(derive_normals); PIPELINE(normal_xy_difference); PIPELINE(blend_normals);
#undef PIPELINE
	if (problem)
	{
		snprintf(error, error_size, "texture_refine pipelines: %s", problem.description.UTF8String);
		return NULL;
	}
	context->down2 = make_filter(context, lanczos3, 3, 1, 1);
	context->up_lanczos[2] = make_filter(context, lanczos3, 3, 2, 0);
	context->up_lanczos[4] = make_filter(context, lanczos3, 3, 4, 0);
	context->up_lanczos[8] = make_filter(context, lanczos3, 3, 8, 0);
	context->up_mitchell[2] = make_filter(context, mitchell, 2, 2, 0);
	context->up_mitchell[4] = make_filter(context, mitchell, 2, 4, 0);
	context->up_mitchell[8] = make_filter(context, mitchell, 2, 8, 0);
	context->placeholder = [device newBufferWithLength:sizeof(float) options:MTLResourceStorageModeShared];
	if (!context->queue || !context->placeholder || !context->down2.weights || !context->up_lanczos[2].weights
		|| !context->up_lanczos[4].weights || !context->up_lanczos[8].weights || !context->up_mitchell[2].weights
		|| !context->up_mitchell[4].weights || !context->up_mitchell[8].weights)
	{
		snprintf(error, error_size, "texture_refine: could not allocate the command queue or filter weights");
		return NULL;
	}
	return (struct texture_refine_context *)(__bridge_retained void *)context;
}

void texture_refine_destroy(struct texture_refine_context *handle)
{
	if (handle)
		(void)(__bridge_transfer TextureRefineContext *)(void *)handle;
}

/* a nil allocation (over maxBufferLength, or memory pressure) marks the call failed and yields the placeholder */
static id<MTLBuffer> checked(TextureRefineContext *context, id<MTLBuffer> allocated, size_t floats)
{
	if (allocated)
		return allocated;
	if (!context->failed)
		fprintf(stderr, "texture_refine: could not allocate a buffer of %zu bytes\n", floats * sizeof(float));
	context->failed = YES;
	return context->placeholder;
}

static id<MTLBuffer> buffer(TextureRefineContext *context, size_t floats)
{
	return checked(context, [context->device newBufferWithLength:floats * sizeof(float)
		options:MTLResourceStorageModeShared], floats);
}

static id<MTLBuffer> copy_of(TextureRefineContext *context, const float *values, size_t floats)
{
	return checked(context, [context->device newBufferWithBytes:values length:floats * sizeof(float)
		options:MTLResourceStorageModeShared], floats);
}

static void dispatch_1d(id<MTLComputeCommandEncoder> encoder, id<MTLComputePipelineState> pipeline, size_t count)
{
	NSUInteger width = MIN(pipeline.maxTotalThreadsPerThreadgroup, 256);

	[encoder setComputePipelineState:pipeline];
	[encoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
}

/* one axis of a separable filter. A 1-long axis is skipped when downsampling (pipeline.downsample2 skips it) but
   resampled when upsampling: every tap then wraps or clamps to texel 0 with weights summing to 1, which is NumPy's
   broadcast of the axis consistency.upsample skips, and the result has the full size the later passes index */
static id<MTLBuffer> pass(TextureRefineContext *context, id<MTLComputeCommandEncoder> encoder,
	id<MTLBuffer> src, uint32_t *w, uint32_t *h, uint32_t channels, uint32_t axis, const struct filter *f, int wrap)
{
	uint32_t size = axis == 0 ? *w : *h;
	struct resample_params p;
	id<MTLBuffer> dst;

	if (context->failed)
		return context->placeholder;
	if (size == 1 && f->down == 2)
		return src;
	p.in_w = *w; p.in_h = *h;
	p.out_w = axis == 0 ? *w * f->up / f->down : *w;
	p.out_h = axis == 1 ? *h * f->up / f->down : *h;
	p.channels = channels; p.axis = axis; p.up = f->up; p.down = f->down; p.taps = f->taps; p.wrap = (uint32_t)wrap;
	p.first_tap = f->first_tap;
	dst = buffer(context, (size_t)p.out_w * p.out_h * channels);
	if (context->failed)
		return dst;
	[encoder setComputePipelineState:context->resample];
	[encoder setBuffer:src offset:0 atIndex:0];
	[encoder setBuffer:dst offset:0 atIndex:1];
	[encoder setBytes:&p length:sizeof(p) atIndex:2];
	[encoder setBuffer:f->weights offset:0 atIndex:3];
	[encoder dispatchThreads:MTLSizeMake(p.out_w, p.out_h, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
	*w = p.out_w; *h = p.out_h;
	return dst;
}

/* pipeline.downsample2 (rows, then columns) log2(scale) times: consistency.down */
static id<MTLBuffer> down(TextureRefineContext *context, id<MTLComputeCommandEncoder> encoder,
	id<MTLBuffer> src, uint32_t w, uint32_t h, uint32_t channels, uint32_t scale, int wrap)
{
	for (; scale > 1; scale /= 2)
	{
		src = pass(context, encoder, src, &w, &h, channels, 1, &context->down2, wrap);
		src = pass(context, encoder, src, &w, &h, channels, 0, &context->down2, wrap);
	}
	return src;
}

/* consistency.upsample (rows, then columns) */
static id<MTLBuffer> up(TextureRefineContext *context, id<MTLComputeCommandEncoder> encoder,
	id<MTLBuffer> src, uint32_t w, uint32_t h, uint32_t channels, uint32_t scale, int mitchell_filter, int wrap)
{
	const struct filter *f = mitchell_filter ? &context->up_mitchell[scale] : &context->up_lanczos[scale];

	src = pass(context, encoder, src, &w, &h, channels, 1, f, wrap);
	return pass(context, encoder, src, &w, &h, channels, 0, f, wrap);
}

static void elementwise(TextureRefineContext *context, id<MTLComputeCommandEncoder> encoder,
	id<MTLComputePipelineState> pipeline, NSArray<id<MTLBuffer>> *buffers, struct elementwise_params p)
{
	NSUInteger i;

	if (context->failed)
		return;
	for (i = 0; i < buffers.count; i++)
		[encoder setBuffer:buffers[i] offset:0 atIndex:i];
	[encoder setBytes:&p length:sizeof(p) atIndex:buffers.count];
	dispatch_1d(encoder, pipeline, p.count);
}

/* bump.derive_normals of a (w, h) height field times factor (texels at this size), wrapping, into 3 floats a texel */
static void derive(TextureRefineContext *context, id<MTLComputeCommandEncoder> encoder, id<MTLBuffer> height,
	id<MTLBuffer> normals, uint32_t w, uint32_t h, float factor)
{
	struct normal_params p = { w, h, factor };

	if (context->failed)
		return;
	[encoder setComputePipelineState:context->derive_normals];
	[encoder setBuffer:height offset:0 atIndex:0];
	[encoder setBuffer:normals offset:0 atIndex:1];
	[encoder setBytes:&p length:sizeof(p) atIndex:2];
	[encoder dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
}

/* the mismatch mask at the original's size: blur (a Gaussian of radius ceil(3 sigma)), then the smoothstep */
static void finish_mask(TextureRefineContext *context, id<MTLComputeCommandEncoder> encoder,
	id<MTLBuffer> *difference, uint32_t w, uint32_t h, const struct texture_refine_params *params, int wrap)
{
	if (params->sigma > 0)   /* consistency.blur: none at sigma 0 */
	{
		int radius = (int)fmax(1, ceil(3 * params->sigma)), k;
		NSMutableData *data = [NSMutableData dataWithLength:sizeof(float) * (2 * radius + 1)];
		float *weights = data.mutableBytes;
		double sum = 0;
		struct filter gaussian;
		id<MTLBuffer> strong;
		uint32_t bw = w, bh = h;

		for (k = -radius; k <= radius; k++)
			sum += weights[k + radius] = (float)exp(-0.5 * (k / params->sigma) * (k / params->sigma));
		for (k = 0; k < 2 * radius + 1; k++)
			weights[k] = (float)(weights[k] / sum);
		strong = copy_of(context, weights, 2 * radius + 1);   /* the encoder retains it once set */
		gaussian = (struct filter){ strong, (uint32_t)(2 * radius + 1), 1, 1, -radius };
		*difference = pass(context, encoder, *difference, &bw, &bh, 1, 1, &gaussian, wrap);
		*difference = pass(context, encoder, *difference, &bw, &bh, 1, 0, &gaussian, wrap);
	}
	elementwise(context, encoder, context->smoothstep_mask, @[ *difference ],
		(struct elementwise_params){ w * h, 1, params->low / 255, params->high / 255, 0 });
}

/* ends encoding; unless an allocation failed, commits and waits. 0 when everything was allocated and the GPU
   reported no error */
static int run(TextureRefineContext *context, id<MTLCommandBuffer> command, id<MTLComputeCommandEncoder> encoder)
{
	[encoder endEncoding];
	if (context->failed)
		return -1;
	[command commit];
	[command waitUntilCompleted];
	return command.error ? -1 : 0;
}

static int valid(uint32_t width, uint32_t height, uint32_t scale, const struct texture_refine_params *params)
{
	return width > 0 && height > 0 && (scale == 2 || scale == 4 || scale == 8) && params;
}

int texture_refine_color(struct texture_refine_context *handle, float *top, const float *reference, uint32_t width,
	uint32_t height, uint32_t channels, uint32_t scale, int wrap, const struct texture_refine_params *params,
	int fallback, float *mask)
{
	if (!handle || !valid(width, height, scale, params) || channels < 1 || channels > 4)
		return -1;
	unwrap(handle)->failed = NO;   /* this call's allocations decide */
	@autoreleasepool
	{
		TextureRefineContext *context = unwrap(handle);
		size_t small = (size_t)width * height, large = small * scale * scale;
		uint32_t big_w = width * scale, big_h = height * scale, i;
		id<MTLBuffer> out = copy_of(context, top, large * channels), raw = copy_of(context, top, large * channels);
		id<MTLBuffer> ref = copy_of(context, reference, small * channels);
		id<MTLBuffer> difference = buffer(context, small), residual = buffer(context, small * channels);
		id<MTLCommandBuffer> command = [context->queue commandBuffer];
		id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];   /* serial: each pass sees the last */

		if (!encoder)
		{
			fprintf(stderr, "texture_refine: no command buffer or encoder\n");
			return -1;
		}

		/* the mismatch on the raw output, before back-projection (consistency.mismatch) */
		elementwise(context, encoder, context->max_abs_difference, @[ ref, down(context, encoder, raw, big_w, big_h, channels,
			scale, wrap), difference ], (struct elementwise_params){ (uint32_t)small, channels, 0, 0, 0 });
		finish_mask(context, encoder, &difference, width, height, params, wrap);
		/* back-projection, clipped to 0..1 first and after each round (consistency.back_project) */
		elementwise(context, encoder, context->clamp_unit, @[ out ], (struct elementwise_params){ (uint32_t)(large * channels),
			0, 0, 0, 0 });
		for (i = 0; i < params->iterations; i++)
		{
			elementwise(context, encoder, context->subtract, @[ ref, down(context, encoder, out, big_w, big_h, channels, scale,
				wrap), residual ], (struct elementwise_params){ (uint32_t)(small * channels), 0, 0, 0, 0 });
			elementwise(context, encoder, context->add_to, @[ out, up(context, encoder, residual, width, height, channels, scale,
				0, wrap) ], (struct elementwise_params){ (uint32_t)(large * channels), 0, 0, 0, 1 });
		}
		if (fallback)   /* consistency.fallback_blend */
			elementwise(context, encoder, context->blend, @[ out, up(context, encoder, ref, width, height, channels, scale, 0,
				wrap), up(context, encoder, difference, width, height, 1, scale, 1, wrap) ],
				(struct elementwise_params){ (uint32_t)large, channels, 0, 0, 1 });
		if (run(context, command, encoder))
			return -1;
		memcpy(top, out.contents, large * channels * sizeof(float));
		if (mask)
			memcpy(mask, difference.contents, small * sizeof(float));
		return 0;
	}
}

int texture_refine_bump(struct texture_refine_context *handle, const float *top_height, const float *original_height,
	uint32_t width, uint32_t height, uint32_t scale, const struct texture_refine_params *params, int fallback,
	float *normals, float *mask)
{
	if (!handle || !valid(width, height, scale, params) || !normals)
		return -1;
	unwrap(handle)->failed = NO;   /* this call's allocations decide */
	@autoreleasepool
	{
		TextureRefineContext *context = unwrap(handle);
		size_t small = (size_t)width * height, large = small * scale * scale;
		uint32_t big_w = width * scale, big_h = height * scale, i;
		id<MTLBuffer> out = copy_of(context, top_height, large), ref = copy_of(context, original_height, small);
		id<MTLBuffer> n_small = buffer(context, small * 3), n_ref = buffer(context, small * 3);
		id<MTLBuffer> n_out = buffer(context, large * 3);
		id<MTLBuffer> difference = buffer(context, small), residual = buffer(context, small);
		id<MTLCommandBuffer> command = [context->queue commandBuffer];
		id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];

		if (!encoder)
		{
			fprintf(stderr, "texture_refine: no command buffer or encoder\n");
			return -1;
		}
		/* the mismatch: both sides re-derived at 1x, the larger X or Y difference of the float normals over 2, so in
		   encoded levels / 255 (bump.refine) */
		derive(context, encoder, down(context, encoder, out, big_w, big_h, 1, scale, 1), n_small, width, height, 1);
		derive(context, encoder, ref, n_ref, width, height, 1);
		elementwise(context, encoder, context->normal_xy_difference, @[ n_small, n_ref, difference ],
			(struct elementwise_params){ (uint32_t)small, 3, 0, 0, 0 });
		finish_mask(context, encoder, &difference, width, height, params, 1);
		/* back-projection on the height, unclamped */
		for (i = 0; i < params->iterations; i++)
		{
			elementwise(context, encoder, context->subtract, @[ ref, down(context, encoder, out, big_w, big_h, 1, scale, 1),
				residual ], (struct elementwise_params){ (uint32_t)small, 0, 0, 0, 0 });
			elementwise(context, encoder, context->add_to, @[ out, up(context, encoder, residual, width, height, 1, scale, 0, 1) ],
				(struct elementwise_params){ (uint32_t)large, 0, 0, 0, 0 });
		}
		/* at scale x a texel is 1/scale as wide: heights in texels grow by scale */
		derive(context, encoder, out, n_out, big_w, big_h, (float)scale);
		if (fallback)   /* toward the round trip's normals from a Lanczos resize of the original height */
		{
			id<MTLBuffer> n_target = buffer(context, large * 3);

			derive(context, encoder, up(context, encoder, ref, width, height, 1, scale, 0, 1), n_target, big_w, big_h,
				(float)scale);
			elementwise(context, encoder, context->blend_normals, @[ n_out, n_target, up(context, encoder, difference, width,
				height, 1, scale, 1, 1) ], (struct elementwise_params){ (uint32_t)large, 3, 0, 0, 0 });
		}
		if (run(context, command, encoder))
			return -1;
		memcpy(normals, n_out.contents, large * 3 * sizeof(float));
		if (mask)
			memcpy(mask, difference.contents, small * sizeof(float));
		return 0;
	}
}
