/* Head-tracked stereo (display.stereo = "head", visionOS 26 and later): the
Compositor's views drive the game's eyes, and the head drives its look.

At the game's frame begin, host_stereo_frame opens the Compositor's frame
(theater mode's, host_theater.m) and reads each view: its offset from the
device in meters, which becomes world units (one is 3.048 m), and its
frustum's tangents, recovered from the projection. The head's yaw and pitch
since the last frame turn the player's look (port/linux/game/stereo.c); its
roll only tilts the eye cameras. The game renders each eye, then
gpu_present_stereo (gpu_metal.m) hands the pictures here: each fills its view,
with the game's depth (already reverse-Z, gpu_metal.m's reversed_depth) for
the Compositor's reprojection, and the HUD's pieces float in front of it
(host_stereo_hud.h): the reticle where the crosshair points, the rest in
bands that turn with the head's yaw only, level with the room. A frame
without eyes (the main menu, a load) shows its picture on the UI's quad, the
same way.

Stereo on the theater screen (display.stereo = "screen") reads the same frame
for the eyes' positions only: the look stays on the stick, and screen_eyes
reports where the viewer's eyes are against the screen, from which the guest
maps the game as a 3D TV (stereo.c). host_theater_present_eyes puts each
eye's picture on the screen for its view.

Elsewhere host_stereo_frame leaves the frame mono. */
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include "host_stereo.h"
#include "host_config.h"
#include "host.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#if TARGET_OS_VISION
#import <CompositorServices/CompositorServices.h>
#import <Metal/Metal.h>
#include <os/proc.h>
#include <simd/simd.h>
#include <stdatomic.h>
#include "host_theater.h"
#include "host_stereo_head.h"
#include "host_stereo_vignette.h"
#include "host_stereo_hud.h"
#include "halo_stereo_window.h"
#include "halo_stereo_cutscene.h"
#include "halo_stereo_veil.h"

/* one world unit in meters */
#define METERS_PER_UNIT 3.048f

/* stereo on the screen: the nearest an eye may be to the screen's plane,
and how far toward its edges an eye's offset may go, as a share of the
screen's half width or height, so the frusta stay valid when the screen is
seen nearly edge-on (where the picture then stops following the eye) */
#define SCREEN_MINIMUM_DISTANCE 0.25f
#define SCREEN_EDGE_SHARE 0.9f

/* the eyes' picture size, while the head drives the view */
static int picture_width, picture_height;
/* foveated eye passes this frame (foveated_eyes): each eye's rate map and
its parameter data for the shaders, and the size the eyes' targets are
allocated at; nil and 0 otherwise */
static id<MTLRasterizationRateMap> eye_rate_maps[2];
static id<MTLBuffer> eye_rate_map_parameters[2];
static int foveated_allocated_width, foveated_allocated_height;
/* what foveated_eyes last logged, to log again only on a change */
static int foveated_eyes_logged = -1;
static unsigned long foveated_eyes_key;
/* the memory left to the app (os_proc_available_memory) is logged at the
first foveated frame since the space opened, and at the first stereo frame
after a level loads: a load stalls the game's loop, so that is the first
frame after MEMORY_LOAD_GAP_SECONDS without one (and the first since the
space opened). Both come before the game has allocated its eyes' color and
depth targets at the quality (it does that on the first foveated frame, after
this returns to it) and before the level's textures have streamed in, so a
settled reading follows at the MEMORY_SETTLED_FOVEATED_FRAMES'th foveated
frame since the space opened. The render quality is chosen against the least
of them (the stereo spec's "Foveation and render quality": under 500 MB, the
next lower quality) */
#define MEMORY_LOAD_GAP_SECONDS 1.0
#define MEMORY_SETTLED_FOVEATED_FRAMES 900
static int memory_foveated_logged;
static unsigned long memory_foveated_frames;
static NSTimeInterval memory_last_frame_at;
/* the head's last pose, while ARKit places it */
static struct host_stereo_head head;
static atomic_int world_recenter_active, world_recenter_pending;
static float head_last_back[3], head_last_position[3];
static unsigned long stereo_frames;
/* frames on the screen, presents, and whether the depth range warning was
logged, since the space opened: the once-only logs repeat for each opening
(host_stereo_space_opened) */
static unsigned long screen_frames, stereo_presents, ui_presents;
/* the last frame's HUD went whole on the UI's quad: -1 none yet since the
space opened */
static int ui_shown = -1;
/* the last frame showed the zoomed picture */
static int zoom_logged;
static int depth_reported;
/* the Compositor's frame is open for this game frame (host_stereo_frame)
with display.stereo = "head", whatever the frame is (the full view, the film
on the screen, or no eyes) */
static int head_frame_open;

/* display.stereo = "head", read once */
static int head_configured(void)
{
	static int configured = -1;

	if (configured < 0)
	{
		char value[16];

		host_config_string("display.stereo", "off", value, sizeof(value));
		configured = !strcmp(value, "head");
	}
	return configured;
}

static id<MTLRenderPipelineState> eye_pipeline, eye_foveated_pipeline, hud_pipeline;
/* the immersive cutscene's blur (Task 12k): its pipelines, and each eye's
two quarter-size targets (the blur ends in the first) */
static id<MTLRenderPipelineState> blur_down_pipeline, blur_down_foveated_pipeline, blur_pipeline;
static id<MTLTexture> blur_targets[2][2];
#define BLUR_FORMAT MTLPixelFormatRGBA16Float
static MTLPixelFormat pipeline_color, pipeline_depth;
static id<MTLDepthStencilState> depth_always;
/* the HUD's depth reduction (hud_depth_encode); nil: the pieces rest */
static id<MTLComputePipelineState> hud_depth_pipeline;
static id<MTLSamplerState> linear_sampler, nearest_sampler;

static NSString *const shader_source =
	@"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct picture_vertex { float4 position [[position]]; float2 coordinate; };\n"
	/* the eye's picture fills its view: its frustum is the view's */
	"vertex picture_vertex eye_vertex(uint index [[vertex_id]])\n"
	"{\n"
	"	float2 corner = float2(index & 1, index >> 1);\n"
	"	picture_vertex out;\n"
	"	out.position = float4(corner.x * 2 - 1, 1 - corner.y * 2, 0, 1);\n"
	"	out.coordinate = corner;\n"
	"	return out;\n"
	"}\n"
	"struct eye_pixel { float4 color [[color(0)]]; float depth [[depth(any)]]; };\n"
	/* the game's colors are display-encoded; an sRGB target encodes what it's
	given. Its depth is reverse-Z already, for its near and far planes, which
	the drawable's depth range is set to; scaled when the near plane had to
	move out to the Compositor's (stereo_depth_range). Never 0: the Compositor
	takes depth 0 for nothing there (black, or the room), and the sky, the
	clear and what only transparent effects drew all reach it as 0, so they
	take the depth of something half way to the far plane. The comfort
	vignette (input.comfort_vignette, host_stereo_vignette.h) darkens the
	color only, toward black away from straight ahead, alike in both eyes,
	and never its depth */
	HOST_STEREO_VIGNETTE_STRING(HOST_STEREO_VIGNETTE_SOURCE) "\n"
	"struct eye_uniforms { uint decode_srgb; float depth_scale; float depth_floor; float brightness; float vignette;\n"
	"	float4 tangents; float vignette_inner; float vignette_outer; uint expanding; float bars; float4 window;\n"
	"	float4 tint; float tint_depth; float4x4 screen_from_view; uint cutscene; float4 frame;\n"
	"	float4x4 frame_from_view; float4 look; };\n"
	/* the eyes' veil (halo_stereo_veil.h): the picture toward its blur by
	share, then desaturated and darkened by dim, for whatever veils it */
	HALO_STEREO_VEIL_STRING(HALO_STEREO_VEIL_SOURCE) "\n"
	"static float3 eye_veil(float3 color, float3 blur, float share, float dim)\n"
	"{\n"
	"	float sharp_luma = halo_stereo_veil_luma(color.r, color.g, color.b);\n"
	"	float blurred_luma = halo_stereo_veil_luma(blur.r, blur.g, blur.b);\n"
	"	return float3(halo_stereo_veil(color.r, blur.r, sharp_luma, blurred_luma, share, dim),\n"
	"		halo_stereo_veil(color.g, blur.g, sharp_luma, blurred_luma, share, dim),\n"
	"		halo_stereo_veil(color.b, blur.b, sharp_luma, blurred_luma, share, dim));\n"
	"}\n"
	/* Task 12k's immersive cutscene (halo_stereo_cutscene.h): 0 inside the
	director's frame to 1 outside it, at the view's coordinate */
	HALO_STEREO_CUTSCENE_STRING(HALO_STEREO_CUTSCENE_SOURCE) "\n"
	"static float eye_outside_frame(float2 coordinate, constant eye_uniforms &u)\n"
	"{\n"
	"	if (!u.cutscene)\n"
	"		return 0.0;\n"
	"	float3 view = float3(mix(-u.tangents.x, u.tangents.y, coordinate.x), mix(u.tangents.z, -u.tangents.w,\n"
	"		coordinate.y), -1.0);\n"
	"	float3 f = (u.frame_from_view * float4(view, 0.0)).xyz;\n"
	"	return halo_stereo_cutscene_outside(f.x, f.y, f.z, u.frame.x, u.frame.y, u.frame.z);\n"
	"}\n"
	/* the cutscene window's expansion (halo_stereo_window.h): whether the
	view's coordinate is outside the growing window (0), inside it (1) or on
	its bars (2); always inside while it isn't expanding */
	HALO_STEREO_WINDOW_STRING(HALO_STEREO_WINDOW_SOURCE) "\n"
	"static int eye_window(float2 coordinate, constant eye_uniforms &u)\n"
	"{\n"
	"	if (!u.expanding)\n"
	"		return 1;\n"
	"	float3 view = float3(mix(-u.tangents.x, u.tangents.y, coordinate.x), mix(u.tangents.z, -u.tangents.w,\n"
	"		coordinate.y), -1.0);\n"
	"	float3 d = (u.screen_from_view * float4(view, 0.0)).xyz;\n"
	"	return halo_stereo_window_test(d.x, d.y, d.z, u.window.x, u.window.y, u.window.z, u.window.w, u.bars);\n"
	"}\n"
	/* outside the window: the theater's surroundings, as around the film's
	screen: the script fade's tint if any, else nothing (the clear: the dark,
	or the room) */
	"static eye_pixel eye_outside(constant eye_uniforms &u)\n"
	"{\n"
	"	eye_pixel out;\n"
	"	out.color = u.tint;\n"
	"	out.depth = u.tint_depth;\n"
	"	return out;\n"
	"}\n"
	/* the picture and depth at at (normalized), the vignette at the view's
	own coordinate; black on the expanding window's bars */
	"static eye_pixel eye_shade(float2 coordinate, float2 at, texture2d<float> picture, depth2d<float> depth,\n"
	"	texture2d<float> blurred, sampler linear, sampler nearest, constant eye_uniforms &u, int shown)\n"
	"{\n"
	"	float3 color = shown == 2 ? float3(0.0) : picture.sample(linear, at).rgb;\n"
	/* outside the immersive cutscene's frame: the blurred picture (laid out
	as the view, unfoveated), desaturated and darkened by the dim, as
	halo_stereo_cutscene_dimmed */
	"	float outside = eye_outside_frame(coordinate, u);\n"
	"	if (outside > 0.0)\n"
	"	{\n"
	"		float3 blur = blurred.sample(linear, coordinate).rgb;\n"
	"		float luma = dot(blur, float3(0.299, 0.587, 0.114));\n"
	"		float keep = 1.0 - u.frame.w;\n"
	"		color = mix(color, (luma + (blur - luma) * keep) * keep, outside);\n"
	"	}\n"
	/* a seat's look fade (display.seat_look_fade): the whole view veiled,
	look.x the dim and look.y the blur's share (0 where no blur is read) */
	"	if (u.look.x > 0.0 || u.look.y > 0.0)\n"
	"		color = eye_veil(color, u.look.y > 0.0 ? blurred.sample(linear, coordinate).rgb : color, u.look.y,\n"
	"			u.look.x);\n"
	"	if (u.decode_srgb)\n"
	"		color = select(pow((color + 0.055) / 1.055, 2.4), color / 12.92, color <= 0.04045);\n"
	"	float edge = u.vignette > 0.0 ? host_stereo_vignette_edge(coordinate.x, coordinate.y, u.tangents.x,\n"
	"		u.tangents.y, u.tangents.z, u.tangents.w, u.vignette_inner, u.vignette_outer) : 0.0;\n"
	"	eye_pixel out;\n"
	"	out.color = float4(color * u.brightness * (1.0 - u.vignette * edge), 1);\n"
	"	out.depth = clamp(depth.sample(nearest, at) * u.depth_scale, u.depth_floor, 1.0);\n"
	"	return out;\n"
	"}\n"
	"fragment eye_pixel eye_fragment(picture_vertex in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"	depth2d<float> depth [[texture(1)]], texture2d<float> blurred [[texture(2)]], sampler linear [[sampler(0)]],\n"
	"	sampler nearest [[sampler(1)]], constant eye_uniforms &u [[buffer(0)]])\n"
	"{\n"
	"	int shown = eye_window(in.coordinate, u);\n"
	"	if (shown == 0 && u.tint.w <= 0.0)\n"
	"		discard_fragment();\n"
	"	if (shown == 0)\n"
	"		return eye_outside(u);\n"
	"	return eye_shade(in.coordinate, in.coordinate, picture, depth, blurred, linear, nearest, u, shown);\n"
	"}\n"
	/* foveated eye passes (foveated_eyes): the game's picture and depth were
	rendered through the eye's rate map, so a screen point (the normalized
	coordinate times the screen size, inside it) is read where the map put
	it, kept half a texel inside the map's physical region: the targets are
	allocated larger, and what lies past that region is stale */
	"struct eye_foveation { float2 screen; float2 physical; float2 allocated; };\n"
	"fragment eye_pixel eye_foveated_fragment(picture_vertex in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"	depth2d<float> depth [[texture(1)]], texture2d<float> blurred [[texture(2)]], sampler linear [[sampler(0)]],\n"
	"	sampler nearest [[sampler(1)]], constant eye_uniforms &u [[buffer(0)]],\n"
	"	constant rasterization_rate_map_data &map [[buffer(1)]], constant eye_foveation &f [[buffer(2)]])\n"
	"{\n"
	"	int shown = eye_window(in.coordinate, u);\n"
	"	if (shown == 0 && u.tint.w <= 0.0)\n"
	"		discard_fragment();\n"
	"	if (shown == 0)\n"
	"		return eye_outside(u);\n"
	"	rasterization_rate_map_decoder decoder(map);\n"
	"	float2 screen = clamp(in.coordinate * f.screen, float2(0.0), f.screen - 1.0 / 256.0);\n"
	"	float2 physical = clamp(decoder.map_screen_to_physical_coordinates(screen), float2(0.5), f.physical - 0.5);\n"
	"	return eye_shade(in.coordinate, physical / f.allocated, picture, depth, blurred, linear, nearest, u, shown);\n"
	"}\n"
	/* the immersive cutscene's blur (Task 12k): the eye's picture at a
	quarter of its size each axis, laid out as the view (a foveated eye read
	through its rate map, as eye_foveated_fragment), each texel the mean of
	the 4x4 under it from four bilinear reads; then a separable Gaussian
	across and down (halo_stereo_cutscene.h's taps and sigma) */
	"fragment float4 blur_down_fragment(picture_vertex in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"	sampler linear [[sampler(0)]])\n"
	"{\n"
	"	float2 texel = 1.0 / float2(picture.get_width(), picture.get_height());\n"
	"	return 0.25 * (picture.sample(linear, in.coordinate + texel * float2(-1, -1)) +\n"
	"		picture.sample(linear, in.coordinate + texel * float2(1, -1)) +\n"
	"		picture.sample(linear, in.coordinate + texel * float2(-1, 1)) +\n"
	"		picture.sample(linear, in.coordinate + texel * float2(1, 1)));\n"
	"}\n"
	"fragment float4 blur_down_foveated_fragment(picture_vertex in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"	sampler linear [[sampler(0)]], constant rasterization_rate_map_data &map [[buffer(1)]],\n"
	"	constant eye_foveation &f [[buffer(2)]])\n"
	"{\n"
	"	rasterization_rate_map_decoder decoder(map);\n"
	"	float4 sum = float4(0.0);\n"
	"	for (int i = 0; i < 4; i++)\n"
	"	{\n"
	"		float2 offset = float2((i & 1) ? 1.0 : -1.0, (i & 2) ? 1.0 : -1.0);\n"
	"		float2 screen = clamp(in.coordinate * f.screen + offset, float2(0.0), f.screen - 1.0 / 256.0);\n"
	"		float2 physical = clamp(decoder.map_screen_to_physical_coordinates(screen), float2(0.5), f.physical - 0.5);\n"
	"		sum += picture.sample(linear, physical / f.allocated);\n"
	"	}\n"
	"	return 0.25 * sum;\n"
	"}\n"
	"struct blur_uniforms { float2 step; int taps; float sigma; };\n"
	"fragment float4 blur_fragment(picture_vertex in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"	sampler linear [[sampler(0)]], constant blur_uniforms &u [[buffer(0)]])\n"
	"{\n"
	"	float4 sum = float4(0.0);\n"
	"	float total = 0.0;\n"
	"	for (int tap = -u.taps; tap <= u.taps; tap++)\n"
	"	{\n"
	"		float weight = exp(-float(tap * tap) / (2.0 * u.sigma * u.sigma));\n"
	"		sum += weight * picture.sample(linear, in.coordinate + float(tap) * u.step);\n"
	"		total += weight;\n"
	"	}\n"
	"	return sum / total;\n"
	"}\n"
	/* one of the HUD's quads (host_stereo_hud.h): its texture's rectangle,
	and its center and half extents in its frame, which clip_from_frame
	takes to the view */
	"struct hud_uniforms { float4x4 clip_from_frame; float4 source; float4 center; float4 x_axis; float4 y_axis;\n"
	"	uint decode_srgb; float brightness; uint opaque; float opacity; float4 solid; uint use_solid; };\n"
	"vertex picture_vertex hud_vertex(uint index [[vertex_id]], constant hud_uniforms &u [[buffer(0)]])\n"
	"{\n"
	"	float2 corner = float2(index & 1, index >> 1);\n"
	"	picture_vertex out;\n"
	"	out.position = u.clip_from_frame * float4(u.center.xyz + (corner.x * 2 - 1) * u.x_axis.xyz +\n"
	"		(1 - corner.y * 2) * u.y_axis.xyz, 1);\n"
	"	out.coordinate = mix(u.source.xy, u.source.zw, corner);\n"
	"	return out;\n"
	"}\n"
	/* The HUD layer holds premultiplied color and, in alpha, how much of the
	picture still shows (d3d8_device.c, hud_layer_blend): 1 where nothing
	drew. Over the world it's the color plus the world times that, which the
	blend makes of one minus it. Where nothing drew the quad leaves the world
	and its depth; elsewhere its depth is the quad's, so the Compositor
	reprojects it as the quad it is. A mono picture on the UI's quad is
	opaque (its alpha is the game's scratch) */
	"fragment float4 hud_fragment(picture_vertex in [[stage_in]], texture2d<float> hud [[texture(0)]],\n"
	"	sampler linear [[sampler(0)]], constant hud_uniforms &u [[buffer(0)]])\n"
	"{\n"
	/* only inside the quad's rectangle: linear filtering at its edge would
	take a sliver of what's beside it */
	"	float2 half_texel = 0.5 / float2(hud.get_width(), hud.get_height());\n"
	"	float4 color = u.use_solid ? float4(u.solid.rgb, 0.0) :\n"
	"		hud.sample(linear, clamp(in.coordinate, u.source.xy + half_texel, u.source.zw - half_texel));\n"
	"	float covered = u.opaque ? 1.0 : 1.0 - color.a;\n"
	"	if (max(covered, max(color.r, max(color.g, color.b))) < 1.0 / 255.0)\n"
	"		discard_fragment();\n"
	"	if (u.decode_srgb)\n"
	"		color.rgb = select(pow((color.rgb + 0.055) / 1.055, 2.4), color.rgb / 12.92, color.rgb <= 0.04045);\n"
	"	return float4(color.rgb * u.brightness * u.opacity, covered * u.opacity);\n"
	"}\n"
	/* the HUD's depth (host_stereo_hud_depth_ease): over each of a view's
	pieces' rectangles of the eye's depth (physical texels, x0 y0 x1 y1, the
	end excluded), the largest depth (reverse-Z: the nearest) of the texels
	whose stencil has neither the weapon's value 1 nor the first-person
	body's value 4, and how many were skipped. HUD_DEPTH_TILES threadgroups
	share a rectangle, each thread striding it; a simd reduction, the threadgroup's, then
	one atomic each into the piece's result (a depth's bits order as the
	depth, which is never negative). The results start at 0: an empty
	rectangle gives the far plane */
	"struct hud_depth_result { atomic_uint depth; atomic_uint skipped; };\n"
	"kernel void hud_depth_reduce(depth2d<float, access::read> depth [[texture(0)]],\n"
	"	texture2d<uint, access::read> stencil [[texture(1)]], constant uint4 *rectangles [[buffer(0)]],\n"
	"	constant uint &use_stencil [[buffer(1)]], device hud_depth_result *results [[buffer(2)]],\n"
	"	uint2 group [[threadgroup_position_in_grid]], uint2 groups [[threadgroups_per_grid]],\n"
	"	uint item [[thread_index_in_threadgroup]], uint2 size [[threads_per_threadgroup]],\n"
	"	uint lane [[thread_index_in_simdgroup]], uint simd_group [[simdgroup_index_in_threadgroup]],\n"
	"	uint simds [[simdgroups_per_threadgroup]])\n"
	"{\n"
	"	threadgroup float depths[32];\n"
	"	threadgroup uint skips[32];\n"
	"	uint4 r = rectangles[group.y];\n"
	"	uint width = r.z > r.x ? r.z - r.x : 0, count = width * (r.w > r.y ? r.w - r.y : 0);\n"
	"	uint threads = size.x;\n"
"	float nearest = 0.0;\n"
	"	uint skipped = 0;\n"
	"	for (uint i = group.x * threads + item; i < count; i += groups.x * threads)\n"
	"	{\n"
	"		uint2 at = uint2(r.x + i % width, r.y + i / width);\n"
	"		if (use_stencil != 0 && (stencil.read(at).r & 5u) != 0)\n"
	"			skipped++;\n"
	"		else\n"
	"			nearest = max(nearest, depth.read(at));\n"
	"	}\n"
	"	nearest = simd_max(nearest);\n"
	"	skipped = simd_sum(skipped);\n"
	"	if (lane == 0)\n"
	"	{\n"
	"		depths[simd_group] = nearest;\n"
	"		skips[simd_group] = skipped;\n"
	"	}\n"
	"	threadgroup_barrier(mem_flags::mem_threadgroup);\n"
	"	if (item == 0 && count > 0)\n"
	"	{\n"
	"		for (uint s = 1; s < simds; s++)\n"
	"		{\n"
	"			nearest = max(nearest, depths[s]);\n"
	"			skipped += skips[s];\n"
	"		}\n"
	"		atomic_fetch_max_explicit(&results[group.y].depth, as_type<uint>(nearest), memory_order_relaxed);\n"
	"		atomic_fetch_add_explicit(&results[group.y].skipped, skipped, memory_order_relaxed);\n"
	"	}\n"
	"}\n";

/* the eye depth's floor when the game's planes aren't usable: the
Compositor's default range is about 0.1 m to infinity, where this is about
1 km away */
#define STEREO_DEPTH_FLOOR_DEFAULT 0.0001f

#if TARGET_OS_VISION
/* a view's frustum tangents (left, right, up, down, all positive) from its
projection: an off-axis projection's x scale is 2 / (left + right) and its
x offset (right - left) / (left + right); likewise y */
static simd_float4 view_tangents(cp_drawable_t drawable, size_t view_index) API_AVAILABLE(visionos(26.0))
{
	simd_float4x4 projection = cp_drawable_compute_projection(drawable,
		cp_axis_direction_convention_right_up_back, view_index);

	return (simd_float4){
		(1.0f - projection.columns[2].x) / projection.columns[0].x,
		(1.0f + projection.columns[2].x) / projection.columns[0].x,
		(1.0f + projection.columns[2].y) / projection.columns[1].y,
		(1.0f - projection.columns[2].y) / projection.columns[1].y,
	};
}

/* Foveation's measurement (display.foveation): the Compositor's rate maps,
logged once per opening and whenever they change, since Apple publishes no
screen size, physical size or density for any quality. Per map: its
layers, screen size, granularity and each layer's physical size; per view:
its map and layer, the logical viewport, the drawable's texture size, its
tangents and the density at its center (the screen's pixels per degree
there, times the map's physical-to-screen ratio). Then, per map and layer,
five fixed screen points mapped to physical ones (whether a layered map's
layers are identical), and per view the rate along its center row and
column, sampled every FOVEATION_PROFILE_STEP pixels, with how far from the
center, in degrees, the rate stays at least FOVEATION_SHARP_RATE */
#define FOVEATION_PROFILE_STEP 64.0f
#define FOVEATION_SHARP_RATE 0.9f
/* while the runtime quality eases toward the configured one, a short line
at most this often, in frames; after the limit (a quality the layer never
quite reaches), the full log regardless */
#define FOVEATION_EASING_FRAMES 45
#define FOVEATION_EASING_LIMIT 450
/* past the limit, a changed map is logged in full at most this often, in
seconds */
#define FOVEATION_RELOG_SECONDS 1.0

static NSString *foveation_logged_key;
static unsigned long foveation_frames, foveation_easing_logged;
static NSTimeInterval foveation_logged_at;

/* a screen point through a map's layer */
static MTLCoordinate2D foveation_physical(id<MTLRasterizationRateMap> map, NSUInteger layer, float x, float y)
{
	return [map mapScreenToPhysicalCoordinates:MTLCoordinate2DMake(x, y) forLayer:layer];
}

static float degrees_from_tangent(float tangent)
{
	return atanf(fabsf(tangent)) * 180.0f / (float)M_PI;
}

/* the rates along a line of a view, from start to end (screen pixels along
one axis, the other fixed), sampled every FOVEATION_PROFILE_STEP pixels:
each sample's rate (physical pixels per screen pixel) into rates, with the
sample's start position; returns the count */
static int foveation_profile(id<MTLRasterizationRateMap> map, NSUInteger layer, BOOL along_x, float fixed,
	float start, float end, float *rates, float *positions, int capacity)
{
	int count = 0;
	float position = start;

	while (position < end - 0.5f && count < capacity)
	{
		float next = fminf(position + FOVEATION_PROFILE_STEP, end);
		MTLCoordinate2D a = along_x ? foveation_physical(map, layer, position, fixed) :
			foveation_physical(map, layer, fixed, position);
		MTLCoordinate2D b = along_x ? foveation_physical(map, layer, next, fixed) :
			foveation_physical(map, layer, fixed, next);

		rates[count] = ((along_x ? b.x - a.x : b.y - a.y)) / (next - position);
		positions[count] = position;
		count++;
		position = next;
	}
	return count;
}

/* from the sample holding center outward (direction -1 or 1), the farthest
screen position at which the rate is still at least the threshold; center
when the center's own sample is below it */
static float foveation_sharp_edge(const float *rates, const float *positions, int count, float end, float center,
	int direction, float threshold)
{
	int index = 0;
	float edge = center;

	while (index + 1 < count && positions[index + 1] <= center)
		index++;
	for (; index >= 0 && index < count && rates[index] >= threshold; index += direction)
		edge = direction > 0 ? (index + 1 < count ? positions[index + 1] : end) : positions[index];
	return edge;
}

static void foveation_measure(cp_drawable_t drawable) API_AVAILABLE(visionos(26.0))
{
	float quality, runtime, default_quality;
	const char *layout, *offered;
	size_t map_count = cp_drawable_get_rasterization_rate_map_count(drawable);
	size_t views = cp_drawable_get_view_count(drawable);
	NSMutableString *key, *line;

	if (!host_theater_foveation_state(&quality, &runtime, &default_quality, &layout, &offered))
		return;
	foveation_frames++;
	/* easing toward the configured quality: a short line now and then */
	if (fabsf(runtime - quality) > 0.005f && foveation_frames < FOVEATION_EASING_LIMIT)
	{
		if (foveation_easing_logged == 0 || foveation_frames - foveation_easing_logged >= FOVEATION_EASING_FRAMES)
		{
			/* each map's screen size beside its physical one: whether the
			screen size follows the eased quality too */
			NSMutableString *text = [NSMutableString stringWithFormat:@"stereo: foveation easing: runtime quality "
				"%.3f toward %.3f", runtime, quality];

			for (size_t index = 0; index < map_count; index++)
			{
				id<MTLRasterizationRateMap> map = cp_drawable_get_rasterization_rate_map(drawable, index);
				MTLSize screen = map.screenSize, physical = [map physicalSizeForLayer:0];

				[text appendFormat:@"; map %zu: screen %lux%lu, physical layer 0 %lux%lu", index,
					(unsigned long)screen.width, (unsigned long)screen.height, (unsigned long)physical.width,
					(unsigned long)physical.height];
			}
			host_logf(HOST_LOG_INFO, "%s", text.UTF8String);
			foveation_easing_logged = foveation_frames;
		}
		return;
	}
	/* the key leaves out the runtime quality, which can jitter across a
	rounding boundary; the sizes it sets are in the key */
	key = [NSMutableString stringWithFormat:@"%.2f %zu %zu", quality, map_count, views];
	line = [NSMutableString stringWithFormat:@"stereo: foveation at quality %.3f (runtime %.3f, default %.3f), "
		"layout %s (offered %s): %zu map%s for %zu view%s", quality, runtime, default_quality, layout, offered, map_count,
		map_count == 1 ? "" : "s", views, views == 1 ? "" : "s"];
	for (size_t index = 0; index < map_count; index++)
	{
		id<MTLRasterizationRateMap> map = cp_drawable_get_rasterization_rate_map(drawable, index);
		MTLSize screen = map.screenSize, granularity = map.physicalGranularity;

		[line appendFormat:@"; map %zu: %lu layer%s, screen %lux%lu, granularity %lux%lu", index,
			(unsigned long)map.layerCount, map.layerCount == 1 ? "" : "s", (unsigned long)screen.width,
			(unsigned long)screen.height, (unsigned long)granularity.width, (unsigned long)granularity.height];
		[key appendFormat:@" %lux%lu", (unsigned long)screen.width, (unsigned long)screen.height];
		for (NSUInteger layer = 0; layer < map.layerCount; layer++)
		{
			MTLSize physical = [map physicalSizeForLayer:layer];

			[line appendFormat:@", physical layer %lu %lux%lu", (unsigned long)layer, (unsigned long)physical.width,
				(unsigned long)physical.height];
			[key appendFormat:@" %lux%lu", (unsigned long)physical.width, (unsigned long)physical.height];
		}
	}
	for (size_t view_index = 0; view_index < views; view_index++)
	{
		cp_view_texture_map_t texture_map = cp_view_get_view_texture_map(cp_drawable_get_view(drawable, view_index));
		size_t texture = cp_view_texture_map_get_texture_index(texture_map);
		MTLViewport viewport = cp_view_texture_map_get_viewport(texture_map);
		id<MTLTexture> color = cp_drawable_get_color_texture(drawable, texture);
		id<MTLRasterizationRateMap> map = host_theater_view_rate_map(drawable, texture_map);
		size_t slice = cp_view_texture_map_get_slice_index(texture_map);
		NSUInteger layer = map && slice < map.layerCount ? slice : 0;
		simd_float4 t = view_tangents(drawable, view_index);
		float center_x = (float)viewport.originX + (float)viewport.width * t.x / (t.x + t.y);
		float center_y = (float)viewport.originY + (float)viewport.height * t.z / (t.z + t.w);
		float ratio_x = 1.0f, ratio_y = 1.0f;

		if (map)
		{
			MTLCoordinate2D a = foveation_physical(map, layer, center_x - 4.0f, center_y);
			MTLCoordinate2D b = foveation_physical(map, layer, center_x + 4.0f, center_y);
			MTLCoordinate2D c = foveation_physical(map, layer, center_x, center_y - 4.0f);
			MTLCoordinate2D d = foveation_physical(map, layer, center_x, center_y + 4.0f);

			ratio_x = (b.x - a.x) / 8.0f;
			ratio_y = (d.y - c.y) / 8.0f;
		}
		[line appendFormat:@"; view %zu (map %zu layer %lu, texture %zu slice %zu): viewport (%.0f %.0f %.0f %.0f), "
			"drawable texture %lux%lu, view tangents %.3f %.3f %.3f %.3f, center %.1f px/degree (rate %.3f by %.3f; "
			"%.1f px/degree across the view)", view_index,
			map ? (texture < map_count ? texture : 0) : 0, (unsigned long)layer, texture, slice, viewport.originX,
			viewport.originY, viewport.width, viewport.height, (unsigned long)color.width, (unsigned long)color.height,
			t.x, t.y, t.z, t.w, (float)viewport.width / (t.x + t.y) * (float)M_PI / 180.0f * ratio_x, ratio_x,
			ratio_y, (float)viewport.width / (degrees_from_tangent(t.x) + degrees_from_tangent(t.y))];
		[key appendFormat:@" (%.0f %.0f %.0f %.0f) %lux%lu", viewport.originX, viewport.originY, viewport.width,
			viewport.height, (unsigned long)color.width, (unsigned long)color.height];
	}
	if (foveation_logged_key && [key isEqualToString:foveation_logged_key])
		return;
	/* a quality the layer never quite settles at: at most once a second */
	if (foveation_logged_key && foveation_frames >= FOVEATION_EASING_LIMIT &&
		NSProcessInfo.processInfo.systemUptime - foveation_logged_at < FOVEATION_RELOG_SECONDS)
		return;
	foveation_logged_key = key;
	foveation_logged_at = NSProcessInfo.processInfo.systemUptime;
	host_logf(HOST_LOG_INFO, "%s", line.UTF8String);
	/* the same five screen points through every layer of every map */
	for (size_t index = 0; index < map_count; index++)
	{
		id<MTLRasterizationRateMap> map = cp_drawable_get_rasterization_rate_map(drawable, index);
		float width = (float)map.screenSize.width, height = (float)map.screenSize.height;
		const float points[5][2] = { { 0.5f, 0.5f }, { 0.1f, 0.1f }, { 0.9f, 0.1f }, { 0.1f, 0.9f }, { 0.9f, 0.9f } };

		for (NSUInteger layer = 0; layer < map.layerCount; layer++)
		{
			NSMutableString *text = [NSMutableString stringWithFormat:@"stereo: foveation map %zu layer %lu: points "
				"(screen -> physical)", index, (unsigned long)layer];

			for (int point = 0; point < 5; point++)
			{
				float x = points[point][0] * width, y = points[point][1] * height;
				MTLCoordinate2D physical = foveation_physical(map, layer, x, y);

				[text appendFormat:@"%s (%.1f %.1f) -> (%.2f %.2f)", point ? "," : "", x, y, physical.x, physical.y];
			}
			host_logf(HOST_LOG_INFO, "%s", text.UTF8String);
		}
	}
	/* each view's profile through its map and layer */
	for (size_t view_index = 0; view_index < views; view_index++)
	{
		enum { CAPACITY = 128 };
		cp_view_texture_map_t texture_map = cp_view_get_view_texture_map(cp_drawable_get_view(drawable, view_index));
		id<MTLRasterizationRateMap> map = host_theater_view_rate_map(drawable, texture_map);
		size_t texture = cp_view_texture_map_get_texture_index(texture_map);
		size_t slice = cp_view_texture_map_get_slice_index(texture_map);
		MTLViewport viewport = cp_view_texture_map_get_viewport(texture_map);
		simd_float4 t = view_tangents(drawable, view_index);
		float x0 = (float)viewport.originX, y0 = (float)viewport.originY;
		float x1 = x0 + (float)viewport.width, y1 = y0 + (float)viewport.height;
		float center_x = x0 + (float)viewport.width * t.x / (t.x + t.y);
		float center_y = y0 + (float)viewport.height * t.z / (t.z + t.w);
		float row[CAPACITY], row_at[CAPACITY], column[CAPACITY], column_at[CAPACITY];
		int row_count, column_count;
		NSUInteger layer;
		NSMutableString *text;

		if (!map)
			continue;
		layer = slice < map.layerCount ? slice : 0;
		row_count = foveation_profile(map, layer, YES, center_y, x0, x1, row, row_at, CAPACITY);
		column_count = foveation_profile(map, layer, NO, center_x, y0, y1, column, column_at, CAPACITY);
		text = [NSMutableString stringWithFormat:@"stereo: foveation map %zu layer %lu profile (view %zu, every %.0f "
			"px from the view's edge; the center at %.0f %.0f): row", texture < map_count ? texture : 0,
			(unsigned long)layer, view_index, FOVEATION_PROFILE_STEP, center_x, center_y];
		for (int index = 0; index < row_count; index++)
			[text appendFormat:@" %.2f", row[index]];
		[text appendString:@"; column"];
		for (int index = 0; index < column_count; index++)
			[text appendFormat:@" %.2f", column[index]];
		/* the sharp region: how far out the rate stays at least the threshold,
		in degrees from the view's tangents; and against the center's own rate,
		for a quality whose center is below it */
		for (int relative = 0; relative < 2; relative++)
		{
			float center_rate = row_count ? row[0] : 1.0f;

			for (int index = 0; index < row_count; index++)
				if (row_at[index] <= center_x)
					center_rate = row[index];
			float threshold = relative ? FOVEATION_SHARP_RATE * center_rate : FOVEATION_SHARP_RATE;
			float left = foveation_sharp_edge(row, row_at, row_count, x1, center_x, -1, threshold);
			float right = foveation_sharp_edge(row, row_at, row_count, x1, center_x, 1, threshold);
			float up = foveation_sharp_edge(column, column_at, column_count, y1, center_y, -1, threshold);
			float down = foveation_sharp_edge(column, column_at, column_count, y1, center_y, 1, threshold);
			float left_degrees = degrees_from_tangent((center_x - left) / (float)viewport.width * (t.x + t.y));
			float right_degrees = degrees_from_tangent((right - center_x) / (float)viewport.width * (t.x + t.y));
			float up_degrees = degrees_from_tangent((center_y - up) / (float)viewport.height * (t.z + t.w));
			float down_degrees = degrees_from_tangent((down - center_y) / (float)viewport.height * (t.z + t.w));

			[text appendFormat:@"; sharp (rate at least %.2f%s): left %.1f right %.1f up %.1f down %.1f degrees, "
				"radius %.1f", threshold, relative ? ", 0.9 of the center's" : "", left_degrees, right_degrees,
				up_degrees, down_degrees, fminf(fminf(left_degrees, right_degrees), fminf(up_degrees, down_degrees))];
		}
		host_logf(HOST_LOG_INFO, "%s", text.UTF8String);
	}
}
#endif

/* eye_foveated_fragment's sizes */
struct eye_foveation
{
	simd_float2 screen, physical, allocated;
};

struct eye_uniforms
{
	uint32_t decode_srgb;
	float depth_scale;
	float depth_floor;
	float brightness;
	float vignette;
	/* the view's frustum: left, right, up, down tangents; the vignette's
	inner and outer angles in radians, the same for both eyes */
	simd_float4 tangents;
	float vignette_inner, vignette_outer;
	/* the cutscene window's expansion (halo_stereo_window.h): on, the bars
	it carries, its window (left, right, down, up angles in the screen's
	frame), the theater's fade tint around it and that tint's depth, and the
	view's rotation into the screen's frame */
	uint32_t expanding;
	float bars;
	simd_float4 window;
	simd_float4 tint;
	float tint_depth;
	simd_float4x4 screen_from_view;
	/* Task 12k's immersive cutscene: on; the director's frame's half
	tangents across and up, the soft edge (radians) and the outside's dim;
	and the view's directions into the frame's axes (x right, y up, z
	forward) */
	uint32_t cutscene;
	simd_float4 frame;
	simd_float4x4 frame_from_view;
	/* a seat's look fade: the dim and the blur's share (0 without the
	blur); the rest unused */
	simd_float4 look;
};

/* blur_fragment's: one tap's step (normalized), the taps each side, sigma */
struct blur_uniforms
{
	simd_float2 step;
	int32_t taps;
	float sigma;
};

struct hud_uniforms
{
	simd_float4x4 clip_from_frame;
	simd_float4 source, center, x_axis, y_axis;
	uint32_t decode_srgb;
	float brightness;
	uint32_t opaque;
	float opacity;
	simd_float4 solid;
	uint32_t use_solid;
};

static const simd_float4 laser_color = { 0.45f, 0.75f, 1.0f, 1.0f };

static BOOL prepare(id<MTLDevice> device, MTLPixelFormat color, MTLPixelFormat depth)
{
	if (eye_pipeline && pipeline_color == color && pipeline_depth == depth)
		return YES;
	NSError *error = nil;
	id<MTLLibrary> library = [device newLibraryWithSource:shader_source options:nil error:&error];
	if (!library)
	{
		host_logf(HOST_LOG_ERROR, "stereo: the presenter's shaders didn't compile: %s", error.description.UTF8String);
		return NO;
	}
	MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
	descriptor.vertexFunction = [library newFunctionWithName:@"eye_vertex"];
	descriptor.fragmentFunction = [library newFunctionWithName:@"eye_fragment"];
	descriptor.colorAttachments[0].pixelFormat = color;
	descriptor.depthAttachmentPixelFormat = depth;
	if (depth == MTLPixelFormatDepth32Float_Stencil8)
		descriptor.stencilAttachmentPixelFormat = depth;
	eye_pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	descriptor.fragmentFunction = [library newFunctionWithName:@"eye_foveated_fragment"];
	if (eye_pipeline)
		eye_foveated_pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	descriptor.vertexFunction = [library newFunctionWithName:@"hud_vertex"];
	descriptor.fragmentFunction = [library newFunctionWithName:@"hud_fragment"];
	descriptor.colorAttachments[0].blendingEnabled = YES;
	descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
	descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	/* the view stays opaque under the HUD */
	descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
	descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	if (eye_pipeline && eye_foveated_pipeline)
		hud_pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	{
		MTLRenderPipelineDescriptor *blur = [MTLRenderPipelineDescriptor new];

		blur.vertexFunction = [library newFunctionWithName:@"eye_vertex"];
		blur.colorAttachments[0].pixelFormat = BLUR_FORMAT;
		blur.fragmentFunction = [library newFunctionWithName:@"blur_down_fragment"];
		blur_down_pipeline = hud_pipeline ? [device newRenderPipelineStateWithDescriptor:blur error:&error] : nil;
		blur.fragmentFunction = [library newFunctionWithName:@"blur_down_foveated_fragment"];
		blur_down_foveated_pipeline = blur_down_pipeline ? [device newRenderPipelineStateWithDescriptor:blur
			error:&error] : nil;
		blur.fragmentFunction = [library newFunctionWithName:@"blur_fragment"];
		blur_pipeline = blur_down_foveated_pipeline ? [device newRenderPipelineStateWithDescriptor:blur error:&error] :
			nil;
		/* without them the immersive cutscene draws sharp throughout
		(cutscene_blur); the presenter itself doesn't depend on them */
		if (hud_pipeline && !blur_pipeline)
		{
			host_logf(HOST_LOG_ERROR, "stereo: the immersive cutscene's blur pipelines failed: %s",
				error.description.UTF8String);
			blur_down_pipeline = blur_down_foveated_pipeline = nil;
		}
	}
	if (!eye_pipeline || !eye_foveated_pipeline || !hud_pipeline)
	{
		host_logf(HOST_LOG_ERROR, "stereo: the presenter's pipelines failed: %s", error.description.UTF8String);
		eye_pipeline = eye_foveated_pipeline = hud_pipeline = nil;
		return NO;
	}
	pipeline_color = color;
	pipeline_depth = depth;
	{
		id<MTLFunction> reduce = [library newFunctionWithName:@"hud_depth_reduce"];

		hud_depth_pipeline = reduce ? [device newComputePipelineStateWithFunction:reduce error:&error] : nil;
		if (!hud_depth_pipeline)
			host_logf(HOST_LOG_ERROR, "stereo: the HUD depth's pipeline failed, so the HUD rests at its distance: %s",
				error.description.UTF8String);
	}
	/* the eyes write the game's depth as it is; the HUD draws over them,
	depth test off */
	MTLDepthStencilDescriptor *depth_descriptor = [MTLDepthStencilDescriptor new];
	depth_descriptor.depthCompareFunction = MTLCompareFunctionAlways;
	depth_descriptor.depthWriteEnabled = YES;
	depth_always = [device newDepthStencilStateWithDescriptor:depth_descriptor];
	MTLSamplerDescriptor *sampler_descriptor = [MTLSamplerDescriptor new];
	sampler_descriptor.sAddressMode = sampler_descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
	nearest_sampler = [device newSamplerStateWithDescriptor:sampler_descriptor];
	sampler_descriptor.minFilter = MTLSamplerMinMagFilterLinear;
	sampler_descriptor.magFilter = MTLSamplerMinMagFilterLinear;
	linear_sampler = [device newSamplerStateWithDescriptor:sampler_descriptor];
	return YES;
}

/* prepare, without the game thread waiting on Metal's compiler while
shaders compile off it (gpu_metal_async_compiles): the first call for a
pair of formats builds the pipelines on a worker, and until they're ready
the frame presents empty (host_theater_frame_end_empty). Nothing reads the
pipelines (nor the blur's and the HUD depth's) while prepare_building is
set: the presents check it first, and the worker clears it last. 1 ready, 0
failed (a failure isn't retried for the same formats), -1 building */
static _Atomic int prepare_building;
static BOOL prepare_failed;
static MTLPixelFormat prepare_failed_color, prepare_failed_depth;

static int prepare_ready(id<MTLDevice> device, MTLPixelFormat color, MTLPixelFormat depth)
{
	if (atomic_load(&prepare_building))
		return -1;
	if (!gpu_metal_async_compiles() || (eye_pipeline && pipeline_color == color && pipeline_depth == depth))
		return prepare(device, color, depth) ? 1 : 0;
	if (prepare_failed && prepare_failed_color == color && prepare_failed_depth == depth)
		return 0;
	atomic_store(&prepare_building, 1);
	host_logf(HOST_LOG_INFO, "stereo: building the presenter's pipelines on a worker; empty frames until they're ready");
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^
	{
		@autoreleasepool
		{
			NSTimeInterval started = [NSProcessInfo processInfo].systemUptime;
			BOOL built = prepare(device, color, depth);

			prepare_failed = !built;
			prepare_failed_color = color;
			prepare_failed_depth = depth;
			host_logf(HOST_LOG_INFO, "stereo: the presenter's pipelines %s after %.0f ms", built ? "are ready" : "failed",
				([NSProcessInfo processInfo].systemUptime - started) * 1e3);
			atomic_store(&prepare_building, 0);
		}
	});
	return -1;
}

/* The drawable's depth range for the game's planes, and the factor its depth
takes. Reverse-Z depth with a far plane f is n (f - z) / (z (f - n)): for a
near plane the Compositor allows, n' (no nearer than the layer's minimum,
host_theater_minimum_near; cp_drawable_set_depth_range aborts on a nearer one),
it's the game's times n' (f - n) / (n (f - n')), and what's nearer than n'
pins to 1. Without usable planes (none, or the far not beyond the near) the
Compositor keeps its default range and the depth is written as far: the
picture is reprojected as if distant. The floor is the depth half way to the
far plane, n' / (f - n'): far enough to reproject as distant, and not 0, which
the Compositor takes for nothing there; without usable planes, a small depth
in the Compositor's default range. The floor also hides a game depth that is
wrongly empty (static geometry without depth, headset session 1): it would
reproject as if half way to the far plane rather than vanish, so watch the
game's own empty share instead (d3d8_device.c, debug.gpu_stats). A far plane
nearer than twice the near would put the floor above 1; it is kept below. */
static simd_float2 stereo_depth_range(float near_meters, float far_meters, float *scale, float *floor)
{
	float near = fmaxf(near_meters, host_theater_minimum_near());

	if (!(near_meters > 0.0f) || !(far_meters > near * 1.001f) || !isfinite(far_meters))
	{
		if (!depth_reported++)
			host_logf(HOST_LOG_WARN, "stereo: depth range %.3f to %.1f m isn't usable; depth is written as far",
				near_meters, far_meters);
		*scale = 0.0f;
		*floor = STEREO_DEPTH_FLOOR_DEFAULT;
		return (simd_float2){ 0.0f, 0.0f };
	}
	*scale = near * (far_meters - near_meters) / (near_meters * (far_meters - near));
	*floor = fminf(near / (far_meters - near), 0.5f);
	return (simd_float2){ far_meters, near };
}

/* the head's turn since the last frame and its roll now, from the device's
pose in the room (host_stereo_head.c) */
static void head_turn(struct halo_stereo_frame *frame, simd_float4x4 origin_from_device)
{
	float right[3] = { origin_from_device.columns[0].x, origin_from_device.columns[0].y, origin_from_device.columns[0].z };
	float up[3] = { origin_from_device.columns[1].x, origin_from_device.columns[1].y, origin_from_device.columns[1].z };
	float back[3] = { origin_from_device.columns[2].x, origin_from_device.columns[2].y, origin_from_device.columns[2].z };
	int recenter_active = atomic_load(&world_recenter_active);
	int recenter_pending = !recenter_active && atomic_exchange(&world_recenter_pending, 0);

	if (recenter_active || recenter_pending)
		host_stereo_head_recenter(&head);
	host_stereo_head_turn(&head, right, up, back, frame);
	if (recenter_pending)
		host_logf(HOST_LOG_INFO, "stereo: room recentered; yaw correction %.2f degrees",
			head.room_yaw_offset * 180.0f / (float)M_PI);
	memcpy(head_last_back, back, sizeof(back));
	head_last_position[0] = origin_from_device.columns[3].x;
	head_last_position[1] = origin_from_device.columns[3].y;
	head_last_position[2] = origin_from_device.columns[3].z;
}

/* debug.head_sweep, read once (head_sweep): the synthetic head's amplitude
(radians; 0 without the setting, negative until read) and period (seconds),
and the presentation time its sine starts from */
static float sweep_amplitude = -1.0f, sweep_period;
static double sweep_start;

/* debug.head_sweep: the head turns by itself, left and right in a sine of the
frame's predicted presentation time, level, in place of ARKit's pose, so the
view's tracking can be checked in the simulator (which has no device anchor)
with debug.head_yaw_log. Returns 0 without the setting */
static int head_sweep(struct halo_stereo_frame *frame)
{
	double now = host_theater_presentation_time();
	float yaw;

	if (sweep_amplitude < 0.0f)
	{
		char value[64];
		float amplitude, period;

		sweep_amplitude = 0.0f;
		host_config_string("debug.head_sweep", "", value, sizeof(value));
		if (value[0] && sscanf(value, "%f,%f", &amplitude, &period) == 2 && amplitude > 0.0f && amplitude <= 90.0f &&
			period >= 0.5f)
		{
			sweep_amplitude = amplitude * (float)M_PI / 180.0f;
			sweep_period = period;
			host_logf(HOST_LOG_INFO, "stereo: debug.head_sweep: the head turns %.1f degrees each way every %.2f s, "
				"in place of ARKit's pose", amplitude, period);
		}
		else if (value[0])
			host_logf(HOST_LOG_WARN, "stereo: debug.head_sweep \"%s\" isn't \"amplitude_degrees,period_seconds\" "
				"(up to 90 degrees, at least 0.5 s); no sweep", value);
	}
	if (sweep_amplitude == 0.0f || now <= 0.0)
		return 0;
	if (sweep_start == 0.0)
		sweep_start = now;
	yaw = sweep_amplitude * sinf(2.0f * (float)M_PI * (float)fmod((now - sweep_start) / sweep_period, 1.0));
	{
		/* ARKit's axes: x right, y up, z back; yaw left positive */
		float right[3] = { cosf(yaw), 0.0f, -sinf(yaw) };
		float up[3] = { 0.0f, 1.0f, 0.0f };
		float back[3] = { sinf(yaw), 0.0f, cosf(yaw) };

		host_stereo_head_turn(&head, right, up, back, frame);
	}
	return 1;
}

/* Stereo on the screen: the viewer's eyes against the screen, in the
screen's frame (its center the origin, x right, y up, z toward the viewer).

For an eye at (ex, ey, d) in meters, d in front of the screen, the frame
reports the offset (ex, ey, d) / 3.048 (world units: right, up, back) and
the frustum through the screen's edges:
	left = (half_width + ex) / d     right = (half_width - ex) / d
	up = (half_height - ey) / d      down = (half_height + ey) / d
The guest doesn't render these eyes: it takes from them the viewer's eye
separation, the screen's half width (the tangents add to the width over the
distance) and the head's offset from the screen's axis (the eyes' midpoint),
and maps the game onto the screen as a 3D TV, the eyes at the game's camera
(stereo.c's screen_mapping_eyes; the film and SCREEN gameplay). The
orientation of the head plays no part. */
static void screen_eyes(struct halo_stereo_frame *frame, cp_drawable_t drawable, simd_float4x4 origin_from_device,
	int anchored) API_AVAILABLE(visionos(26.0))
{
	simd_float4x4 origin_from_screen, screen_from_device;
	simd_float2 half_size;
	size_t views = cp_drawable_get_view_count(drawable);
	int width, height;
	int eye;
	static int screen_stats = -1;

	if (views == 0)
		return;
	if (screen_stats < 0)
	{
		char value[16];

		host_config_string("debug.gpu_stats", "false", value, sizeof(value));
		screen_stats = !strcmp(value, "true");
	}
	host_theater_screen(0, &origin_from_screen, &half_size);
	screen_from_device = simd_mul(simd_inverse(origin_from_screen), origin_from_device);
	/* the simulator has one view: both eyes are it */
	for (eye = 0; eye < 2; eye++)
	{
		size_t view_index = (size_t)eye < views ? (size_t)eye : views - 1;
		simd_float4x4 device_from_view = cp_view_get_transform(cp_drawable_get_view(drawable, view_index));
		simd_float4 position = simd_mul(screen_from_device, device_from_view.columns[3]);
		float ex = simd_clamp(position.x, -SCREEN_EDGE_SHARE * half_size.x, SCREEN_EDGE_SHARE * half_size.x);
		float ey = simd_clamp(position.y, -SCREEN_EDGE_SHARE * half_size.y, SCREEN_EDGE_SHARE * half_size.y);
		float d = fmaxf(position.z, SCREEN_MINIMUM_DISTANCE);
		struct halo_stereo_eye *e = &frame->eyes[eye];

		e->offset[0] = ex / METERS_PER_UNIT;
		e->offset[1] = ey / METERS_PER_UNIT;
		e->offset[2] = d / METERS_PER_UNIT;
		e->left = (half_size.x + ex) / d;
		e->right = (half_size.x - ex) / d;
		e->up = (half_size.y - ey) / d;
		e->down = (half_size.y + ey) / d;
		if (screen_frames == 0)
			host_logf(HOST_LOG_INFO, "stereo: on the screen, view %zu of %zu: the eye at %.4f %.4f %.4f m from "
				"the screen's center (%.2f by %.2f m); tangents left %.3f right %.3f up %.3f down %.3f",
				view_index, views, position.x, position.y, position.z, 2.0f * half_size.x, 2.0f * half_size.y,
				e->left, e->right, e->up, e->down);
		/* debug.gpu_stats: where the left eye is against the screen every
		few seconds, to check that leaning moves the eyes (the window's
		parallax), and whether ARKit placed the head */
		if (eye == 0 && screen_stats && screen_frames % 270 == 0)
			host_logf(HOST_LOG_INFO, "stereo: on the screen, frame %lu: the left eye at %.3f %.3f %.3f m from the "
				"screen's center, %s", screen_frames, position.x, position.y, position.z,
				anchored ? "ARKit places the head" : "no device anchor (the eyes hold still)");
	}
	/* the screen's picture size and shape: the eyes' frusta have its shape */
	host_theater_picture_size(&width, &height);
	frame->eye_width = width;
	frame->eye_height = height;
	frame->eye_count = 2;
	if (screen_frames++ == 0)
		host_logf(HOST_LOG_INFO, "stereo: the game is in stereo on the screen; %zu view%s, eyes %dx%d", views,
			views == 1 ? "" : "s", width, height);
}

/* the game's eye passes through the rate maps (debug.foveation_eye_passes,
on by default with display.foveation): each eye's targets are allocated at
the drawable's color texture size for its view, which the configuration's
maximum render quality sets, and its passes go through its view's map, so a
lower quality (the Compositor eases it) renders into the top left of the
same targets. Only with one single-layer map per view and a texture per view
(the dedicated layout, which the M2 offers): a layered map would need the
right view's passes to select its layer, which these passes don't. Otherwise
the eyes render unfoveated at the maps' screen size, and only the presenter
goes through the maps (composite-only) */
static void foveated_eyes(struct halo_stereo_frame *frame, cp_drawable_t drawable, size_t views)
	API_AVAILABLE(visionos(26.0))
{
	id<MTLRasterizationRateMap> maps[2] = { nil, nil };
	int allocated_width = 0, allocated_height = 0, eye;
	const char *why = NULL;
	unsigned long key;
	/* read once: this runs every foveated frame, and host_config reopens
	config.toml on each call */
	static int eye_passes = -1;

	if (eye_passes < 0)
	{
		char value[16];

		host_config_string("debug.foveation_eye_passes", "true", value, sizeof(value));
		eye_passes = strcmp(value, "false") != 0;
	}
	if (!eye_passes)
		why = "debug.foveation_eye_passes is off";
	else if (views < 2)
		why = "the drawable has one view";
	for (eye = 0; eye < 2 && !why; eye++)
	{
		cp_view_texture_map_t texture_map = cp_view_get_view_texture_map(cp_drawable_get_view(drawable, (size_t)eye));
		size_t texture = cp_view_texture_map_get_texture_index(texture_map);
		id<MTLTexture> color = cp_drawable_get_color_texture(drawable, texture);
		id<MTLRasterizationRateMap> map = host_theater_view_rate_map(drawable, texture_map);
		MTLSize physical = map ? [map physicalSizeForLayer:0] : MTLSizeMake(0, 0, 0);

		if (!map || map.layerCount != 1 || color.textureType != MTLTextureType2D ||
			(eye == 1 && (map == maps[0] || texture == cp_view_texture_map_get_texture_index(
				cp_view_get_view_texture_map(cp_drawable_get_view(drawable, 0))))))
			why = "the layout isn't dedicated (a single-layer map and a texture per view)";
		else if (physical.width > color.width || physical.height > color.height)
			why = "a map's physical size is larger than its view's texture";
		else
		{
			maps[eye] = map;
			if ((int)color.width > allocated_width)
				allocated_width = (int)color.width;
			if ((int)color.height > allocated_height)
				allocated_height = (int)color.height;
		}
	}
	if (why)
		maps[0] = maps[1] = nil;
	for (eye = 0; eye < 2; eye++)
	{
		eye_rate_maps[eye] = maps[eye];
		eye_rate_map_parameters[eye] = nil;
		if (maps[eye])
		{
			id<MTLBuffer> parameters = [maps[eye].device newBufferWithLength:maps[eye].parameterBufferSizeAndAlign.size
				options:MTLResourceStorageModeShared];

			[maps[eye] copyParameterDataToBuffer:parameters offset:0];
			eye_rate_map_parameters[eye] = parameters;
		}
	}
	foveated_allocated_width = why ? 0 : allocated_width;
	foveated_allocated_height = why ? 0 : allocated_height;
	frame->foveated_width = foveated_allocated_width;
	frame->foveated_height = foveated_allocated_height;
	/* once, and on a change of the sizes or the reason */
	key = why ? (unsigned long)(uintptr_t)why : ((unsigned long)frame->eye_width << 48) ^
		((unsigned long)frame->eye_height << 32) ^ ((unsigned long)allocated_width << 16) ^ (unsigned long)allocated_height;
	if (foveated_eyes_logged == (why == NULL) && foveated_eyes_key == key)
		return;
	foveated_eyes_logged = why == NULL;
	foveated_eyes_key = key;
	if (why)
		host_logf(HOST_LOG_INFO, "stereo: foveated: the eyes render unfoveated at the rate map's screen size %dx%d, "
			"and only the presenter goes through the maps (composite-only): %s", frame->eye_width, frame->eye_height,
			why);
	else
	{
		MTLSize left = [maps[0] physicalSizeForLayer:0], right = [maps[1] physicalSizeForLayer:0];

		host_logf(HOST_LOG_INFO, "stereo: foveated: the eyes render through the rate maps: screen %dx%d, targets "
			"allocated at %dx%d, physical now %lux%lu and %lux%lu", frame->eye_width, frame->eye_height,
			allocated_width, allocated_height, (unsigned long)left.width, (unsigned long)left.height,
			(unsigned long)right.width, (unsigned long)right.height);
	}
}

/* "stereo: available memory N MB at WHEN (foveation ...)" */
static void log_available_memory(const char *when)
{
	float quality = 0.0f, runtime = 0.0f;
	unsigned long long megabytes = (unsigned long long)os_proc_available_memory() / (1024 * 1024);

	if (host_theater_foveation_state(&quality, &runtime, NULL, NULL, NULL))
		host_logf(HOST_LOG_INFO, "stereo: available memory %llu MB %s (foveation at quality %.3f, runtime %.3f)",
			megabytes, when, quality, runtime);
	else
		host_logf(HOST_LOG_INFO, "stereo: available memory %llu MB %s (unfoveated)", megabytes, when);
}

static void stereo_frame(struct halo_stereo_frame *frame) API_AVAILABLE(visionos(26.0))
{
	simd_float4x4 origin_from_device;
	int anchored;
	size_t views;
	int eye;

	if ((frame->mode != HALO_STEREO_HEAD && frame->mode != HALO_STEREO_SCREEN) || !host_theater_frame_begin(1))
	{
		head.known = 0;
		picture_width = picture_height = 0;
		return;
	}
	cp_drawable_t drawable = host_theater_drawable(0, &origin_from_device, &anchored);
	head_frame_open = head_configured();
	{
		NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;

		if (memory_last_frame_at == 0.0)
			log_available_memory("at the first stereo frame since the space opened (a level loaded)");
		else if (now - memory_last_frame_at >= MEMORY_LOAD_GAP_SECONDS)
		{
			char when[128];

			snprintf(when, sizeof(when), "at the first stereo frame after %.1f s without one (a level load)",
				now - memory_last_frame_at);
			log_available_memory(when);
		}
		memory_last_frame_at = now;
	}
	/* the rate maps, whatever this frame shows: a10 opens on a film */
	foveation_measure(drawable);
	/* on the screen, the head moves only the eyes: no turn, and the game
	renders at the screen's picture size (host_theater_picture_size) */
	if (frame->mode == HALO_STEREO_SCREEN)
	{
		head.known = 0;
		picture_width = picture_height = 0;
		screen_eyes(frame, drawable, origin_from_device, anchored);
		return;
	}
	views = cp_drawable_get_view_count(drawable);
	if (views == 0)
		return;
	/* the simulator has one view: both eyes are it */
	for (eye = 0; eye < 2; eye++)
	{
		size_t view_index = (size_t)eye < views ? (size_t)eye : views - 1;
		cp_view_t view = cp_drawable_get_view(drawable, view_index);
		simd_float4x4 device_from_view = cp_view_get_transform(view);
		simd_float4x4 projection = cp_drawable_compute_projection(drawable,
			cp_axis_direction_convention_right_up_back, view_index);
		struct halo_stereo_eye *e = &frame->eyes[eye];

		/* right, up and back, as the device's axes and the eye's offset are */
		e->offset[0] = device_from_view.columns[3].x / METERS_PER_UNIT;
		e->offset[1] = device_from_view.columns[3].y / METERS_PER_UNIT;
		e->offset[2] = device_from_view.columns[3].z / METERS_PER_UNIT;
		/* an off-axis projection's x scale is 2 / (left + right) and its
		x offset (right - left) / (left + right); likewise y */
		e->right = (1.0f + projection.columns[2].x) / projection.columns[0].x;
		e->left = (1.0f - projection.columns[2].x) / projection.columns[0].x;
		e->up = (1.0f + projection.columns[2].y) / projection.columns[1].y;
		e->down = (1.0f - projection.columns[2].y) / projection.columns[1].y;
		/* once: the tangents beside the projection they come from, to check
		the signs on a device (cp_view_get_tangents, the other source, aborts
		with __BUG_IN_CLIENT__ when an app built with this SDK calls it) */
		if (stereo_frames == 0)
			host_logf(HOST_LOG_INFO, "stereo: view %zu of %zu: offset %.4f %.4f %.4f m, forward %.3f %.3f %.3f; "
				"tangents left %.3f right %.3f up %.3f down %.3f from the projection's x scale %.4f, y scale %.4f, "
				"x offset %.4f, y offset %.4f", view_index, views, device_from_view.columns[3].x,
				device_from_view.columns[3].y, device_from_view.columns[3].z, -device_from_view.columns[2].x,
				-device_from_view.columns[2].y, -device_from_view.columns[2].z, e->left, e->right, e->up, e->down,
				projection.columns[0].x, projection.columns[1].y, projection.columns[2].x, projection.columns[2].y);
	}
	{
		cp_view_texture_map_t texture_map = cp_view_get_view_texture_map(cp_drawable_get_view(drawable, 0));
		MTLViewport viewport = cp_view_texture_map_get_viewport(texture_map);
		id<MTLRasterizationRateMap> rate_map = host_theater_view_rate_map(drawable, texture_map);

		/* even, as the picture size the game renders at (host_stereo_picture_size).
		Foveated (display.foveation), the map's screen size, not rounded: the
		presenter writes the view through the map, and the game's eye passes
		render through it too, or unfoveated at that size (foveated_eyes).
		Their viewports and scissors then cover the map's whole screen, since
		Metal warns that a partial one under a rate map "may not behave as
		expected" (a 5008-pixel scissor on a 5009-pixel screen was one) */
		if (rate_map)
		{
			frame->eye_width = (int32_t)rate_map.screenSize.width;
			frame->eye_height = (int32_t)rate_map.screenSize.height;
			frame->foveated = 1;
			if (stereo_frames == 0)
				host_logf(HOST_LOG_INFO, "stereo: foveated: the eyes' screen size is the rate map's, %dx%d (the "
					"view's logical viewport %.0fx%.0f)", frame->eye_width, frame->eye_height, viewport.width,
					viewport.height);
			foveated_eyes(frame, drawable, views);
			if (!memory_foveated_logged)
			{
				memory_foveated_logged = 1;
				log_available_memory("at the first foveated frame");
			}
			if (++memory_foveated_frames == MEMORY_SETTLED_FOVEATED_FRAMES)
			{
				char when[96];

				snprintf(when, sizeof(when), "at the settled reading, foveated frame %d",
					MEMORY_SETTLED_FOVEATED_FRAMES);
				log_available_memory(when);
			}
		}
		else
		{
			frame->eye_width = (int32_t)viewport.width & ~1;
			frame->eye_height = (int32_t)viewport.height & ~1;
		}
		picture_width = frame->eye_width;
		picture_height = frame->eye_height;
	}
	/* debug.head_sweep's head, else ARKit's pose; without either (the
	simulator, or a lost anchor) the head holds its last pose: no turn */
	if (!head_sweep(frame))
	{
		if (anchored)
			head_turn(frame, origin_from_device);
		else
			host_stereo_head_hold(&head, frame);
	}
	host_spatial_frame(drawable, anchored ? &origin_from_device : NULL);
	frame->eye_count = 2;
	if (stereo_frames++ == 0)
		host_logf(HOST_LOG_INFO, "stereo: the head drives the view; %zu view%s of %dx%d, %s", views,
			views == 1 ? "" : "s", frame->eye_width, frame->eye_height,
			anchored ? "ARKit places the head" : "no device anchor (the head holds still)");
}
#endif

/* display.frame_repeat, clamped to 0..3: how many extra refreshes each of
the Compositor's frames stays up in stereo (0: every refresh, 90 Hz; 1: 45
Hz). Theater mode's mono screen keeps every refresh */
int host_stereo_frame_repeat(void)
{
#if TARGET_OS_VISION
	char value[16];
	int repeat;

	if (!head_configured())
	{
		host_config_string("display.stereo", "off", value, sizeof(value));
		if (strcmp(value, "screen"))
			return 0;
	}
	repeat = (int)host_config_real("display.frame_repeat", 0.0);
	return repeat < 0 ? 0 : repeat > 3 ? 3 : repeat;
#else
	return 0;
#endif
}

void host_stereo_space_opened(void *layer_renderer)
{
#if TARGET_OS_VISION
	/* the frame rate: the layer renderer's repeat count, set for each
	opening (a new layer renderer starts at 0) */
	if (layer_renderer)
	{
		cp_layer_renderer_t renderer = (__bridge cp_layer_renderer_t)layer_renderer;
		int repeat = host_stereo_frame_repeat();

		cp_layer_renderer_set_minimum_frame_repeat_count(renderer, repeat);
		host_logf(HOST_LOG_INFO, "stereo: the frame repeat count is %d (asked %d): a frame every %d refresh%s",
			cp_layer_renderer_get_minimum_frame_repeat_count(renderer), repeat, repeat + 1, repeat ? "es" : "");
	}
	stereo_frames = 0;
	screen_frames = 0;
	stereo_presents = 0;
	ui_presents = 0;
	ui_shown = -1;
	zoom_logged = 0;
	depth_reported = 0;
	foveation_logged_key = nil;
	foveation_frames = 0;
	foveation_easing_logged = 0;
	foveated_eyes_logged = -1;
	memory_foveated_logged = 0;
	memory_foveated_frames = 0;
	memory_last_frame_at = 0.0;
#else
	(void)layer_renderer;
#endif
}

void host_stereo_frame(struct halo_stereo_frame *frame)
{
	/* no eyes and no turn unless this frame supplies them, whatever the guest
	left in the struct: a turn replayed from an earlier frame would spin the
	view while ARKit has lost the head */
	frame->eye_count = 0;
	frame->head_yaw = 0.0f;
	frame->head_pitch = 0.0f;
	frame->head_roll = 0.0f;
	frame->eye_width = 0;
	frame->eye_height = 0;
	frame->foveated = 0;
	frame->foveated_width = 0;
	frame->foveated_height = 0;
#if TARGET_OS_VISION
	head_frame_open = 0;
	host_spatial_frame(NULL, NULL);
	eye_rate_maps[0] = eye_rate_maps[1] = nil;
	eye_rate_map_parameters[0] = eye_rate_map_parameters[1] = nil;
	foveated_allocated_width = foveated_allocated_height = 0;
	if (@available(visionOS 26.0, *))
		stereo_frame(frame);
#endif
}

int host_stereo_last_head(float back[3], float position[3])
{
#if TARGET_OS_VISION
	if (head.known)
	{
		memcpy(back, head_last_back, sizeof(head_last_back));
		memcpy(position, head_last_position, sizeof(head_last_position));
		return 1;
	}
#endif
	(void)back;
	(void)position;
	return 0;
}

int host_stereo_picture_size(int *width, int *height)
{
#if TARGET_OS_VISION
	if (picture_width > 0 && picture_height > 0)
	{
		*width = picture_width;
		*height = picture_height;
		return 1;
	}
#endif
	(void)width;
	(void)height;
	return 0;
}

int host_stereo_foveated_size(int *screen_width, int *screen_height, int *allocated_width, int *allocated_height)
{
#if TARGET_OS_VISION
	if (eye_rate_maps[0] && foveated_allocated_width > 0 && picture_width > 0)
	{
		*screen_width = picture_width;
		*screen_height = picture_height;
		*allocated_width = foveated_allocated_width;
		*allocated_height = foveated_allocated_height;
		return 1;
	}
#endif
	*screen_width = *screen_height = *allocated_width = *allocated_height = 0;
	return 0;
}

id<MTLRasterizationRateMap> host_stereo_rate_map(int eye)
{
#if TARGET_OS_VISION
	if (eye >= 0 && eye < 2)
		return eye_rate_maps[eye];
#else
	(void)eye;
#endif
	return nil;
}

id<MTLBuffer> host_stereo_rate_map_parameters(int eye)
{
#if TARGET_OS_VISION
	if (eye >= 0 && eye < 2)
		return eye_rate_map_parameters[eye];
#else
	(void)eye;
#endif
	return nil;
}

int host_stereo_ready(void)
{
#if TARGET_OS_VISION
	return host_theater_frame_ready();
#else
	return 0;
#endif
}

#if TARGET_OS_VISION
/* the level frame in the device's frame: at the device, turned by its yaw
in the room only (host_stereo_hud_level_yaw) */
static simd_float4x4 device_from_level(simd_float4x4 origin_from_device)
{
	float right[3] = { origin_from_device.columns[0].x, origin_from_device.columns[0].y,
		origin_from_device.columns[0].z };
	float back[3] = { origin_from_device.columns[2].x, origin_from_device.columns[2].y,
		origin_from_device.columns[2].z };
	float yaw = host_stereo_hud_level_yaw(right, back);
	simd_float4x4 origin_from_level = (simd_float4x4){ {
		{ cosf(yaw), 0.0f, -sinf(yaw), 0.0f },
		{ 0.0f, 1.0f, 0.0f, 0.0f },
		{ sinf(yaw), 0.0f, cosf(yaw), 0.0f },
		origin_from_device.columns[3],
	} };

	return simd_mul(simd_inverse(origin_from_device), origin_from_level);
}

/* draws the quads over a view, each from its texture in layers (indexed by
the quad's layer, HOST_STEREO_HUD_LAYER_*; a quad whose texture is nil or
past layer_count isn't drawn): clip_from_device is the view's projection
from the device's frame, level the level frame in the device's */
static void hud_draw(id<MTLRenderCommandEncoder> encoder, __unsafe_unretained id<MTLTexture> const *layers,
	int layer_count, const struct host_stereo_hud_quad *quads, int count, simd_float4x4 clip_from_device,
	simd_float4x4 level, uint32_t decode_srgb, float brightness, uint32_t opaque) API_AVAILABLE(visionos(26.0))
{
	int index;

	[encoder setRenderPipelineState:hud_pipeline];
	[encoder setDepthStencilState:depth_always];
	[encoder setFragmentSamplerState:linear_sampler atIndex:0];
	for (index = 0; index < count; index++)
	{
		const struct host_stereo_hud_quad *quad = &quads[index];
		struct hud_uniforms uniforms;

		if (quad->layer < 0 || quad->layer >= layer_count || !layers[quad->layer])
			continue;
		memset(&uniforms, 0, sizeof(uniforms));
		uniforms.clip_from_frame = quad->frame == HOST_STEREO_HUD_LEVEL ? simd_mul(clip_from_device, level) :
			clip_from_device;
		uniforms.source = (simd_float4){ quad->source[0], quad->source[1], quad->source[2], quad->source[3] };
		uniforms.center = (simd_float4){ quad->center[0], quad->center[1], quad->center[2], 0.0f };
		uniforms.x_axis = (simd_float4){ quad->x_axis[0], quad->x_axis[1], quad->x_axis[2], 0.0f };
		uniforms.y_axis = (simd_float4){ quad->y_axis[0], quad->y_axis[1], quad->y_axis[2], 0.0f };
		uniforms.decode_srgb = decode_srgb;
		uniforms.brightness = brightness;
		uniforms.opaque = opaque || quad->opaque;
		uniforms.opacity = quad->laser ? HOST_STEREO_HUD_LASER_OPACITY * quad->laser_opacity :
			quad->dim ? 0.5f : 1.0f;
		uniforms.solid = laser_color;
		uniforms.use_solid = quad->laser != 0;
		[encoder setFragmentTexture:layers[quad->layer] atIndex:0];
		[encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:0];
		[encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	}
}
#endif

#if TARGET_OS_VISION
/* where the HUD's groups go: display.hud_corner_across, _corner_up,
_tracker_down, _messages_up and _scale, and how far out the HUD and the UI
rest, display.hud_distance; read once, at the first present, clamped to the
eyes' shared view and the distance's range (host_stereo_hud_placement_clamp) */
static const struct host_stereo_hud_placement *hud_placement(void)
{
	static struct host_stereo_hud_placement placement = HOST_STEREO_HUD_PLACEMENT_DEFAULT;
	static int read;

	if (!read)
	{
		placement.across = (float)host_config_real("display.hud_corner_across", placement.across);
		placement.up = (float)host_config_real("display.hud_corner_up", placement.up);
		placement.tracker_down = (float)host_config_real("display.hud_tracker_down", placement.tracker_down);
		placement.messages_up = (float)host_config_real("display.hud_messages_up", placement.messages_up);
		placement.scale = (float)host_config_real("display.hud_scale", placement.scale);
		placement.distance = (float)host_config_real("display.hud_distance", placement.distance);
		if (host_stereo_hud_placement_clamp(&placement))
			host_logf(HOST_LOG_INFO, "stereo: a display.hud_* angle is outside 0 to %.0f degrees, the scale outside "
				"%.1f to %.1f or the distance outside %.1f to %.1f m; clamped", HOST_STEREO_HUD_SHARED_DEGREES,
				HOST_STEREO_HUD_SCALE_MIN, HOST_STEREO_HUD_SCALE_MAX, HOST_STEREO_HUD_DISTANCE_MIN,
				HOST_STEREO_HUD_DISTANCE_MAX);
		host_logf(HOST_LOG_INFO, "stereo: HUD at across %.1f up %.1f tracker %.1f messages %.1f scale %.2f, "
			"%.2f m out", placement.across, placement.up, placement.tracker_down, placement.messages_up, placement.scale,
			placement.distance);
		read = 1;
	}
	return &placement;
}

/* the cutscene window's expansion: each view's window on the last frame
drawn here (halo_stereo_window_at keeps it from shrinking) */
static struct halo_stereo_expansion_memory expansion_memory;

/* a matrix's rotation as rows, for halo_stereo_window_include_view */
static void rotation_rows(simd_float4x4 matrix, float rows[3][3])
{
	for (int row = 0; row < 3; row++)
		for (int column = 0; column < 3; column++)
			rows[row][column] = matrix.columns[column][row];
}

/* the immersive cutscene's HUD quad (Task 12k): the HUD layer whole over
the director's frame, in the device's frame, distance away (the placement's) */
static int cutscene_hud_quad(const float forward[3], const float up[3], const float tangents[2], float distance,
	struct host_stereo_hud_quad *quads)
{
	struct host_stereo_hud_quad *quad = &quads[0];
	float right[3] = { forward[1] * up[2] - forward[2] * up[1], forward[2] * up[0] - forward[0] * up[2],
		forward[0] * up[1] - forward[1] * up[0] };

	memset(quad, 0, sizeof(*quad));
	quad->source[2] = quad->source[3] = 1.0f;
	for (int i = 0; i < 3; i++)
	{
		quad->center[i] = forward[i] * distance;
		quad->x_axis[i] = right[i] * distance * tangents[0];
		quad->y_axis[i] = up[i] * distance * tangents[1];
	}
	quad->frame = HOST_STEREO_HUD_HEAD;
	quad->layer = HOST_STEREO_HUD_LAYER_HUD;
	quad->catch_all = 1;
	return 1;
}

/* debug.gpu_stats, read once */
static int gpu_stats_logging(void)
{
	static int stats = -1;

	if (stats < 0)
	{
		char value[16];

		host_config_string("debug.gpu_stats", "false", value, sizeof(value));
		stats = strcmp(value, "true") == 0;
	}
	return stats;
}

/* the HUD's depth: display.hud_depth, _share, _floor, _pull_in, _relax and
_relax_delay, read once, at the first present, resting at the placement's
distance (host_stereo_hud_depth_settings_clamp) */
static const struct host_stereo_hud_depth_settings *hud_depth_settings(void)
{
	static struct host_stereo_hud_depth_settings settings = HOST_STEREO_HUD_DEPTH_SETTINGS_DEFAULT;
	static int read;

	if (!read)
	{
		char value[16];

		host_config_string("display.hud_depth", "true", value, sizeof(value));
		settings.enabled = !strcmp(value, "true");
		settings.share = (float)host_config_real("display.hud_depth_share", settings.share);
		settings.floor = (float)host_config_real("display.hud_depth_floor", settings.floor);
		settings.pull_in = (float)host_config_real("display.hud_depth_pull_in", settings.pull_in);
		settings.relax = (float)host_config_real("display.hud_depth_relax", settings.relax);
		settings.relax_delay = (float)host_config_real("display.hud_depth_relax_delay", settings.relax_delay);
		settings.distance = hud_placement()->distance;
		if (host_stereo_hud_depth_settings_clamp(&settings))
			host_logf(HOST_LOG_INFO, "stereo: a display.hud_depth_* setting is outside its range; clamped");
		host_logf(HOST_LOG_INFO, "stereo: HUD depth %s: each piece at %.2f of the nearest under it, %.2f to %.2f m, "
			"pulling in over %.2f s, relaxing over %.2f s after %.2f s", settings.enabled ? "on" : "off",
			settings.share, settings.floor, settings.distance, settings.pull_in, settings.relax, settings.relax_delay);
		read = 1;
	}
	return &settings;
}

/* The nearest depth under each piece (host_stereo_hud.h), from the eyes'
depth after the guest's frame: a ring of result buffers, each the
hud_depth_reduce kernel's for both eyes' pieces, encoded in the views'
command buffer before the views and published by its completion handler;
host_stereo_present reads the newest published at its start (a frame or
two late, inside the pull-in's time) */
#define HUD_DEPTH_RING 3
#define HUD_DEPTH_THREADS 256
/* the threadgroups sharing each piece's rectangle (hud_depth_reduce) */
#define HUD_DEPTH_TILES 32
struct hud_depth_result
{
	float depth;
	uint32_t skipped;
};
static id<MTLBuffer> hud_depth_buffers[HUD_DEPTH_RING];
static float hud_depth_near[HUD_DEPTH_RING], hud_depth_far[HUD_DEPTH_RING];
static unsigned long hud_depth_serial[HUD_DEPTH_RING], hud_depth_encoded, hud_depth_read;
static _Atomic int hud_depth_busy[HUD_DEPTH_RING];
static _Atomic int hud_depth_newest = -1;
/* each eye's depth's stencil, viewed X32_Stencil8, and the texture bound
in its place when the depth has no stencil plane */
static id<MTLTexture> hud_depth_stencil_source[2], hud_depth_stencil_view[2], hud_depth_no_stencil;
static int hud_depth_stencil_logged = -1;
/* each piece's ease, its distance, the nearest under it (meters; infinite:
none measured) and the weapon's and body's texels skipped there, and the
distance its last log line gave */
static struct host_stereo_hud_depth_state hud_depth_states[HOST_STEREO_HUD_PIECE_COUNT];
static float hud_depth_distance[HOST_STEREO_HUD_PIECE_COUNT];
static float hud_depth_nearest[HOST_STEREO_HUD_PIECE_COUNT] = { INFINITY, INFINITY, INFINITY, INFINITY, INFINITY };
static unsigned long hud_depth_skipped[HOST_STEREO_HUD_PIECE_COUNT];
static float hud_depth_logged[HOST_STEREO_HUD_PIECE_COUNT];
static const char *const hud_piece_names[HOST_STEREO_HUD_PIECE_COUNT] = { "reticle", "counters", "meters",
	"messages", "tracker" };

/* the newest published result, once: each piece's nearest over the two
views, in meters (depth 0, the sky or nothing drawn, is the far plane) */
static void hud_depth_take(void)
{
	int newest = atomic_load(&hud_depth_newest);

	if (newest < 0 || hud_depth_serial[newest] == hud_depth_read)
		return;
	hud_depth_read = hud_depth_serial[newest];
	const struct hud_depth_result *results = hud_depth_buffers[newest].contents;
	float near = hud_depth_near[newest], far = hud_depth_far[newest];

	for (int piece = 0; piece < HOST_STEREO_HUD_PIECE_COUNT; piece++)
	{
		const struct hud_depth_result *left = &results[piece], *right = &results[HOST_STEREO_HUD_PIECE_COUNT + piece];
		float depth = fmaxf(left->depth, right->depth);

		hud_depth_nearest[piece] = !(near > 0.0f) || !(far > near) ? INFINITY : depth > 0.0f ?
			near * far / (depth * (far - near) + near) : far;
		hud_depth_skipped[piece] = left->skipped + right->skipped;
	}
}

/* an eye's depth's stencil as a texture the kernel reads, or nil when it
has no stencil plane */
static id<MTLTexture> hud_depth_stencil(int eye, id<MTLTexture> depth)
{
	if (depth.pixelFormat != MTLPixelFormatDepth32Float_Stencil8)
		return nil;
	if (hud_depth_stencil_source[eye] != depth)
	{
		hud_depth_stencil_source[eye] = depth;
		hud_depth_stencil_view[eye] = [depth newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
	}
	return hud_depth_stencil_view[eye];
}

/* encodes the reduction of each view's pieces' footprints (the quads,
host_stereo_hud_footprint) over its eye's depth into commands, unless every
buffer of the ring is in use or is the newest result */
static void hud_depth_encode(id<MTLCommandBuffer> commands, cp_drawable_t drawable, size_t views,
	const struct host_stereo_hud_quad *quads, int quad_count, float layout_width, simd_float4x4 level, id<MTLTexture> const *colors,
	id<MTLTexture> const *depths, float near_meters, float far_meters) API_AVAILABLE(visionos(26.0))
{
	int slot = -1, newest = atomic_load(&hud_depth_newest);

	for (int candidate = 0; candidate < HUD_DEPTH_RING && slot < 0; candidate++)
		if (candidate != newest && !atomic_load(&hud_depth_busy[candidate]))
			slot = candidate;
	if (slot < 0)
		return;
	if (!hud_depth_buffers[slot])
		hud_depth_buffers[slot] = [commands.device newBufferWithLength:2 * HOST_STEREO_HUD_PIECE_COUNT *
			sizeof(struct hud_depth_result) options:MTLResourceStorageModeShared];
	if (!hud_depth_no_stencil)
	{
		MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Uint
			width:1 height:1 mipmapped:NO];

		descriptor.usage = MTLTextureUsageShaderRead;
		hud_depth_no_stencil = [commands.device newTextureWithDescriptor:descriptor];
	}
	if (!hud_depth_buffers[slot] || !hud_depth_no_stencil)
		return;
	memset(hud_depth_buffers[slot].contents, 0, hud_depth_buffers[slot].length);
	id<MTLComputeCommandEncoder> compute = [commands computeCommandEncoder];
	NSUInteger width = hud_depth_pipeline.threadExecutionWidth;
	NSUInteger threads = MIN((NSUInteger)HUD_DEPTH_THREADS, hud_depth_pipeline.maxTotalThreadsPerThreadgroup) / width *
		width;
	int stenciled = 0;

	[compute setComputePipelineState:hud_depth_pipeline];
	for (size_t view_index = 0; view_index < views && view_index < 2; view_index++)
	{
		cp_view_t view = cp_drawable_get_view(drawable, view_index);
		int eye = view_index == 0 ? 0 : 1;
		simd_float4x4 projection = cp_drawable_compute_projection(drawable, cp_axis_direction_convention_right_up_back,
			view_index);
		simd_float4x4 clip_from_device = simd_mul(projection, simd_inverse(cp_view_get_transform(view)));
		simd_float4 t = view_tangents(drawable, view_index);
		const float tangents[4] = { t.x, t.y, t.z, t.w };
		float clip[16], device_from_level[16], footprint[HOST_STEREO_HUD_PIECE_COUNT][4];
		int found[HOST_STEREO_HUD_PIECE_COUNT] = { 0 };
		uint32_t rectangles[HOST_STEREO_HUD_PIECE_COUNT][4];
		id<MTLTexture> depth = depths[eye], stencil = hud_depth_stencil(eye, depth);
		uint32_t use_stencil = stencil != nil;
		/* foveated eye passes: the depth is in the map's physical layout */
		id<MTLRasterizationRateMap> map = eye_rate_maps[eye] && colors[eye].width ==
			(NSUInteger)foveated_allocated_width && colors[eye].height == (NSUInteger)foveated_allocated_height ?
			eye_rate_maps[eye] : nil;
		MTLSize physical = map ? [map physicalSizeForLayer:0] : MTLSizeMake(depth.width, depth.height, 1);

		memcpy(clip, &clip_from_device, sizeof(clip));
		memcpy(device_from_level, &level, sizeof(device_from_level));
		/* each piece's rectangle: the union of its quads' footprints */
		for (int index = 0; index < quad_count; index++)
		{
			int piece = host_stereo_hud_piece(&quads[index]);
			struct host_stereo_hud_quad measured;
			float rectangle[4];

			/* (the reticle's: only the square around the crosshair) */
			host_stereo_hud_depth_quad(&quads[index], layout_width, &measured);
			if (piece < 0 || !host_stereo_hud_footprint(&measured, clip, device_from_level, tangents, rectangle))
				continue;
			if (!found[piece])
				memcpy(footprint[piece], rectangle, sizeof(rectangle));
			else
			{
				footprint[piece][0] = fminf(footprint[piece][0], rectangle[0]);
				footprint[piece][1] = fminf(footprint[piece][1], rectangle[1]);
				footprint[piece][2] = fmaxf(footprint[piece][2], rectangle[2]);
				footprint[piece][3] = fmaxf(footprint[piece][3], rectangle[3]);
			}
			found[piece] = 1;
		}
		/* in the eye's texels: the screen's through the rate map (separable,
		so a rectangle stays one), kept inside its physical region */
		for (int piece = 0; piece < HOST_STEREO_HUD_PIECE_COUNT; piece++)
		{
			float screen[4], x0, y0, x1, y1;

			memset(rectangles[piece], 0, sizeof(rectangles[piece]));
			if (!found[piece])
				continue;
			if (map)
			{
				host_stereo_hud_footprint_screen(footprint[piece], (float)picture_width, (float)picture_height, screen);
				MTLCoordinate2D top_left = foveation_physical(map, 0, screen[0], screen[1]);
				MTLCoordinate2D bottom_right = foveation_physical(map, 0, screen[2], screen[3]);

				x0 = top_left.x;
				y0 = top_left.y;
				x1 = bottom_right.x;
				y1 = bottom_right.y;
			}
			else
			{
				host_stereo_hud_footprint_screen(footprint[piece], (float)depth.width, (float)depth.height, screen);
				x0 = screen[0];
				y0 = screen[1];
				x1 = screen[2];
				y1 = screen[3];
			}
			rectangles[piece][0] = (uint32_t)fmaxf(0.0f, floorf(x0));
			rectangles[piece][1] = (uint32_t)fmaxf(0.0f, floorf(y0));
			rectangles[piece][2] = (uint32_t)fminf((float)physical.width, ceilf(x1));
			rectangles[piece][3] = (uint32_t)fminf((float)physical.height, ceilf(y1));
		}
		[compute setTexture:depth atIndex:0];
		[compute setTexture:stencil ? stencil : hud_depth_no_stencil atIndex:1];
		[compute setBytes:rectangles length:sizeof(rectangles) atIndex:0];
		[compute setBytes:&use_stencil length:sizeof(use_stencil) atIndex:1];
		[compute setBuffer:hud_depth_buffers[slot] offset:(NSUInteger)eye * HOST_STEREO_HUD_PIECE_COUNT *
			sizeof(struct hud_depth_result) atIndex:2];
		[compute dispatchThreadgroups:MTLSizeMake(HUD_DEPTH_TILES, HOST_STEREO_HUD_PIECE_COUNT, 1)
			threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
		stenciled |= use_stencil;
	}
	[compute endEncoding];
	if (stenciled != hud_depth_stencil_logged)
	{
		host_logf(HOST_LOG_INFO, "stereo: HUD depth: %s", stenciled ? "the eyes' depth has a stencil plane: the "
			"weapon's (value 1) and the first-person body's (value 4) texels are skipped" : "the eyes' depth has no "
			"stencil plane: nothing is skipped");
		hud_depth_stencil_logged = stenciled;
	}
	hud_depth_near[slot] = near_meters;
	hud_depth_far[slot] = far_meters;
	hud_depth_serial[slot] = ++hud_depth_encoded;
	atomic_store(&hud_depth_busy[slot], 1);
	[commands addCompletedHandler:^(id<MTLCommandBuffer> completed)
	{
		if (completed.status == MTLCommandBufferStatusCompleted)
			atomic_store(&hud_depth_newest, slot);
		atomic_store(&hud_depth_busy[slot], 0);
	}];
}

/* the immersive cutscene's presenter GPU time (Task 12k, Step 3): the blur's
and the views' command buffers summed for each frame, logged under
debug.gpu_stats over every CUTSCENE_TIMED_FRAMES frames: mean and worst */
#define CUTSCENE_TIMED_FRAMES 300
static _Atomic uint64_t cutscene_gpu_nanoseconds;
static uint64_t cutscene_gpu_counted, cutscene_gpu_worst, cutscene_gpu_sum;
static unsigned long cutscene_gpu_frames;

static void cutscene_time(id<MTLCommandBuffer> commands)
{
	[commands addCompletedHandler:^(id<MTLCommandBuffer> completed)
	{
		if (completed.GPUEndTime > completed.GPUStartTime)
			atomic_fetch_add(&cutscene_gpu_nanoseconds,
				(uint64_t)((completed.GPUEndTime - completed.GPUStartTime) * 1e9));
	}];
}

/* once a frame: what completed since the last (a frame or so behind) */
static void cutscene_time_frame(int cutscene)
{
	static int stats = -1;
	uint64_t total = atomic_load(&cutscene_gpu_nanoseconds), frame = total - cutscene_gpu_counted;

	if (stats < 0)
	{
		char value[16];

		host_config_string("debug.gpu_stats", "false", value, sizeof(value));
		stats = strcmp(value, "true") == 0;
	}
	cutscene_gpu_counted = total;
	if (!cutscene || !stats)
		return;
	cutscene_gpu_sum += frame;
	if (frame > cutscene_gpu_worst)
		cutscene_gpu_worst = frame;
	if (++cutscene_gpu_frames == CUTSCENE_TIMED_FRAMES)
	{
		host_logf(HOST_LOG_INFO, "stereo: the immersive cutscene's presenter (the blur and the views): GPU %.2f ms "
			"a frame (mean of %d), worst %.2f ms", (double)cutscene_gpu_sum / 1e6 / CUTSCENE_TIMED_FRAMES,
			CUTSCENE_TIMED_FRAMES, (double)cutscene_gpu_worst / 1e6);
		cutscene_gpu_frames = 0;
		cutscene_gpu_sum = cutscene_gpu_worst = 0;
	}
}

/* the immersive cutscene's blur of both eyes into blur_targets[eye][0] (a
quarter of the eyes' screen size, laid out as the view), in a command buffer
of its own committed before the views'; 0 (no blur: the frame is drawn
sharp throughout) without pipelines, a picture size or targets */
static int cutscene_blur(id<MTLCommandQueue> queue, id<MTLTexture> left, id<MTLTexture> right)
	API_AVAILABLE(visionos(26.0))
{
	id<MTLTexture> colors[2] = { left, right };
	int width = picture_width / HALO_STEREO_CUTSCENE_BLUR_SCALE, height = picture_height / HALO_STEREO_CUTSCENE_BLUR_SCALE;
	static int logged;

	if (!blur_pipeline || width < 1 || height < 1 || !colors[0] || !colors[1])
		return 0;
	if (!blur_targets[0][0] || (int)blur_targets[0][0].width != width || (int)blur_targets[0][0].height != height)
	{
		MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:BLUR_FORMAT
			width:(NSUInteger)width height:(NSUInteger)height mipmapped:NO];

		descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		descriptor.storageMode = MTLStorageModePrivate;
		for (int eye = 0; eye < 2; eye++)
			for (int target = 0; target < 2; target++)
				blur_targets[eye][target] = [queue.device newTextureWithDescriptor:descriptor];
		if (!blur_targets[0][0] || !blur_targets[0][1] || !blur_targets[1][0] || !blur_targets[1][1])
			return 0;
		if (!logged)
		{
			host_logf(HOST_LOG_INFO, "stereo: the immersive cutscene's blur: %dx%d targets, a %d-tap Gaussian (sigma %.1f) "
				"each way", width, height, 2 * HALO_STEREO_CUTSCENE_TAPS + 1, (double)HALO_STEREO_CUTSCENE_SIGMA);
			logged = 1;
		}
	}
	id<MTLCommandBuffer> commands = [queue commandBuffer];
	for (int eye = 0; eye < 2; eye++)
	{
		for (int step = 0; step < 3; step++)
		{
			MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
			/* down into the first target, across into the second, down into the first */
			id<MTLTexture> target = blur_targets[eye][step == 1 ? 1 : 0];

			pass.colorAttachments[0].texture = target;
			pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
			[encoder setFragmentSamplerState:linear_sampler atIndex:0];
			if (step == 0)
			{
				if (eye_rate_maps[eye] && colors[eye].width == (NSUInteger)foveated_allocated_width &&
					colors[eye].height == (NSUInteger)foveated_allocated_height)
				{
					MTLSize physical = [eye_rate_maps[eye] physicalSizeForLayer:0];
					struct eye_foveation foveation = {
						{ (float)picture_width, (float)picture_height },
						{ (float)physical.width, (float)physical.height },
						{ (float)colors[eye].width, (float)colors[eye].height } };

					[encoder setRenderPipelineState:blur_down_foveated_pipeline];
					[encoder setFragmentBuffer:eye_rate_map_parameters[eye] offset:0 atIndex:1];
					[encoder setFragmentBytes:&foveation length:sizeof(foveation) atIndex:2];
				}
				else
					[encoder setRenderPipelineState:blur_down_pipeline];
				[encoder setFragmentTexture:colors[eye] atIndex:0];
			}
			else
			{
				struct blur_uniforms uniforms = { { step == 1 ? 1.0f / (float)width : 0.0f,
					step == 2 ? 1.0f / (float)height : 0.0f }, HALO_STEREO_CUTSCENE_TAPS, HALO_STEREO_CUTSCENE_SIGMA };

				[encoder setRenderPipelineState:blur_pipeline];
				[encoder setFragmentTexture:blur_targets[eye][step == 1 ? 0 : 1] atIndex:0];
				[encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
			}
			[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
			[encoder endEncoding];
		}
	}
	gpu_metal_count_gpu_time(commands);
	cutscene_time(commands);
	[commands commit];
	return 1;
}
#endif

void host_stereo_present(id<MTLCommandQueue> queue, id<MTLTexture> left, id<MTLTexture> right,
	id<MTLTexture> left_depth, id<MTLTexture> right_depth, __unsafe_unretained id<MTLTexture> const *hud_layers,
	const float (*hud_group_extent)[4], float hud_aspect, int hud_ui, const float reticle[3],
	const struct host_stereo_hud_aim *aim, const float hud_tangents[2], id<MTLTexture> zoom, const float zoom_tangents[2], float near_meters,
	float far_meters, float brightness, float vignette, float ui_dim, int expanding, float expansion,
	float expansion_bars, const float fade[4], int cutscene, const float cutscene_forward[3],
	const float cutscene_up[3], const float cutscene_tangents[2], float cutscene_dim, float look_dim,
	float look_blur)
{
	id<MTLTexture> hud = hud_layers[HOST_STEREO_HUD_LAYER_HUD];
	/* the HUD's pieces need its layer or the UI's (a pause can leave the
	HUD undrawn) */
	int hud_shown = hud || hud_layers[HOST_STEREO_HUD_LAYER_UI];

#if TARGET_OS_VISION
	if (@available(visionOS 26.0, *))
	{
		/* (the pipelines building on a worker: prepare_ready) */
		if (atomic_load(&prepare_building))
		{
			host_theater_frame_end_empty(queue, 0);
			return;
		}
		id<MTLTexture> colors[2] = { left, right }, depths[2] = { left_depth, right_depth };
		size_t count = host_theater_drawable_count();
		float depth_scale, depth_floor;
		simd_float2 depth_range = stereo_depth_range(near_meters, far_meters, &depth_scale, &depth_floor);
		float layout_width = (hud_aspect > 0.0f ? hud_aspect : 4.0f / 3.0f) * HOST_STEREO_HUD_LINES;
		struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
		/* the zoomed picture, in place of the eyes' over the whole view, under
		the HUD; the guest passes it only when its pass ran, which waits
		while a menu holds the HUD layer */
		struct host_stereo_hud_quad zoom_quad;
		int zoom_shown = zoom && !expanding && host_stereo_hud_zoom(zoom_tangents, hud_placement()->distance,
			&zoom_quad);
		/* each HUD piece in front of the nearest thing under it (the newest
		measure of the eyes' depth), eased; at rest over the zoomed picture,
		the immersive cutscene and the expanding window, where the pieces
		aren't over the world */
		const struct host_stereo_hud_depth_settings *depth_settings = hud_depth_settings();
		int depth_measured = depth_settings->enabled && hud_depth_pipeline && hud_shown && !zoom_shown && !expanding &&
			!(cutscene && hud && !hud_ui) && depth_range.x > 0.0f && left_depth;
		NSTimeInterval now = [NSProcessInfo processInfo].systemUptime;
		static NSTimeInterval depth_last_at, depth_first_at;
		float dt = depth_last_at > 0.0 ? (float)fmin(fmax(now - depth_last_at, 0.0), 0.25) : 0.0f;

		if (depth_last_at <= 0.0)
		{
			depth_first_at = now;
			for (int piece = 0; piece < HOST_STEREO_HUD_PIECE_COUNT; piece++)
				hud_depth_logged[piece] = depth_settings->distance;
		}
		depth_last_at = now;
		hud_depth_take();
		for (int piece = 0; piece < HOST_STEREO_HUD_PIECE_COUNT; piece++)
		{
			if (!depth_measured)
			{
				hud_depth_nearest[piece] = INFINITY;
				hud_depth_skipped[piece] = 0;
			}
			hud_depth_distance[piece] = host_stereo_hud_depth_ease(&hud_depth_states[piece], hud_depth_nearest[piece],
				dt, depth_settings);
			/* debug.gpu_stats: a line each frame a piece moves more than 5 cm
			from its last line */
			if (gpu_stats_logging() && fabsf(hud_depth_distance[piece] - hud_depth_logged[piece]) > 0.05f)
			{
				char nearest[24];

				if (isfinite(hud_depth_nearest[piece]))
					snprintf(nearest, sizeof(nearest), "%.2f m", hud_depth_nearest[piece]);
				else
					snprintf(nearest, sizeof(nearest), "none");
				host_logf(HOST_LOG_INFO, "stereo: HUD depth: frame %lu t %.3f %s at %.2f m (nearest %s under it, %lu "
					"texels of weapon or body skipped)", stereo_presents, now - depth_first_at, hud_piece_names[piece],
					hud_depth_distance[piece], nearest, hud_depth_skipped[piece]);
				hud_depth_logged[piece] = hud_depth_distance[piece];
			}
		}
		struct host_stereo_hud_aim aimed = {0};
		if (aim)
		{
			aimed = *aim;
			host_stereo_hud_aim_depth_clamp(&aimed, depth_range.y, depth_range.x);
		}
		int quad_count = hud_shown ? host_stereo_hud_layout_aimed(layout_width, hud_ui, reticle,
			aim ? &aimed : NULL, hud_tangents,
			hud_group_extent, hud_placement(), depth_settings->enabled ? hud_depth_distance : NULL, quads) : 0;
		/* the immersive cutscene (Task 12k): the HUD layer (its titles and
		bars) whole on the director's frame, as on the film's screen, at the
		HUD's distance; a menu keeps the usual layout */
		if (cutscene && hud && !hud_ui)
			quad_count = cutscene_hud_quad(cutscene_forward, cutscene_up, cutscene_tangents, hud_placement()->distance,
				quads);
		/* a seat's look fade (display.seat_look_fade): the dim and the
		blur's share */
		look_dim = fmaxf(0.0f, fminf(1.0f, look_dim));
		look_blur = fmaxf(0.0f, fminf(1.0f, look_blur));
		/* and the blur, once a frame for both eyes, before the views: the
		immersive cutscene's outside, or a seat's look fade */
		int eyes_blurred = (cutscene || look_blur > 0.0f) && cutscene_blur(queue, left, right);
		int cutscene_blurred = cutscene && eyes_blurred;
		/* while the cutscene window expands, the HUD's pieces and the
		crosshairs wait for it to cover the view (they would hang over the
		room outside it); a menu's UI quad shows, as the eyes darken for it */
		if (expanding)
		{
			int kept = 0;

			for (int quad = 0; quad < quad_count; quad++)
				if (quads[quad].layer == HOST_STEREO_HUD_LAYER_UI)
					quads[kept++] = quads[quad];
			quad_count = kept;
		}
		/* whether this frame begins an expansion: no last window to keep */
		int expansion_fresh = halo_stereo_expansion_fresh(&expansion_memory, expanding, expansion);
		/* the widgets' dim (the guest's ui_dim) darkens the eyes as it does
		the game's picture in mono, where it multiplies the gamma-encoded
		color: about the 2.2 power of that in linear light */
		float ui_keep = 1.0f - fmaxf(0.0f, fminf(1.0f, ui_dim));
		/* outside the expanding window the theater's surroundings show: the
		dark, or the room */
		double clear_alpha = expanding && !host_theater_dark() ? 0.0 : 1.0;

		if (stereo_presents++ == 0)
			host_logf(HOST_LOG_INFO, "stereo: first present: eyes %lux%lu%s, %s (laid out at %.3f:1; its groups at "
				"%.2f mm a line in the periphery, the reticle at %.2f, %.2f m ahead; the rest head-locked at the "
				"HUD pass's half tangents %.3f by %.3f), depth range %.3f to %.1f m, %zu drawable%s",
				(unsigned long)left.width, (unsigned long)left.height,
				eye_rate_maps[0] && left.width == (NSUInteger)foveated_allocated_width ? " (foveated: allocated)" : "",
				hud ? "a HUD" : "no HUD", hud_aspect,
				1000.0f * HOST_STEREO_HUD_METERS_PER_LINE * hud_placement()->scale * hud_placement()->distance /
				HOST_STEREO_HUD_LINE_DISTANCE,
				1000.0f * HOST_STEREO_HUD_METERS_PER_LINE * hud_placement()->distance / HOST_STEREO_HUD_LINE_DISTANCE,
				hud_placement()->distance, hud_tangents ? hud_tangents[0] : 0.0f,
				hud_tangents ? hud_tangents[1] : 0.0f, near_meters, far_meters, count, count == 1 ? "" : "s");
		/* the zoomed picture as it comes and goes */
		if ((zoom_shown != 0) != zoom_logged)
		{
			if (zoom_shown)
				host_logf(HOST_LOG_INFO, "stereo: zoomed: the zoomed picture (%lux%lu) fills the view in place of the "
					"eyes' on a head-locked quad %.2f by %.2f m, %.1f m ahead (half tangents %.3f by %.3f)",
					(unsigned long)zoom.width, (unsigned long)zoom.height, 2.0f * zoom_quad.x_axis[0],
					2.0f * zoom_quad.y_axis[1], -zoom_quad.center[2], zoom_tangents[0], zoom_tangents[1]);
			else
				host_logf(HOST_LOG_INFO, "stereo: the zoomed picture is gone: the eyes' pictures");
			zoom_logged = zoom_shown != 0;
		}
		/* each change between the HUD's pieces and the UI's quad */
		if (hud_shown && (hud_ui != 0) != ui_shown)
		{
			host_logf(HOST_LOG_INFO, "stereo: %s", hud_ui ? "a menu, a help panel, the console or a progress bar: "
				"the UI layer on the UI's quad over the HUD's pieces" : "no UI: the HUD's pieces alone");
			ui_shown = hud_ui != 0;
		}

		for (size_t index = 0; index < count; index++)
		{
			simd_float4x4 origin_from_device;
			cp_drawable_t drawable = host_theater_drawable(index, &origin_from_device, NULL);
			simd_float4x4 level = device_from_level(origin_from_device);
			id<MTLCommandBuffer> commands;
			size_t views;

			/* reverse-Z: far first; the projections follow it */
			if (depth_range.x > 0.0f)
				cp_drawable_set_depth_range(drawable, depth_range);
			int ready = prepare_ready(queue.device, cp_drawable_get_color_texture(drawable, 0).pixelFormat,
				cp_drawable_get_depth_texture(drawable, 0).pixelFormat);
			if (ready <= 0)
			{
				if (ready < 0)
					host_theater_frame_end_empty(queue, index);
				else
					host_theater_frame_end();
				return;
			}
			commands = [queue commandBuffer];
			views = cp_drawable_get_view_count(drawable);
			/* the nearest depth under each HUD piece, for a later frame: before
			the views, which overwrite nothing it reads */
			if (depth_measured && index == 0)
				hud_depth_encode(commands, drawable, views, quads, quad_count, layout_width, level, colors, depths, near_meters,
					far_meters);
			/* the comfort vignette's angles, the same for every view: full at
			the nearest edge of any view's frustum */
			float vignette_outer = 0.0f;
			for (size_t view_index = 0; view_index < views; view_index++)
			{
				simd_float4 tangents = view_tangents(drawable, view_index);
				float nearest = atanf(fminf(fminf(tangents.x, tangents.y), fminf(tangents.z, tangents.w)));

				if (view_index == 0 || nearest < vignette_outer)
					vignette_outer = nearest;
			}
			if (!(vignette_outer > 0.0f))
				vignette = 0.0f;
			/* the cutscene window's expansion: the screen's pose and size, and
			the window it ends at, just past every view's edges */
			simd_float4x4 screen_from_origin = matrix_identity_float4x4;
			simd_float2 screen_half = { 0.0f, 0.0f };
			float expansion_end[4];
			if (expanding)
			{
				simd_float4x4 origin_from_screen;

				host_theater_screen(index, &origin_from_screen, &screen_half);
				screen_from_origin = simd_inverse(origin_from_screen);
				halo_stereo_window_empty(expansion_end);
				for (size_t view_index = 0; view_index < views; view_index++)
				{
					simd_float4 t = view_tangents(drawable, view_index);
					const float tangents[4] = { t.x, t.y, t.z, t.w };
					float rows[3][3];

					rotation_rows(simd_mul(screen_from_origin, simd_mul(origin_from_device,
						cp_view_get_transform(cp_drawable_get_view(drawable, view_index)))), rows);
					halo_stereo_window_include_view(expansion_end, tangents, rows);
				}
			}
			for (size_t view_index = 0; view_index < views; view_index++)
			{
				cp_view_t view = cp_drawable_get_view(drawable, view_index);
				cp_view_texture_map_t map = cp_view_get_view_texture_map(view);
				size_t texture = cp_view_texture_map_get_texture_index(map);
				size_t slice = cp_view_texture_map_get_slice_index(map);
				id<MTLTexture> color = cp_drawable_get_color_texture(drawable, texture);
				id<MTLTexture> depth = cp_drawable_get_depth_texture(drawable, texture);
				MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
				/* view 0 is the left eye */
				int eye = view_index == 0 ? 0 : 1;
				uint32_t decode_srgb = color.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB ||
					color.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB;
				struct eye_uniforms eye_uniforms = { decode_srgb, depth_scale, depth_floor,
					brightness * (decode_srgb ? powf(ui_keep, 2.2f) : ui_keep),
					fmaxf(0.0f, fminf(1.0f, vignette)), view_tangents(drawable, view_index),
					HOST_STEREO_VIGNETTE_CLEAR_SHARE * vignette_outer, vignette_outer };
				/* a seat's look fade darkens the HUD's pieces and the zoomed
				picture as the eyes', in linear light on an sRGB target */
				float look_keep = decode_srgb ? powf(1.0f - look_dim, 2.2f) : 1.0f - look_dim;
				simd_float4x4 projection = cp_drawable_compute_projection(drawable,
					cp_axis_direction_convention_right_up_back, view_index);

				/* the immersive cutscene's frame as this view sees it */
				if (cutscene_blurred)
				{
					simd_float4x4 device_from_view = cp_view_get_transform(view);
					simd_float3 forward = { cutscene_forward[0], cutscene_forward[1], cutscene_forward[2] };
					simd_float3 up = { cutscene_up[0], cutscene_up[1], cutscene_up[2] };
					simd_float3 right = simd_cross(forward, up);
					simd_float4x4 frame_from_device = simd_matrix_from_rows(
						(simd_float4){ right.x, right.y, right.z, 0.0f }, (simd_float4){ up.x, up.y, up.z, 0.0f },
						(simd_float4){ forward.x, forward.y, forward.z, 0.0f }, (simd_float4){ 0.0f, 0.0f, 0.0f, 1.0f });

					eye_uniforms.cutscene = 1;
					eye_uniforms.frame = (simd_float4){ cutscene_tangents[0], cutscene_tangents[1],
						HALO_STEREO_CUTSCENE_SOFT_EDGE, fmaxf(0.0f, fminf(1.0f, cutscene_dim)) };
					eye_uniforms.frame_from_view = simd_mul(frame_from_device, device_from_view);
				}
				eye_uniforms.look = (simd_float4){ look_dim, eyes_blurred ? look_blur : 0.0f, 0.0f, 0.0f };
				/* the window this frame: from the screen as this eye sees it
				toward the end, never smaller than its last frame's */
				if (expanding)
				{
					simd_float4x4 screen_from_view = simd_mul(screen_from_origin, simd_mul(origin_from_device,
						cp_view_get_transform(view)));
					const float eye_position[3] = { screen_from_view.columns[3].x, screen_from_view.columns[3].y,
						screen_from_view.columns[3].z };
					float start[4], window[4];
					int fresh = expansion_fresh || view_index >= HALO_STEREO_EXPANSION_VIEWS;

					halo_stereo_screen_window(eye_position, screen_half.x, screen_half.y, start);
					halo_stereo_window_at(start, expansion_end, expansion,
						fresh ? NULL : expansion_memory.window[view_index], window);
					if (view_index < HALO_STEREO_EXPANSION_VIEWS)
						memcpy(expansion_memory.window[view_index], window, sizeof(window));
					if (expansion_fresh && index == 0 && view_index == 0)
						host_logf(HOST_LOG_INFO, "stereo: the cutscene window expands: view 0 from %.1f..%.1f by "
							"%.1f..%.1f degrees (the screen) toward %.1f..%.1f by %.1f..%.1f, its bars at %.2f",
							start[0] * 57.29578f, start[1] * 57.29578f, start[2] * 57.29578f, start[3] * 57.29578f,
							expansion_end[0] * 57.29578f, expansion_end[1] * 57.29578f, expansion_end[2] * 57.29578f,
							expansion_end[3] * 57.29578f, expansion_bars);
					eye_uniforms.expanding = 1;
					eye_uniforms.bars = expansion_bars;
					eye_uniforms.window = (simd_float4){ window[0], window[1], window[2], window[3] };
					eye_uniforms.tint = host_theater_fade_tint(fade, decode_srgb);
					eye_uniforms.tint_depth = host_theater_fade_depth(projection);
					eye_uniforms.screen_from_view = screen_from_view;
				}

				pass.colorAttachments[0].texture = color;
				pass.colorAttachments[0].slice = slice;
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, clear_alpha);
				pass.depthAttachment.texture = depth;
				pass.depthAttachment.slice = slice;
				pass.depthAttachment.loadAction = MTLLoadActionClear;
				pass.depthAttachment.storeAction = MTLStoreActionStore;
				pass.depthAttachment.clearDepth = 0.0;
				if (depth.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
				{
					pass.stencilAttachment.texture = depth;
					pass.stencilAttachment.slice = slice;
					pass.stencilAttachment.loadAction = MTLLoadActionClear;
					pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
				}
				if (color.textureType == MTLTextureType2DArray)
					pass.renderTargetArrayLength = 1;
				/* foveated: every draw into the view goes through its rate map,
				the eye's picture, the zoomed picture and the HUD's quads alike;
				the viewport stays the texture map's, the logical one in the
				map's screen coordinates */
				pass.rasterizationRateMap = host_theater_view_rate_map(drawable, map);
				id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
				[encoder setViewport:cp_view_texture_map_get_viewport(map)];
				[encoder setDepthStencilState:depth_always];
				/* the eye's picture, unless the zoomed picture takes its place */
				if (!zoom_shown)
				{
					/* foveated eye passes: the eye's picture is in its map's
					physical layout, at the top left of its targets (foveated_eyes) */
					if (eye_rate_maps[eye] && colors[eye].width == (NSUInteger)foveated_allocated_width &&
						colors[eye].height == (NSUInteger)foveated_allocated_height)
					{
						MTLSize physical = [eye_rate_maps[eye] physicalSizeForLayer:0];
						struct eye_foveation foveation = {
							{ (float)picture_width, (float)picture_height },
							{ (float)physical.width, (float)physical.height },
							{ (float)colors[eye].width, (float)colors[eye].height } };

						[encoder setRenderPipelineState:eye_foveated_pipeline];
						[encoder setFragmentBuffer:eye_rate_map_parameters[eye] offset:0 atIndex:1];
						[encoder setFragmentBytes:&foveation length:sizeof(foveation) atIndex:2];
					}
					else
						[encoder setRenderPipelineState:eye_pipeline];
					[encoder setFragmentTexture:colors[eye] atIndex:0];
					[encoder setFragmentTexture:depths[eye] atIndex:1];
					/* (the eye's own picture where no blur is read) */
					[encoder setFragmentTexture:eyes_blurred ? blur_targets[eye][0] : colors[eye] atIndex:2];
					[encoder setFragmentSamplerState:linear_sampler atIndex:0];
					[encoder setFragmentSamplerState:nearest_sampler atIndex:1];
					[encoder setFragmentBytes:&eye_uniforms length:sizeof(eye_uniforms) atIndex:0];
					[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
				}
				/* the zoomed picture, opaque on the HUD's plane with the plane's
				depth, then the HUD's pieces at the shape the game lays it out in
				(its texture is the eyes' size, whose pixels needn't be square),
				which stay readable over it */
				if (zoom_shown || quad_count > 0)
				{
					simd_float4x4 clip_from_device = simd_mul(projection, simd_inverse(cp_view_get_transform(view)));

					/* (dimmed as the eye's picture would be: the cut's fade, the
					widgets' dim and a seat's look fade, eye_uniforms.brightness) */
					if (zoom_shown)
					{
						__unsafe_unretained id<MTLTexture> zoom_layers[1] = { zoom };

						hud_draw(encoder, zoom_layers, 1, &zoom_quad, 1, clip_from_device, level, decode_srgb,
							eye_uniforms.brightness * look_keep, 1);
					}
					/* the HUD's pieces and the crosshair fade with a seat's look
					fade; a menu's UI quad, last, never does */
					int faded = quad_count;
					while (faded > 0 && quads[faded - 1].layer == HOST_STEREO_HUD_LAYER_UI)
						faded--;
					if (faded > 0)
						hud_draw(encoder, hud_layers, HOST_STEREO_HUD_LAYER_COUNT, quads, faded, clip_from_device,
							level, decode_srgb, brightness * look_keep, 0);
					if (quad_count > faded)
						hud_draw(encoder, hud_layers, HOST_STEREO_HUD_LAYER_COUNT, quads + faded, quad_count - faded,
							clip_from_device, level, decode_srgb, brightness, 0);
				}
				[encoder endEncoding];
			}
			cp_drawable_encode_present(drawable, commands);
			gpu_metal_count_gpu_time(commands);
			if (cutscene_blurred)
				cutscene_time(commands);
			[commands commit];
		}
		cutscene_time_frame(cutscene_blurred);
		host_theater_frame_end();
	}
#else
	(void)cutscene;
	(void)cutscene_forward;
	(void)cutscene_up;
	(void)cutscene_tangents;
	(void)cutscene_dim;
	(void)look_dim;
	(void)look_blur;
	(void)expanding;
	(void)expansion;
	(void)expansion_bars;
	(void)fade;
	(void)queue;
	(void)left;
	(void)right;
	(void)left_depth;
	(void)right_depth;
	(void)hud_layers;
	(void)hud_group_extent;
	(void)hud_aspect;
	(void)hud_ui;
	(void)reticle;
	(void)hud_tangents;
	(void)zoom;
	(void)zoom_tangents;
	(void)near_meters;
	(void)far_meters;
	(void)brightness;
	(void)ui_dim;
#endif
}

int host_stereo_ui_ready(void)
{
#if TARGET_OS_VISION
	return head_frame_open && host_theater_frame_ready();
#else
	return 0;
#endif
}

void host_stereo_present_ui(id<MTLCommandQueue> queue, id<MTLTexture> picture, id<MTLTexture> hud, float brightness)
{
#if TARGET_OS_VISION
	if (@available(visionOS 26.0, *))
	{
		if (atomic_load(&prepare_building))
		{
			host_theater_frame_end_empty(queue, 0);
			return;
		}
		size_t count = host_theater_drawable_count();
		struct host_stereo_hud_quad quad;
		BOOL dark = host_theater_dark();

		host_stereo_hud_ui((float)picture.width / (float)picture.height, hud_placement()->distance, &quad);
		if (ui_presents++ == 0)
			host_logf(HOST_LOG_INFO, "stereo: a menu or a load: its %lux%lu picture%s on the UI's quad, %.2f by "
				"%.2f m %.1f m ahead, level, turning with the head", (unsigned long)picture.width,
				(unsigned long)picture.height, hud ? " and the HUD layer" : "", 2.0f * quad.x_axis[0],
				2.0f * quad.y_axis[1], -quad.center[2]);
		for (size_t index = 0; index < count; index++)
		{
			simd_float4x4 origin_from_device;
			cp_drawable_t drawable = host_theater_drawable(index, &origin_from_device, NULL);
			simd_float4x4 level = device_from_level(origin_from_device);
			id<MTLCommandBuffer> commands;
			size_t views;

			int ready = prepare_ready(queue.device, cp_drawable_get_color_texture(drawable, 0).pixelFormat,
				cp_drawable_get_depth_texture(drawable, 0).pixelFormat);
			if (ready <= 0)
			{
				if (ready < 0)
					host_theater_frame_end_empty(queue, index);
				else
					host_theater_frame_end();
				return;
			}
			commands = [queue commandBuffer];
			views = cp_drawable_get_view_count(drawable);
			for (size_t view_index = 0; view_index < views; view_index++)
			{
				cp_view_t view = cp_drawable_get_view(drawable, view_index);
				cp_view_texture_map_t map = cp_view_get_view_texture_map(view);
				size_t texture = cp_view_texture_map_get_texture_index(map);
				size_t slice = cp_view_texture_map_get_slice_index(map);
				id<MTLTexture> color = cp_drawable_get_color_texture(drawable, texture);
				id<MTLTexture> depth = cp_drawable_get_depth_texture(drawable, texture);
				MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
				uint32_t decode_srgb = color.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB ||
					color.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB;
				simd_float4x4 projection = cp_drawable_compute_projection(drawable,
					cp_axis_direction_convention_right_up_back, view_index);

				/* around the quad, the theater's surroundings: dark, or the room
				(passthrough) */
				pass.colorAttachments[0].texture = color;
				pass.colorAttachments[0].slice = slice;
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, dark ? 1.0 : 0.0);
				pass.depthAttachment.texture = depth;
				pass.depthAttachment.slice = slice;
				pass.depthAttachment.loadAction = MTLLoadActionClear;
				pass.depthAttachment.storeAction = MTLStoreActionStore;
				pass.depthAttachment.clearDepth = 0.0;
				if (depth.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
				{
					pass.stencilAttachment.texture = depth;
					pass.stencilAttachment.slice = slice;
					pass.stencilAttachment.loadAction = MTLLoadActionClear;
					pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
				}
				if (color.textureType == MTLTextureType2DArray)
					pass.renderTargetArrayLength = 1;
				/* foveated: every draw into the view goes through its rate map,
				the picture and the quads alike; the viewport stays the texture
				map's, the logical one in the map's screen coordinates */
				pass.rasterizationRateMap = host_theater_view_rate_map(drawable, map);
				id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
				[encoder setViewport:cp_view_texture_map_get_viewport(map)];
				simd_float4x4 clip_from_device = simd_mul(projection, simd_inverse(cp_view_get_transform(view)));

				{
					__unsafe_unretained id<MTLTexture> picture_layers[1] = { picture };

					hud_draw(encoder, picture_layers, 1, &quad, 1, clip_from_device, level, decode_srgb, brightness, 1);
				}
				/* the menu over it, by its transmittance; the quad's depth is
				written already */
				if (hud)
				{
					__unsafe_unretained id<MTLTexture> menu_layers[1] = { hud };

					hud_draw(encoder, menu_layers, 1, &quad, 1, clip_from_device, level, decode_srgb, brightness, 0);
				}
				[encoder endEncoding];
			}
			cp_drawable_encode_present(drawable, commands);
			gpu_metal_count_gpu_time(commands);
			[commands commit];
		}
		host_theater_frame_end();
	}
#else
	(void)queue;
	(void)picture;
	(void)hud;
	(void)brightness;
#endif
}

void host_stereo_world_recenter(int active)
{
#if TARGET_OS_VISION
	atomic_store(&world_recenter_pending, 1);
	atomic_store(&world_recenter_active, active != 0);
#else
	(void)active;
#endif
}

float host_stereo_room_yaw_offset(void)
{
#if TARGET_OS_VISION
	return head.room_yaw_offset;
#else
	return 0.0f;
#endif
}

void host_stereo_room_vector(float vector[3])
{
#if TARGET_OS_VISION
	host_stereo_head_room_vector(&head, vector);
#else
	(void)vector;
#endif
}

int host_stereo_room_recenter_pending(void)
{
#if TARGET_OS_VISION
	return atomic_load(&world_recenter_active) || atomic_load(&world_recenter_pending);
#else
	return 0;
#endif
}
