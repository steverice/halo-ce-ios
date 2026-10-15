/*
GPU_METAL.M

The Metal backend of gpu.h (display.renderer = "metal"), in the iOS and tvOS
host. It reports the capabilities GL reports on Apple's OpenGL ES 3.0, so the
front end does exactly what it does under GL, and it reproduces GL's results
where Metal works differently:

- GL applies every upload in order with the draws around it; Metal runs a
  frame's commands after they are recorded. A CPU write into a buffer or
  texture that a recorded or running command buffer reads gets new storage
  instead ("renaming", buffer_write and texture_upload).
- Frames in flight are bounded by a semaphore with FRAMES slots, signaled when
  each frame's last command buffer completes; per-frame memory (streams,
  constant snapshots) has a slot per frame.
- Render passes begin at the first draw or clear for a pair of targets and end
  when the pair changes, at copies, mipmap generation, readback and Present.

Every entry point runs in an autorelease pool: the guest runs on the UI thread
but never returns to its run loop, which would otherwise drain them.
*/

#include "gpu.h"
#include "ios_host.h"
#include "metal_state_cache.h"
/* GPU_VALIDATION_REPORT: this backend's fallbacks and skips, by name, in a
build with render validation; nothing otherwise */
#include "gpu_validate.h"
#include "metal_compile_async.h"
#include "host_benchmark.h"
#include "halo_display.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/QuartzCore.h>
#include <TargetConditionals.h>
#if __has_include(<MetalFX/MetalFX.h>) && !TARGET_OS_SIMULATOR
#import <MetalFX/MetalFX.h>
#define HAVE_METALFX 1
#endif
#include <math.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <mach/mach.h>

/* the platform layer's (host_gpu.c) */
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* frames in flight, as gpu_gl.c's STREAM_BUFFER_RING */
#define FRAMES 3
/* the size of a transient chunk: gpu_gl.c's STREAM_BUFFER_SIZE */
#define CHUNK_SIZE (16 * 1024 * 1024)
/* constant buffer offsets: Metal's strictest alignment on any family */
#define CONSTANT_ALIGNMENT 256

/* ---------- records

Handles index tables of these; 0 is none. */

@interface MetalTexture : NSObject
{
@public
	struct gpu_texture_description description;
	id<MTLTexture> texture;          /* nil until an upload texture's first upload */
	unsigned long levels;            /* description.levels, at most what the size allows */
	uint64_t used;                   /* the last command buffer serial that read or wrote it */
}
@end
@implementation MetalTexture
@end

@interface MetalBuffer : NSObject
{
@public
	id<MTLBuffer> buffer;
	uint64_t used;
	BOOL transient;                  /* a stream chunk: never renamed */
}
@end
@implementation MetalBuffer
@end

@interface MetalShader : NSObject
{
@public
	id<MTLFunction> function;
	/* a pixel shader's text, and its compilations with exact border colors
	(exact_borders), compiled on first use: one for each EXACT_BORDERS mask,
	nil until tried, and a bit per mask tried */
	NSString *source;
	id<MTLFunction> exact[16];
	uint16_t exact_tried;
	/* a vertex shader's library, and its function specialized for each set
	of attribute kinds it is drawn with (vertex_function) */
	id<MTLLibrary> library;
	NSMutableDictionary<NSData *, id<MTLFunction>> *specialized;
	/* entered while its library compiles on a worker thread (warming, or
	display.async_shaders): a pipeline that needs it is built once it's
	empty (pipeline_request). The worker writes the fields above before it
	leaves. Specializations and variants are made under @synchronized on
	the record, since workers make them too */
	dispatch_group_t compiled;
}
@end
@implementation MetalShader
@end

/* a handle table: index 0 holds NSNull. objects owns the records; lookup
mirrors them as plain pointers, which table_get reads: a draw looks up 20 or
more handles, and each was a message and an autoreleased return before */
@interface MetalTable : NSObject
{
@public
	NSMutableArray *objects;
	NSMutableIndexSet *free_handles;
	__unsafe_unretained id *lookup;
	NSUInteger lookup_capacity;
}
@end
@implementation MetalTable
- (instancetype)init
{
	if ((self = [super init]))
	{
		objects = [NSMutableArray arrayWithObject:[NSNull null]];
		free_handles = [NSMutableIndexSet indexSet];
	}
	return self;
}
- (void)mirror:(NSUInteger)handle object:(id)object
{
	if (handle >= lookup_capacity)
	{
		NSUInteger capacity = lookup_capacity ? lookup_capacity : 1024;

		while (capacity <= handle)
			capacity *= 2;
		lookup = (__unsafe_unretained id *)realloc((void *)lookup, capacity * sizeof(*lookup));
		memset((void *)(lookup + lookup_capacity), 0, (capacity - lookup_capacity) * sizeof(*lookup));
		lookup_capacity = capacity;
	}
	lookup[handle] = object;
}
- (uint32_t)add:(id)object
{
	NSUInteger handle = free_handles.firstIndex;

	if (handle == NSNotFound)
	{
		[objects addObject:object];
		handle = objects.count - 1;
	}
	else
	{
		[free_handles removeIndex:handle];
		objects[handle] = object;
	}
	[self mirror:handle object:object];
	return (uint32_t)handle;
}
- (void)remove:(uint32_t)handle
{
	if (!handle || handle >= objects.count || objects[handle] == [NSNull null])
		return;
	objects[handle] = [NSNull null];
	[self mirror:handle object:nil];
	[free_handles addIndex:handle];
}
@end

/* ---------- state */

static id<MTLDevice> device;
static id<MTLCommandQueue> queue;
static CAMetalLayer *layer;
/* reversed-Z (see depth_compare_function) */
static float reversed_depth(float depth);
@class MetalTexture;
/* foveated eyes' targets (the foveation section) */
static BOOL foveation_sizes(MetalTexture *record, MTLSize *screen, MTLSize *physical);
static id<MTLTexture> foveation_resolve(MetalTexture *record, NSUInteger width, NSUInteger height);
#include "host_config.h"
#include "host_frame_pacer.h"
#include "host_stereo.h"
#if TARGET_OS_VISION
#include "host_theater.h"
#include "host_stereo_hud.h"
/* display.immersive: frames go to the theater screen while its space is open */
static BOOL theater_wanted;
/* the cut's fade through black (gpu_metal_present_stereo) */
static float stereo_cut_brightness(int view, int covered, int requested);
#endif
static MetalTable *textures, *buffers, *shaders;
static int metal_debug;
/* display.compressed_textures, where the GPU has BC formats: DXT textures
upload as they are, not decoded to BGRA8 (gpu_capabilities.s3tc) */
static BOOL compressed_textures;
/* debug.metal_specialize = false: vertex_function returns the unspecialized
function, its fallback when specializing fails */
static BOOL specialize_off;
/* gpu_capabilities.base_vertex: indexed draws pass their base vertex, which
needs GPU family Apple3 (an A9) or a Mac; an A8 (the Apple TV HD) has no
base vertex drawing, so there, and with debug.metal_base_vertex = false, the
front end rebases each draw's indices and the draws take none */
static BOOL base_vertex_draws;
/* debug.fixed_timestep: visibility answers wait for the GPU (visibility_result) */
static BOOL visibility_wait;
/* visibility tests count the samples that passed (MTLVisibilityResultModeCounting,
GPU_OCCLUSION_EXACT) where the GPU can: from the A9's family (Apple3) and on
every Mac. An A8 only says whether any passed (GPU_OCCLUSION_ANY_SAMPLE). */
static BOOL visibility_counts;
/* an answer: the serial of the command buffer that gave it over the count
(or the yes-or-no), which fits a 4K frame's pixels three times over */
#define VISIBILITY_COUNT_BITS 25
#define VISIBILITY_COUNT_MAXIMUM ((1ull << VISIBILITY_COUNT_BITS) - 1)

/* the open command buffer and render pass */
static id<MTLCommandBuffer> commands;
static id<MTLRenderCommandEncoder> encoder;
/* what encoder holds, to skip calls that set it again (metal_state_cache.h;
debug.metal_state_cache) */
static struct metal_state_cache state_cache;
/* the encoder's depth clip mode: 1 after setDepthClipMode:MTLDepthClipModeClamp
(only the first-person body's draws, gpu_raster_state.depth_clamp); outside
the state cache, so a frame with no such draw makes no call and counts none */
static int encoder_depth_clamp;
_Static_assert(GPU_STAGE_COUNT == 4 && GPU_ATTRIBUTE_COUNT == 16, "metal_state_cache.h's slots");
static gpu_texture pass_color, pass_depth;
static unsigned long pass_commands;
/* the open command buffer's serial; every earlier one has been committed */
static uint64_t current_serial = 1;
static _Atomic uint64_t completed_serial;
/* the latest command buffer committed, which completes after every earlier one */
static id<MTLCommandBuffer> last_committed;

/* frames */
static dispatch_semaphore_t frame_slots;
static int frame_started;
static unsigned long frame_slot;
static unsigned long frames;
static unsigned long renames;

/* frame pacing (pacing_schedule): GPU time of completed command buffers, and
the time this frame's CPU spent waiting rather than working */
static _Atomic uint64_t pacing_gpu_nanoseconds;
/* The GPU runs consecutive frames' command buffers overlapped (the next one
starts while the last finishes), so their start-to-end spans overlap: adding
them up counted the shared time twice. pacing_gpu_nanoseconds counts only the
time some command buffer was running, from the spans' union. Completion
handlers can run on several threads, hence the lock. */
static os_unfair_lock gpu_busy_lock = OS_UNFAIR_LOCK_INIT;
static CFTimeInterval gpu_busy_until;

static void gpu_busy_add(CFTimeInterval start, CFTimeInterval end)
{
	CFTimeInterval counted = 0.0;

	os_unfair_lock_lock(&gpu_busy_lock);
	if (end > gpu_busy_until)
	{
		counted = end - (start > gpu_busy_until ? start : gpu_busy_until);
		gpu_busy_until = end;
	}
	os_unfair_lock_unlock(&gpu_busy_lock);
	if (counted > 0.0)
		atomic_fetch_add(&pacing_gpu_nanoseconds, (uint64_t)(counted * 1e9));
}
static CFTimeInterval pacing_waited;

#if TARGET_OS_VISION
/* the presenters' command buffers (host_stereo.m, host_theater.m): their GPU
time counts with the game's, so stereo's frame-time line has the whole
frame's */
void gpu_metal_count_gpu_time(id<MTLCommandBuffer> commands)
{
	[commands addCompletedHandler:^(id<MTLCommandBuffer> completed)
	{
		if (completed.GPUEndTime > completed.GPUStartTime)
			gpu_busy_add(completed.GPUStartTime, completed.GPUEndTime);
	}];
}
#endif

/* ---------- visibility tests: state (the functions are after the draws)

A test counts into entries of a ring in a shared buffer, in Metal's boolean
mode, one entry for each render pass its draws reach: a pass writes its
entries when it ends, over what they held. When a test ends, its slot and
entries join the open command buffer's list, and that buffer's completion
handler ORs each test's entries into the slot's answer, so an answer read
frames later doesn't depend on a ring entry that has since been reused. */

/* Metal's largest visibility offset on iOS is 65,528 bytes */
#define VISIBILITY_ENTRIES 8191

/* what a command buffer's completion handler resolves */
struct visibility_pending
{
	uint32_t slot, first, count;
};

static id<MTLBuffer> visibility_buffer;
/* each slot's answer as (serial << 1) | answer, written by completion
handlers before they store completed_serial. Metal doesn't promise handlers
run in commit order, so a handler keeps the answer from the latest serial */
static _Atomic uint64_t visibility_answers[GPU_VISIBILITY_SLOTS];
/* the serial of the command buffer each ring entry was last handed to */
static uint64_t visibility_entry_serials[VISIBILITY_ENTRIES];
static struct
{
	int active;
	unsigned long next;                /* the ring's next entry */
	unsigned long first, count;        /* the active test's entries */
	int entry_open;                    /* the open pass counts into entry first + count - 1 */
	NSMutableData *pending;            /* the open command buffer's struct visibility_pending list */
	struct
	{
		unsigned long first, count;
		uint64_t serial;
	} slots[GPU_VISIBILITY_SLOTS];
} visibility;

/* ---------- transient memory: a frame slot's streams and constant snapshots,
in chunks that live until the slot's frame comes around again */

struct transient
{
	/* chunk handles in the buffer table */
	gpu_buffer chunks[64];
	unsigned long chunk_count, chunk, offset;
};

static struct transient streams[FRAMES], snapshots[FRAMES];

/* a handle's record, or nil: an array read, inlined, with no message */
static inline __attribute__((always_inline)) id table_get(MetalTable *table, uint32_t handle)
{
	return handle < table->lookup_capacity ? table->lookup[handle] : nil;
}

static inline __attribute__((always_inline)) MetalBuffer *buffer_record(gpu_buffer handle)
{
	return table_get(buffers, handle);
}

static inline __attribute__((always_inline)) MetalTexture *texture_record(gpu_texture handle)
{
	return table_get(textures, handle);
}

/* room for size bytes at alignment in the current chunk, moving to the next
(created as needed, never smaller than CHUNK_SIZE) when it doesn't fit; never
wraps, since recorded draws may still read what is behind */
static MetalBuffer *transient_room(struct transient *memory, uint32_t size, uint32_t alignment)
{
	MetalBuffer *record;

	for (;;)
	{
		if (memory->chunk < memory->chunk_count)
		{
			record = buffer_record(memory->chunks[memory->chunk]);
			memory->offset = (memory->offset + alignment - 1) & ~(unsigned long)(alignment - 1);
			if (memory->offset + size <= record->buffer.length)
				return record;
			memory->chunk++;
			memory->offset = 0;
			continue;
		}
		if (memory->chunk_count == sizeof(memory->chunks) / sizeof(memory->chunks[0]))
			host_fatal("Metal: a frame streamed more than %lu chunks", memory->chunk_count);
		record = [MetalBuffer new];
		record->buffer = [device newBufferWithLength:size > CHUNK_SIZE ? size : CHUNK_SIZE
			options:MTLResourceStorageModeShared];
		record->transient = YES;
		memory->chunks[memory->chunk_count++] = [buffers add:record];
	}
}

/* copies size bytes into transient memory; returns the offset and sets
*buffer to the chunk's handle */
static uint32_t transient_copy(struct transient *memory, const void *data, uint32_t size, uint32_t alignment,
	gpu_buffer *buffer)
{
	MetalBuffer *record = transient_room(memory, size, alignment);
	uint32_t offset = (uint32_t)memory->offset;

	memcpy((unsigned char *)record->buffer.contents + offset, data, size);
	memory->offset += size;
	*buffer = memory->chunks[memory->chunk];
	return offset;
}

static void transient_reset(struct transient *memory)
{
	memory->chunk = 0;
	memory->offset = 0;
}

/* ---------- command buffers and frames */

static BOOL busy(uint64_t used)
{
	return used > atomic_load(&completed_serial);
}

/* waits for the frame slot about to be reused, once per frame, before
anything writes into its transient memory */
static void frame_begin(void)
{
	CFTimeInterval waited;

	if (frame_started)
		return;
	waited = CACurrentMediaTime();
	dispatch_semaphore_wait(frame_slots, DISPATCH_TIME_FOREVER);
	pacing_waited += CACurrentMediaTime() - waited;
	transient_reset(&streams[frame_slot]);
	transient_reset(&snapshots[frame_slot]);
	frame_started = 1;
}

static id<MTLCommandBuffer> command_buffer(void)
{
	frame_begin();
	if (!commands)
	{
		MTLCommandBufferDescriptor *descriptor = [MTLCommandBufferDescriptor new];

		/* each encoder's execution status in a failed buffer's error, only
		with debug.gl_debug: a debugging aid, for which the driver keeps
		track of every encoder of every frame. A failure is logged either
		way (commit). */
		if (metal_debug)
			descriptor.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
		commands = [queue commandBufferWithDescriptor:descriptor];
		if (metal_debug)
			commands.label = [NSString stringWithFormat:@"frame %lu", frames];
	}
	return commands;
}

/* the open pass's depth-stencil target, whose store is decided when the pass
ends (pass_finish) */
static id<MTLTexture> pass_depth_texture;

static void pass_attachment_traffic(id<MTLTexture> texture, MTLLoadAction load, MTLStoreAction store, double bytes_per_pixel);

/* debug.gl_debug: one frame in every 600 has its render passes and blits
listed, in the order the GPU runs them, and logged at its Present, so a GPU
trace's encoders can be matched with their targets and load and store actions;
nil in every other frame */
static NSMutableString *pass_log;

static const char *load_name(MTLLoadAction load)
{
	return load == MTLLoadActionLoad ? "load" : load == MTLLoadActionClear ? "clear" : "dontcare";
}

static const char *store_name(MTLStoreAction store)
{
	return store == MTLStoreActionStore ? "store" : store == MTLStoreActionDontCare ? "dontcare" : "unknown";
}

/* display.native_hud: the pass on the display-sized back buffer, which
gpu_metal_upscale opens in a command buffer of its own (the frame's earlier
passes are committed first) and the HUD's draws continue. The HUD leaves it
for a moment to draw into a small target of its own (the motion tracker's
blips) and comes back to draw that target over the scene; ending the pass
there and opening it again would store the whole back buffer and load it
back, every frame. So a pass that interrupts it is encoded in another command
buffer, with the display-sized pass's encoder set aside still open (parked),
and that buffer is committed first when the draws come back: Metal runs
command buffers in the order they are committed, so the interrupting pass
runs after the scene and before the whole display-sized pass, the draws made
before it included. That is the order the game asked for only if the
interrupting pass neither reads what those earlier draws wrote nor writes
what they read, which is checked: it may not sample the back buffer
(gpu_metal_draw), and its targets may not have been used in the display-sized
pass's command buffer (native_side_allowed). Otherwise the pass ends and
reopens, in order. */
static struct
{
	/* the back buffer whose pass is open or parked; 0: none */
	gpu_texture color;
	BOOL parked;
	/* parked: the pass's command buffer, encoder and encoder state */
	id<MTLCommandBuffer> commands;
	id<MTLRenderCommandEncoder> encoder;
	struct metal_state_cache cache;
	int depth_clamp, entry_open;
	unsigned long pass_commands;
	/* since the last frame's log line */
	unsigned long parks, in_order;
} native_pass;

static void pass_finish_open(BOOL frame_end);

static void native_park(void)
{
	native_pass.commands = commands;
	native_pass.encoder = encoder;
	native_pass.cache = state_cache;
	native_pass.depth_clamp = encoder_depth_clamp;
	native_pass.entry_open = visibility.entry_open;
	native_pass.pass_commands = pass_commands;
	native_pass.parked = YES;
	native_pass.parks++;
	commands = nil;
	encoder = nil;
	metal_state_reset(&state_cache);
	encoder_depth_clamp = 0;
	visibility.entry_open = 0;
	pass_color = pass_depth = 0;
	pass_commands = 0;
	if (pass_log)
		[pass_log appendString:@"; parked for"];
}

/* ends the interrupting pass, commits its command buffer ahead of the
display-sized pass's, and makes that pass the open one again */
static void native_unpark(void)
{
	pass_finish_open(NO);
	if (commands)
	{
		[commands addCompletedHandler:^(id<MTLCommandBuffer> completed)
		{
			if (completed.status == MTLCommandBufferStatusError)
				platform_log("Metal: a command buffer ahead of the HUD's failed: %s", completed.error.description.UTF8String);
			if (completed.GPUEndTime > completed.GPUStartTime)
				gpu_busy_add(completed.GPUStartTime, completed.GPUEndTime);
		}];
		[commands commit];
	}
	commands = native_pass.commands;
	encoder = native_pass.encoder;
	state_cache = native_pass.cache;
	encoder_depth_clamp = native_pass.depth_clamp;
	visibility.entry_open = native_pass.entry_open;
	pass_commands = native_pass.pass_commands;
	pass_color = native_pass.color;
	pass_depth = 0;
	native_pass.commands = nil;
	native_pass.encoder = nil;
	native_pass.parked = NO;
	if (pass_log)
		[pass_log appendString:@"\n  the display-sized pass continues"];
}

/* ends the open pass. The depth-stencil target is stored, unless the pass
ends the frame (Present): nothing reads the screen's depth after it, and at
the display's size it's tens of megabytes of writes a frame */
static void pass_finish(BOOL frame_end)
{
	/* display.native_hud: with its pass parked, the pass that ends is that
	one, after the interrupting pass ahead of it */
	if (native_pass.parked)
		native_unpark();
	native_pass.color = 0;
	pass_finish_open(frame_end);
}

static void pass_finish_open(BOOL frame_end)
{
	if (encoder && pass_depth_texture)
	{
		MTLStoreAction store = frame_end ? MTLStoreActionDontCare : MTLStoreActionStore;

		[encoder setDepthStoreAction:store];
		[encoder setStencilStoreAction:store];
		pass_attachment_traffic(pass_depth_texture, MTLLoadActionDontCare, store, 5.0);
		if (pass_log)
			[pass_log appendFormat:@"; depth-stencil %s", store_name(store)];
	}
	if (encoder && pass_log)
		[pass_log appendFormat:@"; %lu draws and clears", pass_commands];
	pass_depth_texture = nil;
	[encoder endEncoding];
	encoder = nil;
	metal_state_reset(&state_cache);
	encoder_depth_clamp = 0;
	visibility.entry_open = 0;
	pass_color = pass_depth = 0;
	pass_commands = 0;
}

static void pass_end(void)
{
	pass_finish(NO);
}

static void async_frame_end(void);

/* commits the open command buffer; the frame's last one (at Present) also
frees its frame slot when it completes */
static void commit(BOOL frame_end)
{
	id<MTLCommandBuffer> committed;
	uint64_t serial = current_serial;

	NSData *pending = visibility.pending;
	id<MTLBuffer> results = visibility_buffer;

	pass_end();
	committed = command_buffer();
	visibility.pending = [NSMutableData data];
	[committed addCompletedHandler:^(id<MTLCommandBuffer> completed)
	{
		const struct visibility_pending *tests = pending.bytes;
		const uint64_t *entries = results.contents;
		unsigned long test, entry;

		if (completed.status == MTLCommandBufferStatusError)
		{
			platform_log("Metal: command buffer %llu failed: %s", (unsigned long long)serial,
				completed.error.description.UTF8String);
			GPU_VALIDATION_REPORT(GPU_VALIDATION_COMMAND_BUFFER_FAILED, 0, "command buffer %llu: %s", (unsigned long long)serial,
				completed.error.description.UTF8String);
		}
		if (completed.GPUEndTime > completed.GPUStartTime)
			gpu_busy_add(completed.GPUStartTime, completed.GPUEndTime);
		for (test = 0; test < pending.length / sizeof(*tests); test++)
		{
			/* the samples its draws passed, added up (a yes-or-no test's
			entries are 0 or not, and any that isn't makes the answer 1) */
			uint64_t answer = 0;

			for (entry = 0; entry < tests[test].count; entry++)
				answer += entries[(tests[test].first + entry) % VISIBILITY_ENTRIES];
			if (!visibility_counts)
				answer = answer != 0;
			else if (answer > VISIBILITY_COUNT_MAXIMUM)
				answer = VISIBILITY_COUNT_MAXIMUM;
			{
				uint64_t value = (serial << VISIBILITY_COUNT_BITS) | answer;
				uint64_t seen = atomic_load(&visibility_answers[tests[test].slot]);

				while (seen >> VISIBILITY_COUNT_BITS <= serial &&
					!atomic_compare_exchange_weak(&visibility_answers[tests[test].slot], &seen, value))
					;
			}
		}
		{
			uint64_t seen = atomic_load(&completed_serial);

			while (seen < serial && !atomic_compare_exchange_weak(&completed_serial, &seen, serial))
				;
		}
		if (frame_end)
			dispatch_semaphore_signal(frame_slots);
	}];
	[committed commit];
	last_committed = committed;
	commands = nil;
	current_serial++;
	if (frame_end)
	{
		frame_started = 0;
		frame_slot = (frame_slot + 1) % FRAMES;
		async_frame_end();
	}
}

static void use_texture(MetalTexture *record)
{
	if (record)
		record->used = current_serial;
}

static void use_buffer(MetalBuffer *record)
{
	if (record)
		record->used = current_serial;
}

/* ---------- textures */

static unsigned long level_dimension(uint32_t size, unsigned long level)
{
	return size >> level ? size >> level : 1;
}

static MTLPixelFormat texture_format(const struct gpu_texture_description *description)
{
	if (description->format == GPU_FORMAT_DEPTH_STENCIL)
		return MTLPixelFormatDepth32Float_Stencil8;
	/* compressed_textures is set only where these exist */
	if (@available(iOS 16.4, tvOS 16.4, visionOS 1.0, *))
	{
		if (description->format == GPU_FORMAT_BC1)
			return MTLPixelFormatBC1_RGBA;
		if (description->format == GPU_FORMAT_BC2)
			return MTLPixelFormatBC2_RGBA;
		if (description->format == GPU_FORMAT_BC3)
			return MTLPixelFormatBC3_RGBA;
	}
	return MTLPixelFormatBGRA8Unorm;
}

/* a level's bytes per row and per image: 4x4 blocks of 8 bytes (BC1) or 16
(BC2, BC3) for the compressed formats (gpu_capabilities.s3tc), else 4 bytes
a texel */
static void level_layout(uint32_t format, unsigned long width, unsigned long height,
	unsigned long *row, unsigned long *image)
{
	if (format == GPU_FORMAT_BC1 || format == GPU_FORMAT_BC2 || format == GPU_FORMAT_BC3)
	{
		*row = (width + 3) / 4 * (format == GPU_FORMAT_BC1 ? 8 : 16);
		*image = *row * ((height + 3) / 4);
	}
	else
	{
		*row = width * 4;
		*image = *row * height;
	}
}

/* the storage a texture's description asks for */
static id<MTLTexture> texture_storage(MetalTexture *record)
{
	const struct gpu_texture_description *description = &record->description;
	MTLTextureDescriptor *storage = [MTLTextureDescriptor new];

	storage.textureType = description->type == GPU_TEXTURE_CUBE ? MTLTextureTypeCube :
		description->type == GPU_TEXTURE_3D ? MTLTextureType3D : MTLTextureType2D;
	storage.pixelFormat = texture_format(description);
	storage.width = description->width ? description->width : 1;
	storage.height = description->height ? description->height : 1;
	storage.depth = description->type == GPU_TEXTURE_3D && description->depth ? description->depth : 1;
	storage.mipmapLevelCount = record->levels;
	if (description->usage == GPU_USAGE_RENDER_TARGET)
	{
		storage.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		storage.storageMode = MTLStorageModePrivate;
	}
	else
	{
		/* the GPU only reads upload textures, so the CPU's copy is always current */
		storage.usage = MTLTextureUsageShaderRead;
		storage.storageMode = MTLStorageModeShared;
	}
	return [device newTextureWithDescriptor:storage];
}

/* a new render target's contents are undefined under Metal, and zero under
GL, which shows on a frame that presents a back buffer nothing drew. The
clear runs in a command buffer of its own, committed now, so the open pass
stays open: the queue runs it before anything that uses the target. */
static void target_zero(id<MTLTexture> texture)
{
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	id<MTLCommandBuffer> zero = [queue commandBuffer];

	if (texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
	{
		pass.depthAttachment.texture = texture;
		pass.depthAttachment.loadAction = MTLLoadActionClear;
		pass.depthAttachment.storeAction = MTLStoreActionStore;
		pass.depthAttachment.clearDepth = reversed_depth(0.0f);
		pass.stencilAttachment.texture = texture;
		pass.stencilAttachment.loadAction = MTLLoadActionClear;
		pass.stencilAttachment.storeAction = MTLStoreActionStore;
		pass.stencilAttachment.clearStencil = 0;
	}
	else
	{
		pass.colorAttachments[0].texture = texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
	}
	[[zero renderCommandEncoderWithDescriptor:pass] endEncoding];
	[zero commit];
}

static gpu_texture gpu_metal_texture_create(const struct gpu_texture_description *description)
{
	@autoreleasepool
	{
		MetalTexture *record = [MetalTexture new];
		uint32_t largest = description->width;
		unsigned long possible = 1;

		record->description = *description;
		if (description->height > largest)
			largest = description->height;
		if (description->type == GPU_TEXTURE_3D && description->depth > largest)
			largest = description->depth;
		while (largest >> possible)
			possible++;
		/* the D3D level count comes from the format word unchecked
		(xbox_textures.c); GL ignores levels past the smallest, Metal can't
		create them */
		record->levels = description->levels < 1 ? 1 : description->levels > possible ? possible : description->levels;
		if (description->format != GPU_FORMAT_BGRA8 && description->format != GPU_FORMAT_DEPTH_STENCIL &&
			!(compressed_textures && description->usage != GPU_USAGE_RENDER_TARGET &&
				(description->format == GPU_FORMAT_BC1 || description->format == GPU_FORMAT_BC2 ||
				description->format == GPU_FORMAT_BC3)))
			platform_log("Metal: texture format %u is not supported; it samples black", description->format);
		/* render targets have storage at once; upload textures get theirs
		from their first upload, as under GL, and sample black until then */
		else if (description->usage == GPU_USAGE_RENDER_TARGET)
		{
			record->texture = texture_storage(record);
			target_zero(record->texture);
		}
		return [textures add:record];
	}
}

static void gpu_metal_texture_upload(gpu_texture texture, uint32_t face, uint32_t level, const void *data, uint32_t size)
{
	@autoreleasepool
	{
		MetalTexture *record = texture_record(texture);
		const struct gpu_texture_description *description;
		unsigned long width, height, depth, row, image;

		if (!record || record->description.format == GPU_FORMAT_DEPTH_STENCIL || level >= record->levels ||
			(record->description.format != GPU_FORMAT_BGRA8 && !compressed_textures))
		{
			GPU_VALIDATION_REPORT(GPU_VALIDATION_UPLOAD_IGNORED, 1, "Metal: texture %u face %u level %u: no such texture or "
				"level, a depth target, or a format this GPU does not take", texture, face, level);
			return;
		}
		description = &record->description;
		if (!record->texture || busy(record->used))
		{
			id<MTLTexture> previous = record->texture;

			record->texture = texture_storage(record);
			/* nothing has used the new version yet: the rest of this
			refresh's levels go into it instead of renaming it again */
			record->used = 0;
			/* a refresh starts at face 0, level 0 and uploads everything;
			anything else keeps the levels it doesn't replace */
			if (previous && (face || level))
			{
				unsigned long copy_face, copy_level, faces = description->type == GPU_TEXTURE_CUBE ? 6 : 1;

				platform_log("Metal: texture %u face %u level %u uploaded while in use", texture, face, level);
				for (copy_face = 0; copy_face < faces; copy_face++)
					for (copy_level = 0; copy_level < record->levels; copy_level++)
					{
						unsigned long w = level_dimension(description->width, copy_level);
						unsigned long h = level_dimension(description->height, copy_level);
						unsigned long d = description->type == GPU_TEXTURE_3D ? level_dimension(description->depth, copy_level) : 1;
						unsigned long copy_row, copy_image;
						NSMutableData *texels;

						level_layout(description->format, w, h, &copy_row, &copy_image);
						texels = [NSMutableData dataWithLength:copy_image * d];
						[previous getBytes:texels.mutableBytes bytesPerRow:copy_row bytesPerImage:copy_image
							fromRegion:MTLRegionMake3D(0, 0, 0, w, h, d) mipmapLevel:copy_level slice:copy_face];
						[record->texture replaceRegion:MTLRegionMake3D(0, 0, 0, w, h, d) mipmapLevel:copy_level
							slice:copy_face withBytes:texels.bytes bytesPerRow:copy_row bytesPerImage:copy_image];
					}
			}
			if (previous)
				renames++;
		}
		width = level_dimension(description->width, level);
		height = level_dimension(description->height, level);
		depth = description->type == GPU_TEXTURE_3D ? level_dimension(description->depth, level) : 1;
		level_layout(description->format, width, height, &row, &image);
		if (size < image * depth)
		{
			GPU_VALIDATION_REPORT(GPU_VALIDATION_UPLOAD_IGNORED, 2, "Metal: texture %u face %u level %u: %u bytes for a level "
				"of %lu", texture, face, level, size, image * depth);
			return;
		}
		[record->texture replaceRegion:MTLRegionMake3D(0, 0, 0, width, height, depth) mipmapLevel:level
			slice:description->type == GPU_TEXTURE_CUBE ? face : 0 withBytes:data bytesPerRow:row
			bytesPerImage:image];
	}
}

static void gpu_metal_texture_destroy(gpu_texture texture)
{
	@autoreleasepool
	{
		/* a committed command buffer keeps what it uses. The handle can be
		reused at once, so a pass still open on it ends first: a new target
		with the same handle would otherwise continue the old one's pass */
		if (texture && (texture == pass_color || texture == pass_depth))
			pass_end();
		[textures remove:texture];
	}
}

static uint32_t gpu_metal_texture_read(gpu_texture texture, void *pixels, uint32_t size)
{
	@autoreleasepool
	{
		MetalTexture *record = texture_record(texture);
		const struct gpu_texture_description *description = record ? &record->description : NULL;
		unsigned long width, height;
		id<MTLBuffer> staging;
		id<MTLBlitCommandEncoder> blit;

		if (!record || !record->texture || description->type != GPU_TEXTURE_2D ||
			(description->format != GPU_FORMAT_BGRA8 && description->format != GPU_FORMAT_DEPTH_STENCIL) ||
			(description->format == GPU_FORMAT_DEPTH_STENCIL && description->usage != GPU_USAGE_RENDER_TARGET))
		{
			GPU_VALIDATION_REPORT(GPU_VALIDATION_READ_UNSUPPORTED, 0, "Metal: texture %u cannot be read back", texture);
			return 0;
		}
		width = description->width;
		height = description->height;
		/* a foveated eye's target: resolved to its screen size first, so the
		picture has the view's shape and each pixel weighs its screen area
		(the depth's empty share), then read as an ordinary one */
		{
			MTLSize screen, physical;
			id<MTLTexture> resolved;

			if (foveation_sizes(record, &screen, &physical))
			{
				width = screen.width;
				height = screen.height;
				if (size < width * height * 4)
					return 0;
				resolved = foveation_resolve(record, width, height);
				if (!resolved)
					return 0;
				staging = [device newBufferWithLength:width * height * 4 options:MTLResourceStorageModeShared];
				blit = [command_buffer() blitCommandEncoder];
				[blit copyFromTexture:resolved sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
					sourceSize:MTLSizeMake(width, height, 1) toBuffer:staging destinationOffset:0
					destinationBytesPerRow:width * 4 destinationBytesPerImage:width * height * 4];
				[blit endEncoding];
				{
					id<MTLCommandBuffer> waited = commands;

					commit(NO);
					[waited waitUntilCompleted];
				}
				memcpy(pixels, staging.contents, width * height * 4);
				if (description->format == GPU_FORMAT_DEPTH_STENCIL)
				{
					float *depth = pixels;
					unsigned long index;

					for (index = 0; index < width * height; index++)
						depth[index] = reversed_depth(depth[index]);
				}
				return 1;
			}
		}
		if (size < width * height * 4)
			return 0;
		/* a depth target's depth plane, which holds the reversed depth
		(reversed_depth): read back as the game's */
		if (description->format == GPU_FORMAT_DEPTH_STENCIL)
		{
			float *depth = pixels;
			unsigned long index;

			staging = [device newBufferWithLength:width * height * 4 options:MTLResourceStorageModeShared];
			pass_end();
			blit = [command_buffer() blitCommandEncoder];
			[blit copyFromTexture:record->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
				sourceSize:MTLSizeMake(width, height, 1) toBuffer:staging destinationOffset:0
				destinationBytesPerRow:width * 4 destinationBytesPerImage:width * height * 4
				options:MTLBlitOptionDepthFromDepthStencil];
			[blit endEncoding];
			use_texture(record);
			{
				id<MTLCommandBuffer> waited = commands;

				commit(NO);
				[waited waitUntilCompleted];
			}
			memcpy(pixels, staging.contents, width * height * 4);
			for (index = 0; index < width * height; index++)
				depth[index] = reversed_depth(depth[index]);
			return 1;
		}
		/* BGRA8Unorm is already the BGRA byte order gpu.h reads back, and row
		0 is the top */
		if (description->usage != GPU_USAGE_RENDER_TARGET)
		{
			[record->texture getBytes:pixels bytesPerRow:width * 4 fromRegion:MTLRegionMake2D(0, 0, width, height)
				mipmapLevel:0];
			return 1;
		}
		staging = [device newBufferWithLength:width * height * 4 options:MTLResourceStorageModeShared];
		pass_end();
		blit = [command_buffer() blitCommandEncoder];
		[blit copyFromTexture:record->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
			sourceSize:MTLSizeMake(width, height, 1) toBuffer:staging destinationOffset:0
			destinationBytesPerRow:width * 4 destinationBytesPerImage:width * height * 4];
		[blit endEncoding];
		use_texture(record);
		{
			id<MTLCommandBuffer> waited = commands;

			commit(NO);
			[waited waitUntilCompleted];
		}
		memcpy(pixels, staging.contents, width * height * 4);
		return 1;
	}
}

/* level 0 of source into level of destination (mip composites); the size is
the destination level's, as gpu_gl.c copies it */
static void gpu_metal_texture_copy_level(gpu_texture source, gpu_texture destination, uint32_t level)
{
	@autoreleasepool
	{
		MetalTexture *from = texture_record(source), *to = texture_record(destination);
		id<MTLBlitCommandEncoder> blit;
		unsigned long width, height;

		if (!from || !to || !from->texture || !to->texture || level >= to->levels)
		{
			GPU_VALIDATION_REPORT(GPU_VALIDATION_COPY_IGNORED, 1, "Metal: texture %u into level %u of texture %u: a texture "
				"without storage, or no such level", source, level, destination);
			return;
		}
		width = level_dimension(to->description.width, level);
		height = level_dimension(to->description.height, level);
		if (width > from->texture.width || height > from->texture.height)
		{
			GPU_VALIDATION_REPORT(GPU_VALIDATION_COPY_IGNORED, 2, "Metal: texture %u is smaller than level %u of texture %u",
				source, level, destination);
			return;
		}
		pass_end();
		blit = [command_buffer() blitCommandEncoder];
		if (metal_debug)
			blit.label = @"copy level";
		if (pass_log)
			[pass_log appendFormat:@"\n  blit: copy %lux%lu of texture %u into level %u of texture %u", width, height, source,
				level, destination];
		[blit copyFromTexture:from->texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
			sourceSize:MTLSizeMake(width, height, 1) toTexture:to->texture destinationSlice:0
			destinationLevel:level destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
		use_texture(from);
		use_texture(to);
	}
}

/* the levels after base_level from base_level */
static void gpu_metal_texture_generate_mipmaps(gpu_texture texture, uint32_t base_level)
{
	@autoreleasepool
	{
		MetalTexture *record = texture_record(texture);
		id<MTLTexture> levels;
		id<MTLBlitCommandEncoder> blit;

		if (!record || !record->texture || base_level + 1 >= record->levels)
		{
			GPU_VALIDATION_REPORT(GPU_VALIDATION_MIPMAPS_IGNORED, record != nil, "Metal: texture %u from level %u: no storage, "
				"or no levels after it", texture, base_level);
			return;
		}
		levels = base_level ? [record->texture newTextureViewWithPixelFormat:record->texture.pixelFormat
			textureType:MTLTextureType2D levels:NSMakeRange(base_level, record->levels - base_level)
			slices:NSMakeRange(0, 1)] : record->texture;
		pass_end();
		blit = [command_buffer() blitCommandEncoder];
		if (metal_debug)
			blit.label = @"mipmaps";
		if (pass_log)
			[pass_log appendFormat:@"\n  blit: mipmaps of texture %u (%lux%lu) from level %u, %lu levels", texture,
				(unsigned long)record->texture.width, (unsigned long)record->texture.height, base_level, record->levels];
		[blit generateMipmapsForTexture:levels];
		[blit endEncoding];
		use_texture(record);
	}
}

/* ---------- buffers and streams */

static gpu_buffer gpu_metal_buffer_create(uint32_t size)
{
	@autoreleasepool
	{
		MetalBuffer *record = [MetalBuffer new];

		record->buffer = [device newBufferWithLength:size ? size : 1 options:MTLResourceStorageModeShared];
		return [buffers add:record];
	}
}

static void gpu_metal_buffer_write(gpu_buffer buffer, uint32_t offset, uint32_t size, const void *data, uint32_t flags)
{
	@autoreleasepool
	{
		MetalBuffer *record = buffer_record(buffer);

		if (!record || offset > record->buffer.length || size > record->buffer.length - offset)
			return;
		/* a range no queued draw reads (GPU_WRITE_UNUSED), or a buffer nothing
		recorded or running uses, takes the write in place; otherwise the
		buffer gets new storage holding the old contents and the write, and
		the commands already recorded keep the old storage */
		if (!(flags & GPU_WRITE_UNUSED) && busy(record->used))
		{
			id<MTLBuffer> renamed = [device newBufferWithLength:record->buffer.length
				options:MTLResourceStorageModeShared];

			memcpy(renamed.contents, record->buffer.contents, record->buffer.length);
			record->buffer = renamed;
			record->used = 0;
			renames++;
		}
		memcpy((unsigned char *)record->buffer.contents + offset, data, size);
	}
}

static void gpu_metal_stream_reserve(uint32_t vertex_bytes, uint32_t index_bytes)
{
	@autoreleasepool
	{
		/* a new chunk never discards the frame's earlier streams, so there is
		nothing to keep together; only the slot must be ours */
		frame_begin();
	}
}

static uint32_t gpu_metal_stream(uint32_t kind, const void *data, uint32_t size, gpu_buffer *buffer)
{
	@autoreleasepool
	{
		__unsafe_unretained MetalBuffer *record;
		uint32_t offset;

		frame_begin();
		/* copies size bytes and keeps the next stream 16-aligned (gpu_gl.c
		copies the rounded size, reading past the caller's data) */
		offset = transient_copy(&streams[frame_slot], data, size, 16, buffer);
		record = buffer_record(*buffer);
		use_buffer(record);
		return offset;
	}
}

/* ---------- shaders */

/* vertex shaders' and pixel shaders' (gpu_metal_initialize) */
static MTLCompileOptions *compile_options, *pixel_compile_options;

/* debug.gl_debug: the shader work done while drawing, which a frame waits
for: shader libraries compiled, vertex functions specialized and pipelines
made (each the newLibrary, newFunction or newRenderPipelineState call
alone), with their time and the longest one; logged and reset at a frame
with any (gpu_metal_present) */
enum { COMPILE_LIBRARY, COMPILE_SPECIALIZE, COMPILE_PIPELINE, COMPILE_KINDS };
static struct
{
	unsigned long count[COMPILE_KINDS];
	double seconds[COMPILE_KINDS], longest[COMPILE_KINDS];
	unsigned long total_count[COMPILE_KINDS];
	double total_seconds[COMPILE_KINDS];
	/* made while drawing, outside warming (the map's list missed them) */
	unsigned long during_play[COMPILE_KINDS];
} compile_stats;
/* (workers count their compiles too, under display.async_shaders) */
static os_unfair_lock compile_stats_lock = OS_UNFAIR_LOCK_INIT;

/* play: made for a draw, outside warming */
static void compile_count(int kind, CFTimeInterval started, int play)
{
	double seconds = CACurrentMediaTime() - started;

	host_benchmark_compile(seconds);
	os_unfair_lock_lock(&compile_stats_lock);
	compile_stats.count[kind]++;
	compile_stats.seconds[kind] += seconds;
	if (seconds > compile_stats.longest[kind])
		compile_stats.longest[kind] = seconds;
	compile_stats.total_count[kind]++;
	compile_stats.total_seconds[kind] += seconds;
	if (play)
		compile_stats.during_play[kind]++;
	os_unfair_lock_unlock(&compile_stats_lock);
}

/* compiling at map load (gpu_warm_begin to gpu_warm_end): shaders compile
in parallel, joining warm_group until they are done, and the pipelines the
map's list names wait in warm_list for warm_end, which builds them in
parallel too (or, under display.async_shaders, starts them building and
returns) */
static BOOL warming;
static dispatch_group_t warm_group;
static NSMutableData *warm_list;

/* display.async_shaders (GPU_INITIALIZE_ASYNC_SHADERS, which the front end
leaves off in a deterministic run: the runners, check-metal, an input
replay, a benchmark): nothing the game thread does waits on Metal's
compiler. On visionOS the game thread is the main thread, and a cold
launch's compiles there held it long enough for the system to end the app.
Shader libraries compile on worker threads; a pipeline is claimed by the
first draw that needs it (or the map's list at warm_end) and built on a
worker once its shaders are compiled (pipeline_request), then published to
pipeline_table; until then its draws are skipped (gpu_metal_draw) and the
window may show its last whole frame again (metal_hold_frame). Without it
everything compiles on the game thread at its first use, as before, so
those runs draw exactly the same frames. */
static BOOL async_compiles;
/* shaders and pipelines still compiling on workers */
static _Atomic unsigned long compiles_outstanding;
/* the launch's totals and this frame's skips (metal_compile_async.h) */
static struct metal_compile_totals compile_totals;
static struct metal_skip_frame skip_frame;
static struct metal_hold window_hold;
/* the window's frames held in a row at most: half a second at 60 Hz */
#define HOLD_LIMIT 30
/* gpu_metal_present held this frame (async_frame_end counts it) */
static BOOL frame_held;
/* when the backend started (the launch's lines), and the frames in a row
with skipped draws */
static CFTimeInterval compile_started;
static unsigned long skipping_frames;

static const char *compile_kind_name(int kind)
{
	return kind == COMPILE_LIBRARY ? "shader library" : kind == COMPILE_SPECIALIZE ? "vertex specialization" :
		"pipeline";
}

/* the workers' queue: a draw's compiles ahead of a map list's */
static dispatch_queue_t compile_queue(int play)
{
	return dispatch_get_global_queue(play ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
}

int gpu_metal_async_compiles(void)
{
	return async_compiles;
}

/* a shader's library made into its record: its function (a vertex shader's
unspecialized one) and what later specializations or variants compile from;
NO, logged, if it can't be */
static BOOL shader_finish(MetalShader *record, uint32_t stage, NSString *text, id<MTLLibrary> library, NSError *error)
{
	/* debug.gl_debug: the warnings of a shader that compiled */
	if (library && error && metal_debug)
		platform_log("the %s shader compiled with warnings:\n%s", stage == GPU_SHADER_VERTEX ? "vertex" : "pixel",
			error.localizedDescription.UTF8String);
	if (!library)
	{
		platform_log("cannot compile the %s shader:\n%s\n%s", stage == GPU_SHADER_VERTEX ? "vertex" : "pixel",
			error.localizedDescription.UTF8String, text.UTF8String);
		GPU_VALIDATION_REPORT(GPU_VALIDATION_SHADER_COMPILE_FAILED, stage << 8 | 1, "Metal: cannot compile a %s shader: %s",
			stage == GPU_SHADER_VERTEX ? "vertex" : "pixel", error.localizedDescription.UTF8String);
		return NO;
	}
	/* a vertex shader's attribute kinds are function constants, which the
	unspecialized function leaves undefined: its fetches read the kinds
	from the attribute table (nv2a_msl.c) */
	error = nil;
	record->function = stage == GPU_SHADER_VERTEX ?
		[library newFunctionWithName:@"vertex_main" constantValues:[MTLFunctionConstantValues new] error:&error] :
		[library newFunctionWithName:@"fragment_main"];
	if (!record->function)
	{
		platform_log("cannot make the %s shader's function: %s", stage == GPU_SHADER_VERTEX ? "vertex" : "pixel",
			error ? error.localizedDescription.UTF8String : "no entry point");
		GPU_VALIDATION_REPORT(GPU_VALIDATION_SHADER_COMPILE_FAILED, stage << 8 | 2, "Metal: cannot make a %s shader's function",
			stage == GPU_SHADER_VERTEX ? "vertex" : "pixel");
		return NO;
	}
	if (stage != GPU_SHADER_VERTEX)
		record->source = text;
	else
		record->library = library;
	return YES;
}

/* the translators' MSL (nv2a_msl.c); 0 if it doesn't compile, which the front
end counts as a draw skipped for its program, as under GL. While warming,
the handle comes back at once and the shader compiles in the background (and
so under display.async_shaders); one that fails then has no function, and
its draws are skipped as a pipeline that can't be made (draw_pipeline) */
static gpu_shader gpu_metal_shader_create(uint32_t stage, const char *source)
{
	@autoreleasepool
	{
		NSError *error = nil;
		NSString *text = @(source);
		MTLCompileOptions *options = stage == GPU_SHADER_VERTEX ? compile_options : pixel_compile_options;
		MetalShader *record = [MetalShader new];
		CFTimeInterval started;
		id<MTLLibrary> library;

		record->compiled = dispatch_group_create();
		/* (each on a worker thread, compiled synchronously there: not with
		newLibraryWithSource's completion handler, which runs on Metal's
		compiler queue, where shader_finish's newFunctionWithName waits on
		that same queue and libdispatch traps) */
		if (warming || async_compiles)
		{
			BOOL play = !warming;
			dispatch_group_t group = warming ? warm_group : nil;

			dispatch_group_enter(record->compiled);
			if (group)
				dispatch_group_enter(group);
			atomic_fetch_add(&compiles_outstanding, 1);
			dispatch_async(compile_queue(play), ^
			{
				@autoreleasepool
				{
					NSError *failure = nil;
					CFTimeInterval begun = CACurrentMediaTime();
					id<MTLLibrary> compiled = [device newLibraryWithSource:text options:options error:&failure];

					if (play)
						compile_count(COMPILE_LIBRARY, begun, 1);
					metal_compile_finished(&compile_totals, COMPILE_LIBRARY, CACurrentMediaTime() - begun);
					shader_finish(record, stage, text, compiled, failure);
					atomic_fetch_sub(&compiles_outstanding, 1);
					dispatch_group_leave(record->compiled);
					if (group)
						dispatch_group_leave(group);
				}
			});
			return [shaders add:record];
		}
		started = CACurrentMediaTime();
		library = [device newLibraryWithSource:text options:options error:&error];
		compile_count(COMPILE_LIBRARY, started, 1);
		if (!shader_finish(record, stage, text, library, error))
			return 0;
		return [shaders add:record];
	}
}

/* a pixel shader's function for a draw whose stages in exact (a mask of
exact_borders) rebuild their border colors: the shader compiled again with
EXACT_BORDERS on the first draw that asks for that mask. nil if it doesn't
compile (logged once), and the draw then samples the plain way. (Workers
building pipelines call it too, hence the lock on the record.) */
static id<MTLFunction> pixel_function(MetalShader *pixel, unsigned exact)
{
	if (!exact)
		return pixel->function;
	@synchronized (pixel)
	{
		if (!(pixel->exact_tried & (1u << exact)) && pixel->source)
		{
			@autoreleasepool
			{
				NSError *error = nil;
				NSString *text = [NSString stringWithFormat:@"#define EXACT_BORDERS %u\n%@", exact, pixel->source];
				/* relaxed, as the plain variant (gpu_metal_initialize) */
				id<MTLLibrary> library = [device newLibraryWithSource:text options:pixel_compile_options error:&error];

				pixel->exact_tried |= 1u << exact;
				if (library)
					pixel->exact[exact] = [library newFunctionWithName:@"fragment_main"];
				if (!pixel->exact[exact])
				{
					platform_log("cannot compile the pixel shader with exact border colors (stages %#x): %s", exact,
						library ? "no entry point" : error.localizedDescription.UTF8String);
					GPU_VALIDATION_REPORT(GPU_VALIDATION_EXACT_BORDERS_FAILED, exact, "Metal: a pixel shader does not compile "
						"with exact border colors (stages %#x)", exact);
				}
			}
		}
		return pixel->exact[exact];
	}
}

/* ---------- foveation (Task 10)

A foveated eye's targets (gpu_texture_description.foveated_eye) are
allocated at the Compositor's drawable size per view and drawn through the
eye's rate map (host_stereo_rate_map), which fills their top left: every
pass that writes one binds the map, and its viewports and scissors are in
the map's screen coordinates, the eye's logical size (target_size). What
reads them maps each screen point to where the map put it: the presenter
(host_stereo.m), the game's shaders (nv2a_msl.c's foveated sampler) and
foveation_resolve here, which draws a picture at its screen shape for the
film's paths, the screenshots and the depth reads. */

/* debug.rate_map_test (the Mac's runner): the mono screen's targets, which
the front end marks as eye 1's, render through a synthetic map the size of
the screen, made here (test_map) since there's no Compositor: full rate in
the middle third each way, half outside it, except the top band at three
quarters, so a flip shows as the dense band at the bottom. Each map with its
parameter data, by screen size */
static BOOL rate_map_test;
static NSMutableDictionary<NSString *, NSArray *> *test_maps;

static NSArray *test_map(NSUInteger width, NSUInteger height)
{
	NSString *key = [NSString stringWithFormat:@"%lux%lu", (unsigned long)width, (unsigned long)height];
	NSArray *entry = test_maps[key];

	if (!entry)
	{
		/* The samples are points from edge to edge, the rate between them
		interpolated: 31 a side put samples 10 to 20 on the middle third's
		edges and inside it, so it is all at 1.0. The sides' rates are
		nudged until the middle third's offset from the screen is even each
		way: the GPU takes derivatives over 2x2 quads in physical pixels, and
		an odd offset pairs other rows or columns than the reference does,
		which changes the texture level of detail a little on detailed
		surfaces (a10's ship, by up to 22 levels) though the remap is exact */
		enum { SAMPLES = 31 };
		static const float sides[] = { 0.5f, 0.55f, 0.45f, 0.6f, 0.4f };
		static const float tops[] = { 0.75f, 0.7f, 0.8f, 0.65f, 0.85f };
		float horizontal[SAMPLES], vertical[SAMPLES];
		id<MTLRasterizationRateMap> map = nil;
		id<MTLBuffer> parameters;
		MTLSize physical;
		MTLCoordinate2D middle = { 0.0f, 0.0f };
		float side = sides[0], top = tops[0];
		int index, tried;

		if (![device supportsRasterizationRateMapWithLayerCount:1])
		{
			platform_log("Metal: debug.rate_map_test: this GPU has no rate maps");
			rate_map_test = NO;
			return nil;
		}
		for (tried = 0; tried < 25; tried++)
		{
			MTLRasterizationRateLayerDescriptor *layer;
			float screen_x = (float)(width / 2) + 0.5f, screen_y = (float)(height / 2) + 0.5f;

			side = sides[tried % 5];
			top = tops[tried / 5];
			for (index = 0; index < SAMPLES; index++)
			{
				BOOL inside = index >= 10 && index <= 20;

				horizontal[index] = inside ? 1.0f : side;
				vertical[index] = inside ? 1.0f : index < 10 ? top : 0.5f;
			}
			layer = [[MTLRasterizationRateLayerDescriptor alloc] initWithSampleCount:MTLSizeMake(SAMPLES, SAMPLES, 0)
				horizontal:horizontal vertical:vertical];
			map = [device newRasterizationRateMapWithDescriptor:[MTLRasterizationRateMapDescriptor
				rasterizationRateMapDescriptorWithScreenSize:MTLSizeMake(width, height, 0) layer:layer]];
			if (!map)
				return nil;
			middle = [map mapScreenToPhysicalCoordinates:MTLCoordinate2DMake(screen_x, screen_y) forLayer:0];
			middle.x = screen_x - middle.x;
			middle.y = screen_y - middle.y;
			if (fmodf(middle.x, 2.0f) == 0.0f && fmodf(middle.y, 2.0f) == 0.0f)
				break;
		}
		parameters = [device newBufferWithLength:map.parameterBufferSizeAndAlign.size options:MTLResourceStorageModeShared];
		[map copyParameterDataToBuffer:parameters offset:0];
		physical = [map physicalSizeForLayer:0];
		platform_log("Metal: debug.rate_map_test: a %lux%lu screen renders through a synthetic map into %lux%lu "
			"(granularity %lux%lu; the sides at %.2f, the top at %.2f, the bottom at 0.50): the middle third is "
			"1:1, %.2f, %.2f pixels from the screen's", (unsigned long)width, (unsigned long)height,
			(unsigned long)physical.width, (unsigned long)physical.height, (unsigned long)map.physicalGranularity.width,
			(unsigned long)map.physicalGranularity.height, side, top, middle.x, middle.y);
		entry = @[ map, parameters ];
		test_maps[key] = entry;
	}
	return entry;
}

/* the eye's map for a foveated target this frame, or nil */
static id<MTLRasterizationRateMap> foveation_map(MetalTexture *record)
{
	id<MTLRasterizationRateMap> map;

	if (!record || !record->description.foveated_eye)
		return nil;
	map = host_stereo_rate_map(record->description.foveated_eye - 1);
	if (!map && rate_map_test)
		map = test_map(record->description.width, record->description.height).firstObject;
	return map;
}

/* the map's parameter data, for a shader's rasterization_rate_map_decoder */
static id<MTLBuffer> foveation_parameters(MetalTexture *record)
{
	id<MTLBuffer> parameters;

	if (!record || !record->description.foveated_eye)
		return nil;
	parameters = host_stereo_rate_map_parameters(record->description.foveated_eye - 1);
	if (!parameters && rate_map_test)
		parameters = test_map(record->description.width, record->description.height).lastObject;
	return parameters;
}

/* a foveated target's screen size (the eye's logical size, which the game's
viewports cover; a test map's, the target's) and its map's physical size;
NO if it isn't foveated this frame */
static BOOL foveation_sizes(MetalTexture *record, MTLSize *screen, MTLSize *physical)
{
	id<MTLRasterizationRateMap> map = foveation_map(record);
	int screen_width, screen_height, allocated_width, allocated_height;

	if (!map)
		return NO;
	if (host_stereo_rate_map(record->description.foveated_eye - 1))
	{
		if (!host_stereo_foveated_size(&screen_width, &screen_height, &allocated_width, &allocated_height))
			return NO;
		*screen = MTLSizeMake((NSUInteger)screen_width, (NSUInteger)screen_height, 1);
	}
	else
		*screen = MTLSizeMake(record->description.width, record->description.height, 1);
	*physical = [map physicalSizeForLayer:0];
	return YES;
}

/* ---------- render passes */

/* the targets' size in the units of their viewports and scissors: a
foveated target's screen size, else its texture's */
static void target_size(gpu_texture color, gpu_texture depth, unsigned long *width, unsigned long *height)
{
	MetalTexture *record = texture_record(color ? color : depth);
	MTLSize screen, physical;

	if (foveation_sizes(record, &screen, &physical))
	{
		*width = screen.width;
		*height = screen.height;
		return;
	}
	*width = record && record->texture ? record->texture.width : 0;
	*height = record && record->texture ? record->texture.height : 0;
}

/* the color attachment of depth-only passes, by size */
static NSMutableDictionary<NSString *, id<MTLTexture>> *scratch_colors;

static id<MTLTexture> scratch_color(NSUInteger width, NSUInteger height)
{
	NSString *key = [NSString stringWithFormat:@"%lux%lu", (unsigned long)width, (unsigned long)height];
	id<MTLTexture> texture = scratch_colors[key];

	if (!texture)
	{
		MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
			width:width height:height mipmapped:NO];

		descriptor.usage = MTLTextureUsageRenderTarget;
		descriptor.storageMode = MTLStorageModeMemoryless;
		texture = [device newTextureWithDescriptor:descriptor];
		scratch_colors[key] = texture;
	}
	return texture;
}

/* opens a pass on the pair, unless it is the open one; a load action of
Clear clears what it names before anything in the pass */
/* render passes and the memory their attachments load and store, which on a
tile-based GPU is most of what a pass costs beyond its draws; logged every
600 frames (gpu_metal_present) */
static struct
{
	unsigned long passes;
	double loaded, stored;           /* bytes */
} pass_traffic;

static void pass_attachment_traffic(id<MTLTexture> texture, MTLLoadAction load, MTLStoreAction store, double bytes_per_pixel)
{
	double bytes;

	if (!texture)
		return;
	bytes = (double)texture.width * (double)texture.height * bytes_per_pixel;
	if (load == MTLLoadActionLoad)
		pass_traffic.loaded += bytes;
	if (store == MTLStoreActionStore)
		pass_traffic.stored += bytes;
}

static void pass_traffic_count(MTLRenderPassDescriptor *pass)
{
	pass_traffic.passes++;
	pass_attachment_traffic(pass.colorAttachments[0].texture, pass.colorAttachments[0].loadAction,
		pass.colorAttachments[0].storeAction, 4.0);
	pass_attachment_traffic(pass.depthAttachment.texture, pass.depthAttachment.loadAction,
		pass.depthAttachment.storeAction, 4.0);
	pass_attachment_traffic(pass.stencilAttachment.texture, pass.stencilAttachment.loadAction,
		pass.stencilAttachment.storeAction, 1.0);
}

/* display.native_hud (native_pass): a pass on these targets may run ahead
of the display-sized pass: nothing in that pass's command buffer has used
them yet (a draw made before this one that sampled one would see what this
pass draws) */
static BOOL native_side_allowed(MetalTexture *color, MetalTexture *depth)
{
	return !(color && color->used == current_serial) && !(depth && depth->used == current_serial);
}

static BOOL pass_begin(gpu_texture color, gpu_texture depth, const struct gpu_clear *clear)
{
	MetalTexture *color_record = texture_record(color), *depth_record = texture_record(depth);
	MTLRenderPassDescriptor *pass;

	if (encoder && pass_color == color && pass_depth == depth && !clear)
		return YES;
	if ((color && (!color_record || !color_record->texture)) || (depth && (!depth_record || !depth_record->texture)) ||
		(!color && !depth))
		return NO;
	/* display.native_hud (native_pass): the draws come back to the parked
	pass; or leave it for a pass that may run ahead of it; or neither */
	if (native_pass.parked && color == native_pass.color && !depth && !clear)
	{
		native_unpark();
		return YES;
	}
	if (native_pass.color && color != native_pass.color && native_side_allowed(color_record, depth_record) &&
		(native_pass.parked || (encoder && pass_color == native_pass.color && !pass_depth)))
	{
		if (native_pass.parked)
			pass_finish_open(NO);
		else
			native_park();
	}
	else
	{
		if (native_pass.color && encoder)
			native_pass.in_order++;
		pass_end();
	}
	pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.visibilityResultBuffer = visibility_buffer;
	/* a foveated eye's targets render through its map; both attachments are
	the eye's then, at the same allocated size */
	pass.rasterizationRateMap = foveation_map(color ? color_record : depth_record);
	if (color && depth && foveation_map(color_record) != foveation_map(depth_record))
	{
		static int logged;

		if (!logged++)
			platform_log("Metal: a pass pairs targets %u and %u, of which only one is a foveated eye's", color, depth);
	}
	if (!color)
	{
		/* a depth-only pass: the game's pixel shaders still write a color
		(and may discard), so they need an attachment to write it to, which
		never leaves the GPU's tile memory */
		pass.colorAttachments[0].texture = scratch_color(depth_record->texture.width, depth_record->texture.height);
		pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
		pass.colorAttachments[0].storeAction = MTLStoreActionDontCare;
	}
	else
	{
		pass.colorAttachments[0].texture = color_record->texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		if (clear && (clear->flags & GPU_CLEAR_COLOR))
		{
			float red = (float)((clear->color >> 16) & 0xff) / 255.0f, green = (float)((clear->color >> 8) & 0xff) / 255.0f;
			float blue = (float)(clear->color & 0xff) / 255.0f, alpha = (float)(clear->color >> 24) / 255.0f;

			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].clearColor = MTLClearColorMake(red, green, blue, alpha);
		}
		use_texture(color_record);
	}
	if (depth)
	{
		pass.depthAttachment.texture = depth_record->texture;
		pass.depthAttachment.loadAction = clear && (clear->flags & GPU_CLEAR_DEPTH) ? MTLLoadActionClear : MTLLoadActionLoad;
		pass.depthAttachment.storeAction = MTLStoreActionUnknown;
		pass.depthAttachment.clearDepth = reversed_depth(clear ? clear->depth : 1.0f);
		pass.stencilAttachment.texture = depth_record->texture;
		pass.stencilAttachment.loadAction = clear && (clear->flags & GPU_CLEAR_STENCIL) ? MTLLoadActionClear : MTLLoadActionLoad;
		pass.stencilAttachment.storeAction = MTLStoreActionUnknown;
		pass.stencilAttachment.clearStencil = clear ? clear->stencil & 0xff : 0;
		pass_depth_texture = depth_record->texture;
		use_texture(depth_record);
	}
	pass_traffic_count(pass);
	if (pass_log)
	{
		id<MTLTexture> target = pass.colorAttachments[0].texture;

		[pass_log appendFormat:@"\n  pass: color %u %lux%lu %s/%s", color, (unsigned long)target.width,
			(unsigned long)target.height, color ? load_name(pass.colorAttachments[0].loadAction) : "memoryless",
			store_name(pass.colorAttachments[0].storeAction)];
		if (depth)
			[pass_log appendFormat:@", depth %u %lux%lu %s, stencil %s", depth, (unsigned long)depth_record->texture.width,
				(unsigned long)depth_record->texture.height, load_name(pass.depthAttachment.loadAction),
				load_name(pass.stencilAttachment.loadAction)];
	}
	encoder = [command_buffer() renderCommandEncoderWithDescriptor:pass];
	metal_state_reset(&state_cache);
	encoder_depth_clamp = 0;
	if (metal_debug)
		encoder.label = [NSString stringWithFormat:@"targets %u/%u", color, depth];
	pass_color = color;
	pass_depth = depth;
	pass_commands = 0;
	return YES;
}

/* ---------- clears */

/* the pipelines that draw a clear as a triangle covering the scissor: by
color format (or none), depth-stencil format (or none) and write mask */
static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *clear_pipelines;
static NSMutableDictionary<NSNumber *, id<MTLDepthStencilState>> *clear_depth_states;
/* display.upscaler = "metalfx": a back buffer smaller than its place in the
drawable is scaled up by MetalFX's spatial scaler (edge-aware, with
sharpening) into upscaled, which the present quad then draws 1:1, rather
than stretched by the quad's bilinear sampler. The scaler is made again when
either size changes. */
static BOOL metalfx_wanted;
#ifdef HAVE_METALFX
static id<MTLFXSpatialScaler> upscaler API_AVAILABLE(ios(16.0), tvos(16.0), visionos(1.0));
#endif
static id<MTLTexture> upscaled;

static id<MTLFunction> clear_vertex, clear_fragment, present_vertex, present_fragment;
static id<MTLRenderPipelineState> present_pipeline;
/* gpu_upscale's: the present quad into a BGRA8 render target with no depth */
static id<MTLRenderPipelineState> upscale_pipeline;
static id<MTLSamplerState> present_sampler;

static NSString *const utility_source =
	@"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct ClearOut { float4 position [[position]]; };\n"
	"vertex ClearOut clear_vertex(uint id [[vertex_id]], constant float &depth [[buffer(0)]])\n"
	"{\n"
	"\tClearOut out;\n"
	"\tout.position = float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, depth, 1.0);\n"
	"\treturn out;\n"
	"}\n"
	"fragment float4 clear_fragment(constant float4 &color [[buffer(0)]]) { return color; }\n"
	"struct PresentOut { float4 position [[position]]; float2 coordinates; };\n"
	"vertex PresentOut present_vertex(uint id [[vertex_id]])\n"
	"{\n"
	"\tPresentOut out;\n"
	"\tfloat2 corner = float2(id & 1, id >> 1);\n"
	"\tout.position = float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);\n"
	"\tout.coordinates = corner;\n"
	"\treturn out;\n"
	"}\n"
	"fragment float4 present_fragment(PresentOut in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"\tsampler linear [[sampler(0)]])\n"
	"{\n"
	"\treturn float4(picture.sample(linear, in.coordinates).rgb, 1.0);\n"
	"}\n"
	/* gpu_upscale: alpha too, which the game's later draws may read */
	"fragment float4 upscale_fragment(PresentOut in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"\tsampler linear [[sampler(0)]])\n"
	"{\n"
	"\treturn picture.sample(linear, in.coordinates);\n"
	"}\n";

static id<MTLRenderPipelineState> clear_pipeline(MTLPixelFormat color, MTLPixelFormat depth, uint32_t mask)
{
	NSNumber *key = @((color << 20) | (depth << 4) | mask);
	id<MTLRenderPipelineState> pipeline = clear_pipelines[key];

	if (!pipeline)
	{
		MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
		NSError *error = nil;

		descriptor.vertexFunction = clear_vertex;
		descriptor.fragmentFunction = clear_fragment;
		descriptor.colorAttachments[0].pixelFormat = color;
		descriptor.colorAttachments[0].writeMask = (mask & GPU_CHANNEL_RED ? MTLColorWriteMaskRed : 0) |
			(mask & GPU_CHANNEL_GREEN ? MTLColorWriteMaskGreen : 0) | (mask & GPU_CHANNEL_BLUE ? MTLColorWriteMaskBlue : 0) |
			(mask & GPU_CHANNEL_ALPHA ? MTLColorWriteMaskAlpha : 0);
		descriptor.depthAttachmentPixelFormat = depth;
		descriptor.stencilAttachmentPixelFormat = depth;
		pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
		if (!pipeline)
			host_fatal("Metal: cannot create the clear pipeline: %s", error.description.UTF8String);
		clear_pipelines[key] = pipeline;
	}
	return pipeline;
}

static id<MTLDepthStencilState> clear_depth_state(BOOL depth, BOOL stencil)
{
	NSNumber *key = @((depth ? 1 : 0) | (stencil ? 2 : 0));
	id<MTLDepthStencilState> state = clear_depth_states[key];

	if (!state)
	{
		MTLDepthStencilDescriptor *descriptor = [MTLDepthStencilDescriptor new];
		MTLStencilDescriptor *replace = [MTLStencilDescriptor new];

		descriptor.depthCompareFunction = MTLCompareFunctionAlways;
		descriptor.depthWriteEnabled = depth;
		if (stencil)
		{
			replace.stencilCompareFunction = MTLCompareFunctionAlways;
			replace.depthStencilPassOperation = MTLStencilOperationReplace;
			replace.writeMask = 0xff;
			descriptor.frontFaceStencil = replace;
			descriptor.backFaceStencil = replace;
		}
		state = [device newDepthStencilStateWithDescriptor:descriptor];
		clear_depth_states[key] = state;
	}
	return state;
}

/* rect clamped to a width x height target; NO if nothing is left */
static BOOL clamp_rect(const struct gpu_rect *rect, unsigned long width, unsigned long height, MTLScissorRect *clamped)
{
	long left = rect->x < 0 ? 0 : rect->x, top = rect->y < 0 ? 0 : rect->y;
	long right = (long)rect->x + rect->width, bottom = (long)rect->y + rect->height;

	if (right > (long)width)
		right = (long)width;
	if (bottom > (long)height)
		bottom = (long)height;
	if (right <= left || bottom <= top)
		return NO;
	*clamped = (MTLScissorRect){ (NSUInteger)left, (NSUInteger)top, (NSUInteger)(right - left), (NSUInteger)(bottom - top) };
	return YES;
}

/* each rectangle, as glClear inside glScissor does it: no viewport, depth
range, color mask (the clear's channel_mask instead) or visibility test */
static void gpu_metal_clear(const struct gpu_clear *clear, const struct gpu_rect *rectangles, uint32_t count)
{
	@autoreleasepool
	{
		uint32_t flags = clear->flags & (GPU_CLEAR_COLOR | (clear->depth_target ? GPU_CLEAR_DEPTH | GPU_CLEAR_STENCIL : 0));
		unsigned long width, height, index;
		uint32_t mask = flags & GPU_CLEAR_COLOR ? clear->channel_mask & 0xf : 0;
		MetalTexture *color = texture_record(clear->color_target), *depth = texture_record(clear->depth_target);

		float color_value[4], depth_value = reversed_depth(clear->depth);

		frame_begin();
		if (!flags || !count || (!mask && !(flags & (GPU_CLEAR_DEPTH | GPU_CLEAR_STENCIL))))
			return;
		target_size(clear->color_target, clear->depth_target, &width, &height);
		/* a whole-target clear of every channel, before anything else in the
		pass, is the pass's load action */
		if (count == 1 && rectangles[0].x <= 0 && rectangles[0].y <= 0 &&
			(long)rectangles[0].x + rectangles[0].width >= (long)width &&
			(long)rectangles[0].y + rectangles[0].height >= (long)height &&
			(!(flags & GPU_CLEAR_COLOR) || mask == 0xf) &&
			!(encoder && pass_color == clear->color_target && pass_depth == clear->depth_target && pass_commands))
		{
			struct gpu_clear whole = *clear;

			whole.flags = flags;
			pass_begin(clear->color_target, clear->depth_target, &whole);
			return;
		}
		if (!pass_begin(clear->color_target, clear->depth_target, NULL))
		{
			GPU_VALIDATION_REPORT(GPU_VALIDATION_PASS_NOT_BEGUN, 1, "Metal: no pass for targets %u and %u (one has no "
				"storage); the clear is dropped", clear->color_target, clear->depth_target);
			return;
		}
		color_value[0] = (float)((clear->color >> 16) & 0xff) / 255.0f;
		color_value[1] = (float)((clear->color >> 8) & 0xff) / 255.0f;
		color_value[2] = (float)(clear->color & 0xff) / 255.0f;
		color_value[3] = (float)(clear->color >> 24) / 255.0f;
		/* every pass has a BGRA8 color attachment: a depth-only pass a scratch
		one (pass_begin), which a clear leaves alone */
		[encoder setRenderPipelineState:clear_pipeline(MTLPixelFormatBGRA8Unorm,
			depth && depth->texture ? depth->texture.pixelFormat : MTLPixelFormatInvalid, color ? mask : 0)];
		[encoder setDepthStencilState:clear_depth_state((flags & GPU_CLEAR_DEPTH) != 0, (flags & GPU_CLEAR_STENCIL) != 0)];
		[encoder setStencilReferenceValue:clear->stencil & 0xff];
		[encoder setCullMode:MTLCullModeNone];
		[encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
		[encoder setViewport:(MTLViewport){ 0.0, 0.0, (double)width, (double)height, 0.0, 1.0 }];
		[encoder setVertexBytes:&depth_value length:sizeof(depth_value) atIndex:0];
		[encoder setFragmentBytes:color_value length:sizeof(color_value) atIndex:0];
		/* a clear passes no samples to a visibility test, as glClear doesn't */
		if (visibility.entry_open)
			[encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
		for (index = 0; index < count; index++)
		{
			MTLScissorRect scissor;

			if (!clamp_rect(&rectangles[index], width, height, &scissor))
				continue;
			[encoder setScissorRect:scissor];
			[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			pass_commands++;
		}
		/* the test's next draw opens a new entry rather than counting into
		this one again in the same pass */
		visibility.entry_open = 0;
		/* the next draw sets again what the quad changed */
		metal_state_clear_quad(&state_cache);
	}
}

/* ---------- draws */

/* per-attribute vertex buffers start here (nv2a_msl.c's bindings) */
#define VERTEX_STREAM_BINDING 10

/* the vertex shader's description of each attribute (AttributeTable,
nv2a_msl.c): the GPU_ATTRIBUTE_* format; the stream (GPU_STREAM_CONSTANT or
above: the constant); its byte offset in the buffer bound for it; and the
vertex stride */
struct metal_attribute
{
	uint32_t format, stream, offset, stride;
};

struct metal_attribute_table
{
	struct metal_attribute entries[GPU_ATTRIBUTE_COUNT];
	float constants[GPU_ATTRIBUTE_COUNT][4];
};

/* what a pipeline is made of besides the shaders' text. Every descriptor
field draw_pipeline sets must be in it (a sample count for anti-aliasing,
say): the caches return a pipeline for an equal key, and nothing checks the
descriptor against the target it draws to. */
struct pipeline_key
{
	gpu_shader vertex_shader, pixel_shader;
	uint8_t blend, source, destination, operation, write_mask, alpha_source, alpha_destination;
	/* the stages that rebuild their border colors (exact_borders) */
	uint8_t exact_borders;
	uint32_t color_format, depth_format;
	/* the vertex function's specialization (attribute_kinds) */
	uint8_t attribute_kinds[GPU_ATTRIBUTE_COUNT];
};

/* debug.gl_debug's count of pipeline changes (gpu_metal_draw), logged every
600 frames, and the last draw's shaders */
static struct
{
	unsigned long changes, same_shaders;
	gpu_shader vertex_shader, pixel_shader;
} pipeline_changes;

/* a direct-mapped cache in front of pipelines, depth_states and samplers:
nearly every draw repeats a recent key, which then costs a hash and a
compare of its bytes rather than an NSData and a dictionary lookup. The
dictionaries keep every object for good, so the cache needn't retain them. */
#define FRONT_CACHE_ENTRIES 256
#define FRONT_CACHE_KEY 48
struct front_cache
{
	struct
	{
		uint8_t key[FRONT_CACHE_KEY];
		__unsafe_unretained id object;
	} entries[FRONT_CACHE_ENTRIES];
};

static struct front_cache pipeline_cache, depth_state_cache, sampler_cache;

static unsigned long front_cache_index(const void *key, size_t length)
{
	const uint8_t *bytes = key;
	uint32_t hash = 2166136261u;
	size_t index;

	/* FNV-1a over 32-bit words (and any bytes after the last whole one) */
	for (index = 0; index + 4 <= length; index += 4)
	{
		uint32_t word;

		memcpy(&word, bytes + index, sizeof(word));
		hash = (hash ^ word) * 16777619u;
	}
	for (; index < length; index++)
		hash = (hash ^ bytes[index]) * 16777619u;
	/* the multiplies carry a difference only upward, so one in the last
	word's top byte (attribute kind 15) reaches only bits 24-31: fold those
	down into the 8 bits the index keeps */
	hash ^= hash >> 16;
	return (hash ^ (hash >> 8)) & (FRONT_CACHE_ENTRIES - 1);
}

/* the object cached for key, or nil */
static id front_cache_get(struct front_cache *cache, const void *key, size_t length)
{
	unsigned long index = front_cache_index(key, length);

	return cache->entries[index].object && !memcmp(cache->entries[index].key, key, length) ?
		cache->entries[index].object : nil;
}

static void front_cache_put(struct front_cache *cache, const void *key, size_t length, id object)
{
	unsigned long index = front_cache_index(key, length);

	memcpy(cache->entries[index].key, key, length);
	cache->entries[index].object = object;
}

/* every pipeline made, by its pipeline_key: the object, or NSNull for one
that can't be made, retained for good (pipeline_publish). Draws claim the
keys they miss, and workers publish them under display.async_shaders */
static struct metal_compile_table pipeline_table;
static NSMutableDictionary<NSData *, id<MTLDepthStencilState>> *depth_states;
static NSMutableDictionary<NSData *, id<MTLSamplerState>> *samplers;
/* bound where a draw has nothing: black textures of each type (as GL samples
a texture with no storage), a sampler, and a vertex buffer for constant
attributes */
static id<MTLTexture> empty_textures[4];
static id<MTLSamplerState> empty_sampler;
static id<MTLBuffer> empty_buffer;
/* the frame's last constant snapshot, which draws share until a serial moves */
static struct
{
	unsigned long frame;
	uint64_t constants_serial;
	uint32_t uniforms_serial;
	gpu_buffer buffer;
	uint32_t offset;
	BOOL valid;
} snapshot;
static _Atomic BOOL layout_checked;

static MTLBlendFactor blend_factor(uint8_t factor)
{
	switch (factor)
	{
	case GPU_BLEND_ONE: return MTLBlendFactorOne;
	case GPU_BLEND_SOURCE_COLOR: return MTLBlendFactorSourceColor;
	case GPU_BLEND_ONE_MINUS_SOURCE_COLOR: return MTLBlendFactorOneMinusSourceColor;
	case GPU_BLEND_SOURCE_ALPHA: return MTLBlendFactorSourceAlpha;
	case GPU_BLEND_ONE_MINUS_SOURCE_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
	case GPU_BLEND_DESTINATION_ALPHA: return MTLBlendFactorDestinationAlpha;
	case GPU_BLEND_ONE_MINUS_DESTINATION_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
	case GPU_BLEND_DESTINATION_COLOR: return MTLBlendFactorDestinationColor;
	case GPU_BLEND_ONE_MINUS_DESTINATION_COLOR: return MTLBlendFactorOneMinusDestinationColor;
	case GPU_BLEND_SOURCE_ALPHA_SATURATE: return MTLBlendFactorSourceAlphaSaturated;
	case GPU_BLEND_CONSTANT_COLOR: return MTLBlendFactorBlendColor;
	case GPU_BLEND_ONE_MINUS_CONSTANT_COLOR: return MTLBlendFactorOneMinusBlendColor;
	case GPU_BLEND_CONSTANT_ALPHA: return MTLBlendFactorBlendAlpha;
	case GPU_BLEND_ONE_MINUS_CONSTANT_ALPHA: return MTLBlendFactorOneMinusBlendAlpha;
	default: return MTLBlendFactorZero;
	}
}

static MTLBlendOperation blend_operation(uint8_t operation)
{
	switch (operation)
	{
	case GPU_BLEND_OP_SUBTRACT: return MTLBlendOperationSubtract;
	case GPU_BLEND_OP_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
	case GPU_BLEND_OP_MIN: return MTLBlendOperationMin;
	case GPU_BLEND_OP_MAX: return MTLBlendOperationMax;
	default: return MTLBlendOperationAdd;
	}
}

/* GPU_COMPARE_* is MTLCompareFunction's order */
static MTLCompareFunction compare_function(uint8_t function)
{
	return function <= GPU_COMPARE_ALWAYS ? (MTLCompareFunction)function : MTLCompareFunctionNever;
}

/* Reversed-Z: the vertex shaders write w - z (nv2a_vsh.c's MSL epilogue), so
a depth the game gives as d is stored as 1 - d, and its depth tests and
biases point the other way. With Depth32Float that keeps depth precision at a
distance, which a longer draw distance (rasterizer_far_clip_distance) needs */
static float reversed_depth(float depth)
{
	return 1.0f - depth;
}

static MTLCompareFunction depth_compare_function(uint8_t function)
{
	switch (function)
	{
	case GPU_COMPARE_LESS: return MTLCompareFunctionGreater;
	case GPU_COMPARE_LESS_EQUAL: return MTLCompareFunctionGreaterEqual;
	case GPU_COMPARE_GREATER: return MTLCompareFunctionLess;
	case GPU_COMPARE_GREATER_EQUAL: return MTLCompareFunctionLessEqual;
	default: return compare_function(function);
	}
}

static MTLStencilOperation stencil_operation(uint8_t operation)
{
	switch (operation)
	{
	case GPU_STENCIL_ZERO: return MTLStencilOperationZero;
	case GPU_STENCIL_REPLACE: return MTLStencilOperationReplace;
	case GPU_STENCIL_INCREMENT_CLAMP: return MTLStencilOperationIncrementClamp;
	case GPU_STENCIL_DECREMENT_CLAMP: return MTLStencilOperationDecrementClamp;
	case GPU_STENCIL_INVERT: return MTLStencilOperationInvert;
	case GPU_STENCIL_INCREMENT_WRAP: return MTLStencilOperationIncrementWrap;
	case GPU_STENCIL_DECREMENT_WRAP: return MTLStencilOperationDecrementWrap;
	default: return MTLStencilOperationKeep;
	}
}

static MTLColorWriteMask write_mask(uint8_t mask)
{
	return (mask & 1 ? MTLColorWriteMaskRed : 0) | (mask & 2 ? MTLColorWriteMaskGreen : 0) |
		(mask & 4 ? MTLColorWriteMaskBlue : 0) | (mask & 8 ? MTLColorWriteMaskAlpha : 0);
}

/* with debug.gl_debug, once: struct Uniforms (nv2a_msl.c) must have struct
gpu_uniforms' layout, which the backend copies as is */
static void check_uniform_layout(MTLRenderPipelineReflection *reflection)
{
	NSMutableArray<id<MTLBinding>> *bindings = [NSMutableArray arrayWithArray:reflection.vertexBindings];
	unsigned long checked = 0, wrong = 0;

	[bindings addObjectsFromArray:reflection.fragmentBindings];
	for (id<MTLBinding> binding in bindings)
	{
		MTLStructType *type;

		if (![binding.name isEqualToString:@"u"] || binding.type != MTLBindingTypeBuffer || !binding.used)
			continue;
		type = ((id<MTLBufferBinding>)binding).bufferStructType;
#define CHECK_ROW(name, glsl_type, count, uniform_stage) GPU_UNIFORM_IF_NOT_CONSTANTS_##uniform_stage( \
		{ \
			MTLStructMember *member = [type memberByName:@#name]; \
			if (!member || member.offset != offsetof(struct gpu_uniforms, name)) \
			{ \
				platform_log("uniform layout: " #name " is at %ld in MSL, %lu in C", \
					member ? (long)member.offset : -1L, (unsigned long)offsetof(struct gpu_uniforms, name)); \
				wrong++; \
			} \
			checked++; \
		})
		GPU_UNIFORMS(CHECK_ROW)
#undef CHECK_ROW
	}
	platform_log("uniform layout: %lu rows checked, %lu wrong", checked, wrong);
}

/* the pipeline for a draw, or nil if it can't be made (cached either way) */
/* a draw's attributes, decided once (gpu_metal_draw) for both the pipeline
(the vertex function's specialization) and the bindings (bind_attributes), so
the two can't disagree: each streamed attribute's buffer record, nil for a
constant (GPU_STREAM_NONE never arrives: the front end makes every unused
attribute a constant; a stream with no buffer is one too), and its kind, as
the vertex shaders' function constants take it (nv2a_msl.c): 0 for a
constant, else its GPU_ATTRIBUTE_* format */
struct draw_attributes
{
	__unsafe_unretained MetalBuffer *records[GPU_ATTRIBUTE_COUNT];
	uint8_t kinds[GPU_ATTRIBUTE_COUNT];
};

static void draw_attributes_decide(const struct gpu_draw *draw, struct draw_attributes *attributes)
{
	int index;

	for (index = 0; index < GPU_ATTRIBUTE_COUNT; index++)
	{
		const struct gpu_vertex_attribute *attribute = &draw->attributes[index];

		attributes->records[index] = attribute->stream < GPU_STREAM_CONSTANT ?
			buffer_record(draw->streams[attribute->stream].buffer) : nil;
		attributes->kinds[index] = attributes->records[index] ? attribute->format : 0;
	}
}

/* a vertex shader's function specialized for these attribute kinds, which
folds its fetches' format switches away; made with the pipeline, so once
for each pair. The unspecialized function if it can't be made (logged
once): it reads the same kinds from the attribute table. play: made for a
draw, not a map's list. (Workers building pipelines call it too, hence the
lock on the record.) */
static id<MTLFunction> vertex_function(MetalShader *vertex, const uint8_t kinds[GPU_ATTRIBUTE_COUNT], int play)
{
	NSData *name = [NSData dataWithBytes:kinds length:GPU_ATTRIBUTE_COUNT];
	id<MTLFunction> function;

	if (!vertex->library || specialize_off)
		return vertex->function;
	@synchronized (vertex)
	{
		if (!vertex->specialized)
			vertex->specialized = [NSMutableDictionary dictionary];
		function = vertex->specialized[name];
		if (!function)
		{
			MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
			NSError *error = nil;
			NSUInteger index;
			CFTimeInterval started;

			for (index = 0; index < GPU_ATTRIBUTE_COUNT; index++)
			{
				uint32_t kind = kinds[index];

				[values setConstantValue:&kind type:MTLDataTypeUInt atIndex:index];
			}
			started = CACurrentMediaTime();
			function = [vertex->library newFunctionWithName:@"vertex_main" constantValues:values error:&error];
			compile_count(COMPILE_SPECIALIZE, started, play);
			if (!function)
			{
				/* (once for each shader and set of kinds, which the dictionary
				then keeps with the fallback) */
				platform_log("Metal: cannot specialize vertex shader %p for its attributes, which it then reads per draw: %s",
					(__bridge void *)vertex, error.localizedDescription.UTF8String);
				GPU_VALIDATION_REPORT(GPU_VALIDATION_SPECIALIZATION_FAILED, 0, "Metal: a vertex shader cannot be specialized "
					"for its attributes: %s", error.localizedDescription.UTF8String);
				function = vertex->function;
			}
			vertex->specialized[name] = function;
		}
	}
	return function;
}

/* a pipeline's descriptor, its functions made first (vertex_function,
pixel_function), which may compile */
static MTLRenderPipelineDescriptor *pipeline_descriptor(const struct pipeline_key *key, MetalShader *vertex,
	MetalShader *pixel, int play)
{
	MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
	MTLRenderPipelineColorAttachmentDescriptor *attachment = descriptor.colorAttachments[0];

	descriptor.vertexFunction = vertex_function(vertex, key->attribute_kinds, play);
	descriptor.fragmentFunction = pixel_function(pixel, key->exact_borders) ?: pixel->function;
	attachment.pixelFormat = (MTLPixelFormat)key->color_format;
	attachment.writeMask = write_mask(key->write_mask);
	if (key->blend)
	{
		attachment.blendingEnabled = YES;
		attachment.sourceRGBBlendFactor = blend_factor(key->source);
		attachment.sourceAlphaBlendFactor = blend_factor(key->alpha_source);
		attachment.destinationRGBBlendFactor = blend_factor(key->destination);
		attachment.destinationAlphaBlendFactor = blend_factor(key->alpha_destination);
		attachment.rgbBlendOperation = attachment.alphaBlendOperation = blend_operation(key->operation);
	}
	descriptor.depthAttachmentPixelFormat = (MTLPixelFormat)key->depth_format;
	descriptor.stencilAttachmentPixelFormat = (MTLPixelFormat)key->depth_format;
	return descriptor;
}

/* the pipelines this device has compiled before, kept between launches in
the app's caches folder: a pipeline found there is loaded rather than
compiled, and one that isn't is compiled and added, then written out at the
next warm_end (archive_save). The file's name holds the app's build, the
GPU and the system's version, which an archive is only good for; any other
file there is a stale one and is deleted, and one that can't be read is
replaced by an empty archive. */
static id<MTLBinaryArchive> archive;
static NSURL *archive_url;
static BOOL archive_dirty;
/* the archive's reads (pipelines loaded from it, in parallel at warm_end)
share this lock, and its writes (pipelines added, the file written) hold it
alone: Metal's headers promise nothing about using one archive from several
threads at once */
static pthread_rwlock_t archive_lock = PTHREAD_RWLOCK_INITIALIZER;

static void archive_open(void)
{
	NSFileManager *files = [NSFileManager defaultManager];
	/* (the bundle's own folder: an app outside a sandbox, the Mac app, shares
	its caches folder with every other) */
	NSURL *folder = [[[files URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject
		URLByAppendingPathComponent:[NSBundle mainBundle].bundleIdentifier ?: @"VISR" isDirectory:YES]
		URLByAppendingPathComponent:@"metal-pipelines" isDirectory:YES];
	NSString *build = [NSBundle mainBundle].infoDictionary[@"CFBundleVersion"] ?: @"0";
	NSString *system = [NSProcessInfo processInfo].operatingSystemVersionString;
	NSMutableString *name = [NSMutableString stringWithFormat:@"%@ %@ %@", build, device.name, system];
	MTLBinaryArchiveDescriptor *descriptor = [MTLBinaryArchiveDescriptor new];
	NSError *error = nil;
	NSUInteger index;

	/* binary archives need GPU family Apple3 (an A9) or a Mac */
	if (![device supportsFamily:MTLGPUFamilyApple3] && ![device supportsFamily:MTLGPUFamilyMac2])
	{
		platform_log("Metal: no pipeline archive (this GPU has no binary archives); every launch compiles its pipelines");
		return;
	}

	/* a file name: letters, digits, dots and dashes */
	for (index = 0; index < name.length; index++)
	{
		unichar c = [name characterAtIndex:index];

		if (!(isalnum(c) || c == '.' || c == '-'))
			[name replaceCharactersInRange:NSMakeRange(index, 1) withString:@"_"];
	}
	[files createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:nil];
	archive_url = [folder URLByAppendingPathComponent:[name stringByAppendingString:@".binarchive"]];
	for (NSURL *old in [files contentsOfDirectoryAtURL:folder includingPropertiesForKeys:nil options:0 error:nil])
	{
		if (![old.lastPathComponent isEqualToString:archive_url.lastPathComponent])
		{
			platform_log("Metal: deleting a stale pipeline archive, %s", old.lastPathComponent.UTF8String);
			[files removeItemAtURL:old error:nil];
		}
	}
	if ([files fileExistsAtPath:archive_url.path])
	{
		descriptor.url = archive_url;
		archive = [device newBinaryArchiveWithDescriptor:descriptor error:&error];
		if (!archive)
		{
			platform_log("Metal: the pipeline archive can't be read (%s); starting a new one",
				error.localizedDescription.UTF8String);
			[files removeItemAtURL:archive_url error:nil];
		}
	}
	if (!archive)
	{
		descriptor.url = nil;
		archive = [device newBinaryArchiveWithDescriptor:descriptor error:&error];
		if (!archive)
			platform_log("Metal: no pipeline archive: %s", error.localizedDescription.UTF8String);
	}
	platform_log("Metal: pipeline archive %s", archive_url.lastPathComponent.UTF8String);
}

/* writes the archive out if pipelines were added since (on a worker, under
display.async_shaders, while others may add to it) */
static void archive_save(void)
{
	NSError *error = nil;
	CFTimeInterval started = CACurrentMediaTime();

	if (!archive)
		return;
	pthread_rwlock_wrlock(&archive_lock);
	if (!archive_dirty)
	{
		pthread_rwlock_unlock(&archive_lock);
		return;
	}
	if ([archive serializeToURL:archive_url error:&error])
	{
		platform_log("Metal: wrote the pipeline archive in %.0f ms", (CACurrentMediaTime() - started) * 1e3);
		archive_dirty = NO;
	}
	else
	{
		/* (diagnosis, 2026-10-06: the Mac app's writes to its caches folder
		fail with "cannot create temporary file"; try the temporary folder,
		then move the file into place) */
		NSURL *temporary = [NSURL fileURLWithPath:[NSTemporaryDirectory()
			stringByAppendingPathComponent:archive_url.lastPathComponent]];
		NSError *second = nil, *moved = nil;
		char folder[1024];

		struct rlimit limit = { 0 };
		int descriptor, open_files = 0;
		char user_temporary[1024] = "?";

		getrlimit(RLIMIT_NOFILE, &limit);
		for (descriptor = 0; descriptor < (int)(limit.rlim_cur < 65536 ? limit.rlim_cur : 65536); descriptor++)
			open_files += fcntl(descriptor, F_GETFD) != -1;
		confstr(_CS_DARWIN_USER_TEMP_DIR, user_temporary, sizeof(user_temporary));
		platform_log("Metal: the pipeline archive write failed (%s); TMPDIR %s, user temporary folder %s, HOME %s, "
			"working folder %s, its folder %s, %d files open of %llu (hard limit %llu)",
			error.localizedDescription.UTF8String, getenv("TMPDIR") ?: "(unset)", user_temporary,
			getenv("HOME") ?: "(unset)", getcwd(folder, sizeof(folder)) ?: "?",
			[[NSFileManager defaultManager] isWritableFileAtPath:archive_url.URLByDeletingLastPathComponent.path] ?
			"writable" : "not writable", open_files, (unsigned long long)limit.rlim_cur, (unsigned long long)limit.rlim_max);
		[[NSFileManager defaultManager] removeItemAtURL:temporary error:nil];
		if ([archive serializeToURL:temporary error:&second])
		{
			[[NSFileManager defaultManager] removeItemAtURL:archive_url error:nil];
			if ([[NSFileManager defaultManager] moveItemAtURL:temporary toURL:archive_url error:&moved])
			{
				archive_dirty = NO;
				platform_log("Metal: wrote the pipeline archive by way of the temporary folder in %.0f ms (directly: %s)",
					(CACurrentMediaTime() - started) * 1e3, error.localizedDescription.UTF8String);
			}
			else
				platform_log("Metal: wrote the pipeline archive to the temporary folder but can't move it: %s",
					moved.localizedDescription.UTF8String);
		}
		else
			platform_log("Metal: can't write the pipeline archive %s: %s; nor to the temporary folder: %s",
				archive_url.path.UTF8String, error.description.UTF8String, second.description.UTF8String);
	}
	pthread_rwlock_unlock(&archive_lock);
}

/* the pipeline for a descriptor, or NSNull, logged, if it can't be made
(as a GL link failure: the draws are skipped): loaded from the archive when
it has it, else compiled and added to it. Safe on any thread. */
static id pipeline_make(MTLRenderPipelineDescriptor *descriptor)
{
	NSError *error = nil;
	id pipeline = nil;

	if (descriptor.vertexFunction && descriptor.fragmentFunction)
	{
		if (archive)
		{
			descriptor.binaryArchives = @[archive];
			pthread_rwlock_rdlock(&archive_lock);
			pipeline = [device newRenderPipelineStateWithDescriptor:descriptor
				options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
			pthread_rwlock_unlock(&archive_lock);
		}
		if (!pipeline)
		{
			/* (compiled without the archive: Metal would read it outside
			archive_lock, while another thread adds to it) */
			descriptor.binaryArchives = nil;
			pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
			if (pipeline && archive)
			{
				pthread_rwlock_wrlock(&archive_lock);
				if ([archive addRenderPipelineFunctionsWithDescriptor:descriptor error:nil])
					archive_dirty = YES;
				pthread_rwlock_unlock(&archive_lock);
			}
		}
	}

	if (!pipeline)
	{
		platform_log("cannot link a shader program: %s",
			error ? error.localizedDescription.UTF8String : "a shader did not compile");
		GPU_VALIDATION_REPORT(GPU_VALIDATION_PIPELINE_FAILED, 1, "Metal: a pipeline cannot be built: %s",
			error ? error.localizedDescription.UTF8String : "a shader did not compile");
		pipeline = [NSNull null];
	}
	return pipeline;
}

/* a key's description in the front end's terms (gpu.h), and back */
static void pipeline_describe(const struct pipeline_key *key, struct gpu_pipeline_description *description)
{
	memset(description, 0, sizeof(*description));
	description->vertex_shader = key->vertex_shader;
	description->pixel_shader = key->pixel_shader;
	description->blend = key->blend;
	description->source = key->source;
	description->destination = key->destination;
	description->operation = key->operation;
	description->alpha_source = key->alpha_source;
	description->alpha_destination = key->alpha_destination;
	description->write_mask = key->write_mask;
	description->exact_borders = key->exact_borders;
	description->depth = key->depth_format != MTLPixelFormatInvalid;
	memcpy(description->attribute_kinds, key->attribute_kinds, sizeof(description->attribute_kinds));
}

static void pipeline_key_from_description(const struct gpu_pipeline_description *description, struct pipeline_key *key)
{
	memset(key, 0, sizeof(*key));
	key->vertex_shader = description->vertex_shader;
	key->pixel_shader = description->pixel_shader;
	key->blend = description->blend != 0;
	if (key->blend)
	{
		key->source = description->source;
		key->destination = description->destination;
		key->operation = description->operation;
		/* alpha's factors are color's but in the stereo HUD layer's draws
		(gpu_blend_state.alpha_separate); a list's line from before it
		carried them reads as color's (halo_shader_list_warm) */
		key->alpha_source = description->alpha_source;
		key->alpha_destination = description->alpha_destination;
	}
	key->write_mask = description->write_mask & 0xf;
	key->exact_borders = description->exact_borders & 0xf;
	/* every pass draws to BGRA8 (pass_begin's scratch color in a depth-only
	one) and depth-stencil targets are Depth32Float_Stencil8 */
	key->color_format = (uint32_t)MTLPixelFormatBGRA8Unorm;
	key->depth_format = (uint32_t)(description->depth ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatInvalid);
	memcpy(key->attribute_kinds, description->attribute_kinds, sizeof(key->attribute_kinds));
}

/* pipelines built for draws outside warming, which gpu_pipeline_built_take
hands to the front end to log and record (shader_list.c), oldest first */
static NSMutableData *built_during_play;
static unsigned long built_taken;

/* a pipeline built on this thread (the game thread, or a worker): its
descriptor, whose functions may compile first, then the pipeline; the first
under debug.gl_debug also checks the uniform layout. play: for a draw */
static id pipeline_build(const struct pipeline_key *key, MetalShader *vertex, MetalShader *pixel, int play)
{
	MTLRenderPipelineDescriptor *descriptor = pipeline_descriptor(key, vertex, pixel, play);
	CFTimeInterval started = CACurrentMediaTime();
	id pipeline = nil;

	if (metal_debug && !atomic_load(&layout_checked) && descriptor.vertexFunction && descriptor.fragmentFunction)
	{
		MTLRenderPipelineReflection *reflection = nil;

		pipeline = [device newRenderPipelineStateWithDescriptor:descriptor
			options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo reflection:&reflection error:nil];
		if (pipeline && !atomic_exchange(&layout_checked, YES))
			check_uniform_layout(reflection);
	}
	if (!pipeline)
		pipeline = pipeline_make(descriptor);
	if (play)
		compile_count(COMPILE_PIPELINE, started, 1);
	return pipeline;
}

/* files a key's pipeline (or NSNull) in pipeline_table, which keeps it for
good; returns what the table holds for the key (the first, if another got
there before), nil if it couldn't be filed */
static id pipeline_publish(const struct pipeline_key *key, id pipeline)
{
	void *extra = metal_compile_publish(&pipeline_table, key, sizeof(*key), (__bridge_retained void *)pipeline);

	if (!extra)
		return pipeline;
	CFBridgingRelease(extra);
	return (__bridge id)metal_compile_lookup(&pipeline_table, key, sizeof(*key));
}

/* builds a claimed key's pipeline on a worker once both shaders are
compiled (their groups empty), and publishes it; joins group, if any, until
then */
static void pipeline_request(const struct pipeline_key *key, MetalShader *vertex, MetalShader *pixel,
	dispatch_group_t group, int play)
{
	struct pipeline_key copy = *key;
	dispatch_queue_t queue = compile_queue(play);

	atomic_fetch_add(&compiles_outstanding, 1);
	if (group)
		dispatch_group_enter(group);
	dispatch_group_notify(vertex->compiled, queue, ^
	{
		dispatch_group_notify(pixel->compiled, queue, ^
		{
			@autoreleasepool
			{
				CFTimeInterval begun = CACurrentMediaTime();
				id pipeline = pipeline_build(&copy, vertex, pixel, play);

				metal_compile_finished(&compile_totals, COMPILE_PIPELINE, CACurrentMediaTime() - begun);
				pipeline_publish(&copy, pipeline);
				atomic_fetch_sub(&compiles_outstanding, 1);
				if (group)
					dispatch_group_leave(group);
			}
		});
	});
}

/* the pipeline for a draw, or nil if it can't be made (cached either way)
or, with pending set, isn't made yet (display.async_shaders: it's building) */
static id<MTLRenderPipelineState> draw_pipeline(const struct gpu_draw *draw, const struct draw_attributes *attributes,
	MetalShader *vertex, MetalShader *pixel, unsigned exact, MTLPixelFormat color, MTLPixelFormat depth, BOOL *pending)
{
	struct pipeline_key key;
	void *found;
	id pipeline;

	*pending = NO;
	memset(&key, 0, sizeof(key));
	key.vertex_shader = draw->vertex_shader;
	key.pixel_shader = draw->pixel_shader;
	key.blend = draw->blend.enable != 0;
	/* GL sets blending's factors only while it is on (gpu_gl.c) */
	if (key.blend)
	{
		key.source = draw->blend.source;
		key.destination = draw->blend.destination;
		key.operation = draw->blend.operation;
		key.alpha_source = draw->blend.alpha_separate ? draw->blend.alpha_source : key.source;
		key.alpha_destination = draw->blend.alpha_separate ? draw->blend.alpha_destination : key.destination;
	}
	key.write_mask = draw->color_target ? draw->blend.color_write_mask & 0xf : 0;
	key.exact_borders = (uint8_t)exact;
	key.color_format = (uint32_t)color;
	key.depth_format = (uint32_t)depth;
	memcpy(key.attribute_kinds, attributes->kinds, sizeof(key.attribute_kinds));
	_Static_assert(sizeof(key) <= FRONT_CACHE_KEY, "pipeline_key fits the front cache");
	_Static_assert(sizeof(key) <= METAL_COMPILE_KEY_BYTES, "pipeline_key fits the pipeline table");
	if ((pipeline = front_cache_get(&pipeline_cache, &key, sizeof(key))))
		return pipeline == [NSNull null] ? nil : pipeline;
	switch (metal_compile_claim(&pipeline_table, &key, sizeof(key), &found))
	{
	case METAL_CLAIM_PENDING:
		*pending = YES;
		return nil;
	case METAL_CLAIM_READY:
		pipeline = (__bridge id)found;
		break;
	default:
	{
		struct gpu_pipeline_description built;

		pipeline_describe(&key, &built);
		[built_during_play appendBytes:&built length:sizeof(built)];
		if (async_compiles)
		{
			pipeline_request(&key, vertex, pixel, nil, 1);
			*pending = YES;
			return nil;
		}
		pipeline = pipeline_publish(&key, pipeline_build(&key, vertex, pixel, 1));
		if (!pipeline)
			return nil;
	}
	}
	front_cache_put(&pipeline_cache, &key, sizeof(key), pipeline);
	return pipeline == [NSNull null] ? nil : pipeline;
}

static uint32_t gpu_metal_pipeline_built_take(struct gpu_pipeline_description *description)
{
	unsigned long count = built_during_play.length / sizeof(*description);

	if (built_taken >= count)
	{
		[built_during_play setLength:0];
		built_taken = 0;
		return 0;
	}
	memcpy(description, (const struct gpu_pipeline_description *)built_during_play.bytes + built_taken++,
		sizeof(*description));
	return 1;
}

static uint32_t gpu_metal_compiles_outstanding(void)
{
	return (uint32_t)atomic_load(&compiles_outstanding);
}

/* ---------- compiling at map load (gpu.h; the front end's shader_list.c) */

static uint32_t gpu_metal_warm_list_read(const char *name, char *text, uint32_t size)
{
	@autoreleasepool
	{
		NSString *path = [[NSBundle mainBundle] pathForResource:@(name) ofType:@"txt" inDirectory:@"shader-lists"];
		NSData *list = path ? [NSData dataWithContentsOfFile:path] : nil;

		if (!list)
			return 0;
		if (list.length <= size)
			memcpy(text, list.bytes, list.length);
		return (uint32_t)list.length;
	}
}

static void gpu_metal_warm_begin(void)
{
	warming = YES;
	warm_list = [NSMutableData data];
	warm_group = dispatch_group_create();
}

static void gpu_metal_pipeline_warm(const struct gpu_pipeline_description *description)
{
	if (warming)
		[warm_list appendBytes:description length:sizeof(*description)];
}

/* display.async_shaders: claims the listed pipelines and starts them
building on workers, each once its shaders are compiled (their
specializations and variants with it), and returns at once: a draw that
needs one first skips until it's published. The archive is written once
they're all done */
static void warm_end_background(void)
{
	const struct gpu_pipeline_description *list = warm_list.bytes;
	unsigned long count = warm_list.length / sizeof(*list), index, requested = 0;
	dispatch_group_t shaders_compiled = warm_group, pipelines_built = dispatch_group_create();
	CFTimeInterval started = CACurrentMediaTime();

	warming = NO;
	for (index = 0; index < count; index++)
	{
		struct pipeline_key key;
		MetalShader *vertex, *pixel;
		void *found;

		pipeline_key_from_description(&list[index], &key);
		vertex = table_get(shaders, key.vertex_shader);
		pixel = table_get(shaders, key.pixel_shader);
		if (!vertex || !pixel || metal_compile_claim(&pipeline_table, &key, sizeof(key), &found) != METAL_CLAIM_NEW)
			continue;
		pipeline_request(&key, vertex, pixel, pipelines_built, 0);
		requested++;
	}
	warm_list = nil;
	warm_group = nil;
	/* (the shaders no listed pipeline uses too) */
	dispatch_group_notify(pipelines_built, compile_queue(0), ^
	{
		dispatch_group_notify(shaders_compiled, compile_queue(0), ^
		{
			@autoreleasepool
			{
				archive_save();
				platform_log("Metal: warmed %lu pipelines (%lu listed) in the background, done %.0f ms after the load "
					"started them", requested, count, (CACurrentMediaTime() - started) * 1e3);
			}
		});
	});
	platform_log("Metal: %lu listed pipelines building in the background; the load goes on", requested);
}

/* waits for the shaders, then builds the listed pipelines: the vertex
functions' specializations and the pipelines themselves in parallel, the
bookkeeping (dictionaries, which aren't thread-safe) on this thread (or,
under display.async_shaders, warm_end_background) */
static void gpu_metal_warm_end(void)
{
	@autoreleasepool
	{
		const struct gpu_pipeline_description *list = warm_list.bytes;
		unsigned long count = warm_list.length / sizeof(*list), index;
		NSMutableArray *names = [NSMutableArray array], *descriptors = [NSMutableArray array];
		NSMutableSet *named = [NSMutableSet set];
		NSMutableArray *specialize = [NSMutableArray array];
		NSMutableDictionary<NSData *, id> *specialized = [NSMutableDictionary dictionary];
		CFTimeInterval started = CACurrentMediaTime(), shaders_done;
		NSMutableArray *made;

		if (!warming)
			return;
		if (async_compiles)
		{
			warm_end_background();
			return;
		}
		dispatch_group_wait(warm_group, DISPATCH_TIME_FOREVER);
		warming = NO;
		shaders_done = CACurrentMediaTime();
		/* the specializations the pipelines need and don't have, made in
		parallel and filed under their shaders here */
		for (index = 0; index < count; index++)
		{
			struct pipeline_key key;
			MetalShader *vertex;
			NSMutableData *name;

			pipeline_key_from_description(&list[index], &key);
			vertex = table_get(shaders, key.vertex_shader);
			if (!vertex || !vertex->library || specialize_off)
				continue;
			name = [NSMutableData dataWithBytes:&key.vertex_shader length:sizeof(key.vertex_shader)];
			[name appendBytes:key.attribute_kinds length:sizeof(key.attribute_kinds)];
			if (vertex->specialized[[NSData dataWithBytes:key.attribute_kinds length:sizeof(key.attribute_kinds)]] ||
				specialized[name])
				continue;
			specialized[name] = [NSNull null];
			[specialize addObject:name];
		}
		{
			NSMutableArray *functions = [NSMutableArray arrayWithCapacity:specialize.count];

			for (index = 0; index < specialize.count; index++)
				[functions addObject:[NSNull null]];
			dispatch_apply(specialize.count, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t item)
			{
				NSData *name = specialize[item];
				gpu_shader handle;
				MetalShader *vertex;
				MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
				NSUInteger attribute;
				id function;

				memcpy(&handle, name.bytes, sizeof(handle));
				vertex = table_get(shaders, handle);
				for (attribute = 0; attribute < GPU_ATTRIBUTE_COUNT; attribute++)
				{
					uint32_t kind = ((const uint8_t *)name.bytes)[sizeof(handle) + attribute];

					[values setConstantValue:&kind type:MTLDataTypeUInt atIndex:attribute];
				}
				function = [vertex->library newFunctionWithName:@"vertex_main" constantValues:values error:nil];
				if (function)
				{
					@synchronized (functions)
					{
						functions[item] = function;
					}
				}
			});
			for (index = 0; index < specialize.count; index++)
			{
				NSData *name = specialize[index];
				gpu_shader handle;

				MetalShader *vertex;

				memcpy(&handle, name.bytes, sizeof(handle));
				vertex = table_get(shaders, handle);
				if (!vertex->specialized)
					vertex->specialized = [NSMutableDictionary dictionary];
				/* (one that failed gets the unspecialized function, as
				vertex_function files it, rather than compiling again there) */
				if (functions[index] == [NSNull null])
				{
					platform_log("Metal: cannot specialize vertex shader %p for its attributes, which it then reads per draw",
						(__bridge void *)vertex);
					GPU_VALIDATION_REPORT(GPU_VALIDATION_SPECIALIZATION_FAILED, 1, "Metal: a listed vertex shader cannot be "
						"specialized for its attributes");
				}
				vertex->specialized[[name subdataWithRange:NSMakeRange(sizeof(handle), GPU_ATTRIBUTE_COUNT)]] =
					functions[index] != [NSNull null] ? functions[index] : vertex->function;
			}
		}
		/* the pixel shaders' exact-border variants the pipelines need
		(pixel_function), compiled in parallel too and filed here, so that
		pipeline_descriptor finds them made */
		{
			NSMutableArray *variants = [NSMutableArray array], *compiled = [NSMutableArray array];
			NSMutableSet *seen = [NSMutableSet set];

			for (index = 0; index < count; index++)
			{
				struct pipeline_key key;
				MetalShader *pixel;
				NSData *variant;

				pipeline_key_from_description(&list[index], &key);
				pixel = table_get(shaders, key.pixel_shader);
				if (!key.exact_borders || !pixel || !pixel->source || (pixel->exact_tried & (1u << key.exact_borders)))
					continue;
				variant = [NSData dataWithBytes:(uint32_t[2]){ key.pixel_shader, key.exact_borders } length:8];
				if ([seen containsObject:variant])
					continue;
				[seen addObject:variant];
				[variants addObject:variant];
				[compiled addObject:[NSNull null]];
			}
			dispatch_apply(variants.count, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t item)
			{
				@autoreleasepool
				{
					const uint32_t *variant = ((NSData *)variants[item]).bytes;
					MetalShader *pixel = table_get(shaders, variant[0]);
					NSString *text = [NSString stringWithFormat:@"#define EXACT_BORDERS %u\n%@", variant[1], pixel->source];
					id<MTLLibrary> library = [device newLibraryWithSource:text options:pixel_compile_options error:nil];
					id function = [library newFunctionWithName:@"fragment_main"];

					if (function)
					{
						@synchronized (compiled)
						{
							compiled[item] = function;
						}
					}
				}
			});
			for (index = 0; index < variants.count; index++)
			{
				const uint32_t *variant = ((NSData *)variants[index]).bytes;
				MetalShader *pixel = table_get(shaders, variant[0]);

				/* (one that failed is left untried, for pixel_function to compile and log) */
				if (compiled[index] == [NSNull null])
					continue;
				pixel->exact_tried |= 1u << variant[1];
				pixel->exact[variant[1]] = compiled[index];
			}
		}
		/* the descriptors, then the pipelines in parallel */
		for (index = 0; index < count; index++)
		{
			struct pipeline_key key;
			MetalShader *vertex, *pixel;
			NSData *name;

			pipeline_key_from_description(&list[index], &key);
			name = [NSData dataWithBytes:&key length:sizeof(key)];
			vertex = table_get(shaders, key.vertex_shader);
			pixel = table_get(shaders, key.pixel_shader);
			if (metal_compile_lookup(&pipeline_table, &key, sizeof(key)) || [named containsObject:name] || !vertex ||
				!pixel)
				continue;
			[names addObject:name];
			[named addObject:name];
			[descriptors addObject:pipeline_descriptor(&key, vertex, pixel, 0)];
		}
		made = [NSMutableArray arrayWithCapacity:names.count];
		for (index = 0; index < names.count; index++)
			[made addObject:[NSNull null]];
		dispatch_apply(names.count, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t item)
		{
			id pipeline = pipeline_make(descriptors[item]);

			@synchronized (made)
			{
				made[item] = pipeline;
			}
		});
		for (index = 0; index < names.count; index++)
			pipeline_publish(((NSData *)names[index]).bytes, made[index]);
		warm_list = nil;
		warm_group = nil;
		archive_save();
		platform_log("Metal: warmed %lu pipelines (%lu listed, %lu specializations) in %.0f ms, after waiting %.0f ms "
			"for the shaders",
			(unsigned long)names.count, count, (unsigned long)specialize.count,
			(CACurrentMediaTime() - shaders_done) * 1e3, (shaders_done - started) * 1e3);
	}
}

static id<MTLDepthStencilState> depth_state(const struct gpu_depth_stencil_state *state)
{
	struct gpu_depth_stencil_state key;
	NSData *name;
	id<MTLDepthStencilState> result;

	/* the reference is the encoder's; tests that are off ignore the rest,
	as GL does (gpu_gl.c sets them only while on) */
	memset(&key, 0, sizeof(key));
	if (state->depth_test)
	{
		key.depth_test = 1;
		key.depth_write = state->depth_write != 0;
		key.depth_function = state->depth_function;
	}
	if (state->stencil_test)
	{
		key.stencil_test = 1;
		key.stencil_function = state->stencil_function;
		key.stencil_fail = state->stencil_fail;
		key.stencil_depth_fail = state->stencil_depth_fail;
		key.stencil_pass = state->stencil_pass;
		key.stencil_read_mask = state->stencil_read_mask & 0xff;
		key.stencil_write_mask = state->stencil_write_mask & 0xff;
	}
	_Static_assert(sizeof(key) <= FRONT_CACHE_KEY, "the depth key fits the front cache");
	if ((result = front_cache_get(&depth_state_cache, &key, sizeof(key))))
		return result;
	name = [NSData dataWithBytes:&key length:sizeof(key)];
	result = depth_states[name];
	if (!result)
	{
		MTLDepthStencilDescriptor *descriptor = [MTLDepthStencilDescriptor new];

		descriptor.depthCompareFunction = key.depth_test ? depth_compare_function(key.depth_function) : MTLCompareFunctionAlways;
		descriptor.depthWriteEnabled = key.depth_write;
		if (key.stencil_test)
		{
			MTLStencilDescriptor *stencil = [MTLStencilDescriptor new];

			stencil.stencilCompareFunction = compare_function(key.stencil_function);
			stencil.stencilFailureOperation = stencil_operation(key.stencil_fail);
			stencil.depthFailureOperation = stencil_operation(key.stencil_depth_fail);
			stencil.depthStencilPassOperation = stencil_operation(key.stencil_pass);
			stencil.readMask = key.stencil_read_mask;
			stencil.writeMask = key.stencil_write_mask;
			descriptor.frontFaceStencil = stencil;
			descriptor.backFaceStencil = stencil;
		}
		result = [device newDepthStencilStateWithDescriptor:descriptor];
		depth_states[name] = result;
	}
	front_cache_put(&depth_state_cache, &key, sizeof(key), result);
	return result;
}

/* whether the GPU has border colors (Apple7 and later, and Macs): BORDER
addressing then samples the game's border color instead of clamping to the
edge, which smeared a shadow map's edge texels into long streaks across the
ground */
static BOOL border_colors;

/* Metal's border color for a D3DCOLOR (ARGB): its own when Metal has it
(transparent black, opaque black, opaque white). Any other color (the motion
sensor's sweep has 0x46000000, the loading bar 0x05050505) is rebuilt in the
pixel shader from this sampler's transparent black border and an opaque white
one (exact_borders). */
static MTLSamplerBorderColor border_color(uint32_t color)
{
	return color == 0xffffffffu ? MTLSamplerBorderColorOpaqueWhite :
		color == 0xff000000u ? MTLSamplerBorderColorOpaqueBlack : MTLSamplerBorderColorTransparentBlack;
}

/* exact border colors, after pfista/halo-og's native Metal renderer: the
stages of a draw whose BORDER addressing has a color Metal lacks, bit n for
stage n. Each samples its texture a second time, with an opaque white border
(sampler 4 + n), and the pixel shader compiled with EXACT_BORDERS
(nv2a_msl.c) rebuilds the game's color from the two, as xemu's custom border
colors sample it. A cube texture never samples its border in Metal. Other
draws are as before. */
static unsigned exact_borders(const struct gpu_draw *draw)
{
	unsigned mask = 0;
	int stage;

	if (!border_colors)
		return 0;
	for (stage = 0; stage < GPU_STAGE_COUNT; stage++)
	{
		const struct gpu_stage *packet = &draw->stages[stage];
		const struct gpu_sampler_state *state = &packet->sampler;
		uint32_t color = state->border_color;

		if (!packet->type || packet->type == GPU_TEXTURE_CUBE ||
			color == 0 || color == 0xff000000u || color == 0xffffffffu)
			continue;
		if (state->address_u == GPU_ADDRESS_BORDER || state->address_v == GPU_ADDRESS_BORDER ||
			(packet->type == GPU_TEXTURE_3D && state->address_w == GPU_ADDRESS_BORDER))
			mask |= 1u << stage;
	}
	return mask;
}

/* a stage's sampler, mapped as gpu_gl.c maps it on OpenGL ES: every filter
but POINT is linear, ANISOTROPIC turns on anisotropy, the first level sampled
is D3D's MAXMIPLEVEL, BORDER addressing clamps to the border (border_colors)
or the edge, and the LOD bias is the shader's. white: the same with an opaque
white border, an exact border stage's second sampler. */
static id<MTLSamplerState> sampler_state(const struct gpu_sampler_state *state, BOOL white)
{
	struct
	{
		struct gpu_sampler_state state;
		uint32_t white;
	} key;
	NSData *name;
	id<MTLSamplerState> result;

	memset(&key, 0, sizeof(key));
	key.state = *state;
	key.white = white;
	_Static_assert(sizeof(key) <= FRONT_CACHE_KEY, "the sampler key fits the front cache");
	if ((result = front_cache_get(&sampler_cache, &key, sizeof(key))))
		return result;
	name = [NSData dataWithBytes:&key length:sizeof(key)];
	result = samplers[name];
	if (!result)
	{
		MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
		const uint8_t addresses[3] = { state->address_u, state->address_v, state->address_w };
		MTLSamplerAddressMode modes[3];
		int axis;

		descriptor.minFilter = state->min_filter == GPU_FILTER_POINT ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
		descriptor.magFilter = state->mag_filter == GPU_FILTER_POINT ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
		descriptor.mipFilter = state->mip_filter == GPU_FILTER_NONE ? MTLSamplerMipFilterNotMipmapped :
			state->mip_filter == GPU_FILTER_POINT ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
		for (axis = 0; axis < 3; axis++)
			modes[axis] = addresses[axis] == GPU_ADDRESS_WRAP ? MTLSamplerAddressModeRepeat :
				addresses[axis] == GPU_ADDRESS_MIRROR ? MTLSamplerAddressModeMirrorRepeat :
				addresses[axis] == GPU_ADDRESS_BORDER && border_colors ? MTLSamplerAddressModeClampToBorderColor :
				MTLSamplerAddressModeClampToEdge;
		if (border_colors)
			descriptor.borderColor = white ? MTLSamplerBorderColorOpaqueWhite : border_color(state->border_color);
		descriptor.sAddressMode = modes[0];
		descriptor.tAddressMode = modes[1];
		descriptor.rAddressMode = modes[2];
		descriptor.lodMinClamp = (float)state->max_mip_level;
		descriptor.maxAnisotropy = state->min_filter == GPU_FILTER_ANISOTROPIC && state->max_anisotropy > 1 ?
			(state->max_anisotropy > 16 ? 16 : state->max_anisotropy) : 1;
		result = [device newSamplerStateWithDescriptor:descriptor];
		samplers[name] = result;
	}
	front_cache_put(&sampler_cache, &key, sizeof(key), result);
	return result;
}

/* the bytes of one vertex of a format, for a stride of 0, which GL reads as
tightly packed */
static uint32_t attribute_size(uint8_t format)
{
	if (format >= GPU_ATTRIBUTE_FLOAT1 && format <= GPU_ATTRIBUTE_FLOAT4)
		return 4u * (format - GPU_ATTRIBUTE_FLOAT1 + 1);
	if (format >= GPU_ATTRIBUTE_SHORT1 && format <= GPU_ATTRIBUTE_SHORT4)
		return 2u * (format - GPU_ATTRIBUTE_SHORT1 + 1);
	if (format >= GPU_ATTRIBUTE_NORMSHORT1 && format <= GPU_ATTRIBUTE_NORMSHORT4)
		return 2u * (format - GPU_ATTRIBUTE_NORMSHORT1 + 1);
	if (format >= GPU_ATTRIBUTE_UBYTE1 && format <= GPU_ATTRIBUTE_UBYTE4)
		return format - GPU_ATTRIBUTE_UBYTE1 + 1u;
	return 4;
}

/* c[192] and struct gpu_uniforms, as the shaders read them, shared by the
frame's draws until a serial moves: a Metal draw reads everything bound, so
each snapshot is whole */
static void bind_constants(const struct gpu_constant_store *constants, const struct gpu_uniforms *uniforms)
{
	__unsafe_unretained MetalBuffer *record;

	if (!snapshot.valid || snapshot.frame != frames || snapshot.constants_serial != constants->serial ||
		snapshot.uniforms_serial != uniforms->serial)
	{
		struct transient *memory = &snapshots[frame_slot];
		unsigned char *base;

		record = transient_room(memory, sizeof(constants->c) + sizeof(*uniforms), CONSTANT_ALIGNMENT);
		snapshot.offset = (uint32_t)memory->offset;
		snapshot.buffer = memory->chunks[memory->chunk];
		base = (unsigned char *)record->buffer.contents + snapshot.offset;
		memcpy(base, constants->c, sizeof(constants->c));
		memcpy(base + sizeof(constants->c), uniforms, sizeof(*uniforms));
		memory->offset += sizeof(constants->c) + sizeof(*uniforms);
		snapshot.frame = frames;
		snapshot.constants_serial = constants->serial;
		snapshot.uniforms_serial = uniforms->serial;
		snapshot.valid = YES;
	}
	record = buffer_record(snapshot.buffer);
	use_buffer(record);
	if (metal_state_object(&state_cache, METAL_STATE_VERTEX_BUFFER, (__bridge void *)record->buffer, snapshot.offset))
		[encoder setVertexBuffer:record->buffer offset:snapshot.offset atIndex:0];
	if (metal_state_object(&state_cache, METAL_STATE_VERTEX_BUFFER + 1, (__bridge void *)record->buffer,
		snapshot.offset + sizeof(constants->c)))
		[encoder setVertexBuffer:record->buffer offset:snapshot.offset + sizeof(constants->c) atIndex:1];
	if (metal_state_object(&state_cache, METAL_STATE_FRAGMENT_BUFFER, (__bridge void *)record->buffer,
		snapshot.offset + sizeof(constants->c)))
		[encoder setFragmentBuffer:record->buffer offset:snapshot.offset + sizeof(constants->c) atIndex:0];
}

static void bind_attributes(const struct gpu_draw *draw, const struct draw_attributes *attributes)
{
	struct metal_attribute_table table;
	int index;

	memset(&table, 0, sizeof(table));
	for (index = 0; index < GPU_ATTRIBUTE_COUNT; index++)
	{
		const struct gpu_vertex_attribute *attribute = &draw->attributes[index];
		struct metal_attribute *entry = &table.entries[index];
		__unsafe_unretained MetalBuffer *record = attributes->records[index];

		entry->format = attribute->format;
		if (!record)
		{
			/* a constant (draw_attributes) */
			entry->stream = GPU_STREAM_CONSTANT;
			memcpy(table.constants[index], draw->constant_values[index], sizeof(table.constants[index]));
			if (metal_state_object(&state_cache, METAL_STATE_ATTRIBUTE_BUFFER + index, (__bridge void *)empty_buffer, 0))
				[encoder setVertexBuffer:empty_buffer offset:0 atIndex:VERTEX_STREAM_BINDING + index];
			continue;
		}
		entry->stream = attribute->stream;
		entry->offset = draw->streams[attribute->stream].offset + attribute->offset;
		entry->stride = draw->streams[attribute->stream].stride ? draw->streams[attribute->stream].stride :
			attribute_size(attribute->format);
		/* use_buffer whether or not the binding is skipped: renaming reads it */
		use_buffer(record);
		if (metal_state_object(&state_cache, METAL_STATE_ATTRIBUTE_BUFFER + index, (__bridge void *)record->buffer, 0))
			[encoder setVertexBuffer:record->buffer offset:0 atIndex:VERTEX_STREAM_BINDING + index];
	}
	[encoder setVertexBytes:&table length:sizeof(table) atIndex:2];
	metal_state_always(&state_cache);
}

/* each stage's last sampler state and its sampler: consecutive draws mostly
sample the same way, and comparing 24 bytes costs less than sampler_state's
hash of its key (4 stages a draw). The samplers dictionary keeps every
sampler for good, so these needn't retain them. */
static struct
{
	struct gpu_sampler_state state;
	__unsafe_unretained id<MTLSamplerState> sampler;
} stage_samplers[GPU_STAGE_COUNT];

static void bind_stages(const struct gpu_draw *draw, unsigned exact)
{
	int stage;
	/* a foveated eye's target read through its map (nv2a_msl.c's foveated
	sampler): the map's parameter data, and per stage its screen and
	physical sizes, then its allocated size */
	float foveation[GPU_STAGE_COUNT][2][4];
	id<MTLBuffer> foveation_map_data = nil;

	memset(foveation, 0, sizeof(foveation));
	for (stage = 0; stage < GPU_STAGE_COUNT; stage++)
	{
		const struct gpu_stage *packet = &draw->stages[stage];
		__unsafe_unretained MetalTexture *record = packet->type ? texture_record(packet->texture) : nil;
		__unsafe_unretained id<MTLTexture> texture = record ? record->texture : nil;
		__unsafe_unretained id<MTLSamplerState> sampler;
		MTLSize screen, physical;

		if (texture && foveation_sizes(record, &screen, &physical))
		{
			foveation[stage][0][0] = (float)screen.width;
			foveation[stage][0][1] = (float)screen.height;
			foveation[stage][0][2] = (float)physical.width;
			foveation[stage][0][3] = (float)physical.height;
			foveation[stage][1][0] = (float)texture.width;
			foveation[stage][1][1] = (float)texture.height;
			foveation_map_data = foveation_parameters(record);
		}

		/* a stage with no texture, or one with no storage yet, samples black,
		as GL's texture 0 or an incomplete texture does */
		if (!texture)
			texture = empty_textures[packet->type <= GPU_TEXTURE_CUBE ? packet->type : 0];
		use_texture(record);
		if (metal_state_object(&state_cache, METAL_STATE_TEXTURE + stage, (__bridge void *)texture, 0))
			[encoder setFragmentTexture:texture atIndex:stage];
		if (!packet->type)
			sampler = empty_sampler;
		else if (stage_samplers[stage].sampler &&
			!memcmp(&stage_samplers[stage].state, &packet->sampler, sizeof(packet->sampler)))
			sampler = stage_samplers[stage].sampler;
		else
		{
			sampler = sampler_state(&packet->sampler, NO);
			stage_samplers[stage].state = packet->sampler;
			stage_samplers[stage].sampler = sampler;
		}
		if (metal_state_object(&state_cache, METAL_STATE_SAMPLER + stage, (__bridge void *)sampler, 0))
			[encoder setFragmentSamplerState:sampler atIndex:stage];
	}
	/* exact border colors (exact_borders): the white samplers and the colors,
	set on every such draw, which are few (the state cache doesn't track them) */
	if (exact)
	{
		float colors[GPU_STAGE_COUNT][4];

		memset(colors, 0, sizeof(colors));
		for (stage = 0; stage < GPU_STAGE_COUNT; stage++)
		{
			uint32_t color = draw->stages[stage].sampler.border_color;

			if (!(exact & (1u << stage)))
				continue;
			[encoder setFragmentSamplerState:sampler_state(&draw->stages[stage].sampler, YES) atIndex:GPU_STAGE_COUNT + stage];
			metal_state_always(&state_cache);
			colors[stage][0] = (float)((color >> 16) & 0xff) / 255.0f;
			colors[stage][1] = (float)((color >> 8) & 0xff) / 255.0f;
			colors[stage][2] = (float)(color & 0xff) / 255.0f;
			colors[stage][3] = (float)(color >> 24) / 255.0f;
		}
		[encoder setFragmentBytes:colors length:sizeof(colors) atIndex:3];
		metal_state_always(&state_cache);
	}
	if (foveation_map_data)
	{
		/* set on every foveated draw (the state cache doesn't track them) */
		[encoder setFragmentBuffer:foveation_map_data offset:0 atIndex:1];
		metal_state_always(&state_cache);
		[encoder setFragmentBytes:foveation length:sizeof(foveation) atIndex:2];
		metal_state_always(&state_cache);
	}
}

/* fans and loops, which Metal can't draw, as 32-bit indexed lists of the
original vertices, written straight into the frame's stream memory; returns
the vertex count, 0 if there's nothing to draw */
/* room for count 32-bit indices in the frame's stream memory: its chunk's
handle and offset, and where to write them */
static uint32_t *stream_indices(uint32_t count, gpu_buffer *buffer, uint32_t *offset)
{
	struct transient *memory = &streams[frame_slot];
	MetalBuffer *record = transient_room(memory, count * (uint32_t)sizeof(uint32_t), 4);
	uint32_t *indices = (uint32_t *)((unsigned char *)record->buffer.contents + memory->offset);

	*offset = (uint32_t)memory->offset;
	*buffer = memory->chunks[memory->chunk];
	memory->offset += count * sizeof(uint32_t);
	return indices;
}

static uint32_t convert_primitive(const struct gpu_draw *draw, MTLPrimitiveType *type, gpu_buffer *buffer, uint32_t *offset)
{
	const uint16_t *source = NULL;
	uint32_t count = draw->count, converted, index, *indices;
	__unsafe_unretained MetalBuffer *record;

	if (draw->index_buffer)
	{
		__unsafe_unretained MetalBuffer *index_record = buffer_record(draw->index_buffer);

		if (!index_record || draw->index_offset + (uint64_t)count * 2 > index_record->buffer.length)
			return 0;
		source = (const uint16_t *)((const unsigned char *)index_record->buffer.contents + draw->index_offset);
	}
#define VERTEX(i) (source ? (uint32_t)source[i] : (uint32_t)(i))
	if (draw->primitive == GPU_PRIMITIVE_TRIANGLE_FAN)
	{
		if (count < 3)
			return 0;
		converted = (count - 2) * 3;
		indices = stream_indices(converted, buffer, offset);
		for (index = 0; index + 2 < count; index++)
		{
			indices[index * 3] = VERTEX(0);
			indices[index * 3 + 1] = VERTEX(index + 1);
			indices[index * 3 + 2] = VERTEX(index + 2);
		}
		*type = MTLPrimitiveTypeTriangle;
	}
	else
	{
		if (count < 2)
			return 0;
		converted = count + 1;
		indices = stream_indices(converted, buffer, offset);
		for (index = 0; index < count; index++)
			indices[index] = VERTEX(index);
		indices[count] = VERTEX(0);
		*type = MTLPrimitiveTypeLineStrip;
	}
#undef VERTEX
	record = buffer_record(*buffer);
	use_buffer(record);
	return converted;
}

static uint32_t gpu_metal_draw(const struct gpu_draw *draw, const struct gpu_constant_store *constants,
	const struct gpu_uniforms *uniforms)
{
	@autoreleasepool
	{
		/* (unretained: the tables and caches keep these for longer than the
		draw, so retaining them would only cost a retain and a release each) */
		__unsafe_unretained MetalShader *vertex = table_get(shaders, draw->vertex_shader);
		__unsafe_unretained MetalShader *pixel = table_get(shaders, draw->pixel_shader);
		__unsafe_unretained MetalTexture *depth = texture_record(draw->depth_target);
		__unsafe_unretained id<MTLRenderPipelineState> pipeline;
		unsigned long width, height;
		struct gpu_rect area;
		MTLScissorRect scissor;
		MTLPrimitiveType type;
		unsigned exact = exact_borders(draw);
		struct draw_attributes attributes;
		BOOL pending;

		/* as GL's program_get: no program, no draw */
		if (!vertex || !pixel)
		{
			GPU_VALIDATION_REPORT(GPU_VALIDATION_SHADER_MISSING, 4, "Metal: shaders %u and %u: one is missing; the draw is "
				"skipped", draw->vertex_shader, draw->pixel_shader);
			return 0;
		}
#if HALO_RENDER_VALIDATION
		/* a GPU without border colors (below Apple7): BORDER addressing clamps to the edge */
		if (!border_colors)
		{
			int stage;

			for (stage = 0; stage < GPU_STAGE_COUNT; stage++)
				if (draw->stages[stage].type && (draw->stages[stage].sampler.address_u == GPU_ADDRESS_BORDER ||
					draw->stages[stage].sampler.address_v == GPU_ADDRESS_BORDER))
					GPU_VALIDATION_REPORT(GPU_VALIDATION_BORDER_UNSUPPORTED, 1, "Metal: stage %d: this GPU has no border colors; "
						"BORDER addressing clamps to the edge", stage);
		}
#endif
		draw_attributes_decide(draw, &attributes);
		pipeline = draw_pipeline(draw, &attributes, vertex, pixel, exact, MTLPixelFormatBGRA8Unorm,
			depth && depth->texture ? depth->texture.pixelFormat : MTLPixelFormatInvalid, &pending);
		/* display.async_shaders: a draw whose pipeline is still building is
		skipped this frame, as one that can't be made is; the runs of draws
		to each pair of targets tell whether a whole pass drew nothing */
		if (async_compiles && (pipeline || pending))
		{
			static int logged;

			metal_skip_draw(&skip_frame, (uint64_t)draw->depth_target << 32 | draw->color_target, pending);
			if (pending && !logged++)
				platform_log("Metal: frame %lu: a draw's pipeline is still compiling; such draws are skipped until "
					"theirs are ready", frames);
		}
		if (!pipeline)
		{
			/* a pipeline still compiling is the setting's doing; one that
			can't be made skips its draws for good */
			if (pending)
				GPU_VALIDATION_REPORT(GPU_VALIDATION_PIPELINE_COMPILING, 0, "Metal: the draw's pipeline is still compiling on a "
					"worker (display.async_shaders); it is skipped until then");
			else
				GPU_VALIDATION_REPORT(GPU_VALIDATION_PIPELINE_FAILED, (uint64_t)draw->vertex_shader << 32 | draw->pixel_shader,
					"Metal: the pipeline for shaders %u and %u could not be built; every draw with it is skipped",
					draw->vertex_shader, draw->pixel_shader);
			return 0;
		}
		/* display.native_hud (native_pass): a draw outside the display-sized
		pass that samples the back buffer must follow that pass's draws so
		far, so the pass ends here and the frame goes on in order */
		if (native_pass.color && draw->color_target != native_pass.color)
		{
			int stage;

			for (stage = 0; stage < GPU_STAGE_COUNT; stage++)
				if (draw->stages[stage].texture == native_pass.color)
				{
					native_pass.in_order++;
					pass_end();
					break;
				}
		}
		if (!pass_begin(draw->color_target, draw->depth_target, NULL))
		{
			GPU_VALIDATION_REPORT(GPU_VALIDATION_PASS_NOT_BEGUN, 0, "Metal: no pass for targets %u and %u (one has no "
				"storage); the draw is dropped", draw->color_target, draw->depth_target);
			return 1;
		}
		target_size(draw->color_target, draw->depth_target, &width, &height);
		/* GL takes a scissor outside the target, Metal doesn't; an empty one
		draws nothing, but the draw happened */
		area = draw->scissor.width > 0 && draw->scissor.height > 0 ? draw->scissor :
			(struct gpu_rect){ 0, 0, (int32_t)width, (int32_t)height };
		if (!clamp_rect(&area, width, height, &scissor))
		{
			GPU_VALIDATION_REPORT(GPU_VALIDATION_DRAW_DROPPED, 1, "Metal: the scissor %d,%d %dx%d leaves nothing of a %lux%lu "
				"target; the draw is dropped", area.x, area.y, area.width, area.height, width, height);
			return 1;
		}
		if (visibility.active && !visibility.entry_open)
		{
			unsigned long entry = visibility.next;
			static int lapped;

			/* 8,191 entries in flight at once: three frames of lens flares
			are far fewer */
			if (busy(visibility_entry_serials[entry]) && !lapped)
			{
				platform_log("Metal: the visibility ring lapped a test still on the GPU; answers may be wrong");
				GPU_VALIDATION_REPORT(GPU_VALIDATION_VISIBILITY_LAPPED, 0, "Metal: the visibility ring lapped a test still on "
					"the GPU");
				lapped = 1;
			}
			visibility_entry_serials[entry] = current_serial;
			if (!visibility.count)
				visibility.first = entry;
			visibility.next = (entry + 1) % VISIBILITY_ENTRIES;
			visibility.count++;
			visibility.entry_open = 1;
			((uint64_t *)visibility_buffer.contents)[entry] = 0;
			[encoder setVisibilityResultMode:visibility_counts ? MTLVisibilityResultModeCounting : MTLVisibilityResultModeBoolean
				offset:entry * sizeof(uint64_t)];
			metal_state_always(&state_cache);
		}
		if (metal_state_object(&state_cache, METAL_STATE_PIPELINE, (__bridge void *)pipeline, 0))
		{
			[encoder setRenderPipelineState:pipeline];
			/* debug.gl_debug: how often the pipeline changes between draws of
			one pass, and how often the shaders stay the same (only blending,
			the write mask or the vertex specialization changed) */
			if (metal_debug)
			{
				pipeline_changes.changes++;
				if (pipeline_changes.vertex_shader == draw->vertex_shader &&
					pipeline_changes.pixel_shader == draw->pixel_shader)
					pipeline_changes.same_shaders++;
			}
		}
		if (metal_debug)
		{
			pipeline_changes.vertex_shader = draw->vertex_shader;
			pipeline_changes.pixel_shader = draw->pixel_shader;
		}
		{
			__unsafe_unretained id<MTLDepthStencilState> depth_stencil = depth_state(&draw->depth_stencil);
			uint32_t reference = draw->depth_stencil.stencil_reference & 0xff;
			MTLViewport viewport = { (double)draw->viewport.x, (double)draw->viewport.y,
				(double)draw->viewport.width, (double)draw->viewport.height, draw->viewport.min_z, draw->viewport.max_z };
			MTLCullMode cull = draw->raster.cull_mode == GPU_CULL_FRONT ? MTLCullModeFront :
				draw->raster.cull_mode == GPU_CULL_BACK ? MTLCullModeBack : MTLCullModeNone;
			MTLWinding winding = draw->raster.front_face == GPU_FRONT_COUNTER_CLOCKWISE ?
				MTLWindingCounterClockwise : MTLWindingClockwise;
			/* glPolygonOffset(slope, constant); filled polygons only, as on ES */
			float bias[3] = { 0.0f, 0.0f, 0.0f };
			float blend[4] = { (float)((draw->blend.color >> 16) & 0xff) / 255.0f,
				(float)((draw->blend.color >> 8) & 0xff) / 255.0f, (float)(draw->blend.color & 0xff) / 255.0f,
				(float)(draw->blend.color >> 24) / 255.0f };

			if (draw->raster.depth_bias_enable)
			{
				bias[0] = -draw->raster.depth_bias_constant;
				bias[1] = -draw->raster.depth_bias_slope;
			}
			if (metal_state_object(&state_cache, METAL_STATE_DEPTH_STENCIL, (__bridge void *)depth_stencil, 0))
				[encoder setDepthStencilState:depth_stencil];
			if (metal_state_value(&state_cache, METAL_STATE_STENCIL_REFERENCE, &reference, sizeof(reference)))
				[encoder setStencilReferenceValue:reference];
			if (metal_state_value(&state_cache, METAL_STATE_VIEWPORT, &viewport, sizeof(viewport)))
				[encoder setViewport:viewport];
			if (metal_state_value(&state_cache, METAL_STATE_SCISSOR, &scissor, sizeof(scissor)))
				[encoder setScissorRect:scissor];
			if (metal_state_value(&state_cache, METAL_STATE_CULL_MODE, &cull, sizeof(cull)))
				[encoder setCullMode:cull];
			if (metal_state_value(&state_cache, METAL_STATE_WINDING, &winding, sizeof(winding)))
				[encoder setFrontFacingWinding:winding];
			if (metal_state_value(&state_cache, METAL_STATE_DEPTH_BIAS, bias, sizeof(bias)))
				[encoder setDepthBias:bias[0] slopeScale:bias[1] clamp:bias[2]];
			if (metal_state_value(&state_cache, METAL_STATE_BLEND_COLOR, blend, sizeof(blend)))
				[encoder setBlendColorRed:blend[0] green:blend[1] blue:blend[2] alpha:blend[3]];
			if ((draw->raster.depth_clamp != 0) != encoder_depth_clamp)
			{
				encoder_depth_clamp = draw->raster.depth_clamp != 0;
				[encoder setDepthClipMode:encoder_depth_clamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
			}
		}
		bind_stages(draw, exact);
		bind_constants(constants, uniforms);
		bind_attributes(draw, &attributes);
		pass_commands++;
		/* the draw call itself, made below unless a conversion leaves nothing */
		metal_state_always(&state_cache);
		metal_state_draw(&state_cache);
		switch (draw->primitive)
		{
		case GPU_PRIMITIVE_TRIANGLE_FAN:
		case GPU_PRIMITIVE_LINE_LOOP:
		{
			gpu_buffer indices;
			uint32_t offset, count = convert_primitive(draw, &type, &indices, &offset);

			/* (the converted indices are the draw's own, so its base vertex
			still applies; 0 for a draw without indices) */
			if (!count)
				GPU_VALIDATION_REPORT(GPU_VALIDATION_DRAW_DROPPED, 2, "Metal: a fan or loop of %u vertices has nothing to draw, "
					"or its indices lie outside their buffer", draw->count);
			if (count && base_vertex_draws)
				[encoder drawIndexedPrimitives:type indexCount:count indexType:MTLIndexTypeUInt32
					indexBuffer:buffer_record(indices)->buffer indexBufferOffset:offset instanceCount:1
					baseVertex:draw->base_vertex baseInstance:0];
			else if (count)
				[encoder drawIndexedPrimitives:type indexCount:count indexType:MTLIndexTypeUInt32
					indexBuffer:buffer_record(indices)->buffer indexBufferOffset:offset];
			return 1;
		}
		case GPU_PRIMITIVE_POINTS: type = MTLPrimitiveTypePoint; break;
		case GPU_PRIMITIVE_LINES: type = MTLPrimitiveTypeLine; break;
		case GPU_PRIMITIVE_LINE_STRIP: type = MTLPrimitiveTypeLineStrip; break;
		case GPU_PRIMITIVE_TRIANGLE_STRIP: type = MTLPrimitiveTypeTriangleStrip; break;
		default: type = MTLPrimitiveTypeTriangle; break;
		}
		if (draw->index_buffer)
		{
			__unsafe_unretained MetalBuffer *record = buffer_record(draw->index_buffer);

			if (!record)
			{
				GPU_VALIDATION_REPORT(GPU_VALIDATION_DRAW_DROPPED, 3, "Metal: index buffer %u does not exist; the draw is "
					"dropped", draw->index_buffer);
				return 1;
			}
			use_buffer(record);
			/* the vertex shaders' vertex_id, which their fetches index the
			streams with (nv2a_msl.c), is the index plus the base vertex.
			Without gpu_capabilities.base_vertex the front end leaves it 0,
			and the call that takes one isn't made: a GPU below family Apple3
			doesn't have it */
			if (base_vertex_draws)
				[encoder drawIndexedPrimitives:type indexCount:draw->count indexType:MTLIndexTypeUInt16
					indexBuffer:record->buffer indexBufferOffset:draw->index_offset instanceCount:1
					baseVertex:draw->base_vertex baseInstance:0];
			else
				[encoder drawIndexedPrimitives:type indexCount:draw->count indexType:MTLIndexTypeUInt16
					indexBuffer:record->buffer indexBufferOffset:draw->index_offset];
		}
		else
		{
			[encoder drawPrimitives:type vertexStart:0 vertexCount:draw->count];
		}
		return 1;
	}
}

/* ---------- visibility tests (the state is declared before the transient memory) */

static void gpu_metal_visibility_begin(void)
{
	visibility.active = 1;
	visibility.count = 0;
	visibility.entry_open = 0;
}

static void gpu_metal_visibility_end(uint32_t slot)
{
	if (visibility.entry_open)
		[encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
	visibility.active = 0;
	visibility.entry_open = 0;
	visibility.slots[slot].first = visibility.first;
	visibility.slots[slot].count = visibility.count;
	visibility.slots[slot].serial = current_serial;
	if (visibility.count)
	{
		struct visibility_pending test = { slot, (uint32_t)visibility.first, (uint32_t)visibility.count };

		[visibility.pending appendBytes:&test length:sizeof(test)];
	}
}

/* how many samples passed (GPU_OCCLUSION_EXACT), or on a GPU that can't
count, whether any did (GPU_OCCLUSION_ANY_SAMPLE). With the fixed
timestep, a result the GPU hasn't written yet is waited for, committing the
open command buffer if it writes it: the game asks again until it has an
answer (lens flares), so an answer now keeps runs repeatable, as a wait under
GL does. In real time that wait makes the CPU and the GPU take turns every
frame, so the answer is the latest the GPU has written for the slot: this
test's, or while the GPU is behind, an earlier one's (a flare fades a frame
late), as desktop GL's persistent results do (gpu_gl.c) */
static uint32_t gpu_metal_visibility_result(uint32_t slot, uint32_t *samples)
{
	@autoreleasepool
	{
		*samples = 0;
		if (!visibility.slots[slot].count)
			return 1;
		if (!visibility_wait)
		{
			*samples = (uint32_t)(atomic_load(&visibility_answers[slot]) & VISIBILITY_COUNT_MAXIMUM);
			return 1;
		}
		if (visibility.slots[slot].serial == current_serial)
			commit(NO);
		/* wait for the answer itself: waitUntilCompleted can return before
		the completion handler has run, and a later command buffer's handler
		can raise completed_serial before this one's has written the answer */
		while ((atomic_load(&visibility_answers[slot]) >> VISIBILITY_COUNT_BITS) < visibility.slots[slot].serial)
			[last_committed waitUntilCompleted];
		*samples = (uint32_t)(atomic_load(&visibility_answers[slot]) & VISIBILITY_COUNT_MAXIMUM);
		return 1;
	}
}

/* ---------- frame pacing

The game draws a frame whenever it can, and the interpolation blends the
world for the moment each frame is shown (render_interpolation.c). For the
motion to look even, every frame must also stay on screen equally long: a
frame shown for one refresh followed by one shown for two judders, though
both are drawn right. So each frame is presented to stay up at least a whole
number of refreshes after the one before (presentDrawable:afterMinimumDuration:;
a schedule of absolute times would have to know the compositor's latency), and
Present returns when the next frame should show, for the blend. The number of
refreshes rises at once when frames stay up longer than asked, and falls after
PACING_SETTLE frames that would fit in fewer with room to spare. With
display.frame_pacing = "tick" only numbers that give a multiple of 30 frames a
second are used (1 or 3 at 90 Hz, 1 or 2 at 60 Hz), so every game tick spans
the same number of frames; "refresh" allows any (2 at 90 Hz: 45 a second).
Pacing is off by default (display.frame_pacing = "off") until it has been
tried on a Vision Pro; frames are measured by the time between Presents
(pacing_schedule). The simulators can't hold a frame, so there it only
measures.

The refresh period comes from a display link on a thread of its own, since
the game's thread doesn't return to its run loop. Every 600 frames the log
says how many refreshes frames stayed on screen, in every mode. */

/* frames that must fit in fewer refreshes (all but PACING_OVER) before the
rate rises, and the misses (frames up longer than asked) among the last
PACING_WINDOW that lower it */
#define PACING_SETTLE 180
#define PACING_OVER 2
#define PACING_WINDOW 30
#define PACING_MISSES 3
/* a frame "fits" in n refreshes when its CPU and GPU time each take at most
this much of them */
#define PACING_HEADROOM 0.8

@interface PacingClock : NSObject
- (void)refresh:(CADisplayLink *)link;
@end

static os_unfair_lock pacing_lock = OS_UNFAIR_LOCK_INIT;
/* the frames a report's percentiles can cover (reports come every 600) */
#define PACING_TIMED_FRAMES 1024
static CFTimeInterval refresh_period; /* CACurrentMediaTime's clock */
/* how many refreshes the display link has called for (frame_context_wait counts them) */
static _Atomic unsigned long refresh_count;
@implementation PacingClock
- (void)refresh:(CADisplayLink *)link
{
	atomic_fetch_add(&refresh_count, 1);
	os_unfair_lock_lock(&pacing_lock);
	refresh_period = link.targetTimestamp - link.timestamp;
	os_unfair_lock_unlock(&pacing_lock);
	/* (the pacer's clock: host_frame_pacer.h) */
	frame_pacer_refresh(link.timestamp, link.targetTimestamp - link.timestamp);
}
@end

static struct
{
	BOOL off, any_rate;
	long refreshes;                  /* a frame's refreshes */
	CFTimeInterval work_started;     /* when the frame being drawn began (the last Present returned) */
	uint64_t gpu_counted;            /* pacing_gpu_nanoseconds at the last Present */
	double cost;                     /* the slowest frame (CPU or GPU) since the rate last changed, in seconds */
	unsigned long over;              /* frames since then too slow for one refresh fewer */
	unsigned long since_change;
	unsigned long misses;            /* frames up longer than asked, since the window began */
	unsigned long shown[6];          /* frames that stayed up 1-4 and 5 or more refreshes, since the last report */
	CFTimeInterval last_present;     /* when Present last asked for a schedule */
	CFTimeInterval due;              /* when the next frame shows, as the schedule reckons it */
	unsigned long window;
	unsigned long reported;
	/* each frame's CPU and GPU time since the last report, for its medians
	and 95th percentiles (a loading stall would swamp an average) */
	double cpu_times[PACING_TIMED_FRAMES], gpu_times[PACING_TIMED_FRAMES];
	unsigned long timed;
} pacing;

static int compare_times(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

/* the median and 95th percentile of count times, in milliseconds (sorts them) */
static void pacing_percentiles(double *times, unsigned long count, double *median, double *high)
{
	*median = *high = 0.0;
	if (!count)
		return;
	qsort(times, count, sizeof(*times), compare_times);
	*median = times[count / 2] * 1000.0;
	*high = times[count * 95 / 100] * 1000.0;
}

static void frame_log_start(void);

static void pacing_start(uint32_t flags)
{
	pacing.off = (flags & (GPU_INITIALIZE_PACING_OFF | GPU_INITIALIZE_FIXED_TIMESTEP)) != 0;
	pacing.any_rate = (flags & GPU_INITIALIZE_PACING_ANY_RATE) != 0;
	pacing.refreshes = 1;
	frame_log_start();
	if (flags & GPU_INITIALIZE_FIXED_TIMESTEP)
		return;
	NSThread *thread = [[NSThread alloc] initWithBlock:^{
		CADisplayLink *link = [CADisplayLink displayLinkWithTarget:[PacingClock new] selector:@selector(refresh:)];

#if !TARGET_OS_VISION
		/* ask for 120 Hz, which the system clamps to what the screen can do: a ProMotion
		iPhone otherwise stays at 60 Hz, even with CADisableMinimumFrameDurationOnPhone
		(Info.plist) */
		link.preferredFrameRateRange = CAFrameRateRangeMake(80, 120, 120);
#endif
		[link addToRunLoop:NSRunLoop.currentRunLoop forMode:NSDefaultRunLoopMode];
		for (;;)
			[NSRunLoop.currentRunLoop run];
	}];
	thread.name = @"Metal frame pacing";
	thread.qualityOfService = NSQualityOfServiceUserInteractive;
	[thread start];
}

/* whether n refreshes a frame is allowed at this refresh rate: with "tick",
a multiple of 30 frames a second when the display runs at one */
static BOOL pacing_allowed(long n, CFTimeInterval period)
{
	double rate = 1.0 / period;
	long ticks = lround(rate / 30.0);

	if (pacing.any_rate || ticks < 1 || fabs(rate - 30.0 * (double)ticks) > 2.0)
		return n <= 4;
	return ticks % n == 0;
}

static long pacing_step(long n, int direction, CFTimeInterval period)
{
	long step;

	for (step = n + direction; step >= 1 && step <= 4; step += direction)
		if (pacing_allowed(step, period))
			return step;
	return n;
}

static void pacing_set(long refreshes, CFTimeInterval period, const char *why)
{
	platform_log("Metal: pacing %ld refresh%s a frame (%.0f frames a second): %s, slowest frame %.1f ms",
		refreshes, refreshes == 1 ? "" : "es", 1.0 / (period * (double)refreshes), why, pacing.cost * 1000.0);
	pacing.refreshes = refreshes;
	pacing.cost = 0.0;
	pacing.over = 0;
	pacing.since_change = 0;
	pacing.window = 0;
	pacing.misses = 0;
}

/* how long drawable must stay up after the frame before it (0: no minimum),
and how long from now until the next frame should show, in microseconds
(0: unknown); picks the rate for the frames after this one.

Frames are measured by the time between Presents. Present waits in
nextDrawable for a drawable the display has let go of, so once frames are
held for whole refreshes, Presents come that many refreshes apart; one that
comes later stayed up longer than asked (a miss). That works where a
drawable's presented handler never reports a time (a visionOS window, the
simulators). The next frame is due a frame's refreshes after this one, or a
frame's refreshes from now when the game ran late; only how much that lead
varies from frame to frame matters to the blend (render_interpolation.c). */
/* ---------- the frames' context, and the frame log

gpu_frame_context (port/linux/game/scene_class.c): the scene class of the
frames from now on, the most frames a second the device's profile wants for
it (0: every refresh) and the map. The rate is kept by holding each frame up
for the whole number of refreshes that gives it (30 at 60 Hz: two), with
the pacing's own number when that is more.

debug.frame_log ("auto": on an Apple TV, where nothing else can show how a
session went; "on", "off") appends a line to frame-log.txt in the data
folder every FRAME_LOG_SECONDS and whenever the context changes, so a
hand-played session can be read afterwards (tools/frame_log.py):

	frames time=125.0 scene=cutscene level=a10 frames=150 fps=30.0 mean_ms=33.4 p95_ms=33.5 p99_ms=50.1
		late=1 gaps=0 size=1280x720 cap=30 hz=59.9 footprint_mb=512 peak_mb=660 thermal=nominal

time is seconds since the game started drawing; a frame's time is from its
Present to the next; late counts frames that stayed up more refreshes than
asked (one, or the cap's); gaps are waits of a quarter second or more (a
load, the background), which are no frame and are left out of the rest. A
"session" line opens each launch. The cost is one comparison and one stored
number a frame. */

#define FRAME_LOG_SECONDS 5.0
#define FRAME_LOG_TIMES 2048

static struct
{
	uint32_t scene_class, rate;
	char level[32];
} frame_context;

static struct
{
	int on;
	FILE *file;
	CFTimeInterval started, window_started;
	unsigned long frames, late, gaps;
	double seconds;
	float times[FRAME_LOG_TIMES];
	unsigned width, height;
} frame_log;

static int frame_time_compare(const void *left, const void *right)
{
	float a = *(const float *)left, b = *(const float *)right;

	return a < b ? -1 : a > b;
}

/* the window so far as a line, and a new one begun */
static void frame_log_flush(CFTimeInterval now, CFTimeInterval period)
{
	static const char *const scenes[] = { "gameplay", "menu", "cutscene" };

	if (frame_log.file && frame_log.frames)
	{
		unsigned long kept = frame_log.frames < FRAME_LOG_TIMES ? frame_log.frames : FRAME_LOG_TIMES;
		task_vm_info_data_t info;
		mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
		unsigned long long footprint = 0, peak = 0;
		NSProcessInfoThermalState thermal = NSProcessInfo.processInfo.thermalState;

		if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
		{
			footprint = (unsigned long long)info.phys_footprint >> 20;
			peak = (unsigned long long)info.ledger_phys_footprint_peak >> 20;
		}
		qsort(frame_log.times, kept, sizeof(frame_log.times[0]), frame_time_compare);
		fprintf(frame_log.file, "frames time=%.1f scene=%s level=%s frames=%lu fps=%.1f mean_ms=%.1f p95_ms=%.1f p99_ms=%.1f "
			"late=%lu gaps=%lu size=%ux%u cap=%u hz=%.1f footprint_mb=%llu peak_mb=%llu thermal=%s\n",
			now - frame_log.started, frame_context.scene_class < 3 ? scenes[frame_context.scene_class] : "other",
			frame_context.level[0] ? frame_context.level : "none", frame_log.frames,
			frame_log.seconds > 0.0 ? (double)frame_log.frames / frame_log.seconds : 0.0,
			frame_log.seconds * 1000.0 / (double)frame_log.frames, (double)frame_log.times[kept * 95 / 100] * 1000.0,
			(double)frame_log.times[kept * 99 / 100] * 1000.0, frame_log.late, frame_log.gaps, frame_log.width,
			frame_log.height, (unsigned)frame_context.rate, period > 0.0 ? 1.0 / period : 0.0, footprint, peak,
			thermal == NSProcessInfoThermalStateNominal ? "nominal" : thermal == NSProcessInfoThermalStateFair ? "fair" :
			thermal == NSProcessInfoThermalStateSerious ? "serious" : "critical");
		fflush(frame_log.file);
	}
	frame_log.frames = frame_log.late = frame_log.gaps = 0;
	frame_log.seconds = 0.0;
	frame_log.window_started = now;
}

static void frame_log_start(void)
{
	char setting[16];

	host_config_string("debug.frame_log", "auto", setting, sizeof(setting));
#if TARGET_OS_TV
	frame_log.on = strcmp(setting, "off") != 0;
#else
	frame_log.on = !strcmp(setting, "on");
#endif
	if (!frame_log.on)
		return;
	/* (the working directory is the data folder) */
	frame_log.file = fopen("frame-log.txt", "a");
	if (!frame_log.file)
	{
		platform_log("debug.frame_log: cannot open frame-log.txt; no frame log");
		frame_log.on = 0;
		return;
	}
	@autoreleasepool
	{
		NSDateFormatter *format = [[NSDateFormatter alloc] init];

		format.dateFormat = @"yyyy-MM-dd'T'HH:mm:ssZ";
		fprintf(frame_log.file, "session start=%s build=%s version=%s\n",
			[format stringFromDate:NSDate.date].UTF8String,
			[[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] description].UTF8String ?: "?",
			[[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] description].UTF8String ?: "?");
		fflush(frame_log.file);
	}
	frame_log.started = frame_log.window_started = CACurrentMediaTime();
	platform_log("debug.frame_log: frame-log.txt in the data folder, a line every %.0f s and at each change of scene class",
		FRAME_LOG_SECONDS);
}

/* one frame: how long the one before it stayed up, in seconds and refreshes
(0: no frame, a wait), against the refreshes asked */
static void frame_log_frame(CFTimeInterval now, CFTimeInterval interval, long up, long asked, CFTimeInterval period)
{
	if (!frame_log.on)
		return;
	if (up <= 0)
	{
		if (interval >= 0.25)
			frame_log.gaps++;
	}
	else
	{
		if (frame_log.frames < FRAME_LOG_TIMES)
			frame_log.times[frame_log.frames] = (float)interval;
		frame_log.frames++;
		frame_log.seconds += interval;
		if (up > asked)
			frame_log.late++;
	}
	if (now - frame_log.window_started >= FRAME_LOG_SECONDS)
		frame_log_flush(now, period);
}

static void gpu_metal_frame_context(uint32_t scene_class, uint32_t frame_rate, const char *level)
{
	CFTimeInterval period;

	if (scene_class == frame_context.scene_class && frame_rate == frame_context.rate &&
		!strncmp(frame_context.level, level ? level : "", sizeof(frame_context.level) - 1))
		return;
	os_unfair_lock_lock(&pacing_lock);
	period = refresh_period;
	os_unfair_lock_unlock(&pacing_lock);
	/* (the frames so far were the old context's) */
	if (frame_log.on)
		frame_log_flush(CACurrentMediaTime(), period);
	if (frame_rate != frame_context.rate)
	{
		if (frame_rate)
			platform_log("Metal: at most %u frames a second from here (the device profile's rate for this scene class)",
				(unsigned)frame_rate);
		else
			platform_log("Metal: a frame every refresh again");
	}
	frame_context.scene_class = scene_class;
	frame_context.rate = frame_rate;
	host_frame_rate_scene((double)frame_rate);
	snprintf(frame_context.level, sizeof(frame_context.level), "%s", level ? level : "");
}

/* the refreshes a frame stays up to keep the context's frame rate: 2 for 30
at 60 Hz, 3 at 90; 1 without a rate or when the display is no faster */
static long frame_context_refreshes(CFTimeInterval period)
{
	double refreshes;

	if (!frame_context.rate || period <= 0.0)
		return 1;
	refreshes = ceil(1.0 / (period * (double)frame_context.rate) - 0.05);
	return refreshes < 1.0 ? 1 : refreshes > 8.0 ? 8 : (long)refreshes;
}

/* Holding a frame up for the scene class's refreshes (presentDrawable:afterMinimumDuration:) does
not by itself slow the game: on an Apple TV HD the layer took a frame every refresh for a whole
cutscene while each was asked to stay up two, and a wait measured from the last Present landed
some frames one refresh after the last and some two (35 frames a second for 30). So a Present under
a frame rate waits for the display itself: until the display link has called for as many refreshes
since the last Present as a frame stays up. Presented right after that call, the frame shows at the
next refresh, that many after the one before. A frame that is already late goes at once. */
static void frame_context_wait(void)
{
	static unsigned long presented_at;
	CFTimeInterval period, began = CACurrentMediaTime();
	long refreshes;

	os_unfair_lock_lock(&pacing_lock);
	period = refresh_period;
	os_unfair_lock_unlock(&pacing_lock);
	refreshes = frame_context_refreshes(period);
	if (refreshes > 1)
	{
		/* (a quarter second at most: a display link that stopped must not stop the game) */
		while (atomic_load(&refresh_count) < presented_at + (unsigned long)refreshes && CACurrentMediaTime() - began < 0.25)
			usleep(500);
		/* (not the frame's work: the CPU times in the log leave it out) */
		pacing_waited += CACurrentMediaTime() - began;
	}
	presented_at = atomic_load(&refresh_count);
}

static CFTimeInterval pacing_schedule(id<CAMetalDrawable> drawable, uint32_t *next_frame_due)
{
	CFTimeInterval now = CACurrentMediaTime(), period, hold, interval;
	long context_refreshes;
	uint64_t gpu_total = atomic_load(&pacing_gpu_nanoseconds);
	double cpu, gpu;
	long lower, up;

	*next_frame_due = 0;
	cpu = pacing.work_started > 0.0 ? now - pacing.work_started - pacing_waited : 0.0;
	gpu = (double)(gpu_total - pacing.gpu_counted) / 1e9;
	pacing.gpu_counted = gpu_total;
	pacing_waited = 0.0;
	interval = pacing.last_present > 0.0 ? now - pacing.last_present : 0.0;
	pacing.last_present = now;
	os_unfair_lock_lock(&pacing_lock);
	period = refresh_period;
	os_unfair_lock_unlock(&pacing_lock);
	/* the device benchmark (host_benchmark.m): every presented frame while a run's window is open */
	host_benchmark_frame(interval, cpu > 0.0 ? cpu : -1.0, gpu, period >= 1.0 / 240.0 && period <= 1.0 / 20.0 ? period : 0.0);
	if (!drawable || period < 1.0 / 240.0 || period > 1.0 / 20.0)
		return 0.0;
	if (!pacing_allowed(pacing.refreshes, period))
		pacing.refreshes = 1;
	hold = pacing.off ? 0.0 : period * (double)pacing.refreshes;
	/* the scene class's frame rate, when it asks for more refreshes a frame than the pacing does */
	context_refreshes = frame_context_refreshes(period);
	if (context_refreshes > 1 && hold < period * (double)context_refreshes)
		hold = period * (double)context_refreshes;
	/* how many refreshes the frame before this one stayed up; a quarter
	second or more is a load or the background, not a frame */
	up = interval > 0.0 && interval < 0.25 ? lround(interval / period) : 0;
	frame_log_frame(now, interval, up, context_refreshes > (pacing.off ? 1 : pacing.refreshes) ? context_refreshes :
		pacing.off ? 1 : pacing.refreshes, period);
	if (up > 0)
	{
		pacing.shown[up > 5 ? 5 : up]++;
		if (!pacing.off && up > pacing.refreshes)
			pacing.misses++;
	}
	if (cpu > 0.0 && pacing.timed < PACING_TIMED_FRAMES)
	{
		pacing.cpu_times[pacing.timed] = cpu;
		pacing.gpu_times[pacing.timed] = gpu;
		pacing.timed++;
	}
	if (frames >= pacing.reported + 600)
	{
		double cpu_median, cpu_high, gpu_median, gpu_high;

		pacing_percentiles(pacing.cpu_times, pacing.timed, &cpu_median, &cpu_high);
		pacing_percentiles(pacing.gpu_times, pacing.timed, &gpu_median, &gpu_high);
		pacing.reported = frames;
		platform_log("Metal: frames shown for 1/2/3/4/5+ refreshes at %.1f Hz: %lu/%lu/%lu/%lu/%lu (pacing %s); "
			"frame CPU %.1f ms, GPU %.1f ms (medians; 95th %.1f, %.1f)", 1.0 / period, pacing.shown[1], pacing.shown[2], pacing.shown[3],
			pacing.shown[4], pacing.shown[5],
			pacing.off ? "off" : pacing.refreshes == 1 ? "1 refresh" : pacing.refreshes == 2 ? "2 refreshes" :
			pacing.refreshes == 3 ? "3 refreshes" : "4 refreshes",
			cpu_median, gpu_median, cpu_high, gpu_high);
		memset(pacing.shown, 0, sizeof(pacing.shown));
		pacing.timed = 0;
	}
	if (pacing.off)
		return 0.0;
	lower = pacing_step(pacing.refreshes, -1, period);
	/* a frame over a quarter second (a load, the background) says nothing
	about the rate */
	if (cpu < 0.25 && gpu < 0.25)
	{
		pacing.cost = fmax(pacing.cost, fmax(cpu, gpu));
		if (fmax(cpu, gpu) > PACING_HEADROOM * period * (double)lower)
			pacing.over++;
	}
	pacing.since_change++;
	if (++pacing.window >= PACING_WINDOW)
	{
		unsigned long misses = pacing.misses;

		pacing.misses = 0;
		pacing.window = 0;
		if (misses >= PACING_MISSES && pacing_step(pacing.refreshes, 1, period) != pacing.refreshes)
			pacing_set(pacing_step(pacing.refreshes, 1, period), period, "frames stayed up longer than asked");
	}
	if (lower != pacing.refreshes && pacing.since_change >= PACING_SETTLE && pacing.over <= PACING_OVER)
		pacing_set(lower, period, "frames fit in fewer refreshes");
	else if (pacing.since_change >= PACING_SETTLE)
	{
		/* start a new measure, so the slowest frame is a recent one */
		pacing.cost = 0.0;
		pacing.over = 0;
		pacing.since_change = 0;
	}
	pacing.due = fmax(pacing.due + hold, now + hold);
	*next_frame_due = (uint32_t)fmax(1.0, (pacing.due - now) * 1e6);
	return hold;
}

/* display.frame_rate in the Mac app (host_gpu_dispatch.c): when the setting
is "unlimited" or a cap above the display's rate, frames stop waiting for the
display. The layer no longer holds a drawable back for the refresh
(displaySyncEnabled), and Present no longer waits in nextDrawable at all: the
window server still hands drawables out at its own pace (two a refresh
behind a locked screen), so a thread of its own asks for the next one, and
a frame that finds none ready is drawn and not shown (free_drawable_take). A
cap at or under the display's rate keeps both waits, so those frames don't
tear, and host_frame_rate_limit holds the cap. The display's rate is the
pacing clock's, which starts a few frames in (not at all with
debug.fixed_timestep, where only "unlimited" lifts the wait). Returns
whether frames go unsynchronized. */
#if TARGET_OS_MACCATALYST
static struct
{
	dispatch_semaphore_t wanted;
	/* a retained id<CAMetalDrawable>, or NULL while the thread waits for one */
	_Atomic(void *) ready;
} free_drawables;
#endif

/* the drawable the thread has waited for, or nil if none has come; again:
the thread then asks for the next one. (With again NO it hands back one
left over from before frames went back to waiting for the display.) */
static id<CAMetalDrawable> free_drawable_take(BOOL again)
{
#if TARGET_OS_MACCATALYST
	void *taken;

	if (!free_drawables.wanted)
	{
		NSThread *thread;

		if (!again)
			return nil;
		free_drawables.wanted = dispatch_semaphore_create(1);
		thread = [[NSThread alloc] initWithBlock:^{
			for (;;)
			{
				dispatch_semaphore_wait(free_drawables.wanted, DISPATCH_TIME_FOREVER);
				@autoreleasepool
				{
					id<CAMetalDrawable> next;

					/* (nil in the background, or after its one-second timeout) */
					while (!(next = [layer nextDrawable]))
						usleep(5000);
					atomic_store(&free_drawables.ready, (__bridge_retained void *)next);
				}
			}
		}];
		thread.name = @"Metal drawables";
		thread.qualityOfService = NSQualityOfServiceUserInteractive;
		[thread start];
	}
	taken = atomic_exchange(&free_drawables.ready, NULL);
	if (!taken)
		return nil;
	if (again)
		dispatch_semaphore_signal(free_drawables.wanted);
	return (__bridge_transfer id<CAMetalDrawable>)taken;
#else
	(void)again;
	return nil;
#endif
}

static BOOL frame_rate_sync(void)
{
#if TARGET_OS_MACCATALYST
	double cap = host_frame_rate_cap();
	CFTimeInterval period;
	BOOL sync;

	if (!host_frame_rate_unlimited() && cap == 0.0)
		return NO;
	os_unfair_lock_lock(&pacing_lock);
	period = refresh_period;
	os_unfair_lock_unlock(&pacing_lock);
	sync = !host_frame_rate_unlimited() && !(period > 0.0 && cap > 1.0 / period + 0.5);
	if (@available(macCatalyst 13.1, *))
	{
		if (layer.displaySyncEnabled != sync)
		{
			layer.displaySyncEnabled = sync;
			platform_log("Metal: frames %s the display's refresh (display.frame_rate)", sync ? "wait for" : "no longer wait for");
		}
		return !sync;
	}
#endif
	return NO;
}

/* the display's 60 Hz blank for the game's vertical blank thread
(d3d8_device.c), which the pacer keeps from the display link's refreshes */
static uint32_t gpu_metal_vertical_blank_wait(void)
{
	return frame_pacer_vertical_blank_wait();
}

/* the game's start of a frame, before it reads input: the pacer may wait
(display.late_start), and is told what the GPU's frames have been costing */
static void gpu_metal_frame_start(void)
{
#if HALO_FRAME_PACING
	static uint64_t counted;
	uint64_t total = atomic_load(&pacing_gpu_nanoseconds);

	frame_pacer_gpu_time((double)(total - counted) / 1e9);
	counted = total;
	frame_pacer_frame_start();
#endif
}

#if HALO_FRAME_PACING && !TARGET_OS_SIMULATOR
/* display.frame_source = "system" (host_frame_pacer.h): the system's own
pacer, a CAMetalDisplayLink (iOS 17, tvOS 17, macOS 14), hands over one
drawable for each refresh, on a thread of its own since the game's thread
never returns to a run loop, and Present takes the latest. One the game
was too late for is let go when the next comes. The pacer asks for the
delay it keeps (preferredFrameLatency) and has no rules of its own here. */
static struct
{
	BOOL active;
	dispatch_semaphore_t came;
	/* a retained id<CAMetalDrawable>, or NULL */
	_Atomic(void *) ready;
} system_frames;

API_AVAILABLE(ios(17.0), tvos(17.0), macCatalyst(17.0))
@interface SystemFrames : NSObject <CAMetalDisplayLinkDelegate>
@end

@implementation SystemFrames
- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update
{
	void *stale = atomic_exchange(&system_frames.ready, (__bridge_retained void *)update.drawable);

	(void)link;
	if (stale)
		(void)(__bridge_transfer id<CAMetalDrawable>)stale;
	else
		dispatch_semaphore_signal(system_frames.came);
}
@end

static void system_frames_start(void)
{
	if (!frame_pacer_wants_system_source())
		return;
	if (@available(iOS 17.0, tvOS 17.0, macCatalyst 17.0, *))
	{
		int latency = frame_pacer_in_flight();
		NSThread *thread;

		system_frames.came = dispatch_semaphore_create(0);
		thread = [[NSThread alloc] initWithBlock:^{
			CAMetalDisplayLink *link = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
			SystemFrames *frames_delegate = [SystemFrames new];

			link.delegate = frames_delegate;
			if (latency)
				link.preferredFrameLatency = (float)latency;
			link.preferredFrameRateRange = CAFrameRateRangeMake(80, 120, 120);
			[link addToRunLoop:NSRunLoop.currentRunLoop forMode:NSDefaultRunLoopMode];
			for (;;)
				[NSRunLoop.currentRunLoop run];
		}];
		thread.name = @"Metal system frames";
		thread.qualityOfService = NSQualityOfServiceUserInteractive;
		[thread start];
		system_frames.active = YES;
		frame_pacer_system_source();
	}
	else
		platform_log("Metal: display.frame_source \"system\" needs iOS 17, tvOS 17 or macOS 14; using \"clock\"");
}

/* the system's next drawable, waited for 100 ms at most (nil in the
background, where none come) */
static id<CAMetalDrawable> system_frame_take(void)
{
	void *taken;

	if (dispatch_semaphore_wait(system_frames.came, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)))
		return nil;
	taken = atomic_exchange(&system_frames.ready, NULL);
	return taken ? (__bridge_transfer id<CAMetalDrawable>)taken : nil;
}
#endif

#if HALO_FRAME_PACING
/* debug.frame_load_gpu: a made-up GPU load, that many extra full-size
draws of the frame's picture into a scratch texture before it is presented,
to see what the pacer's settings do when a frame's work doesn't fit in a
refresh (debug.frame_load_cpu is the pacer's own, host_frame_pacer.c) */
static long frame_load_passes;
static id<MTLTexture> frame_load_target;

static void frame_load_draw(id<MTLTexture> picture)
{
	long index;

	if (frame_load_passes <= 0 || !picture)
		return;
	if (!frame_load_target)
	{
		MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:layer.pixelFormat
			width:4096 height:4096 mipmapped:NO];

		descriptor.usage = MTLTextureUsageRenderTarget;
		descriptor.storageMode = MTLStorageModePrivate;
		frame_load_target = [device newTextureWithDescriptor:descriptor];
	}
	for (index = 0; index < frame_load_passes; index++)
	{
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		id<MTLRenderCommandEncoder> load;

		pass.colorAttachments[0].texture = frame_load_target;
		pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		load = [commands renderCommandEncoderWithDescriptor:pass];
		[load setRenderPipelineState:present_pipeline];
		[load setFragmentTexture:picture atIndex:0];
		[load setFragmentSamplerState:present_sampler atIndex:0];
		[load drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		[load endEncoding];
	}
}
#endif

/* ---------- anti-aliasing passes (gpu_anti_alias)

display.anti_aliasing's FXAA and SMAA, the passes gpu_gl.c runs from
upstream's xgpu_post.c (94882796), in MSL: on a render target in place, over
the rectangle of a window's 3D view. Each pass draws one triangle covering
the rectangle and finds its texels from the pixel's position, rows from the
top as in every target; the target's pixels are copied first, so that no
pass reads what it writes, and only color is written back (the game keeps
values of its own in destination alpha). FXAA is gpu_gl.c's shader line for
line. SMAA is port/third_party/smaa/SMAA.hlsl at its HIGH preset, through
its SMAA_CUSTOM_SL macros: the target's size, which SMAA reads as a global
(SMAA_RT_METRICS), is function constant 16 (0 to 15 are the game's
shaders'), so each pass's pipeline is made
for each size of target (a new one is a compile, once); HLSL's out and inout
parameters become references and its array parameters pointers
(smaa_metal_text). Point samples are linear ones, as in the GLSL that
gpu_gl.c compiles (SMAA takes them at texel centers). */

static NSString *const post_source =
	@"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct PostOut { float4 position [[position]]; };\n"
	"/* one triangle over the whole viewport */\n"
	"vertex PostOut post_vertex(uint id [[vertex_id]])\n"
	"{\n"
	"\tPostOut out;\n"
	"\tout.position = float4(float((id & 1) << 2) - 1.0, float((id & 2) << 1) - 1.0, 0.0, 1.0);\n"
	"\treturn out;\n"
	"}\n";

static NSString *const fxaa_source =
	@"/* 1 / width, 1 / height, width, height; and the rectangle's outermost\n"
	"texel centers, as texture coordinates */\n"
	"struct FxaaParameters { float4 metrics; float4 bounds; };\n"
	"constexpr sampler fxaa_linear(filter::linear, address::clamp_to_edge);\n"
	"constant float EDGE_THRESHOLD = 0.125;\n"
	"constant float EDGE_THRESHOLD_MINIMUM = 0.0312;\n"
	"constant float SUBPIXEL_QUALITY = 0.75;\n"
	"constant int STEPS = 12;\n"
	"constant float STEP_LENGTHS[12] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };\n"
	"static float3 color_at(texture2d<float> color_texture, float4 bounds, float2 position)\n"
	"{\n"
	"\treturn color_texture.sample(fxaa_linear, clamp(position, bounds.xy, bounds.zw), level(0.0)).rgb;\n"
	"}\n"
	"static float luma_at(texture2d<float> color_texture, float4 bounds, float2 position)\n"
	"{\n"
	"\treturn dot(color_at(color_texture, bounds, position), float3(0.299, 0.587, 0.114));\n"
	"}\n"
	"#define color_at(position) color_at(color_texture, p.bounds, position)\n"
	"#define luma_at(position) luma_at(color_texture, p.bounds, position)\n"
	"/* FXAA at position (texture coordinates): false where there is no edge,\n"
	"and the texture's own color stands; else the blended color */\n"
	"static bool fxaa_at(texture2d<float> color_texture, FxaaParameters p, float2 position, thread float3 &blended)\n"
	"{\n"
	"\tfloat4 metrics = p.metrics;\n"
	"\tfloat luma = luma_at(position);\n"
	"\tfloat up = luma_at(position + float2(0.0, -metrics.y));\n"
	"\tfloat down = luma_at(position + float2(0.0, metrics.y));\n"
	"\tfloat left = luma_at(position + float2(-metrics.x, 0.0));\n"
	"\tfloat right = luma_at(position + float2(metrics.x, 0.0));\n"
	"\tfloat highest = max(luma, max(max(up, down), max(left, right)));\n"
	"\tfloat range = highest - min(luma, min(min(up, down), min(left, right)));\n"
	"\n"
	"\t/* no edge: the target has the pixel already */\n"
	"\tif (range < max(EDGE_THRESHOLD_MINIMUM, highest * EDGE_THRESHOLD))\n"
	"\t\treturn false;\n"
	"\tfloat up_left = luma_at(position + float2(-metrics.x, -metrics.y));\n"
	"\tfloat up_right = luma_at(position + float2(metrics.x, -metrics.y));\n"
	"\tfloat down_left = luma_at(position + float2(-metrics.x, metrics.y));\n"
	"\tfloat down_right = luma_at(position + float2(metrics.x, metrics.y));\n"
	"\n"
	"\t/* how much the pixel stands out from its neighborhood (a feature\n"
	"\tthinner than a pixel) */\n"
	"\tfloat average = (2.0 * (up + down + left + right) + up_left + up_right + down_left + down_right) / 12.0;\n"
	"\tfloat subpixel = smoothstep(0.0, 1.0, clamp(abs(average - luma) / range, 0.0, 1.0));\n"
	"\tsubpixel = subpixel * subpixel * SUBPIXEL_QUALITY;\n"
	"\n"
	"\t/* an edge along the rows where the luma changes more from row to row\n"
	"\tthan from column to column; across it, the side it changes more to */\n"
	"\tbool horizontal = abs(up_left + down_left - 2.0 * left) + 2.0 * abs(up + down - 2.0 * luma) +\n"
	"\t\tabs(up_right + down_right - 2.0 * right) >=\n"
	"\t\tabs(up_left + up_right - 2.0 * up) + 2.0 * abs(left + right - 2.0 * luma) +\n"
	"\t\tabs(down_left + down_right - 2.0 * down);\n"
	"\tfloat before = horizontal ? up : left;\n"
	"\tfloat after = horizontal ? down : right;\n"
	"\tbool before_steeper = abs(before - luma) >= abs(after - luma);\n"
	"\tfloat gradient = 0.25 * max(abs(before - luma), abs(after - luma));\n"
	"\tfloat edge_luma = 0.5 * ((before_steeper ? before : after) + luma);\n"
	"\tfloat2 across = (horizontal ? float2(0.0, metrics.y) : float2(metrics.x, 0.0)) * (before_steeper ? -1.0 : 1.0);\n"
	"\tfloat2 along = horizontal ? float2(metrics.x, 0.0) : float2(0.0, metrics.y);\n"
	"\n"
	"\t/* along the edge, half a pixel across, to where its luma ends either way */\n"
	"\tfloat2 end_before = position + 0.5 * across - along;\n"
	"\tfloat2 end_after = position + 0.5 * across + along;\n"
	"\tfloat luma_before = luma_at(end_before) - edge_luma;\n"
	"\tfloat luma_after = luma_at(end_after) - edge_luma;\n"
	"\tbool reached_before = abs(luma_before) >= gradient;\n"
	"\tbool reached_after = abs(luma_after) >= gradient;\n"
	"\tfor (int index = 1; index < STEPS && !(reached_before && reached_after); index++)\n"
	"\t{\n"
	"\t\tif (!reached_before)\n"
	"\t\t{\n"
	"\t\t\tend_before -= along * STEP_LENGTHS[index];\n"
	"\t\t\tluma_before = luma_at(end_before) - edge_luma;\n"
	"\t\t\treached_before = abs(luma_before) >= gradient;\n"
	"\t\t}\n"
	"\t\tif (!reached_after)\n"
	"\t\t{\n"
	"\t\t\tend_after += along * STEP_LENGTHS[index];\n"
	"\t\t\tluma_after = luma_at(end_after) - edge_luma;\n"
	"\t\t\treached_after = abs(luma_after) >= gradient;\n"
	"\t\t}\n"
	"\t}\n"
	"\n"
	"\t/* blended across the edge the more, the nearer the pixel is to an end;\n"
	"\tonly where the luma at that end changes the way the pixel's does */\n"
	"\tfloat distance_before = horizontal ? position.x - end_before.x : position.y - end_before.y;\n"
	"\tfloat distance_after = horizontal ? end_after.x - position.x : end_after.y - position.y;\n"
	"\tfloat pixel_offset = 0.5 - min(distance_before, distance_after) / (distance_before + distance_after);\n"
	"\tbool varies = ((distance_before < distance_after ? luma_before : luma_after) < 0.0) != (luma < edge_luma);\n"
	"\tblended = color_at(position + max(varies ? pixel_offset : 0.0, subpixel) * across);\n"
	"\treturn true;\n"
	"}\n"
	"fragment float4 fxaa_fragment(PostOut in [[stage_in]], texture2d<float> color_texture [[texture(0)]],\n"
	"\tconstant FxaaParameters &p [[buffer(0)]])\n"
	"{\n"
	"\tfloat3 blended;\n"
	"\tif (!fxaa_at(color_texture, p, in.position.xy * p.metrics.xy, blended))\n"
	"\t\tdiscard_fragment();\n"
	"\treturn float4(blended, 1.0);\n"
	"}\n"
	"/* gpu_upscale with FXAA folded in (display.native_hud): the stretch of the\n"
	"scene into the display-sized target, each of its pixels FXAA's at the\n"
	"pixel's place in the scene, or the plain bilinear sample where there is no\n"
	"edge, alpha always the sample's (the game's later draws may read it) */\n"
	"struct UpscaleFxaaParameters { FxaaParameters fxaa; float2 target; };\n"
	"fragment float4 upscale_fxaa_fragment(PostOut in [[stage_in]], texture2d<float> color_texture [[texture(0)]],\n"
	"\tconstant UpscaleFxaaParameters &u [[buffer(0)]])\n"
	"{\n"
	"\tFxaaParameters p = u.fxaa;\n"
	"\tfloat2 position = in.position.xy / u.target;\n"
	"\tfloat4 sampled = color_texture.sample(fxaa_linear, position, level(0.0));\n"
	"\tfloat3 blended;\n"
	"\treturn fxaa_at(color_texture, p, position, blended) ? float4(blended, sampled.a) : sampled;\n"
	"}\n";

/* SMAA.hlsl's macros for Metal (SMAA_CUSTOM_SL), and the global it reads
the target's size from */
static NSString *const smaa_prelude =
	@"#pragma clang diagnostic ignored \"-Wunused-variable\"\n"
	"#pragma clang diagnostic ignored \"-Wunused-function\"\n"
	"constant float4 smaa_metrics [[function_constant(16)]];\n"
	"#define SMAA_RT_METRICS smaa_metrics\n"
	"#define SMAA_CUSTOM_SL 1\n"
	"#define SMAA_PRESET_HIGH 1\n"
	"constexpr sampler smaa_linear(filter::linear, address::clamp_to_edge);\n"
	"#define SMAATexture2D(tex) texture2d<float> tex\n"
	"#define SMAATexturePass2D(tex) tex\n"
	"#define SMAASampleLevelZero(tex, coord) tex.sample(smaa_linear, coord, level(0.0))\n"
	"#define SMAASampleLevelZeroPoint(tex, coord) tex.sample(smaa_linear, coord, level(0.0))\n"
	"#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.sample(smaa_linear, coord, level(0.0), offset)\n"
	"#define SMAASample(tex, coord) tex.sample(smaa_linear, coord)\n"
	"#define SMAASamplePoint(tex, coord) tex.sample(smaa_linear, coord)\n"
	"#define SMAASampleOffset(tex, coord, offset) tex.sample(smaa_linear, coord, offset)\n"
	"#define SMAA_FLATTEN\n"
	"#define SMAA_BRANCH\n"
	"#define discard discard_fragment()\n"
	"#define lerp(a, b, t) mix(a, b, t)\n"
	"#define mad(a, b, c) fma(a, b, c)\n"
	"#define SMAAGather(tex, coord) tex.gather(smaa_linear, coord)\n";

/* the main functions of SMAA's passes, as gpu_gl.c's */
static NSString *const smaa_mains =
	@"fragment float4 smaa_edges_fragment(PostOut in [[stage_in]], texture2d<float> color_texture [[texture(0)]])\n"
	"{\n"
	"\tfloat2 texcoord = in.position.xy * SMAA_RT_METRICS.xy;\n"
	"\tfloat4 offset[3];\n"
	"\tSMAAEdgeDetectionVS(texcoord, offset);\n"
	"\treturn float4(SMAALumaEdgeDetectionPS(texcoord, offset, color_texture), 0.0, 0.0);\n"
	"}\n"
	"fragment float4 smaa_weights_fragment(PostOut in [[stage_in]], texture2d<float> edges_texture [[texture(0)]],\n"
	"\ttexture2d<float> area_texture [[texture(1)]], texture2d<float> search_texture [[texture(2)]])\n"
	"{\n"
	"\tfloat2 texcoord = in.position.xy * SMAA_RT_METRICS.xy;\n"
	"\tfloat2 pixcoord;\n"
	"\tfloat4 offset[3];\n"
	"\tSMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);\n"
	"\treturn SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, edges_texture, area_texture,\n"
	"\t\tsearch_texture, float4(0.0));\n"
	"}\n"
	"fragment float4 smaa_blend_fragment(PostOut in [[stage_in]], texture2d<float> color_texture [[texture(0)]],\n"
	"\ttexture2d<float> weights_texture [[texture(1)]])\n"
	"{\n"
	"\tfloat2 texcoord = in.position.xy * SMAA_RT_METRICS.xy;\n"
	"\tfloat4 offset;\n"
	"\tSMAANeighborhoodBlendingVS(texcoord, offset);\n"
	"\treturn SMAANeighborhoodBlendingPS(texcoord, offset, color_texture, weights_texture);\n"
	"}\n";

/* SMAA.hlsl's text with what MSL lacks rewritten: out and inout parameters
as references in the thread address space, array parameters as pointers,
and the calls that pass a swizzle to an inout parameter (which a reference
cannot bind) through a temporary. nil if a rewrite no longer finds its text
(another SMAA.hlsl than port/third_party/smaa's) */
static NSString *smaa_metal_text(const char *hlsl)
{
	static NSString *const rewrites[][2] =
	{
		{ @"    SMAAMovc(cond.xy, variable.xy, value.xy);\n    SMAAMovc(cond.zw, variable.zw, value.zw);\n",
			@"    variable = select(variable, value, cond);\n" },
		{ @"SMAADetectHorizontalCornerPattern(SMAATexturePass2D(edgesTex), weights.rg, coords.xyzy, d);",
			@"{ float2 corner = weights.rg; SMAADetectHorizontalCornerPattern(SMAATexturePass2D(edgesTex), corner, coords.xyzy, d); weights.rg = corner; }" },
		{ @"SMAADetectVerticalCornerPattern(SMAATexturePass2D(edgesTex), weights.ba, coords.xyxz, d);",
			@"{ float2 corner = weights.ba; SMAADetectVerticalCornerPattern(SMAATexturePass2D(edgesTex), corner, coords.xyxz, d); weights.ba = corner; }" },
		{ @"out float4 offset[3]", @"thread float4 *offset" },
		{ @"float4 offset[3]", @"thread float4 *offset" },
		{ @"inout float2 ", @"thread float2 &" },
		{ @"inout float4 ", @"thread float4 &" },
		{ @"out float2 ", @"thread float2 &" },
		{ @"out float4 ", @"thread float4 &" },
	};
	NSMutableString *text = [NSMutableString stringWithUTF8String:hlsl];
	unsigned long index;

	if (!text)
		return nil;
	for (index = 0; index < sizeof(rewrites) / sizeof(rewrites[0]); index++)
	{
		if (![text replaceOccurrencesOfString:rewrites[index][0] withString:rewrites[index][1] options:NSLiteralSearch
			range:NSMakeRange(0, text.length)])
		{
			platform_log("Metal: SMAA.hlsl has no \"%s\" to rewrite for MSL", rewrites[index][0].UTF8String);
			return nil;
		}
	}
	return text;
}

enum { POST_FXAA, POST_SMAA_EDGES, POST_SMAA_WEIGHTS, POST_SMAA_BLEND, POST_PROGRAMS };

static struct
{
	id<MTLFunction> vertex;
	/* FXAA's function; and SMAA's library, whose functions are made for
	each size of target */
	id<MTLFunction> fxaa, upscale_fxaa;
	id<MTLLibrary> smaa;
	/* FXAA's pipeline, and SMAA's for each size ("WxH" and the pass) */
	id<MTLRenderPipelineState> fxaa_pipeline;
	/* gpu_upscale's with FXAA folded in: all four channels written */
	id<MTLRenderPipelineState> upscale_fxaa_pipeline;
	NSMutableDictionary<NSString *, id<MTLRenderPipelineState>> *smaa_pipelines;
	/* FXAA's, and SMAA's, programs or textures could not be made */
	BOOL failed[2];
	/* the target's pixels before the pass, and SMAA's edges and blending
	weights, at the target's size */
	id<MTLTexture> color, edges, weights;
	/* SMAA's lookup textures */
	id<MTLTexture> area, search;
} post;

static id<MTLRenderPipelineState> post_pipeline(id<MTLFunction> fragment, MTLPixelFormat format, BOOL color_only)
{
	MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
	NSError *error = nil;
	id<MTLRenderPipelineState> pipeline;

	descriptor.vertexFunction = post.vertex;
	descriptor.fragmentFunction = fragment;
	descriptor.colorAttachments[0].pixelFormat = format;
	if (color_only)
		descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
	pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	if (!pipeline)
		platform_log("Metal: cannot make an anti-aliasing pipeline: %s", error.localizedDescription.UTF8String);
	return pipeline;
}

/* SMAA's pipeline for a pass and a size of target, made the first time */
static id<MTLRenderPipelineState> smaa_pipeline(int which, unsigned long width, unsigned long height)
{
	static NSString *const names[POST_PROGRAMS] =
		{ nil, @"smaa_edges_fragment", @"smaa_weights_fragment", @"smaa_blend_fragment" };
	NSString *key = [NSString stringWithFormat:@"%lux%lu/%d", width, height, which];
	id<MTLRenderPipelineState> pipeline = post.smaa_pipelines[key];

	if (!pipeline)
	{
		MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
		float metrics[4] = { 1.0f / (float)width, 1.0f / (float)height, (float)width, (float)height };
		NSError *error = nil;
		id<MTLFunction> fragment;

		[values setConstantValue:metrics type:MTLDataTypeFloat4 atIndex:16];
		fragment = [post.smaa newFunctionWithName:names[which] constantValues:values error:&error];
		if (!fragment)
		{
			platform_log("Metal: cannot make SMAA's %s: %s", names[which].UTF8String, error.localizedDescription.UTF8String);
			return nil;
		}
		pipeline = post_pipeline(fragment, which == POST_SMAA_EDGES ? MTLPixelFormatRG8Unorm :
			which == POST_SMAA_WEIGHTS ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatBGRA8Unorm, which == POST_SMAA_BLEND);
		if (!pipeline)
			return nil;
		post.smaa_pipelines[key] = pipeline;
		platform_log("Metal: SMAA's %s made for %lux%lu", names[which].UTF8String, width, height);
	}
	return pipeline;
}

static id<MTLTexture> post_texture(MTLPixelFormat format, unsigned long width, unsigned long height, BOOL target)
{
	MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
		width:width height:height mipmapped:NO];

	descriptor.usage = MTLTextureUsageShaderRead | (target ? MTLTextureUsageRenderTarget : 0);
	descriptor.storageMode = target ? MTLStorageModePrivate : MTLStorageModeShared;
	return [device newTextureWithDescriptor:descriptor];
}

static uint32_t gpu_metal_anti_alias_prepare(uint32_t method, const char *smaa_shader, const void *area_texels,
	const void *search_texels)
{
	@autoreleasepool
	{
		BOOL smaa = method == GPU_ANTI_ALIAS_SMAA;
		NSError *error = nil;

		if (post.failed[smaa])
			return 0;
		if (!post.vertex)
		{
			id<MTLLibrary> library = [device newLibraryWithSource:[post_source stringByAppendingString:fxaa_source]
				options:pixel_compile_options error:&error];

			post.vertex = [library newFunctionWithName:@"post_vertex"];
			post.fxaa = [library newFunctionWithName:@"fxaa_fragment"];
			post.upscale_fxaa = [library newFunctionWithName:@"upscale_fxaa_fragment"];
			if (!post.vertex || !post.fxaa)
			{
				platform_log("Metal: cannot compile FXAA: %s", error.localizedDescription.UTF8String);
				post.failed[0] = post.failed[1] = YES;
				return 0;
			}
			post.smaa_pipelines = [NSMutableDictionary dictionary];
		}
		if (!smaa)
		{
			if (!post.fxaa_pipeline && !(post.fxaa_pipeline = post_pipeline(post.fxaa, MTLPixelFormatBGRA8Unorm, YES)))
				post.failed[0] = YES;
			if (!post.failed[0] && !post.upscale_fxaa_pipeline)
				post.upscale_fxaa_pipeline = post_pipeline(post.upscale_fxaa, MTLPixelFormatBGRA8Unorm, NO);
			return post.failed[0] ? 0 : 1;
		}
		if (!post.smaa)
		{
			NSString *text = smaa_shader ? smaa_metal_text(smaa_shader) : nil;

			if (text)
				post.smaa = [device newLibraryWithSource:[NSString stringWithFormat:@"%@%@%@\n%@", post_source,
					smaa_prelude, text, smaa_mains] options:pixel_compile_options error:&error];
			if (!post.smaa)
			{
				platform_log("Metal: cannot compile SMAA: %s", error ? error.localizedDescription.UTF8String : "no shader");
				post.failed[1] = YES;
				return 0;
			}
			post.area = post_texture(MTLPixelFormatRG8Unorm, 160, 560, NO);
			[post.area replaceRegion:MTLRegionMake2D(0, 0, 160, 560) mipmapLevel:0 withBytes:area_texels bytesPerRow:160 * 2];
			post.search = post_texture(MTLPixelFormatR8Unorm, 64, 16, NO);
			[post.search replaceRegion:MTLRegionMake2D(0, 0, 64, 16) mipmapLevel:0 withBytes:search_texels bytesPerRow:64];
		}
		return 1;
	}
}

/* a pass over the rectangle of target: its pipeline's fragment function
reads the textures given, from 0 */
static void post_pass(id<MTLTexture> target, BOOL clear, id<MTLRenderPipelineState> pipeline, MTLScissorRect rect,
	NSArray<id<MTLTexture>> *textures, const void *parameters, size_t size, NSString *label)
{
	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	id<MTLRenderCommandEncoder> post_encoder;
	NSUInteger index;

	pass.colorAttachments[0].texture = target;
	pass.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass_traffic_count(pass);
	post_encoder = [command_buffer() renderCommandEncoderWithDescriptor:pass];
	if (metal_debug)
		post_encoder.label = label;
	if (pass_log)
		[pass_log appendFormat:@"\n  %@: %lux%lu at %lu,%lu of %lux%lu", label, (unsigned long)rect.width,
			(unsigned long)rect.height, (unsigned long)rect.x, (unsigned long)rect.y, (unsigned long)target.width,
			(unsigned long)target.height];
	[post_encoder setRenderPipelineState:pipeline];
	[post_encoder setViewport:(MTLViewport){ (double)rect.x, (double)rect.y, (double)rect.width, (double)rect.height,
		0.0, 1.0 }];
	[post_encoder setScissorRect:rect];
	for (index = 0; index < textures.count; index++)
		[post_encoder setFragmentTexture:textures[index] atIndex:index];
	if (parameters)
		[post_encoder setFragmentBytes:parameters length:size atIndex:0];
	[post_encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[post_encoder endEncoding];
}

static void gpu_metal_anti_alias(uint32_t method, gpu_texture target, const struct gpu_rect *rect)
{
	@autoreleasepool
	{
		MetalTexture *record = texture_record(target);
		BOOL smaa = method == GPU_ANTI_ALIAS_SMAA;
		unsigned long width, height;
		MTLScissorRect area;
		id<MTLBlitCommandEncoder> blit;

		if (!record || !record->texture || record->description.format != GPU_FORMAT_BGRA8 ||
			(smaa ? !post.smaa : !post.fxaa_pipeline))
		{
			static BOOL skipped_logged;

			if (!skipped_logged)
			{
				skipped_logged = YES;
				platform_log("Metal: the anti-aliasing pass was skipped (target %s, format %d, programs %s)",
					record && record->texture ? "ok" : "missing", record ? (int)record->description.format : -1,
					(smaa ? post.smaa != nil : post.fxaa_pipeline != nil) ? "ready" : "missing");
			}
			return;
		}
		width = record->texture.width;
		height = record->texture.height;
		if (!clamp_rect(rect, width, height, &area))
			return;
		pass_end();
		if (!post.color || post.color.width != width || post.color.height != height)
		{
			post.color = post_texture(MTLPixelFormatBGRA8Unorm, width, height, YES);
			post.edges = post.weights = nil;
			/* SMAA's pipelines are specialized for one size: the old
			size's go, so a live resize does not keep one set per size */
			[post.smaa_pipelines removeAllObjects];
		}
		if (smaa && !post.edges)
		{
			post.edges = post_texture(MTLPixelFormatRG8Unorm, width, height, YES);
			post.weights = post_texture(MTLPixelFormatRGBA8Unorm, width, height, YES);
		}
		/* the target's pixels, read while the target is drawn into */
		blit = [command_buffer() blitCommandEncoder];
		if (metal_debug)
			blit.label = @"anti-aliasing copy";
		[blit copyFromTexture:record->texture sourceSlice:0 sourceLevel:0
			sourceOrigin:MTLOriginMake(area.x, area.y, 0) sourceSize:MTLSizeMake(area.width, area.height, 1)
			toTexture:post.color destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(area.x, area.y, 0)];
		[blit endEncoding];
		use_texture(record);
		if (!smaa)
		{
			struct { float metrics[4], bounds[4]; } parameters =
			{
				{ 1.0f / (float)width, 1.0f / (float)height, (float)width, (float)height },
				/* (split screen: no texel of the next window is read) */
				{ ((float)area.x + 0.5f) / (float)width, ((float)area.y + 0.5f) / (float)height,
					((float)(area.x + area.width) - 0.5f) / (float)width, ((float)(area.y + area.height) - 0.5f) / (float)height },
			};

			post_pass(record->texture, NO, post.fxaa_pipeline, area, @[ post.color ], &parameters, sizeof(parameters),
				@"FXAA");
			return;
		}
		{
			id<MTLRenderPipelineState> edges = smaa_pipeline(POST_SMAA_EDGES, width, height);
			id<MTLRenderPipelineState> weights = smaa_pipeline(POST_SMAA_WEIGHTS, width, height);
			id<MTLRenderPipelineState> blend = smaa_pipeline(POST_SMAA_BLEND, width, height);

			if (!edges || !weights || !blend)
			{
				post.failed[1] = YES;
				post.smaa = nil;
				return;
			}
			/* the intermediates emptied, all of them, so that no edge of
			another window's pass is found: SMAA's passes write only where
			there is an edge */
			post_pass(post.edges, YES, edges, area, @[ post.color ], NULL, 0, @"SMAA edges");
			post_pass(post.weights, YES, weights, area, @[ post.edges, post.area, post.search ], NULL, 0, @"SMAA weights");
			post_pass(record->texture, NO, blend, area, @[ post.color, post.weights ], NULL, 0, @"SMAA blend");
		}
	}
}

/* ---------- frames */

/* the game's KickPushBuffer: committing here would split render passes */
static void gpu_metal_flush(void)
{
}

/* the back buffer scaled to width x height by MetalFX when it's smaller and
display.upscaler asks for it, else the back buffer itself */
static id<MTLTexture> upscale_into(id<MTLTexture> back_buffer, long width, long height, id<MTLTexture> output);

static id<MTLTexture> upscale(id<MTLTexture> back_buffer, long width, long height)
{
	return upscale_into(back_buffer, width, height, nil);
}

/* (output: a texture of that size to scale into in place of the scaler's
own, if its usage allows; the result says which was written) */
static id<MTLTexture> upscale_into(id<MTLTexture> back_buffer, long width, long height, id<MTLTexture> output)
{
#ifdef HAVE_METALFX
	if (@available(iOS 16.0, tvOS 16.0, visionOS 1.0, *))
	{
		if (!metalfx_wanted || (long)back_buffer.width >= width || (long)back_buffer.height >= height)
			return back_buffer;
		if (!upscaler || upscaler.inputWidth != back_buffer.width || upscaler.inputHeight != back_buffer.height ||
			upscaler.outputWidth != (NSUInteger)width || upscaler.outputHeight != (NSUInteger)height)
		{
			MTLFXSpatialScalerDescriptor *descriptor = [MTLFXSpatialScalerDescriptor new];
			MTLTextureDescriptor *output;

			descriptor.inputWidth = back_buffer.width;
			descriptor.inputHeight = back_buffer.height;
			descriptor.outputWidth = (NSUInteger)width;
			descriptor.outputHeight = (NSUInteger)height;
			descriptor.colorTextureFormat = back_buffer.pixelFormat;
			descriptor.outputTextureFormat = back_buffer.pixelFormat;
			/* the game's colors are display-encoded, not linear */
			descriptor.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
			if (![MTLFXSpatialScalerDescriptor supportsDevice:device] ||
				!(upscaler = [descriptor newSpatialScalerWithDevice:device]))
			{
				platform_log("Metal: MetalFX's spatial scaler is unavailable; the picture is scaled bilinearly");
				GPU_VALIDATION_REPORT(GPU_VALIDATION_UPSCALER_UNAVAILABLE, 0, "Metal: MetalFX's spatial scaler is unavailable; "
					"the picture is scaled bilinearly");
				metalfx_wanted = NO;
				return back_buffer;
			}
			output = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:back_buffer.pixelFormat
				width:(NSUInteger)width height:(NSUInteger)height mipmapped:NO];
			output.storageMode = MTLStorageModePrivate;
			output.usage = upscaler.outputTextureUsage | MTLTextureUsageShaderRead;
			upscaled = [device newTextureWithDescriptor:output];
			platform_log("Metal: MetalFX scales %lux%lu up to %ldx%ld", (unsigned long)back_buffer.width,
				(unsigned long)back_buffer.height, width, height);
		}
		if (!output || (output.usage & upscaler.outputTextureUsage) != upscaler.outputTextureUsage ||
			output.pixelFormat != upscaled.pixelFormat || (long)output.width != width || (long)output.height != height)
			output = upscaled;
		upscaler.colorTexture = back_buffer;
		upscaler.outputTexture = output;
		upscaler.inputContentWidth = back_buffer.width;
		upscaler.inputContentHeight = back_buffer.height;
		[upscaler encodeToCommandBuffer:commands];
		return output;
	}
#endif
	(void)width;
	(void)height;
	(void)output;
	return back_buffer;
}

/* the back buffer letterboxed into the drawable, as gpu_gl.c's gpu_present,
without its vertical flip: GL's window shows row 0 at the bottom, Metal's
drawable at the top */
/* foveation_resolve's shaders, compiled when first needed: the decoder is
Metal 2.3's, and a GPU without rate maps never needs them */
static NSString *const resolve_source =
	@"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct PresentOut { float4 position [[position]]; float2 coordinates; };\n"
	"struct FoveationSizes { float2 screen; float2 physical; float2 allocated; };\n"
	/* a screen point (the normalized coordinate times the screen size, inside
	it) where the map put it, half a texel inside its physical region: the
	targets are allocated larger, and past the region they are stale */
	"static float2 foveated_at(float2 coordinates, constant rasterization_rate_map_data &map,\n"
	"\tconstant FoveationSizes &f)\n"
	"{\n"
	"\trasterization_rate_map_decoder decoder(map);\n"
	"\tfloat2 screen = clamp(coordinates * f.screen, float2(0.0), f.screen - 1.0 / 256.0);\n"
	"\tfloat2 physical = clamp(decoder.map_screen_to_physical_coordinates(screen), float2(0.5), f.physical - 0.5);\n"
	"\treturn physical / f.allocated;\n"
	"}\n"
	"fragment float4 resolve_color(PresentOut in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"\tsampler linear [[sampler(0)]], constant rasterization_rate_map_data &map [[buffer(0)]],\n"
	"\tconstant FoveationSizes &f [[buffer(1)]])\n"
	"{\n"
	"\treturn picture.sample(linear, foveated_at(in.coordinates, map, f));\n"
	"}\n"
	"fragment float resolve_depth(PresentOut in [[stage_in]], depth2d<float> depth [[texture(0)]],\n"
	"\tsampler nearest [[sampler(0)]], constant rasterization_rate_map_data &map [[buffer(0)]],\n"
	"\tconstant FoveationSizes &f [[buffer(1)]])\n"
	"{\n"
	"\treturn depth.sample(nearest, foveated_at(in.coordinates, map, f));\n"
	"}\n";

struct foveation_sizes_uniform
{
	float screen[2], physical[2], allocated[2];
};

static id<MTLRenderPipelineState> resolve_color_pipeline, resolve_depth_pipeline;
static id<MTLSamplerState> resolve_nearest;
/* foveation_resolve's outputs, by kind, size and eye */
static NSMutableDictionary<NSString *, id<MTLTexture>> *resolve_outputs;

/* a foveated target drawn unfoveated at width x height, at its screen
shape, into a new texture: BGRA8 for a color target, R32Float for a depth
target's depth (as stored, reversed). Encoded in the open command buffer,
after the open pass; nil if the target isn't foveated this frame */
static id<MTLTexture> foveation_resolve(MetalTexture *record, NSUInteger width, NSUInteger height)
{
	MTLSize screen, physical;
	BOOL depth = record && record->description.format == GPU_FORMAT_DEPTH_STENCIL;
	MTLTextureDescriptor *descriptor;
	MTLRenderPassDescriptor *pass;
	id<MTLRenderCommandEncoder> resolve;
	id<MTLTexture> output;
	struct foveation_sizes_uniform sizes;

	if (!record || !record->texture || !foveation_sizes(record, &screen, &physical) || !width || !height)
		return nil;
	if (!resolve_color_pipeline)
	{
		NSError *error = nil;
		id<MTLLibrary> library = [device newLibraryWithSource:resolve_source options:nil error:&error];
		MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
		MTLSamplerDescriptor *nearest = [MTLSamplerDescriptor new];

		if (library)
		{
			pipeline.vertexFunction = present_vertex;
			pipeline.fragmentFunction = [library newFunctionWithName:@"resolve_color"];
			pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
			resolve_color_pipeline = [device newRenderPipelineStateWithDescriptor:pipeline error:&error];
			pipeline.fragmentFunction = [library newFunctionWithName:@"resolve_depth"];
			pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
			if (resolve_color_pipeline)
				resolve_depth_pipeline = [device newRenderPipelineStateWithDescriptor:pipeline error:&error];
		}
		if (!resolve_color_pipeline || !resolve_depth_pipeline)
		{
			platform_log("Metal: the foveation resolve's shaders failed: %s", error.description.UTF8String);
			resolve_color_pipeline = resolve_depth_pipeline = nil;
			return nil;
		}
		resolve_nearest = [device newSamplerStateWithDescriptor:nearest];
	}
	/* one output per kind, size and eye, kept: the HEAD-mode menu over the
	film resolves an eye every frame, about 13 MB at quality 0.6. The two
	eyes of one frame get their own; a later frame's resolve is queued after
	the command buffers that read this one's */
	{
		NSString *key = [NSString stringWithFormat:@"%d %lux%lu %u", depth, (unsigned long)width,
			(unsigned long)height, record->description.foveated_eye];

		output = resolve_outputs[key];
		if (!output)
		{
			descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth ? MTLPixelFormatR32Float :
				MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
			descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
			descriptor.storageMode = MTLStorageModePrivate;
			output = [device newTextureWithDescriptor:descriptor];
			/* (the sizes move while the render quality eases: a few at most) */
			if (!resolve_outputs || resolve_outputs.count >= 8)
				resolve_outputs = [NSMutableDictionary dictionary];
			resolve_outputs[key] = output;
		}
	}
	sizes = (struct foveation_sizes_uniform){ { (float)screen.width, (float)screen.height },
		{ (float)physical.width, (float)physical.height },
		{ (float)record->texture.width, (float)record->texture.height } };
	pass_end();
	pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = output;
	pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	resolve = [command_buffer() renderCommandEncoderWithDescriptor:pass];
	if (metal_debug)
		resolve.label = [NSString stringWithFormat:@"foveation resolve of %lux%lu", (unsigned long)screen.width,
			(unsigned long)screen.height];
	[resolve setRenderPipelineState:depth ? resolve_depth_pipeline : resolve_color_pipeline];
	[resolve setViewport:(MTLViewport){ 0.0, 0.0, (double)width, (double)height, 0.0, 1.0 }];
	[resolve setFragmentTexture:record->texture atIndex:0];
	[resolve setFragmentSamplerState:depth ? resolve_nearest : present_sampler atIndex:0];
	[resolve setFragmentBuffer:foveation_parameters(record) offset:0 atIndex:0];
	[resolve setFragmentBytes:&sizes length:sizeof(sizes) atIndex:1];
	[resolve drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	[resolve endEncoding];
	use_texture(record);
	return output;
}

/* a foveated target resolved at its screen shape inside its allocated size,
for a picture drawn whole somewhere else (the film's screen, the UI's
quad); an unfoveated one as it is */
static id<MTLTexture> foveation_picture(MetalTexture *record)
{
	MTLSize screen, physical;
	NSUInteger width, height;
	id<MTLTexture> resolved;

	if (!foveation_sizes(record, &screen, &physical))
		return record->texture;
	width = record->texture.width;
	height = width * screen.height / screen.width;
	if (height > record->texture.height)
	{
		height = record->texture.height;
		width = height * screen.width / screen.height;
	}
	resolved = foveation_resolve(record, width, height);
	return resolved ? resolved : record->texture;
}

/* display.async_shaders, at each frame's last commit: the frame's skipped
draws into the launch's totals, and the launch's line once its first
compiles are done (no frame waited for any of them: these are what the
draws skipped meanwhile, and the slowest compile, which a frame would have
waited for). After it, a line every 600 frames when more draws were skipped
(a map's load, a new effect) */
static void async_frame_end(void)
{
	static int reports;
	unsigned long skipped, outstanding;
	int broken;

	if (!async_compiles)
		return;
	skipped = metal_skip_frame_end(&skip_frame, &broken);
	outstanding = atomic_load(&compiles_outstanding);
	/* (a few lines while draws keep skipping, a frame in every 300) */
	skipping_frames = skipped ? skipping_frames + 1 : 0;
	if (skipping_frames % 300 == 299 && reports < 6)
	{
		reports++;
		platform_log("Metal: async shaders: frame %lu, %.1f s in: %lu frames in a row have skipped draws (%lu in this "
			"one); %lu compiles running, %zu pipelines claimed and not yet made", frames,
			CACurrentMediaTime() - compile_started, skipping_frames, skipped, outstanding,
			metal_compile_pending(&pipeline_table));
	}
	if (metal_compile_frame_end(&compile_totals, skipped, frame_held, outstanding))
	{
		pthread_mutex_lock(&compile_totals.lock);
		platform_log("Metal: async shaders: the first compiles are done at frame %lu, %.1f s in: %lu draws skipped in "
			"%lu frames (%lu held in the window) while %lu compiles ran on workers; the slowest, a %s, took %.0f ms",
			frames, CACurrentMediaTime() - compile_started, compile_totals.draws_skipped, compile_totals.frames_skipping,
			compile_totals.frames_held, compile_totals.compiles, compile_kind_name(compile_totals.slowest_kind),
			compile_totals.slowest * 1e3);
		pthread_mutex_unlock(&compile_totals.lock);
	}
	else if (frames % 600 == 599)
	{
		pthread_mutex_lock(&compile_totals.lock);
		if (compile_totals.summarized && compile_totals.draws_skipped != compile_totals.reported_skips)
		{
			platform_log("Metal: async shaders: so far %lu draws skipped in %lu frames (%lu held in the window), %lu "
				"compiles on workers; the slowest, a %s, took %.0f ms", compile_totals.draws_skipped,
				compile_totals.frames_skipping, compile_totals.frames_held, compile_totals.compiles,
				compile_kind_name(compile_totals.slowest_kind), compile_totals.slowest * 1e3);
			compile_totals.reported_skips = compile_totals.draws_skipped;
		}
		pthread_mutex_unlock(&compile_totals.lock);
	}
	frame_held = NO;
}

/* display.native_hud (gpu.h): the scene's picture into the display-sized
back buffer, where the HUD draws next. The quad goes in the pass the HUD's
draws then continue (pass_begin finds it open), so the two cost one pass; an
area that is the whole target makes the pass's load action a clear. The
scene's pass ends here, and its depth and stencil are not stored: the draws
that follow bind none, and the next scene clears its own. */
static void gpu_metal_upscale(gpu_texture source, gpu_texture destination, const struct gpu_rect *area, uint32_t anti_alias)
{
	@autoreleasepool
	{
		MetalTexture *source_record = texture_record(source), *destination_record = texture_record(destination);
		id<MTLTexture> picture, target;
		MTLScissorRect scissor;
		struct gpu_clear whole_clear;
		BOOL whole, scaled = NO, fxaa;
		struct gpu_rect scene;

		frame_begin();
		if (!source_record || !source_record->texture || !destination_record || !destination_record->texture)
			return;
		picture = source_record->texture;
		target = destination_record->texture;
		if (!clamp_rect(area, target.width, target.height, &scissor))
			return;
		whole = scissor.x == 0 && scissor.y == 0 && scissor.width == target.width && scissor.height == target.height;
		/* FXAA (display.anti_aliasing): in the quad's own shader, unless
		MetalFX scales this picture (it reads the scene itself) or the
		folded pipeline is missing; then as its own pass first, over the
		matching rectangle of the scene */
		scene.x = (int32_t)((unsigned long)scissor.x * picture.width / target.width);
		scene.y = (int32_t)((unsigned long)scissor.y * picture.height / target.height);
		scene.width = (int32_t)((unsigned long)(scissor.x + scissor.width) * picture.width / target.width) - scene.x;
		scene.height = (int32_t)((unsigned long)(scissor.y + scissor.height) * picture.height / target.height) - scene.y;
		fxaa = anti_alias == GPU_ANTI_ALIAS_FXAA && post.upscale_fxaa_pipeline &&
			!(whole && metalfx_wanted && (picture.width < target.width || picture.height < target.height));
		if (anti_alias == GPU_ANTI_ALIAS_FXAA && !fxaa)
			gpu_metal_anti_alias(GPU_ANTI_ALIAS_FXAA, source, &scene);
		command_buffer();
		/* (only the scene's own pass: another target's depth is its own affair) */
		if (encoder && pass_color == source)
			pass_finish(YES);
		else
			pass_end();
		use_texture(source_record);
		/* display.upscaler = "metalfx", which scales whole pictures only:
		straight into the target if it can be the scaler's output, else into
		the scaler's own texture, which the quad copies */
		if (whole)
		{
			picture = upscale_into(picture, (long)target.width, (long)target.height, target);
			if (pass_log && picture != source_record->texture)
				[pass_log appendFormat:@"\n  upscale: MetalFX, texture %u %lux%lu into %s %lux%lu", source,
					(unsigned long)source_record->texture.width, (unsigned long)source_record->texture.height,
					picture == target ? "the target" : "its own texture", (unsigned long)target.width,
					(unsigned long)target.height];
			/* (scaled straight into the target: there is no quad to draw,
			and the pass the HUD continues loads what the scaler wrote) */
			if (picture == target)
				scaled = YES;
		}
		/* the frame so far is committed, so that a pass that interrupts the
		one opened here can run between the two (native_pass) */
		commit(NO);
		/* (again, for the new command buffer: this marks the scene as used
		in it, so the next view's scene is never taken for a pass that may
		run ahead) */
		use_texture(source_record);
		memset(&whole_clear, 0, sizeof(whole_clear));
		whole_clear.flags = GPU_CLEAR_COLOR;
		if (!pass_begin(destination, 0, whole && !scaled ? &whole_clear : NULL))
			return;
		native_pass.color = destination;
		if (scaled)
			return;
		if (pass_log)
			[pass_log appendFormat:@"; upscale: texture %u %lux%lu into %lux%lu at %lu,%lu", source,
				(unsigned long)picture.width, (unsigned long)picture.height, (unsigned long)scissor.width,
				(unsigned long)scissor.height, (unsigned long)scissor.x, (unsigned long)scissor.y];
		[encoder setRenderPipelineState:fxaa ? post.upscale_fxaa_pipeline : upscale_pipeline];
		[encoder setCullMode:MTLCullModeNone];
		[encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
		[encoder setViewport:(MTLViewport){ 0.0, 0.0, (double)target.width, (double)target.height, 0.0, 1.0 }];
		[encoder setScissorRect:scissor];
		[encoder setFragmentTexture:picture atIndex:0];
		[encoder setFragmentSamplerState:present_sampler atIndex:0];
		if (visibility.entry_open)
			[encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
		if (fxaa)
		{
			float width = (float)picture.width, height = (float)picture.height;
			/* the scene's size, the view's outermost texel centers (split
			screen: no texel of the next view is read), and the target's size */
			struct { float metrics[4], bounds[4], target[2], pad[2]; } parameters =
			{
				{ 1.0f / width, 1.0f / height, width, height },
				{ ((float)scene.x + 0.5f) / width, ((float)scene.y + 0.5f) / height,
					((float)(scene.x + scene.width) - 0.5f) / width, ((float)(scene.y + scene.height) - 0.5f) / height },
				{ (float)target.width, (float)target.height }, { 0.0f, 0.0f },
			};

			[encoder setFragmentBytes:&parameters length:sizeof(parameters) atIndex:0];
			/* (post_vertex: one triangle over the viewport) */
			[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		}
		else
			[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
		pass_commands++;
		visibility.entry_open = 0;
		/* the next draw sets everything again */
		metal_state_reset(&state_cache);
	}
}

static uint32_t gpu_metal_present(gpu_texture back_buffer)
{
	@autoreleasepool
	{
		MetalTexture *record = texture_record(back_buffer);
		id<CAMetalDrawable> drawable;
		CFTimeInterval waited;
		uint32_t next_frame_due = 0;
		BOOL unsynced;
		/* the frame's dealings with the pacer (host_frame_pacer.h), which
		decides when it may start toward the display and how long it stays */
		struct frame_pacer_frame paced;

		command_buffer();
		pass_finish(YES);
#if TARGET_OS_VISION
		/* head-tracked stereo without eyes this frame (the main menu, a
		load): the picture goes on the UI's quad, level in front of the head
		(host_stereo_present_ui), not the theater screen */
		if (theater_wanted && record && record->texture && host_theater_active() && host_stereo_ui_ready())
		{
			use_texture(record);
			commit(YES);
			host_stereo_present_ui(queue, record->texture, nil, stereo_cut_brightness(HOST_STEREO_VIEW_UI, 0, 0));
			frames++;
			pacing.work_started = CACurrentMediaTime();
			return 0;
		}
		/* theater mode: the picture, at the screen's size (sharp enough for its
		angle, host_theater_picture_size), goes on the screen
		in the immersive space, whose frames pace the game; the window isn't
		drawn meanwhile */
		if (theater_wanted && record && record->texture && host_theater_active())
		{
			int screen_width, screen_height;
			long window_width, window_height, width, height;
			id<MTLTexture> picture;

			host_theater_picture_size(&screen_width, &screen_height);
			window_width = screen_width;
			window_height = screen_height;
			width = window_width;
			height = window_width * (long)record->description.height / (long)record->description.width;

			if (height > window_height)
			{
				height = window_height;
				width = window_height * (long)record->description.width / (long)record->description.height;
			}
			picture = upscale(foveation_picture(record), width, height);
			use_texture(record);
			commit(YES);
			host_theater_present(queue, picture);
			frames++;
			pacing.work_started = CACurrentMediaTime();
			return 0;
		}
#endif
		/* display.async_shaders: a frame with a pass that drew nothing (its
		pipelines all still compiling) isn't shown; the window keeps the last
		whole one (metal_hold_frame) */
		frame_held = async_compiles && metal_hold_frame(&window_hold, metal_skip_frame_broken(&skip_frame), HOLD_LIMIT);
		unsynced = frame_rate_sync();
		waited = CACurrentMediaTime();
		/* display.frame_rate's cap; its sleep counts as waiting, not as the
		frame's CPU time */
		host_frame_rate_limit();
		/* (unsynchronized, a frame with no drawable ready isn't shown) */
		frame_pacer_frame_begin(&paced, frames, pacing.work_started, !frame_held && !unsynced);
#if HALO_FRAME_PACING && !TARGET_OS_SIMULATOR
		if (system_frames.active)
			drawable = frame_held ? nil : system_frame_take();
		else
#endif
		drawable = frame_held ? nil : unsynced ? free_drawable_take(YES) : (free_drawable_take(NO) ?: [layer nextDrawable]);
		/* (a held frame waits a refresh, as the drawable would have, so the
		game doesn't spin through frames of skipped draws, taking a core from
		the compiles) */
		if (frame_held)
		{
			CFTimeInterval period;

			os_unfair_lock_lock(&pacing_lock);
			period = refresh_period;
			os_unfair_lock_unlock(&pacing_lock);
			usleep((useconds_t)((period >= 1.0 / 240.0 && period <= 1.0 / 20.0 ? period : 1.0 / 60.0) * 1e6));
		}
		pacing_waited += CACurrentMediaTime() - waited;
		/* (a frame not shown isn't scheduled, so its waits aren't taken up
		there: they mustn't count against the next frame that is) */
		if (unsynced && !drawable)
			pacing_waited = 0.0;
		/* nil in the background (or held, or unsynchronized with none ready):
		the frame still commits, so its slot is freed */
		if (drawable && record && record->texture)
		{
			MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
			long window_width = (long)drawable.texture.width, window_height = (long)drawable.texture.height;
			long width = window_width, height = window_width * (long)record->description.height / (long)record->description.width;
			id<MTLRenderCommandEncoder> present;
			id<MTLTexture> picture;

			if (height > window_height)
			{
				height = window_height;
				width = window_height * (long)record->description.width / (long)record->description.height;
			}
#if !TARGET_OS_VISION
			{
				/* (a picture within a pixel of its place is copied, not
				resampled: display.native_hud's back buffer, halo_display.h) */
				struct halo_display_size place = halo_display_present_size(window_width, window_height,
					(long)record->description.width, (long)record->description.height);

				width = place.width;
				height = place.height;
			}
#endif
			/* (debug.rate_map_test: the back buffer resolved first) */
			picture = upscale(foveation_picture(record), width, height);
			pass.colorAttachments[0].texture = drawable.texture;
			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
#if HALO_FRAME_PACING
			frame_load_draw(picture);
#endif
			present = [commands renderCommandEncoderWithDescriptor:pass];
			if (metal_debug)
				present.label = @"present";
			if (pass_log)
				[pass_log appendFormat:@"\n  present: texture %u %lux%lu into %ldx%ld of drawable %ldx%ld %s/%s", back_buffer,
					(unsigned long)picture.width, (unsigned long)picture.height, width, height, window_width, window_height,
					load_name(pass.colorAttachments[0].loadAction), store_name(pass.colorAttachments[0].storeAction)];
			[present setRenderPipelineState:present_pipeline];
			[present setViewport:(MTLViewport){ (double)((window_width - width) / 2), (double)((window_height - height) / 2),
				(double)width, (double)height, 0.0, 1.0 }];
			[present setFragmentTexture:picture atIndex:0];
			[present setFragmentSamplerState:present_sampler atIndex:0];
			[present drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
			[present endEncoding];
			use_texture(record);
			{
				CFTimeInterval hold;

				frame_log.width = (unsigned)picture.width;
				frame_log.height = (unsigned)picture.height;
				frame_context_wait();
				hold = pacing_schedule(drawable, &next_frame_due);

#if HALO_FRAME_PACING
				/* (display.frame_pacing's own holds are whole refreshes
				already; otherwise the pacer says) */
				if (hold == 0.0)
					hold = frame_pacer_present(&paced);
				if (frame_pacer_watches(&paced))
				{
#if TARGET_OS_SIMULATOR
					/* the iOS and tvOS simulators' MTLDrawable has no presented
					handler and no presentedTime: the pacer is told now, with no
					time (it holds no slots there, host_frame_pacer.c) */
					frame_pacer_presented(&paced, 0.0);
#else
					struct frame_pacer_frame shown_frame = paced;

					[drawable addPresentedHandler:^(id<MTLDrawable> shown)
					{
						frame_pacer_presented(&shown_frame, shown.presentedTime);
					}];
#endif
					paced.slot = 0;
				}
#endif
#if TARGET_OS_SIMULATOR
				(void)hold;
				[commands presentDrawable:drawable];
#else
				if (hold > 0.0)
					[commands presentDrawable:drawable afterMinimumDuration:hold];
				else
					[commands presentDrawable:drawable];
#endif
			}
		}
		else
			host_benchmark_frame_unshown(drawable ? 1 : 0);
		/* (a frame with nothing to show: its slot goes back now) */
		frame_pacer_frame_dropped(&paced);
		drawable = nil;
		commit(YES);
		os_unfair_lock_lock(&compile_stats_lock);
		if (metal_debug && (compile_stats.count[COMPILE_LIBRARY] || compile_stats.count[COMPILE_SPECIALIZE] ||
			compile_stats.count[COMPILE_PIPELINE]))
			platform_log("Metal: frame %lu compiled %lu libraries (%.1f ms, longest %.1f), specialized %lu vertex "
				"functions (%.1f ms, longest %.1f), made %lu pipelines (%.1f ms, longest %.1f)", frames,
				compile_stats.count[COMPILE_LIBRARY], compile_stats.seconds[COMPILE_LIBRARY] * 1e3,
				compile_stats.longest[COMPILE_LIBRARY] * 1e3, compile_stats.count[COMPILE_SPECIALIZE],
				compile_stats.seconds[COMPILE_SPECIALIZE] * 1e3, compile_stats.longest[COMPILE_SPECIALIZE] * 1e3,
				compile_stats.count[COMPILE_PIPELINE], compile_stats.seconds[COMPILE_PIPELINE] * 1e3,
				compile_stats.longest[COMPILE_PIPELINE] * 1e3);
		memset(compile_stats.count, 0, sizeof(compile_stats.count));
		memset(compile_stats.seconds, 0, sizeof(compile_stats.seconds));
		memset(compile_stats.longest, 0, sizeof(compile_stats.longest));
		os_unfair_lock_unlock(&compile_stats_lock);
		frames++;
		if (pass_log)
		{
			platform_log("Metal: frame %lu runs, in order:%s", frames - 1, pass_log.UTF8String);
			pass_log = nil;
		}
		if (metal_debug && frames % 600 == 300)
			pass_log = [NSMutableString string];
		if (frames % 600 == 0 && renames)
		{
			platform_log("Metal: %lu renames in the last 600 frames", renames);
			renames = 0;
		}
		if (frames % 600 == 0 && state_cache.draws)
		{
			platform_log("Metal: %.1f draws a frame make %.1f encoder calls each and skip %.1f (state cache %s)",
				(double)state_cache.draws / 600.0, (double)state_cache.issued / state_cache.draws,
				(double)state_cache.skipped / state_cache.draws, state_cache.enabled ? "on" : "off");
			if (metal_debug)
				platform_log("Metal: %.1f pipeline changes a frame, %.1f of them with the same shaders",
					(double)pipeline_changes.changes / 600.0, (double)pipeline_changes.same_shaders / 600.0);
			pipeline_changes.changes = pipeline_changes.same_shaders = 0;
			os_unfair_lock_lock(&compile_stats_lock);
			if (metal_debug)
				platform_log("Metal: so far %lu shader libraries (%.0f ms), %lu vertex specializations (%.0f ms), "
					"%lu pipelines (%.0f ms) made while drawing; outside a map's list: %lu libraries, %lu "
					"specializations, %lu pipelines", compile_stats.total_count[COMPILE_LIBRARY],
					compile_stats.total_seconds[COMPILE_LIBRARY] * 1e3, compile_stats.total_count[COMPILE_SPECIALIZE],
					compile_stats.total_seconds[COMPILE_SPECIALIZE] * 1e3, compile_stats.total_count[COMPILE_PIPELINE],
					compile_stats.total_seconds[COMPILE_PIPELINE] * 1e3, compile_stats.during_play[COMPILE_LIBRARY],
					compile_stats.during_play[COMPILE_SPECIALIZE], compile_stats.during_play[COMPILE_PIPELINE]);
			os_unfair_lock_unlock(&compile_stats_lock);
			metal_state_take_counts(&state_cache);
		}
		if (frames % 600 == 0)
		{
			platform_log("Metal: a frame has %.1f render passes, which load %.0f MB and store %.0f MB",
				(double)pass_traffic.passes / 600.0, pass_traffic.loaded / 600.0 / 1e6, pass_traffic.stored / 600.0 / 1e6);
			memset(&pass_traffic, 0, sizeof(pass_traffic));
			if (native_pass.parks || native_pass.in_order)
				platform_log("Metal: the HUD's display-sized pass was left %.1f times a frame for a pass that ran ahead of "
					"it, and ended early %.1f times a frame", native_pass.parks / 600.0, native_pass.in_order / 600.0);
			native_pass.parks = native_pass.in_order = 0;
		}
		pacing.work_started = CACurrentMediaTime();
		return next_frame_due;
	}
}

/* a stereo frame: in head-tracked stereo (display.stereo = "head") each eye's
picture and depth, and the HUD, go to the Compositor's frame that
host_stereo_frame opened at the frame's begin (host_stereo.m); in stereo on
the screen (display.stereo = "screen") each eye's picture, with the HUD over
it, goes on the theater screen for its view (host_theater_present_eyes);
without a frame (the space closed, another mode) eye 0 goes through the mono
path.

A cutscene (present->cinematic, the 3D film) is on the screen in any mode,
and the script fade (present->fade) tints the space around the screen. A cut
between any two of the full view, the screen and HEAD mode's UI quad (a
cutscene starting or ending in HEAD mode, a load starting or ending, a menu
over the film) goes to black and fades back in over HOST_STEREO_CUT_SECONDS,
unless a script fade already covers it (present->cut_covered,
stereo.c's halo_stereo_cut_covered). */
#if TARGET_OS_VISION
/* what the last stereo frame showed (host_stereo_hud.h's cut, which also
covers the UI's quad), and the frames of the cut's fade still to come */
static struct host_stereo_cut stereo_cut = HOST_STEREO_CUT_INITIAL;

static const char *stereo_view_name(int view)
{
	return view == HOST_STEREO_VIEW_SCREEN ? "the screen" : view == HOST_STEREO_VIEW_UI ? "the UI's quad" : "the full view";
}

/* the brightness of a frame showing view (enum host_stereo_view): coming up
from black over HOST_STEREO_CUT_SECONDS after a cut no script fade covers;
black while requested (stereo's cut over a seat's exit glide) */
static float stereo_cut_brightness(int view, int covered, int requested)
{
	int last = stereo_cut.shown, switched;
	/* a frame stays up for the repeat count plus one refreshes of the
	Vision Pro's 90 Hz, so the fade lasts the same time at 45 */
	float frame_seconds = (float)(host_theater_frame_repeat() + 1) / 90.0f;
	float brightness = host_stereo_cut_brightness(&stereo_cut, view, covered, requested, frame_seconds,
		&switched);

	if (switched)
		platform_log("stereo: from %s to %s through black", stereo_view_name(last), stereo_view_name(view));
	return brightness;
}

/* Stereo's frame times, for the benchmarks (avp-benchmark.sh reads the
"frames shown for" line, as pacing_schedule's): the Compositor paces stereo,
so its frames are measured by their presentation times, and a frame's CPU
time is the time since the last present less the time spent waiting for the
Compositor's frame. The refresh period is the shortest interval between
presentations over the report's frames divided by the repeat count plus one
(an on-time frame's), or 90 Hz's when that falls outside 60 to 120 Hz.
Every 600 stereo frames.

Both times lag by a frame, deliberately: the GPU time is what completed
since the last count (the game's command buffers and the presenters',
gpu_metal_count_gpu_time), and completion handlers land after the next frame
has begun, as in pacing_schedule; the CPU time leaves out this frame's
presenter encode, which comes after the count, and includes the last one's.
Over a report each frame is counted once, so the medians are right. */
static struct
{
	CFTimeInterval last_presentation;
	CFTimeInterval shortest;
	uint64_t gpu_counted;
	CFTimeInterval intervals[PACING_TIMED_FRAMES];
	double cpu_times[PACING_TIMED_FRAMES], gpu_times[PACING_TIMED_FRAMES];
	unsigned long timed, frames;
} stereo_pacing;

static void stereo_pacing_count(void)
{
	CFTimeInterval now = CACurrentMediaTime();
	CFTimeInterval waited = host_theater_take_waited();
	CFTimeInterval presentation = host_theater_presentation_time();
	CFTimeInterval interval = 0.0;
	uint64_t gpu_total = atomic_load(&pacing_gpu_nanoseconds);
	double cpu = pacing.work_started > 0.0 ? now - pacing.work_started - waited : 0.0;
	double gpu = (double)(gpu_total - stereo_pacing.gpu_counted) / 1e9;

	stereo_pacing.gpu_counted = gpu_total;
	if (presentation > 0.0 && stereo_pacing.last_presentation > 0.0)
		interval = presentation - stereo_pacing.last_presentation;
	if (presentation > 0.0)
		stereo_pacing.last_presentation = presentation;
	/* a quarter second or more is a load or the background, not a frame */
	if (interval > 0.0 && interval < 0.25 && cpu > 0.0 && stereo_pacing.timed < PACING_TIMED_FRAMES)
	{
		if (stereo_pacing.shortest <= 0.0 || interval < stereo_pacing.shortest)
			stereo_pacing.shortest = interval;
		stereo_pacing.intervals[stereo_pacing.timed] = interval;
		stereo_pacing.cpu_times[stereo_pacing.timed] = cpu;
		stereo_pacing.gpu_times[stereo_pacing.timed] = gpu;
		stereo_pacing.timed++;
	}
	if (++stereo_pacing.frames % 600 == 0 && stereo_pacing.timed)
	{
		int repeat = host_theater_frame_repeat();
		CFTimeInterval period = stereo_pacing.shortest / (double)(repeat + 1);
		unsigned long shown[6] = { 0 };
		double cpu_median, cpu_high, gpu_median, gpu_high;
		unsigned long index;

		if (period < 1.0 / 120.0 || period > 1.0 / 60.0)
			period = 1.0 / 90.0;
		for (index = 0; index < stereo_pacing.timed; index++)
		{
			long up = lround(stereo_pacing.intervals[index] / period);

			shown[up < 1 ? 1 : up > 5 ? 5 : up]++;
		}
		pacing_percentiles(stereo_pacing.cpu_times, stereo_pacing.timed, &cpu_median, &cpu_high);
		pacing_percentiles(stereo_pacing.gpu_times, stereo_pacing.timed, &gpu_median, &gpu_high);
		platform_log("stereo: frames shown for 1/2/3/4/5+ refreshes at %.1f Hz: %lu/%lu/%lu/%lu/%lu (repeat count %d); "
			"frame CPU %.1f ms, GPU %.1f ms (medians; 95th %.1f, %.1f)", 1.0 / period, shown[1], shown[2], shown[3],
			shown[4], shown[5], repeat, cpu_median, gpu_median, cpu_high, gpu_high);
		stereo_pacing.timed = 0;
		stereo_pacing.shortest = 0.0;
	}
}
#endif

static uint32_t gpu_metal_present_stereo(const struct gpu_stereo_present *present)
{
#if TARGET_OS_VISION
	@autoreleasepool
	{
		MetalTexture *color[2] = { texture_record(present->eye_color[0]), texture_record(present->eye_color[1]) };
		MetalTexture *depth[2] = { texture_record(present->eye_depth[0]), texture_record(present->eye_depth[1]) };
		MetalTexture *hud = present->hud ? texture_record(present->hud) : nil;
		MetalTexture *zoom = present->zoom ? texture_record(present->zoom) : nil;
		BOOL on_screen = present->mode == HALO_STEREO_SCREEN || present->cinematic;

		if (theater_wanted && !on_screen && present->mode == HALO_STEREO_HEAD && color[0] && color[0]->texture && color[1] &&
			color[1]->texture && depth[0] && depth[0]->texture && depth[1] && depth[1]->texture && host_stereo_ready())
		{
			int eye;

			command_buffer();
			/* the eyes' depth is read after the frame, so it's stored */
			pass_end();
			for (eye = 0; eye < 2; eye++)
			{
				use_texture(color[eye]);
				use_texture(depth[eye]);
			}
			/* the HUD layer, the crosshairs' layer and the HUD groups'
			targets (host_stereo_hud.h) */
			MetalTexture *hud_records[HOST_STEREO_HUD_LAYER_COUNT] = { hud };
			__unsafe_unretained id<MTLTexture> hud_layers[HOST_STEREO_HUD_LAYER_COUNT] = { nil };
			int layer;

			hud_records[HOST_STEREO_HUD_LAYER_RETICLE] = present->reticle_layer ?
				texture_record(present->reticle_layer) : nil;
			for (layer = 0; layer < HALO_HUD_GROUP_COUNT; layer++)
				hud_records[HOST_STEREO_HUD_LAYER_GROUP + layer] = present->hud_group[layer] ?
					texture_record(present->hud_group[layer]) : nil;
			hud_records[HOST_STEREO_HUD_LAYER_UI] = present->ui ? texture_record(present->ui) : nil;
			for (layer = 0; layer < HOST_STEREO_HUD_LAYER_COUNT; layer++)
				if (hud_records[layer] && hud_records[layer]->texture)
				{
					use_texture(hud_records[layer]);
					hud_layers[layer] = hud_records[layer]->texture;
				}
			if (zoom && zoom->texture)
				use_texture(zoom);
			commit(YES);
			stereo_pacing_count();
			host_stereo_present(queue, color[0]->texture, color[1]->texture, depth[0]->texture, depth[1]->texture,
				hud_layers, present->hud_group_extent, present->hud_aspect, present->hud_ui, present->reticle,
				present->hud_tangents, zoom ? zoom->texture : nil, present->zoom_tangents, present->near_meters,
				present->far_meters, stereo_cut_brightness(HOST_STEREO_VIEW_FULL, present->cut_covered ||
				present->expanding, present->cut_requested),
				present->vignette, present->ui_dim, present->expanding, present->expansion, present->expansion_bars,
				present->fade, present->cutscene, present->cutscene_forward, present->cutscene_up,
				present->cutscene_tangents, present->cutscene_dim);
			frames++;
			pacing.work_started = CACurrentMediaTime();
			return 0;
		}
		/* HEAD mode with a menu over the film (the main menu's scripted
		scene, the pause menu in a cutscene): the left eye's picture with the
		menu on the UI's quad, as a frame without eyes, not on the screen */
		if (theater_wanted && on_screen && present->hud_ui && color[0] &&
			color[0]->texture && hud && hud->texture && host_stereo_ui_ready())
		{
			id<MTLTexture> picture;

			command_buffer();
			pass_end();
			/* (a foveated eye at its screen shape) */
			picture = foveation_picture(color[0]);
			use_texture(color[0]);
			use_texture(hud);
			commit(YES);
			host_stereo_present_ui(queue, picture, hud->texture,
				stereo_cut_brightness(HOST_STEREO_VIEW_UI, present->cut_covered, 0));
			frames++;
			pacing.work_started = CACurrentMediaTime();
			return 0;
		}
		if (theater_wanted && on_screen && color[0] && color[0]->texture && color[1] &&
			color[1]->texture && host_stereo_ready())
		{
			id<MTLTexture> left, right;

			command_buffer();
			pass_end();
			/* (foveated eyes at their screen shape) */
			left = foveation_picture(color[0]);
			right = foveation_picture(color[1]);
			use_texture(color[0]);
			use_texture(color[1]);
			if (hud && hud->texture)
				use_texture(hud);
			commit(YES);
			stereo_pacing_count();
			host_theater_present_eyes(queue, left, right, hud ? hud->texture : nil,
				present->fade, stereo_cut_brightness(HOST_STEREO_VIEW_SCREEN, present->cut_covered, 0));
			frames++;
			pacing.work_started = CACurrentMediaTime();
			return 0;
		}
		stereo_cut_brightness(HOST_STEREO_VIEW_NONE, 0, 0);
	}
#endif
	/* (the window: eye 0, or a zoomed frame's picture in its place) */
	return gpu_metal_present(present->zoom ? present->zoom : present->eye_color[0]);
}

static uint32_t gpu_metal_call_count_take(void)
{
	return 0;
}

/* ---------- initialization */

static void gpu_metal_initialize(uint32_t flags, struct gpu_capabilities *capabilities)
{
	@autoreleasepool
	{
		NSError *error = nil;
		id<MTLLibrary> utilities;
		MTLRenderPipelineDescriptor *present_descriptor = [MTLRenderPipelineDescriptor new];
		MTLSamplerDescriptor *linear = [MTLSamplerDescriptor new];

		memset(capabilities, 0, sizeof(*capabilities));
		metal_debug = (flags & GPU_INITIALIZE_DEBUG) != 0;
		visibility_wait = (flags & GPU_INITIALIZE_FIXED_TIMESTEP) != 0;
		metalfx_wanted = (flags & GPU_INITIALIZE_METALFX) != 0;
		metal_state_initialize(&state_cache, !(flags & GPU_INITIALIZE_NO_STATE_CACHE));
		specialize_off = (flags & GPU_INITIALIZE_NO_SPECIALIZE) != 0;
		async_compiles = (flags & GPU_INITIALIZE_ASYNC_SHADERS) != 0;
		compile_started = CACurrentMediaTime();
		metal_compile_totals_initialize(&compile_totals);
		built_during_play = [NSMutableData data];
		platform_log(async_compiles ? "Metal: shaders and pipelines compile on worker threads (display.async_shaders); "
			"a draw whose pipeline isn't ready is skipped" : "Metal: shaders and pipelines compile on the game thread at "
			"their first use");
		if (specialize_off)
			platform_log("Metal: vertex shaders are not specialized (debug.metal_specialize)");
#ifndef HAVE_METALFX
		if (metalfx_wanted)
		{
			platform_log("Metal: this build has no MetalFX; the picture is scaled bilinearly");
			GPU_VALIDATION_REPORT(GPU_VALIDATION_UPSCALER_UNAVAILABLE, 1, "Metal: this build has no MetalFX; the picture is "
				"scaled bilinearly");
		}
		metalfx_wanted = NO;
#endif
		pacing_start(flags);
		frame_pacer_start((flags & GPU_INITIALIZE_FIXED_TIMESTEP) != 0);
#if HALO_FRAME_PACING
		frame_load_passes = (long)host_config_real("debug.frame_load_gpu", 0.0);
#endif
		layer = (__bridge CAMetalLayer *)host_sdl_metal_layer();
		device = MTLCreateSystemDefaultDevice();
		if (!layer || !device)
			host_fatal("Metal is unavailable (layer %p, device %p)", (__bridge void *)layer, (__bridge void *)device);
		queue = [device newCommandQueue];
		/* (after the device: the archive is the device's, and named by its GPU) */
		if (!(flags & GPU_INITIALIZE_NO_PIPELINE_ARCHIVE))
			archive_open();
#if TARGET_OS_VISION
		if (flags & GPU_INITIALIZE_IMMERSIVE)
		{
			theater_wanted = YES;
			host_theater_load_settings();
			host_theater_open();
		}
#endif
		/* SDL's view sets the layer's scale and drawableSize (from layoutSubviews) */
		layer.device = device;
		layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
		layer.framebufferOnly = YES;
		textures = [MetalTable new];
		buffers = [MetalTable new];
		shaders = [MetalTable new];
		frame_slots = dispatch_semaphore_create(FRAMES);
		clear_pipelines = [NSMutableDictionary dictionary];
		clear_depth_states = [NSMutableDictionary dictionary];
		utilities = [device newLibraryWithSource:utility_source options:nil error:&error];
		if (!utilities)
			host_fatal("Metal: cannot compile the backend's own shaders: %s", error.description.UTF8String);
		clear_vertex = [utilities newFunctionWithName:@"clear_vertex"];
		clear_fragment = [utilities newFunctionWithName:@"clear_fragment"];
		present_vertex = [utilities newFunctionWithName:@"present_vertex"];
		present_fragment = [utilities newFunctionWithName:@"present_fragment"];
		present_descriptor.vertexFunction = present_vertex;
		present_descriptor.fragmentFunction = present_fragment;
		present_descriptor.colorAttachments[0].pixelFormat = layer.pixelFormat;
		present_pipeline = [device newRenderPipelineStateWithDescriptor:present_descriptor error:&error];
		if (!present_pipeline)
			host_fatal("Metal: cannot create the present pipeline: %s", error.description.UTF8String);
		present_descriptor.fragmentFunction = [utilities newFunctionWithName:@"upscale_fragment"];
		present_descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		upscale_pipeline = [device newRenderPipelineStateWithDescriptor:present_descriptor error:&error];
		if (!upscale_pipeline)
			host_fatal("Metal: cannot create the upscale pipeline: %s", error.description.UTF8String);
		linear.minFilter = MTLSamplerMinMagFilterLinear;
		linear.magFilter = MTLSamplerMinMagFilterLinear;
		present_sampler = [device newSamplerStateWithDescriptor:linear];
		metal_compile_table_initialize(&pipeline_table);
		depth_states = [NSMutableDictionary dictionary];
		samplers = [NSMutableDictionary dictionary];
		scratch_colors = [NSMutableDictionary dictionary];
		test_maps = [NSMutableDictionary dictionary];
		{
			char value[16];

			host_config_string("debug.rate_map_test", "false", value, sizeof(value));
			rate_map_test = !strcmp(value, "true");
		}
		compile_options = [MTLCompileOptions new];
		/* vertex shaders: no fast math. The GLSL is highp, and the compiler
		mustn't reorder its arithmetic; invariance keeps a position computed in
		two passes identical, as GLSL's invariant gl_Position does */
		if (@available(iOS 18.0, tvOS 18.0, visionOS 2.0, *))
			compile_options.mathMode = MTLMathModeSafe;
		else
			compile_options.fastMathEnabled = NO;
		compile_options.preserveInvariance = YES;
		/* pixel shaders: relaxed math, which may reassociate, contract into
		fused multiply-adds and divide by reciprocals, but keeps infinities and
		NaNs. The scene's pixel shaders are bound by ALU work, and under
		exact IEEE arithmetic they took about twice the GPU time of the same
		shaders under ANGLE, which compiles with fast math. No pixel shader
		writes depth, so positions and depth tests are untouched. Before iOS
		18 the only other choice is fast math, which also drops infinities and
		NaNs, so there pixel shaders keep the vertex shaders' options. */
		pixel_compile_options = [compile_options copy];
		/* (relaxed math needs GPU family Apple4, an A11, or a Mac: an A8 or A9
		keeps the vertex shaders' options) */
		if (@available(iOS 18.0, tvOS 18.0, visionOS 2.0, *))
		{
			/* HALO_METAL_PIXEL_MATH in the app's environment, or a file named
			metal-pixel-math in the data folder (as tools make stderr.log), names
			the mode for one run: safe, relaxed or fast. For comparing pictures
			and measuring; a GPU without relaxed math compiles relaxed as safe. */
			BOOL relaxed_available = [device supportsFamily:MTLGPUFamilyApple4] || [device supportsFamily:MTLGPUFamilyMac2];
			char wanted[16] = "";
			const char *math = getenv("HALO_METAL_PIXEL_MATH");
			FILE *file = math ? NULL : fopen("metal-pixel-math", "r");

			if (math)
				snprintf(wanted, sizeof(wanted), "%s", math);
			else if (file)
			{
				if (fscanf(file, "%15s", wanted) != 1)
					wanted[0] = 0;
				fclose(file);
			}
			if (!strcmp(wanted, "fast"))
				pixel_compile_options.mathMode = MTLMathModeFast;
			else if (!strcmp(wanted, "safe"))
				pixel_compile_options.mathMode = MTLMathModeSafe;
			else if (relaxed_available)
				pixel_compile_options.mathMode = MTLMathModeRelaxed;
			/* An A8 (below family Apple3; the Apple TV HD) is bound by its GPU:
			fast math took b30 from 26 to 31 frames a second at a 540 render
			height there, and its frames differ from safe math's by one or two
			levels in under 7% of the pixels (three pixels of 518,400 by more),
			as they do on a Mac. An A9 or A10 keeps safe math until measured. */
			else if (![device supportsFamily:MTLGPUFamilyApple3])
				pixel_compile_options.mathMode = MTLMathModeFast;
			if (wanted[0] || !relaxed_available)
				platform_log("Metal: pixel shaders compile with %s math%s",
					pixel_compile_options.mathMode == MTLMathModeFast ? "fast" :
					pixel_compile_options.mathMode == MTLMathModeRelaxed ? "relaxed" : "safe",
					relaxed_available ? "" : " (this GPU has no relaxed math)");
		}
		{
			static const uint8_t black[4] = { 0, 0, 0, 0xff };
			MTLTextureType types[4] = { MTLTextureType2D, MTLTextureType2D, MTLTextureType3D, MTLTextureTypeCube };
			int type, face;

			for (type = 0; type < 4; type++)
			{
				MTLTextureDescriptor *descriptor = [MTLTextureDescriptor new];

				descriptor.textureType = types[type];
				descriptor.pixelFormat = MTLPixelFormatBGRA8Unorm;
				descriptor.width = descriptor.height = descriptor.depth = 1;
				descriptor.storageMode = MTLStorageModeShared;
				empty_textures[type] = [device newTextureWithDescriptor:descriptor];
				for (face = 0; face < (types[type] == MTLTextureTypeCube ? 6 : 1); face++)
					[empty_textures[type] replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:face
						withBytes:black bytesPerRow:4 bytesPerImage:4];
			}
		}
		empty_sampler = [device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
		empty_buffer = [device newBufferWithLength:64 options:MTLResourceStorageModeShared];
		visibility_buffer = [device newBufferWithLength:VISIBILITY_ENTRIES * sizeof(uint64_t)
			options:MTLResourceStorageModeShared];
		visibility.pending = [NSMutableData data];
		/* the class shows whether Metal's API validation wraps the device
		(MTLDebugDevice; tools/mac_run.py run --metal-validation) */
		platform_log("Metal on %s (%s)", device.name.UTF8String, NSStringFromClass([device class]).UTF8String);
		if (flags & GPU_INITIALIZE_COMPRESSED_TEXTURES)
		{
			if (@available(iOS 16.4, tvOS 16.4, visionOS 1.0, *))
				compressed_textures = device.supportsBCTextureCompression;
			platform_log("Metal: compressed textures %s", compressed_textures ? "upload as BC1-3" :
				"are decoded (this GPU has no BC formats)");
		}

		/* the vertex shaders fetch their own attributes (nv2a_msl.c's
		fetch_attribute), BGRA8 as readily as RGBA8, so the front end needn't
		swizzle colored streams as it uploads them and can draw them from the
		mirror; debug.metal_vertex_bgra = false reports what GL does */
		capabilities->vertex_bgra = (flags & GPU_INITIALIZE_NO_VERTEX_BGRA) ? 0 : 1;
		/* indexed draws take the packet's base vertex (gpu_metal_draw), so the
		front end needn't rebase a draw's indices and can draw them from the
		mirror, on a GPU that has base vertex drawing: family Apple3 (an A9)
		and later, and every Mac. An A8 (the Apple TV HD, family Apple2)
		doesn't, and nor does a simulator that reports no such family;
		there, and with debug.metal_base_vertex = false, this reports what
		GL does. (vertex_bgra needs no such test: it is the shaders' own
		fetch, not a vertex descriptor's format.) */
		base_vertex_draws = !(flags & GPU_INITIALIZE_NO_BASE_VERTEX) &&
			([device supportsFamily:MTLGPUFamilyApple3] || [device supportsFamily:MTLGPUFamilyMac2]);
		if (!base_vertex_draws && !(flags & GPU_INITIALIZE_NO_BASE_VERTEX))
			platform_log("Metal: this GPU has no base vertex drawing (below family Apple3); indices are rebased per draw");
		capabilities->base_vertex = base_vertex_draws ? 1 : 0;
		/* the rest is what GL reports on Apple's OpenGL ES 3.0 (gpu_gl.c's
		gpu_gl_initialize), so the front end behaves as it does under GL */
		capabilities->triangle_fans = 1;
		capabilities->line_loops = 1;
		capabilities->sampler_lod_bias = 0;
#if HALO_FRAME_PACING && !TARGET_OS_SIMULATOR
		system_frames_start();
#endif
		visibility_counts = [device supportsFamily:MTLGPUFamilyApple3] || [device supportsFamily:MTLGPUFamilyMac2];
		{
			/* debug.visibility_any_sample: the yes-or-no answer everywhere, to compare */
			char value[8];

			host_config_string("debug.visibility_any_sample", "false", value, sizeof(value));
			if (!strcmp(value, "true"))
				visibility_counts = NO;
		}
		capabilities->occlusion_mode = visibility_counts ? GPU_OCCLUSION_EXACT : GPU_OCCLUSION_ANY_SAMPLE;
		capabilities->s3tc = compressed_textures ? 1 : 0;
		/* border colors stay inside the backend: the front end doesn't read
		border_clamp, and a GL run and a Metal run report the same line */
		border_colors = [device supportsFamily:MTLGPUFamilyApple7] || [device supportsFamily:MTLGPUFamilyMac2];
		capabilities->border_clamp = 0;
		/* 8192 on an A8 (the Apple TV HD), the one Apple GPU family below Apple3 that still runs */
		capabilities->max_texture_size = [device supportsFamily:MTLGPUFamilyApple3] ||
			[device supportsFamily:MTLGPUFamilyMac2] ? 16384 : 8192;
		capabilities->shader_language = GPU_SHADER_LANGUAGE_MSL;
		capabilities->shader_es = 0;
		capabilities->clip_y_flip = 0;
#if !TARGET_OS_VISION
		/* (not on visionOS: the theater screen, the UI's quad and the
		Compositor's presenters place and scale the HUD themselves) */
		capabilities->upscale = 1;
		capabilities->upscale_anti_alias = 1;
#endif
		capabilities->clip_z_remap = 0;
		GPU_CAPABILITIES_LOG(platform_log, capabilities);
	}
}

#define GPU_METAL_FUNCTION(type, name, parameters, arguments) .name = gpu_metal_##name,
#define GPU_METAL_PROCEDURE(name, parameters, arguments) .name = gpu_metal_##name,
const struct gpu_backend gpu_backend_metal = { GPU_FUNCTIONS(GPU_METAL_FUNCTION, GPU_METAL_PROCEDURE) };
