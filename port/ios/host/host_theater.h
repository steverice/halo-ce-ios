/* Theater mode (display.immersive, visionOS): host_theater.m and Theater.swift */
#pragma once

#include <stdint.h>

#ifdef __OBJC__
#import <Metal/Metal.h>
/* draws picture on the theater screen for the Compositor's open frame (or the
next one) */
void host_theater_present(id<MTLCommandQueue> queue, id<MTLTexture> picture);
/* stereo on the screen (display.stereo = "screen", and the cutscenes' 3D
film in any mode): each eye's picture on the screen for its view (view 0 the
left eye), the HUD (nil: none) blended over each, for the frame
host_stereo_frame opened. The screen's depth is the Compositor's, as in
mono: the picture is on a flat surface in the room. The script fade (RGB and
intensity) tints the space around the screen: in the dark it's the
surroundings' color times the intensity; in the room, the color over it at
that opacity. Brightness (0 to 1) dims the picture toward black, for the
fade of a cut to or from the screen */
void host_theater_present_eyes(id<MTLCommandQueue> queue, id<MTLTexture> left, id<MTLTexture> right,
	id<MTLTexture> hud, const float fade[4], float brightness);
/* gpu_metal.m: counts a presenter's command buffer's GPU time with the
game's (pacing_gpu_nanoseconds), before it commits */
void gpu_metal_count_gpu_time(id<MTLCommandBuffer> commands);
/* gpu_metal.m: 1 while shaders and pipelines compile off the game thread
(display.async_shaders), when the presenters build theirs on a worker too */
int gpu_metal_async_compiles(void);
#endif

#if defined(__OBJC__) && !defined(__swift__)
#import <CompositorServices/CompositorServices.h>
#import <ARKit/ARKit.h>
#include <simd/simd.h>
/* The Compositor's frame, shared by the theater screen and head-tracked
stereo (host_stereo.m). host_theater_frame_begin opens the next frame: it
waits for the frame and its optimal input time, then queries the drawables
and each one's device anchor at its presentation time; 0 without a frame
(the space isn't open, or the frame was canceled). Fresh (at the game's
frame begin), it first ends any frame still open unpresented; otherwise (a
present) it takes the open frame if there is one.
host_theater_frame_ready says whether the open frame can still be drawn,
dropping it if the space closed meanwhile. A present draws each drawable
and calls host_theater_frame_end. */
int host_theater_frame_begin(int fresh);
int host_theater_frame_ready(void);
size_t host_theater_drawable_count(void);
/* the open frame's drawable, where the device was (identity when not
anchored) and whether ARKit placed it; either pointer may be NULL */
cp_drawable_t host_theater_drawable(size_t index, simd_float4x4 *origin_from_device, int *anchored);
void host_theater_frame_end(void);
ar_session_t host_theater_session(void);
void host_theater_session_run(ar_data_provider_t accessory_tracking);
void host_theater_session_restart(void);
void host_spatial_frame(cp_drawable_t drawable, const simd_float4x4 *origin_from_device);
/* ends the open frame with its drawables from first on presented empty,
cleared to nothing (depth 0, transparent): while a presenter's pipelines are
still building on a worker, rather than wait for Metal's compiler */
void host_theater_frame_end_empty(id<MTLCommandQueue> queue, size_t first);
/* the rate map a drawable's view renders through (map is the view's texture
map), or nil when the layer isn't foveated: the map at the view's texture
index (dedicated: one per view), else the first (layered: one map whose
layers are the views) */
id<MTLRasterizationRateMap> host_theater_view_rate_map(cp_drawable_t drawable, cp_view_texture_map_t map);
/* the layer's foveation as Theater.swift configured it: 0 when off; else 1,
with the configured render quality, the layer's runtime quality (which
eases toward the configured one), the device's default quality, the
layout's name and the layouts offered with foveation (any pointer may be
NULL) */
int host_theater_foveation_state(float *quality, float *runtime, float *default_quality, const char **layout,
	const char **offered);
/* for stereo's frame times (gpu_metal.m): the seconds the game spent
waiting for the Compositor's frames (host_theater_frame_begin) since the
last call; the open frame's presentation time (CACurrentMediaTime's clock,
0 without one); and the layer's frame repeat count */
double host_theater_take_waited(void);
double host_theater_presentation_time(void);
int host_theater_frame_repeat(void);
/* the screen's pose in the room for the open frame's drawable (placed first
if a present hasn't placed it yet) and its half width and half height in
meters, at the picture's shape (host_theater_picture_size); the screen's
center is its origin, x right, y up and z toward the viewer */
void host_theater_screen(size_t index, simd_float4x4 *origin_from_screen, simd_float2 *half_size);
/* the script fade's tint around the screen as the theater draws it, for a
target (decode_srgb: an sRGB one): the color to write, opaque over the dark
surroundings, premultiplied at the fade's opacity over the room; alpha 0
without a fade. And the depth of that tint's surface (a little behind the
screen) for a view's projection */
simd_float4 host_theater_fade_tint(const float fade[4], uint32_t decode_srgb);
float host_theater_fade_depth(simd_float4x4 projection);
#endif

/* The immersive scene lets the system show hands again when aiming stops. */
void host_theater_set_upper_limbs(int32_t visible);
void host_theater_chord(int held);
/* The system's recenter phases share a yaw correction with the controllers. */
void host_stereo_world_recenter(int active);

/* reads display.theater_* from config.toml (host_config.c) */
void host_theater_load_settings(void);
/* display.theater_environment = "dark" */
int host_theater_dark(void);
/* the picture's size in pixels for the screen's angle */
void host_theater_picture_size(int *width, int *height);
/* opens the immersive space (Theater.swift) */
void host_theater_open(void);
/* debug.test_theater_reopen (host_sdl.c): closes the immersive space as the
Digital Crown would; presses the window's button that opens it again */
void host_theater_test_close(void);
void host_theater_test_reopen(void);
/* whether the space is open, so gpu_present draws there and not in the window */
int host_theater_active(void);
/* for Theater.swift: the space's layer renderer, unretained */
void host_theater_attach(void *renderer);
/* display.foveation with display.stereo = "head": whether Theater.swift's
makeConfiguration asks for foveation (it also needs the layer's support) */
int host_theater_foveation(void);
/* display.render_quality, clamped to 0..1 */
float host_theater_render_quality(void);
/* for Theater.swift: what makeConfiguration chose: foveation on or off,
whether the layer supports it, the layout (a cp_layer_renderer_layout), the
maximum render quality, the device's default quality and the layouts
offered with foveation, by name */
void host_theater_set_foveation(int enabled, int supported, int layout, float quality, float default_quality,
	const char *offered);
/* for Theater.swift: the layer's nearest allowed near plane, in meters */
void host_theater_set_minimum_near(float meters);
/* that, or 0.1 m (the simulator's reading) before the layer reports one */
float host_theater_minimum_near(void);
void host_theater_log_c(const char *message);
/* for Theater.swift: host_join_link_open, for a join link the space was opened with */
#include "host_join_link.h"
