/* Theater mode (display.immersive, visionOS 26 and later): the game's
picture on a screen standing in the room, drawn into an immersive space's
Compositor Services layer (Theater.swift opens it). The game renders as it does
in the window; gpu_present hands the finished picture here instead of to the
window's drawable. Each eye sees the screen through its own pose and
projection, so the room reads in stereo while the picture is flat; the screen
is fixed in the room (ARKit's device anchor), placed in front of the player
where they first look when the space opens. Without a device anchor (the
simulator) the screen follows the head. The Compositor's frame is shared with
head-tracked stereo (host_stereo.m), which opens it at the game's frame begin
to read the eyes' poses and presents it full view. */
#import <ARKit/ARKit.h>
#import <CompositorServices/CompositorServices.h>
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#include <simd/simd.h>
#include <SDL3/SDL.h>
#include "host_config.h"
#include "host_theater.h"
#include "host_foveation_scale.h"
#include "host_stereo.h"
#include "host.h"
#include "ios_host.h"
#include <math.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

/* about what the Vision Pro's displays resolve at the center of the view: a
picture with more pixels per degree than this is wasted work (MetalFX's cost
grows with its output) */
#define PIXELS_PER_DEGREE 40.0
/* the screen's shape: the game's widescreen layout */
#define SCREEN_ASPECT (16.0 / 9.0)

/* config.toml's display.theater_*: the screen's angle across, in degrees, and
its distance and width in meters; whether it stands in the dark */
static double screen_degrees = 60.0;
static float screen_distance = 4.0f, screen_width = 4.6f;
static int environment_dark;

static cp_layer_renderer_t layer_renderer;
static ar_session_t session;
static ar_world_tracking_provider_t world_tracking;
static BOOL screen_placed;
static simd_float4x4 origin_from_screen;
static id<MTLRenderPipelineState> pipeline, hud_pipeline, fade_pipeline;
static MTLPixelFormat pipeline_color, pipeline_depth;
static id<MTLDepthStencilState> depth_state, depth_fade;
static id<MTLSamplerState> sampler;
static unsigned long theater_frames;
/* stereo frames on the screen since the space opened (the once-only log) */
static unsigned long screen_presents;

/* The Compositor's frame, open from host_theater_frame_begin (at the game's
frame begin in head-tracked stereo, host_stereo.m; else at the present) to
host_theater_frame_end after the present, with each drawable's device anchor
queried at its presentation time */
#define MAXIMUM_DRAWABLES 4
static cp_frame_t open_frame;
static size_t open_count;
static cp_drawable_t open_drawables[MAXIMUM_DRAWABLES];
static simd_float4x4 open_origin_from_device[MAXIMUM_DRAWABLES];
static BOOL open_anchored[MAXIMUM_DRAWABLES];

/* for stereo's frame times (gpu_metal.m): the seconds spent waiting for
the Compositor's frames since the last take, and the open frame's
presentation time */
static CFTimeInterval frame_waited;
static CFTimeInterval open_presentation;

/* forgets the open frame without ending it: the space closed under it */
static void frame_drop(void)
{
	open_frame = NULL;
	open_count = 0;
}

/* the layer's nearest allowed near plane (Theater.swift), 0 until it says */
static float minimum_near;

void host_theater_set_minimum_near(float meters)
{
	minimum_near = meters;
	host_logf(HOST_LOG_INFO, "theater: the nearest near plane the layer allows is %.3f m", meters);
}

float host_theater_minimum_near(void)
{
	return minimum_near > 0.0f ? minimum_near : 0.1f;
}

void host_theater_log_c(const char *message)
{
	host_logf(HOST_LOG_INFO, "%s", message);
}

/* Foveation (display.foveation, HEAD mode): what Theater.swift's
makeConfiguration chose for the layer, and the quality the layer renderer
is set to when it attaches */
static int foveation_enabled;
static float foveation_quality, foveation_default;
static char foveation_layout[16], foveation_offered[64];
/* the layer's state as last logged with foveation on: a layer that never
reaches running is the sign the configuration was rejected */
static int logged_state = -1;

int host_theater_foveation(void)
{
	char value[16];

	host_config_string("display.foveation", "true", value, sizeof(value));
	if (strcmp(value, "true"))
		return 0;
	host_config_string("display.stereo", "off", value, sizeof(value));
	return !strcmp(value, "head");
}

float host_theater_render_quality(void)
{
	double quality = host_config_real("display.render_quality", 0.6);

	return !(quality > 0.0) ? 0.0f : quality > 1.0 ? 1.0f : (float)quality;
}

static const char *layout_name(int layout)
{
	return layout == cp_layer_renderer_layout_dedicated ? "dedicated" :
		layout == cp_layer_renderer_layout_shared ? "shared" :
		layout == cp_layer_renderer_layout_layered ? "layered" : "unknown";
}

void host_theater_set_foveation(int enabled, int supported, int layout, float quality, float default_quality,
	const char *offered)
{
	foveation_enabled = enabled;
	foveation_quality = quality;
	foveation_default = default_quality;
	snprintf(foveation_layout, sizeof(foveation_layout), "%s", layout_name(layout));
	snprintf(foveation_offered, sizeof(foveation_offered), "%s", offered ? offered : "");
	if (enabled)
		host_logf(HOST_LOG_INFO, "theater: foveation on at a maximum render quality of %.2f (the device's default "
			"%.2f), layout %s (offered with foveation: %s)", quality, default_quality, foveation_layout,
			foveation_offered);
	else if (!supported)
		host_logf(HOST_LOG_INFO, "theater: display.foveation is on, but the layer doesn't support foveation; "
			"it stays off");
	else
		host_logf(HOST_LOG_INFO, "theater: display.foveation is on, but foveation offers neither the dedicated "
			"nor the layered layout (offered: %s); it stays off", foveation_offered);
}

int host_theater_foveation_state(float *quality, float *runtime, float *default_quality, const char **layout,
	const char **offered)
{
	if (!foveation_enabled || !layer_renderer)
		return 0;
	if (quality)
		*quality = foveation_quality;
	if (runtime)
	{
		*runtime = -1.0f;
		if (@available(visionOS 26.0, *))
			*runtime = cp_layer_renderer_get_render_quality(layer_renderer);
	}
	if (default_quality)
		*default_quality = foveation_default;
	if (layout)
		*layout = foveation_layout;
	if (offered)
		*offered = foveation_offered;
	return 1;
}

static const char *state_name(cp_layer_renderer_state state)
{
	return state == cp_layer_renderer_state_paused ? "paused" :
		state == cp_layer_renderer_state_running ? "running" :
		state == cp_layer_renderer_state_invalidated ? "invalidated" : "unknown";
}

/* with foveation on, each change of the layer's state */
static void foveation_state_logged(void)
{
	if (!foveation_enabled || !layer_renderer)
		return;
	cp_layer_renderer_state state = cp_layer_renderer_get_state(layer_renderer);
	if ((int)state == logged_state)
		return;
	logged_state = (int)state;
	if (@available(visionOS 26.0, *))
		host_logf(HOST_LOG_INFO, "theater: with foveation, the layer is %s (render quality %.3f)", state_name(state),
			cp_layer_renderer_get_render_quality(layer_renderer));
}

id<MTLRasterizationRateMap> host_theater_view_rate_map(cp_drawable_t drawable, cp_view_texture_map_t map)
{
	size_t count = cp_drawable_get_rasterization_rate_map_count(drawable);
	size_t texture = cp_view_texture_map_get_texture_index(map);

	if (count == 0)
		return nil;
	return cp_drawable_get_rasterization_rate_map(drawable, texture < count ? texture : 0);
}

/* hides or shows the game's window (SDL's), which would stand in front of
the screen while the space is open: its frames go to the screen meanwhile.
An alert or share sheet's window (host_share.m) is left as it is: the space
can open while one is up, waiting for the player, and one put away stays
hidden. Shown again, the game's window becomes the key window unless an
alert or sheet is still up: one put away while it was hidden could not give
it back (host_dismiss_window), and without a key window a keyboard's keys
go nowhere */
static void window_hidden(BOOL hidden)
{
	BOOL present_up = NO;
	UIWindow *game = (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(host_sdl_window()),
		SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);

	for (UIScene *scene in UIApplication.sharedApplication.connectedScenes)
	{
		if (![scene isKindOfClass:UIWindowScene.class])
			continue;
		for (UIWindow *window in ((UIWindowScene *)scene).windows)
		{
			if (!host_is_present_window(window))
				window.hidden = hidden;
			else if (!window.hidden)
				present_up = YES;
		}
	}
	if (!hidden && !present_up && game && !game.isKeyWindow)
		[game makeKeyWindow];
}

void host_theater_load_settings(void)
{
	char environment[32];

	screen_degrees = host_config_real("display.theater_width", 60.0);
	if (screen_degrees < 20.0)
		screen_degrees = 20.0;
	if (screen_degrees > 100.0)
		screen_degrees = 100.0;
	screen_distance = (float)host_config_real("display.theater_distance", 4.0);
	if (screen_distance < 1.0f)
		screen_distance = 1.0f;
	screen_width = 2.0f * screen_distance * tanf((float)(screen_degrees * M_PI / 360.0));
	host_config_string("display.theater_environment", "passthrough", environment, sizeof(environment));
	environment_dark = !strcmp(environment, "dark");
	host_logf(HOST_LOG_INFO, "theater: a %.0f-degree screen (%.1f m wide) %.1f m away, %s around it",
		screen_degrees, screen_width, screen_distance, environment_dark ? "dark" : "the room");
}

int host_theater_dark(void)
{
	return environment_dark;
}

void host_theater_picture_size(int *width, int *height)
{
	*width = (int)lround(screen_degrees * PIXELS_PER_DEGREE) & ~1;
	*height = (int)lround(*width / SCREEN_ASPECT) & ~1;
}

/* While the space is closed, a button on the game's window opens it again
(the stereo spec's D3: closing the space returns to the window). A pinch on
it, or a tap, calls Theater.swift's host_theater_open, as the game did at
start. */
static UIButton *reopen_button;

@interface TheaterReopenTarget : NSObject
@end

@implementation TheaterReopenTarget
- (void)reopen
{
	host_logf(HOST_LOG_INFO, "theater: opening the immersive space again");
	host_theater_open();
}
@end

static TheaterReopenTarget *reopen_target;

/* the game's window: SDL's, in SDL's window scene */
static UIWindow *game_window(void)
{
	for (UIScene *scene in UIApplication.sharedApplication.connectedScenes)
	{
		if (![scene isKindOfClass:UIWindowScene.class] ||
			![NSStringFromClass([(NSObject *)scene.delegate class]) isEqualToString:@"SDLUIKitSceneDelegate"])
			continue;
		for (UIWindow *window in ((UIWindowScene *)scene).windows)
			if (window.rootViewController)
				return window;
	}
	return nil;
}

static void reopen_button_shown(BOOL shown)
{
	if (!shown)
	{
		[reopen_button removeFromSuperview];
		reopen_button = nil;
		return;
	}
	UIView *view = game_window().rootViewController.view;
	if (!view || reopen_button)
		return;
	if (!reopen_target)
		reopen_target = [TheaterReopenTarget new];
	UIButtonConfiguration *configuration = [UIButtonConfiguration filledButtonConfiguration];
	configuration.title = @"Back to the Theater";
	reopen_button = [UIButton buttonWithConfiguration:configuration primaryAction:nil];
	[reopen_button addTarget:reopen_target action:@selector(reopen) forControlEvents:UIControlEventPrimaryActionTriggered];
	reopen_button.translatesAutoresizingMaskIntoConstraints = NO;
	[view addSubview:reopen_button];
	[NSLayoutConstraint activateConstraints:@[
		[reopen_button.topAnchor constraintEqualToAnchor:view.safeAreaLayoutGuide.topAnchor constant:24.0],
		[reopen_button.centerXAnchor constraintEqualToAnchor:view.centerXAnchor],
	]];
	[view bringSubviewToFront:reopen_button];
}

/* debug.test_theater_reopen (host_sdl.c): closes the space as the Digital
Crown would, then presses the button */
void host_theater_test_close(void)
{
	for (UIScene *scene in UIApplication.sharedApplication.connectedScenes)
	{
		if ([scene isKindOfClass:UIWindowScene.class] &&
			[NSStringFromClass([(NSObject *)scene.delegate class]) isEqualToString:@"SDLUIKitSceneDelegate"])
			continue;
		host_logf(HOST_LOG_INFO, "debug.test_theater_reopen: closing the immersive space");
		[UIApplication.sharedApplication requestSceneSessionDestruction:scene.session options:nil errorHandler:nil];
	}
}

void host_theater_test_reopen(void)
{
	host_logf(HOST_LOG_INFO, "debug.test_theater_reopen: %s", reopen_button ? "pressing the button" : "no button to press");
	[reopen_button sendActionsForControlEvents:UIControlEventPrimaryActionTriggered];
}

void host_theater_chord(int held)
{
	static CFTimeInterval began;
	static BOOL fired;

	if (!held)
	{
		began = 0.0;
		fired = NO;
		return;
	}
	if (began == 0.0)
		began = CACurrentMediaTime();
	if (fired || CACurrentMediaTime() - began < 1.0)
		return;
	fired = YES;
	if (layer_renderer)
		host_theater_test_close();
	else
		host_theater_test_reopen();
}

void host_theater_attach(void *renderer)
{
	layer_renderer = (__bridge cp_layer_renderer_t)renderer;
	screen_placed = NO;
	reopen_button_shown(NO);
	window_hidden(YES);
	/* the once-only logs, theater's and stereo's, repeat for each opening */
	theater_frames = 0;
	screen_presents = 0;
	host_stereo_space_opened(renderer);
	host_logf(HOST_LOG_INFO, "theater: the immersive space is open");
	/* the render quality, after the configuration's maximum: the runtime
	value eases toward it. A quality the configuration rejected can't be
	caught in makeConfiguration; it shows as a layer that never reaches
	running (foveation_state_logged) */
	logged_state = -1;
	if (foveation_enabled)
	{
		if (@available(visionOS 26.0, *))
		{
			/* display.render_scale lowers the quality below the configuration's maximum
			(host_foveation_scale.h): the Compositor fixes the views' logical size */
			double scale = host_config_real("display.render_scale", 1.0);
			float quality = host_foveation_scaled_quality(foveation_quality, (float)scale);

			cp_layer_renderer_set_render_quality(layer_renderer, quality);
			host_logf(HOST_LOG_INFO, "theater: the render quality is set to %.3f (display.render_scale %.2f of the "
				"maximum %.3f); the layer is %s, its render "
				"quality %.3f for now", quality, scale, foveation_quality, state_name(cp_layer_renderer_get_state(layer_renderer)),
				cp_layer_renderer_get_render_quality(layer_renderer));
		}
		foveation_state_logged();
	}
	if (!session && ar_world_tracking_provider_is_supported())
	{
		ar_world_tracking_configuration_t configuration = ar_world_tracking_configuration_create();

		world_tracking = ar_world_tracking_provider_create(configuration);
		session = ar_session_create();
		ar_session_run(session, ar_data_providers_create_with_data_providers(world_tracking, nil));
	}
	else if (!session)
		host_logf(HOST_LOG_INFO, "theater: no world tracking here; the screen follows the head");
}

ar_session_t host_theater_session(void)
{
	return world_tracking ? session : nil;
}

void host_theater_session_run(ar_data_provider_t accessory_tracking)
{
	if (!session || !world_tracking)
		return;
	ar_session_run(session, accessory_tracking ?
		ar_data_providers_create_with_data_providers(world_tracking, accessory_tracking, nil) :
		ar_data_providers_create_with_data_providers(world_tracking, nil));
}

void host_theater_session_restart(void)
{
	if (!session || !world_tracking)
		return;
	ar_session_stop(session);
	world_tracking = ar_world_tracking_provider_create(ar_world_tracking_configuration_create());
	session = ar_session_create();
	ar_session_run(session, ar_data_providers_create_with_data_providers(world_tracking, nil));
	host_logf(HOST_LOG_INFO, "theater: ARKit's session started again");
}

int host_theater_active(void)
{
	if (!layer_renderer)
		return 0;
	if (cp_layer_renderer_get_state(layer_renderer) == cp_layer_renderer_state_invalidated)
	{
		host_logf(HOST_LOG_INFO, "theater: the immersive space closed; back to the window");
		frame_drop();
		layer_renderer = nil;
		window_hidden(NO);
		/* nothing tracks the room in the window: stop ARKit until the space
		opens again (host_theater_attach starts it) */
		if (session)
		{
			ar_session_stop(session);
			session = nil;
			world_tracking = nil;
			host_logf(HOST_LOG_INFO, "theater: ARKit stopped until the space opens again");
		}
		reopen_button_shown(YES);
		return 0;
	}
	return 1;
}

static NSString *const shader_source =
	@"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct screen_uniforms { float4x4 clip_from_screen; float2 half_size; uint decode_srgb; float brightness; };\n"
	"struct screen_vertex { float4 position [[position]]; float2 coordinate; };\n"
	"vertex screen_vertex theater_vertex(uint index [[vertex_id]], constant screen_uniforms &u [[buffer(0)]])\n"
	"{\n"
	"	float2 corner = float2(index & 1, index >> 1);\n"
	"	screen_vertex out;\n"
	"	out.position = u.clip_from_screen * float4((corner.x * 2 - 1) * u.half_size.x, (1 - corner.y * 2) * u.half_size.y, 0, 1);\n"
	"	out.coordinate = corner;\n"
	"	return out;\n"
	"}\n"
	"fragment float4 theater_fragment(screen_vertex in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"	sampler linear [[sampler(0)]], constant screen_uniforms &u [[buffer(0)]])\n"
	"{\n"
	"	float3 color = picture.sample(linear, in.coordinate).rgb;\n"
	/* the game's colors are display-encoded; an sRGB target encodes what it's given */
	"	if (u.decode_srgb)\n"
	"		color = select(pow((color + 0.055) / 1.055, 2.4), color / 12.92, color <= 0.04045);\n"
	"	return float4(color * u.brightness, 1);\n"
	"}\n"
	/* the script fade's tint of the room around the screen: full view, at
	a depth behind the screen (fade_depth), its color premultiplied by its
	opacity */
	"struct fade_vertex_out { float4 position [[position]]; };\n"
	"vertex fade_vertex_out fade_vertex(uint index [[vertex_id]], constant float &depth [[buffer(0)]])\n"
	"{\n"
	"	float2 corner = float2(index & 1, index >> 1);\n"
	"	fade_vertex_out out;\n"
	"	out.position = float4(corner.x * 2 - 1, 1 - corner.y * 2, depth, 1);\n"
	"	return out;\n"
	"}\n"
	"fragment float4 fade_fragment(fade_vertex_out in [[stage_in]], constant float4 &color [[buffer(0)]])\n"
	"{\n"
	"	return color;\n"
	"}\n"
	/* stereo on the screen: the HUD over the eye's picture, both display-
	encoded, as the game would have blended it into its back buffer. The HUD
	layer holds premultiplied color and, in alpha, how much of the picture
	still shows (d3d8_device.c, hud_layer_blend) */
	"fragment float4 theater_hud_fragment(screen_vertex in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"	texture2d<float> hud [[texture(1)]], sampler linear [[sampler(0)]], constant screen_uniforms &u [[buffer(0)]])\n"
	"{\n"
	"	float4 over = hud.sample(linear, in.coordinate);\n"
	"	float3 color = saturate(over.rgb + picture.sample(linear, in.coordinate).rgb * over.a);\n"
	"	if (u.decode_srgb)\n"
	"		color = select(pow((color + 0.055) / 1.055, 2.4), color / 12.92, color <= 0.04045);\n"
	"	return float4(color * u.brightness, 1);\n"
	"}\n";

struct screen_uniforms
{
	simd_float4x4 clip_from_screen;
	simd_float2 half_size;
	uint32_t decode_srgb;
	float brightness;
};

static BOOL prepare(id<MTLDevice> device, MTLPixelFormat color, MTLPixelFormat depth)
{
	if (pipeline && pipeline_color == color && pipeline_depth == depth)
		return YES;
	NSError *error = nil;
	id<MTLLibrary> library = [device newLibraryWithSource:shader_source options:nil error:&error];
	if (!library)
	{
		host_logf(HOST_LOG_ERROR, "theater: the screen's shaders didn't compile: %s", error.description.UTF8String);
		return NO;
	}
	MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
	descriptor.vertexFunction = [library newFunctionWithName:@"theater_vertex"];
	descriptor.fragmentFunction = [library newFunctionWithName:@"theater_fragment"];
	descriptor.colorAttachments[0].pixelFormat = color;
	descriptor.depthAttachmentPixelFormat = depth;
	if (depth == MTLPixelFormatDepth32Float_Stencil8)
		descriptor.stencilAttachmentPixelFormat = depth;
	pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	descriptor.fragmentFunction = [library newFunctionWithName:@"theater_hud_fragment"];
	if (pipeline)
		hud_pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	descriptor.vertexFunction = [library newFunctionWithName:@"fade_vertex"];
	descriptor.fragmentFunction = [library newFunctionWithName:@"fade_fragment"];
	if (hud_pipeline)
		fade_pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	if (!pipeline || !hud_pipeline || !fade_pipeline)
	{
		host_logf(HOST_LOG_ERROR, "theater: the screen's pipeline failed: %s", error.description.UTF8String);
		pipeline = hud_pipeline = fade_pipeline = nil;
		return NO;
	}
	pipeline_color = color;
	pipeline_depth = depth;
	MTLDepthStencilDescriptor *depth_descriptor = [MTLDepthStencilDescriptor new];
	/* the Compositor's projections are reverse-Z */
	depth_descriptor.depthCompareFunction = MTLCompareFunctionGreater;
	depth_descriptor.depthWriteEnabled = YES;
	depth_state = [device newDepthStencilStateWithDescriptor:depth_descriptor];
	/* the fade's tint is drawn first, behind the screen: it doesn't test,
	and writes its depth, so the Compositor has a surface for it (depth 0, as
	the clear leaves, is nothing to show) */
	depth_descriptor.depthCompareFunction = MTLCompareFunctionAlways;
	depth_descriptor.depthWriteEnabled = YES;
	depth_fade = [device newDepthStencilStateWithDescriptor:depth_descriptor];
	MTLSamplerDescriptor *sampler_descriptor = [MTLSamplerDescriptor new];
	sampler_descriptor.minFilter = MTLSamplerMinMagFilterLinear;
	sampler_descriptor.magFilter = MTLSamplerMinMagFilterLinear;
	sampler = [device newSamplerStateWithDescriptor:sampler_descriptor];
	return YES;
}

/* how far beyond the screen the fade's tint lies, in meters */
#define FADE_BEYOND_SCREEN 2.0f

/* the depth, for a view's projection, of what lies straight ahead of it at
a distance in meters (reverse-Z: nearer is larger) */
static float fade_depth(simd_float4x4 projection, float distance)
{
	simd_float4 clip = simd_mul(projection, (simd_float4){ 0.0f, 0.0f, -distance, 1.0f });

	return clip.w > 0.0f ? simd_clamp(clip.z / clip.w, 0.0f, 1.0f) : 0.0f;
}

/* stands the screen in front of where the device looks, upright and facing it */
static void place_screen(simd_float4x4 origin_from_device)
{
	simd_float3 position = origin_from_device.columns[3].xyz;
	simd_float3 forward = -origin_from_device.columns[2].xyz;
	float yaw;

	forward.y = 0.0f;
	if (simd_length(forward) < 1e-3f)
		forward = (simd_float3){ 0.0f, 0.0f, -1.0f };
	forward = simd_normalize(forward);
	yaw = atan2f(-forward.x, -forward.z);
	origin_from_screen = (simd_float4x4){ {
		{ cosf(yaw), 0.0f, -sinf(yaw), 0.0f },
		{ 0.0f, 1.0f, 0.0f, 0.0f },
		{ sinf(yaw), 0.0f, cosf(yaw), 0.0f },
		{ position.x + forward.x * screen_distance, position.y, position.z + forward.z * screen_distance, 1.0f },
	} };
}

/* the screen's pose for the open frame's drawable: placed once in the room,
in front of the device the first time ARKit places it, and then left there;
until then (world tracking starts a few frames after the space opens, and
without it the device's pose is the identity) and where nothing tracks the
room, in front of the device every frame. A screen placed from an unplaced
device would stand where the room's origin is, and stay there */
static simd_float4x4 screen_pose(size_t index)
{
	if (!screen_placed)
	{
		place_screen(open_origin_from_device[index]);
		screen_placed = world_tracking && open_anchored[index];
		if (screen_placed)
			host_logf(HOST_LOG_INFO, "theater: the screen stands in the room, %.1f m in front of where the head was",
				screen_distance);
	}
	return origin_from_screen;
}

void host_theater_screen(size_t index, simd_float4x4 *pose, simd_float2 *half_size)
{
	int width, height;

	host_theater_picture_size(&width, &height);
	*pose = screen_pose(index);
	*half_size = (simd_float2){ screen_width / 2.0f, screen_width / 2.0f * (float)height / (float)width };
}

int host_theater_frame_begin(int fresh)
{
	if (@available(visionOS 26.0, *))
	{
		if (open_frame && !fresh)
			return 1;
		/* a frame that began and never presented (a game frame that drew
		nothing): ended unpresented, so its old anchors aren't handed out */
		if (open_frame)
		{
			static unsigned long stale;

			if (stale++ == 0)
				host_logf(HOST_LOG_INFO, "theater: a frame began and never presented; it's ended unpresented");
			host_theater_frame_end();
		}
		foveation_state_logged();
		if (!host_theater_active() || cp_layer_renderer_get_state(layer_renderer) != cp_layer_renderer_state_running)
			return 0;
		/* waits for the Compositor's next frame, which paces the game to it */
		CFTimeInterval wait_began = CACurrentMediaTime();
		cp_frame_t frame = cp_layer_renderer_query_next_frame(layer_renderer);
		if (!frame)
			return 0;
		cp_frame_timing_t timing = cp_frame_predict_timing(frame);
		if (!timing)
			return 0;
		cp_frame_start_update(frame);
		cp_frame_end_update(frame);
		/* the poses are freshest at the optimal input time */
		cp_time_wait_until(cp_frame_timing_get_optimal_input_time(timing));
		frame_waited += CACurrentMediaTime() - wait_began;
		open_presentation = cp_time_to_cf_time_interval(cp_frame_timing_get_presentation_time(timing));
		cp_frame_start_submission(frame);
		cp_drawable_array_t drawables = cp_frame_query_drawables(frame);
		size_t count = cp_drawable_array_get_count(drawables);
		/* none: the frame was canceled and is discarded */
		if (count == 0)
			return 0;
		if (count > MAXIMUM_DRAWABLES)
			count = MAXIMUM_DRAWABLES;
		for (size_t index = 0; index < count; index++)
		{
			cp_drawable_t drawable = cp_drawable_array_get_drawable(drawables, index);

			open_drawables[index] = drawable;
			open_origin_from_device[index] = matrix_identity_float4x4;
			open_anchored[index] = NO;
			if (world_tracking)
			{
				ar_device_anchor_t anchor = ar_device_anchor_create();
				CFTimeInterval when = cp_time_to_cf_time_interval(cp_drawable_get_frame_timing(drawable) ?
					cp_frame_timing_get_presentation_time(cp_drawable_get_frame_timing(drawable)) :
					cp_frame_timing_get_presentation_time(timing));

				if (ar_world_tracking_provider_query_device_anchor_at_timestamp(world_tracking, when, anchor) ==
					ar_device_anchor_query_status_success)
				{
					open_origin_from_device[index] = ar_anchor_get_origin_from_anchor_transform(anchor);
					cp_drawable_set_device_anchor(drawable, anchor);
					open_anchored[index] = YES;
				}
			}
		}
		open_frame = frame;
		open_count = count;
		return 1;
	}
	return 0;
}

int host_theater_frame_ready(void)
{
	if (!open_frame)
		return 0;
	/* the space closing mid-frame: the frame is dropped, and
	host_theater_active returns the game to the window */
	if (!host_theater_active() || cp_layer_renderer_get_state(layer_renderer) != cp_layer_renderer_state_running)
	{
		frame_drop();
		return 0;
	}
	return 1;
}

size_t host_theater_drawable_count(void)
{
	return open_count;
}

cp_drawable_t host_theater_drawable(size_t index, simd_float4x4 *origin_from_device, int *anchored)
{
	if (origin_from_device)
		*origin_from_device = open_origin_from_device[index];
	if (anchored)
		*anchored = open_anchored[index];
	return open_drawables[index];
}

double host_theater_take_waited(void)
{
	CFTimeInterval waited = frame_waited;

	frame_waited = 0.0;
	return waited;
}

double host_theater_presentation_time(void)
{
	return open_frame ? open_presentation : 0.0;
}

int host_theater_frame_repeat(void)
{
	return layer_renderer ? cp_layer_renderer_get_minimum_frame_repeat_count(layer_renderer) : 0;
}

void host_theater_frame_end(void)
{
	if (open_frame)
		cp_frame_end_submission(open_frame);
	frame_drop();
}

void host_theater_frame_end_empty(id<MTLCommandQueue> queue, size_t first)
{
	for (size_t index = first; open_frame && index < open_count; index++)
	{
		cp_drawable_t drawable = open_drawables[index];
		id<MTLCommandBuffer> commands = [queue commandBuffer];

		for (size_t texture = 0; texture < cp_drawable_get_texture_count(drawable); texture++)
		{
			MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
			id<MTLTexture> color = cp_drawable_get_color_texture(drawable, texture);
			id<MTLTexture> depth = cp_drawable_get_depth_texture(drawable, texture);

			pass.colorAttachments[0].texture = color;
			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
			pass.depthAttachment.texture = depth;
			pass.depthAttachment.loadAction = MTLLoadActionClear;
			pass.depthAttachment.storeAction = MTLStoreActionStore;
			pass.depthAttachment.clearDepth = 0.0;
			if (depth.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
			{
				pass.stencilAttachment.texture = depth;
				pass.stencilAttachment.loadAction = MTLLoadActionClear;
				pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
			}
			/* (a layered drawable's views are its slices) */
			if (color.textureType == MTLTextureType2DArray)
				pass.renderTargetArrayLength = color.arrayLength;
			[[commands renderCommandEncoderWithDescriptor:pass] endEncoding];
		}
		cp_drawable_encode_present(drawable, commands);
		[commands commit];
	}
	host_theater_frame_end();
}

/* prepare, without the game thread waiting on Metal's compiler while
shaders compile off it (gpu_metal_async_compiles): the first call for a
pair of formats builds the pipelines on a worker, and until they're ready
the frame presents empty (host_theater_frame_end_empty). Nothing reads the
pipelines while prepare_building is set, and the worker clears it last. 1
ready, 0 failed (a failure isn't retried for the same formats), -1 building */
static _Atomic int prepare_building;
static BOOL prepare_failed;
static MTLPixelFormat prepare_failed_color, prepare_failed_depth;

static int prepare_ready(id<MTLDevice> device, MTLPixelFormat color, MTLPixelFormat depth)
{
	if (atomic_load(&prepare_building))
		return -1;
	if (!gpu_metal_async_compiles() || (pipeline && pipeline_color == color && pipeline_depth == depth))
		return prepare(device, color, depth) ? 1 : 0;
	if (prepare_failed && prepare_failed_color == color && prepare_failed_depth == depth)
		return 0;
	atomic_store(&prepare_building, 1);
	host_logf(HOST_LOG_INFO, "theater: building the screen's pipelines on a worker; empty frames until they're ready");
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^
	{
		@autoreleasepool
		{
			CFTimeInterval started = CACurrentMediaTime();
			BOOL built = prepare(device, color, depth);

			prepare_failed = !built;
			prepare_failed_color = color;
			prepare_failed_depth = depth;
			host_logf(HOST_LOG_INFO, "theater: the screen's pipelines %s after %.0f ms", built ? "are ready" : "failed",
				(CACurrentMediaTime() - started) * 1e3);
			atomic_store(&prepare_building, 0);
		}
	});
	return -1;
}

/* the script fade's tint for a target, premultiplied, with the intensity in
alpha; zero without a fade. Over the room (passthrough) it is the fade's
color (display-encoded, as the game's; decoded for an sRGB target) times its
intensity, blended over the room at that opacity. Over the dark surroundings
it is opaque, the game's own blend of the color over black: the color times
the intensity in display space, then decoded, so mid-fade the surroundings
are as bright as the picture's black faded toward the same color */
static simd_float4 fade_tint(const float *fade, uint32_t decode_srgb, BOOL over_dark)
{
	simd_float3 color;
	float intensity;

	if (!fade || !(fade[3] > 0.0f))
		return (simd_float4){ 0.0f, 0.0f, 0.0f, 0.0f };
	intensity = fminf(fade[3], 1.0f);
	color = simd_clamp((simd_float3){ fade[0], fade[1], fade[2] }, 0.0f, 1.0f);
	if (over_dark)
		color *= intensity;
	if (decode_srgb)
	{
		for (int channel = 0; channel < 3; channel++)
			color[channel] = color[channel] <= 0.04045f ? color[channel] / 12.92f :
				powf((color[channel] + 0.055f) / 1.055f, 2.4f);
	}
	if (over_dark)
		return (simd_float4){ color.x, color.y, color.z, intensity };
	return (simd_float4){ color.x * intensity, color.y * intensity, color.z * intensity, intensity };
}

simd_float4 host_theater_fade_tint(const float fade[4], uint32_t decode_srgb)
{
	simd_float4 tint = fade_tint(fade, decode_srgb, environment_dark);

	if (environment_dark && tint.w > 0.0f)
		tint.w = 1.0f;
	return tint;
}

float host_theater_fade_depth(simd_float4x4 projection)
{
	return fade_depth(projection, screen_distance + FADE_BEYOND_SCREEN);
}

/* the pictures on the screen for the open frame (or the next one): view 0's
the left one, the others the right; the HUD, if any, over each; the script
fade, if any (RGB and intensity), around the screen; the pictures at a
brightness */
static void present_pictures(id<MTLCommandQueue> queue, id<MTLTexture> left, id<MTLTexture> right,
	id<MTLTexture> hud, const float *fade, float brightness)
{
	if (@available(visionOS 26.0, *))
	{
		/* the frame stereo opened at the game's frame begin (in mono, when
		this frame presents mono after all: a menu, a load), or the next one */
		if (!host_theater_frame_begin(0) || !host_theater_frame_ready())
			return;
		/* (the pipelines building on a worker: prepare_ready) */
		if (atomic_load(&prepare_building))
		{
			host_theater_frame_end_empty(queue, 0);
			return;
		}
		size_t count = open_count;
		for (size_t index = 0; index < count; index++)
		{
			cp_drawable_t drawable = open_drawables[index];
			simd_float4x4 origin_from_device = open_origin_from_device[index];

			/* placed now unless stereo on the screen placed it at the frame's
			begin, to put the eyes' frusta through it */
			screen_pose(index);
			id<MTLTexture> first = cp_drawable_get_color_texture(drawable, 0);
			int ready = prepare_ready(queue.device, first.pixelFormat,
				cp_drawable_get_depth_texture(drawable, 0).pixelFormat);
			if (ready <= 0)
			{
				/* the submission has started: end it, as host_stereo_present does */
				if (ready < 0)
					host_theater_frame_end_empty(queue, index);
				else
					host_theater_frame_end();
				return;
			}
			id<MTLCommandBuffer> commands = [queue commandBuffer];
			size_t views = cp_drawable_get_view_count(drawable);
			for (size_t view_index = 0; view_index < views; view_index++)
			{
				cp_view_t view = cp_drawable_get_view(drawable, view_index);
				cp_view_texture_map_t map = cp_view_get_view_texture_map(view);
				size_t texture = cp_view_texture_map_get_texture_index(map);
				id<MTLTexture> color = cp_drawable_get_color_texture(drawable, texture);
				id<MTLTexture> depth = cp_drawable_get_depth_texture(drawable, texture);
				MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
				id<MTLTexture> picture = view_index == 0 ? left : right;
				struct screen_uniforms uniforms;
				simd_float4x4 origin_from_view = simd_mul(origin_from_device, cp_view_get_transform(view));
				simd_float4x4 projection = cp_drawable_compute_projection(drawable,
					cp_axis_direction_convention_right_up_back, view_index);

				pass.colorAttachments[0].texture = color;
				pass.colorAttachments[0].slice = cp_view_texture_map_get_slice_index(map);
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				uint32_t decode_srgb = color.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB ||
					color.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB;
				/* the fade's color as the target takes it: the game's colors are
				display-encoded, an sRGB target encodes what it's given */
				simd_float4 tint = fade_tint(fade, decode_srgb, environment_dark);

				pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, environment_dark ? 1.0 : 0.0);
				pass.depthAttachment.texture = depth;
				pass.depthAttachment.slice = cp_view_texture_map_get_slice_index(map);
				pass.depthAttachment.loadAction = MTLLoadActionClear;
				pass.depthAttachment.storeAction = MTLStoreActionStore;
				pass.depthAttachment.clearDepth = 0.0;
				if (depth.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
				{
					pass.stencilAttachment.texture = depth;
					pass.stencilAttachment.slice = cp_view_texture_map_get_slice_index(map);
					pass.stencilAttachment.loadAction = MTLLoadActionClear;
					pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
				}
				if (color.textureType == MTLTextureType2DArray)
					pass.renderTargetArrayLength = 1;
				/* foveated (HEAD mode's menus, loads and films): the screen and
				the fade's tint go through the view's rate map; the viewport stays
				the texture map's, the logical one in the map's screen coordinates */
				pass.rasterizationRateMap = host_theater_view_rate_map(drawable, map);
				uniforms.clip_from_screen = simd_mul(projection,
					simd_mul(simd_inverse(origin_from_view), origin_from_screen));
				uniforms.half_size = (simd_float2){ screen_width / 2.0f,
					screen_width / 2.0f * (float)picture.height / (float)picture.width };
				uniforms.decode_srgb = decode_srgb;
				uniforms.brightness = brightness;
				id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
				[encoder setViewport:cp_view_texture_map_get_viewport(map)];
				/* the room is the fade's color over it at the fade's opacity,
				premultiplied; the dark surroundings are its color times its
				intensity in display space, opaque (fade_tint). Either is a surface a little behind the
				screen, with that depth: the Compositor shows nothing where the
				depth is the clear's 0, or shows it only in patches */
				if (tint.w > 0.0f)
				{
					simd_float4 color = environment_dark ? (simd_float4){ tint.x, tint.y, tint.z, 1.0f } : tint;
					float behind = fade_depth(projection, screen_distance + FADE_BEYOND_SCREEN);

					[encoder setRenderPipelineState:fade_pipeline];
					[encoder setDepthStencilState:depth_fade];
					[encoder setVertexBytes:&behind length:sizeof(behind) atIndex:0];
					[encoder setFragmentBytes:&color length:sizeof(color) atIndex:0];
					[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
				}
				[encoder setRenderPipelineState:hud ? hud_pipeline : pipeline];
				[encoder setDepthStencilState:depth_state];
				[encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:0];
				[encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
				[encoder setFragmentTexture:picture atIndex:0];
				if (hud)
					[encoder setFragmentTexture:hud atIndex:1];
				[encoder setFragmentSamplerState:sampler atIndex:0];
				[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
				[encoder endEncoding];
			}
			cp_drawable_encode_present(drawable, commands);
			gpu_metal_count_gpu_time(commands);
			[commands commit];
		}
		host_theater_frame_end();
		if (left == right && theater_frames++ == 0)
			host_logf(HOST_LOG_INFO, "theater: first frame on the screen (%zu drawable%s)", count, count == 1 ? "" : "s");
	}
}

void host_theater_present(id<MTLCommandQueue> queue, id<MTLTexture> picture)
{
	present_pictures(queue, picture, picture, nil, NULL, 1.0f);
}

void host_theater_present_eyes(id<MTLCommandQueue> queue, id<MTLTexture> left, id<MTLTexture> right,
	id<MTLTexture> hud, const float fade[4], float brightness)
{
	static BOOL fading;

	if (screen_presents++ == 0)
		host_logf(HOST_LOG_INFO, "theater: first stereo frame on the screen: eyes %lux%lu, %s",
			(unsigned long)left.width, (unsigned long)left.height, hud ? "a HUD" : "no HUD");
	/* each script fade that reaches the room, once as it starts */
	if (fade && fade[3] > 0.0f && !fading)
		host_logf(HOST_LOG_INFO, "theater: a script fade tints the %s: %.2f %.2f %.2f at %.2f",
			environment_dark ? "dark" : "room", fade[0], fade[1], fade[2], fade[3]);
	fading = fade && fade[3] > 0.0f;
	present_pictures(queue, left, right, hud, fade, brightness);
}
