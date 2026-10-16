/*
RENDER.C

symbols in this file:
001743B0 0020:
	_render_effects (0000)
001743D0 0010:
	_render_initialize (0000)
001743E0 0010:
	_render_initialize_for_new_map (0000)
001743F0 0010:
	_render_dispose_from_old_map (0000)
00174400 0010:
	_render_dispose (0000)
00174410 0110:
	_render_nonplayer_frame (0000)
00174520 00f0:
	_render_frame_pregame (0000)
00174610 0020:
	_render_frame_present (0000)
00174630 0070:
	_render_location_visible (0000)
001746A0 0050:
	_rendered_cluster_get (0000)
001746F0 03f0:
	_render_window (0000)
00174AE0 03f0:
	_render_player_frame (0000)
00174ED0 00f0:
	_render_frame (0000)
0029F44C 001f:
	??_C@_0BP@JNDFGHKA@c?3?2halo?2SOURCE?2render?2render?4c?$AA@ (0000)
0029F470 0061:
	??_C@_0GB@NHJDHKGO@location?9?$DOcluster_index?$DO?$DN0?5?$CG?$CG?5lo@ (0000)
0029F4D8 0052:
	??_C@_0FC@DLFICDJP@rendered_cluster_index?$DO?$DN0?5?$CG?$CG?5ren@ (0000)
0029F530 0052:
	??_C@_0FC@NBMIPGBN@window?9?$DOrender_camera?4viewport_b@ (0000)
0029F588 0051:
	??_C@_0FB@IDPOBHAJ@window?9?$DOrender_camera?4viewport_b@ (0000)
0029F5DC 002c:
	??_C@_0CM@LKHLGDKF@window?9?$DOrender_camera?4viewport_b@ (0000)
0029F608 002c:
	??_C@_0CM@HBCHLAAA@window?9?$DOrender_camera?4viewport_b@ (0000)
0029F638 006d:
	??_C@_0GN@PKELCNNH@?$CBmemcmp?$CI?$CGwindow?9?$DOrender_camera?4w@ (0000)
0029F6A8 0071:
	??_C@_0HB@BCLIBPGM@?$CBmemcmp?$CI?$CGwindow?9?$DOrender_camera?4v@ (0000)
0029F720 0048:
	??_C@_0EI@NGOLLMDP@?$CD?$CD?$CD?5ERROR?5something?5is?5wrong?5wit@ (0000)
0030D4D2 0004:
	_render_contrails_enabled (0000)
	_render_particles_enabled (0001)
	_render_particle_systems_enabled (0002)
	_render_weather_particle_systems_enabled (0003)
004B8B22 0001:
	_render_invalid_fog_warning_displayed (0000)
*/

/* ---------- headers */

#include "cseries.h"
#include "cseries/errors.h"
#include "render.h"
#include "render_cameras_internal.h"
#include "render_particles.h"
#include "objects.h"
#include "render_sprite.h"
#include "scenario.h"
#include "structure_bsp_definitions.h"
#include "effects/player_effects.h"
#include "structures/structure_visibility.h"
#include "rasterizer.h"
#include "rasterizer/rasterizer_lights.h"
#include "profile.h"
#include "progress_bar.h"
#include "ui_widget.h"
#include "bink_playback.h"
#include "game.h"
#include "game_engine.h"
#include "interface/first_person_weapons.h"
#include "interface/interface.h"
#include "editor_stubs.h"
#include "render_debug.h"
#include "objects/object_lights_rendering.h"
#include "effects/particle_systems.h"
#include "effects/weather_particle_systems.h"
#include "main/main.h"
#include "main/console.h" /* port: stereo's UI quad (console_is_active) */
#include "game/players.h" /* port: stereo's zoomed pass (player_control_get_zoom_level) */
#include "structures/structures.h"

/* ---------- constants */

enum
{
	_render_target_primary = 0,
	_render_target_secondary,
};

enum
{
	_decal_layer_primary = 0,
	_decal_layer_secondary,
	_decal_layer_light,
	_decal_layer_alpha_tested,
	_decal_layer_water,
};

enum
{
	_render_planar_fog_mode_fully_fogged = 2,
};

/* ---------- macros */

#define RASTERIZER_TARGET_RENDER_PRIMARY_WIDTH halo_screen_width()
#define RASTERIZER_TARGET_RENDER_PRIMARY_HEIGHT 480

/* ---------- structures */

/* ---------- prototypes */

static void render_nonplayer_frame(
	const struct render_window *window,
	long window_type);
static void render_window(
	short local_player_index,
	const struct render_camera *camera,
	const struct render_frustum *frustum,
	const struct render_camera *rasterizer_camera,
	const struct render_frustum *rasterizer_frustum,
	short rasterizer_target,
	boolean has_mirror);
static void render_player_frame(
	struct render_window *window,
	const point2d *screenshot_combined_index);
static void render_player_frame_stereo(
	struct render_window *window,
	struct render_camera *camera);

void render_sky(
	void);

/* ---------- globals */

struct render_globals render;

static boolean render_invalid_fog_warning_displayed;

/* port: TRUE once render_player_frame_stereo drew the eyes this frame */
static boolean render_stereo_eyes_drawn;
/* port: in a stereo eye's render_window, the camera whose apex is the culling
frustum's (render_player_frame_stereo), else NULL */
static const struct render_camera *render_stereo_visibility_camera;
/* port: in a stereo eye's render_window, the frame's one screen flash
(render_player_frame_stereo), else NULL */
static const struct render_screen_flash *render_stereo_screen_flash;

extern short global_screenshot_count;

boolean render_contrails_enabled = TRUE;
boolean render_particles_enabled = TRUE;
boolean render_particle_systems_enabled = TRUE;
boolean render_weather_particle_systems_enabled = TRUE;

/* ---------- public code */

void render_effects(
	boolean enable)
{
	render_weather_particle_systems_enabled = enable;
	render_particle_systems_enabled = enable;
	render_particles_enabled = enable;
	render_contrails_enabled = enable;
}

void render_initialize(
	void)
{
	render_objects_initialize();
}

void render_initialize_for_new_map(
	void)
{
	render_objects_initialize_for_new_map();
}

void render_dispose_from_old_map(
	void)
{
	render_objects_dispose_from_old_map();
}

void render_dispose(
	void)
{
	render_objects_dispose();
}

static void render_nonplayer_frame(
	const struct render_window *window,
	long window_type)
{
	struct rasterizer_window_begin_parameters parameters;

	profile_render_window_start(FALSE);
	memset(&parameters, 0, sizeof(parameters));

	render.camera = window->render_camera;
	render_camera_build_frustum(&render.camera, NULL, &render.frustum, TRUE);

	parameters.camera = window->rasterizer_camera;
	render_camera_build_frustum(&parameters.camera, NULL, &parameters.frustum, TRUE);

	parameters.rasterizer_target = _render_target_primary;
	parameters.suppress_clear = window_type == 0;
	parameters.window_index = NONE;
	rasterizer_window_begin(&parameters);

	switch (window_type)
	{
	case 0:
		/* letterbox bars and split screen dividers: laid out from the
		viewport, so not centered like the menus on a wide screen */
		interface_draw_fullscreen_overlays();
		rasterizer_debug_draw();
		break;

	case 1:
		halo_screen_ui_offset(TRUE);
		game_engine_nonplayer_post_rasterize();
		halo_screen_ui_offset(FALSE);
		break;

	default:
		match_assert("c:\\halo\\SOURCE\\render\\render.c", 287, !"unreachable");
		break;
	}

	rasterizer_window_end();
	profile_render_window_end();

	return;
}

void render_frame_pregame(
	struct render_window const *window,
	struct bitmap_data *bitmap)
{
	struct rasterizer_frame_begin_parameters parameters;
	struct rasterizer_window_begin_parameters rasterizer_parameters;

	render.frame_index++;

	rasterizer_frame_begin(&parameters);
	rasterizer_windows_begin();
	profile_render_window_start(FALSE);

	memset(&rasterizer_parameters, 0, sizeof(rasterizer_parameters));

	render.camera = window->render_camera;
	render_camera_build_frustum(&render.camera, NULL, &render.frustum, TRUE);

	rasterizer_parameters.camera = window->rasterizer_camera;
	render_camera_build_frustum(&rasterizer_parameters.camera, NULL, &rasterizer_parameters.frustum, TRUE);

	rasterizer_parameters.rasterizer_target = 0;
	rasterizer_window_begin(&rasterizer_parameters);

	halo_screen_ui_offset(TRUE);
	render_ui_widgets(0, &window->rasterizer_camera.viewport_bounds);
	halo_screen_ui_offset(FALSE);
	bink_playback_render();

	{
		real progress;
		/* port: the loading screen's own progress while it holds a level's
		load (port/linux/game/loading_screen.c) */
		if (halo_loading_screen_progress(&progress) || game_map_loading_in_progress(&progress))
		{
			progress_bar_display(progress);
		}
	}

	rasterizer_window_end();
	profile_render_window_end();
	rasterizer_windows_end();
	rasterizer_frame_end();

	return;
}

void render_frame_present(
	const point2d *screenshot_index,
	struct bitmap_data *bitmap)
{
	rasterizer_present(bitmap, screenshot_index);

	return;
}

boolean render_location_visible(
	struct location *location)
{
	match_assert("c:\\halo\\SOURCE\\render\\render.c", 584, location->cluster_index>=0 && location->cluster_index<global_structure_bsp_get()->clusters.count);
	return TEST_FLAG(render.visible_cluster_flags[location->cluster_index>>5], location->cluster_index&31);
}

struct rendered_cluster *rendered_cluster_get(
	short rendered_cluster_index)
{
	match_assert("c:\\halo\\SOURCE\\render\\render.c", 592, rendered_cluster_index>=0 && rendered_cluster_index<render.rendered_cluster_count);
	return &render.rendered_clusters[rendered_cluster_index];
}

static void render_window(
	short local_player_index,
	const struct render_camera *camera,
	const struct render_frustum *frustum,
	const struct render_camera *rasterizer_camera,
	const struct render_frustum *rasterizer_frustum,
	short rasterizer_target,
	boolean has_mirror)
{
	struct rasterizer_window_begin_parameters parameters;
	short rendered_cluster_index;

	profile_render_window_start(TRUE);
	render.scene_index++;
	memset(&parameters, 0, sizeof(parameters));

	render.local_player_index = local_player_index;
	render.camera = *camera;
	render.frustum = *frustum;
	parameters.camera = *rasterizer_camera;
	parameters.frustum = *rasterizer_frustum;
	parameters.rasterizer_target = rasterizer_target;
	parameters.has_mirror = has_mirror;
	parameters.window_index = render.window_index;
	parameters.fog = render.fog;

	/* port: in stereo, render.frustum is the culling frustum, whose apex sits
	behind the camera so that it holds both eyes (a little behind it: the
	eyes sit beside the camera, within a few hundredths of a unit); the
	portal traversal and the clusters' frusta project with render.camera and
	render.frustum together, so they take the culling frustum's camera too.
	The cluster stays the camera's own (structure_visibility_find_camera),
	and render.camera is the camera again for the rest */
	if (render_stereo_visibility_camera)
	{
		struct render_camera saved_camera = render.camera;

		render.camera = *render_stereo_visibility_camera;
		structure_visibility_compute();
		render.camera = saved_camera;
	}
	else
		structure_visibility_compute();
	/* port: a stereo eye takes the frame's flash: the game's function ends
	a damage flash's time each call, once per frame in mono */
	if (render_stereo_screen_flash)
		parameters.screen_flash = *render_stereo_screen_flash;
	else
		player_effect_get_screen_flash(local_player_index, &parameters.screen_flash);
	/* port: a campaign level's frames before its first ticks, black as the
	stereo eyes' are (halo_stereo.h) */
	if (!render_stereo_screen_flash && halo_level_start_hold_flat())
	{
		parameters.screen_flash.type = 1;
		parameters.screen_flash.color.alpha = 1.0f;
		parameters.screen_flash.color.red = parameters.screen_flash.color.green = parameters.screen_flash.color.blue = 0.0f;
		parameters.screen_flash.intensity = 1.0f;
	}
	/* port: the view's scene begins (display.native_hud) */
	halo_render_scene_begin(rasterizer_target, rasterizer_camera->viewport_bounds.x0,
		rasterizer_camera->viewport_bounds.y0, rasterizer_camera->viewport_bounds.x1,
		rasterizer_camera->viewport_bounds.y1);
	rasterizer_window_begin(&parameters);

	if (!bink_playback_in_progress())
	{
		build_sprite_prepare_for_window();
		render_sky();
		first_person_weapon_render_update();
		lights_preprocess_scene();
		render_objects();
		structure_render_preprocess();
		structure_render_lightmaps();
		rasterizer_lens_flares_submit_occlusion_tests();
		render_object_shadows();
		lights_render_diffuse();

		rasterizer_decals_begin(_decal_layer_light);
		for (rendered_cluster_index = 0;
			rendered_cluster_index < render.rendered_cluster_count;
			rendered_cluster_index++)
		{
			rasterizer_decals_draw(rendered_cluster_get(rendered_cluster_index)->cluster_index);
		}
		rasterizer_decals_end();

		rasterizer_decals_begin(_decal_layer_alpha_tested);
		for (rendered_cluster_index = 0;
			rendered_cluster_index < render.rendered_cluster_count;
			rendered_cluster_index++)
		{
			rasterizer_decals_draw(rendered_cluster_get(rendered_cluster_index)->cluster_index);
		}
		rasterizer_decals_end();

		structure_render_diffuse_texture();

		rasterizer_decals_begin(_decal_layer_primary);
		for (rendered_cluster_index = 0;
			rendered_cluster_index < render.rendered_cluster_count;
			rendered_cluster_index++)
		{
			rasterizer_decals_draw(rendered_cluster_get(rendered_cluster_index)->cluster_index);
		}
		rasterizer_decals_end();

		rasterizer_decals_begin(_decal_layer_secondary);
		for (rendered_cluster_index = 0;
			rendered_cluster_index < render.rendered_cluster_count;
			rendered_cluster_index++)
		{
			rasterizer_decals_draw(rendered_cluster_get(rendered_cluster_index)->cluster_index);
		}
		rasterizer_decals_end();

		lights_render_specular();
		structure_render_specular_lightmaps();
		structure_render_reflection_lightmap_masks();
		structure_render_reflection_mirrors();
		structure_render_reflections();
		structure_render_transparent_geometry();
		structure_render_fog();
		game_engine_post_rasterize_objects();
		weather_particle_systems_render();
		render_particles();
		particle_systems_render();
		render_contrails_normal();
		rasterizer_transparent_geometry_draw(TRUE);

		rasterizer_decals_begin(_decal_layer_water);
		for (rendered_cluster_index = 0;
			rendered_cluster_index < render.rendered_cluster_count;
			rendered_cluster_index++)
		{
			rasterizer_decals_draw(rendered_cluster_get(rendered_cluster_index)->cluster_index);
		}
		rasterizer_decals_end();

		structure_render_detail_objects();
		rasterizer_transparent_geometry_draw(FALSE);
		rasterizer_transparent_geometry_stop();
		structure_render_fog_screen();
		rasterizer_lens_flares_draw();
		halo_render_before_hud(local_player_index, rasterizer_target, &rasterizer_camera->viewport_bounds);
		/* port: in stereo the screen effects and the flash (the script fade
		too) run per eye, so the picture carries them as in mono; the HUD
		and the widgets draw once, in the HUD pass, which repeats the flash
		over what the HUD covers (render_player_frame_stereo) */
		if (halo_stereo_current_layer() == HALO_STEREO_LAYER_MONO)
		{
			/* port: interface_draw_screen's two halves, with the scene's
			end between them: the weapon's screen effects read the scene and
			are part of it; the HUD, the flash and the menus are drawn over
			it (display.native_hud) */
			interface_draw_screen_effects();
			halo_render_scene_end();
			interface_draw_hud();
			rasterizer_screen_flash();
			halo_screen_ui_offset(TRUE);
			render_ui_widgets(local_player_index, &rasterizer_camera->viewport_bounds);
			halo_screen_ui_offset(FALSE);
		}
		else
		{
			interface_draw_screen_effects();
			/* the zoomed pass takes its flash after the HUD pass draws the
			zoomed view's elements into it, over them as in mono */
			if (halo_stereo_current_layer() != HALO_STEREO_LAYER_ZOOM)
				rasterizer_screen_flash();
		}
	}

	/* port: a view with a movie in the world's place has no scene (and a
	stereo eye's ends here) */
	halo_render_scene_end();
	/* port: in stereo the movie draws once, in the HUD pass: each call can
	decode a frame and wait for the next */
	if (halo_stereo_current_layer() == HALO_STEREO_LAYER_MONO)
		bink_playback_render();
	render_camera_debug_frustum(&render.camera, &render.frustum);
	render_debug();
	editor_render();
	rasterizer_debug_draw();
	rasterizer_window_end();
	profile_render_window_end();

	return;
}

/* port: one window of a stereo frame (render_player_frame). Frustum bounds
are tangents divided by the center camera's: x by the aspect times the
vertical field of view's half tangent, y by that tangent alone. */
static void render_player_frame_stereo(
	struct render_window *window,
	struct render_camera *camera)
{
	const struct halo_stereo_frame *stereo;
	real aspect;
	real field_of_view_tangent;
	real_rectangle2d cull_bounds;
	struct render_camera cull_camera;
	struct render_frustum cull_frustum;
	real minimum_horizontal_tangent;
	real minimum_vertical_tangent;
	real maximum_offset_x;
	real maximum_offset_y;
	real maximum_offset_back;
	real cull_distance_back;
	real_vector3d right;
	real_vector3d up;
	short eye;
	struct rasterizer_window_begin_parameters parameters;
	real time_delta_since_tick_sec;
	struct render_screen_flash screen_flash;
	float fade[4];
	boolean zoomed;
	short first_pass;
	short pass_end;
	struct render_camera zoom_camera;
	struct render_camera zoom_rasterizer_camera;
	real_rectangle2d zoom_bounds;
	struct render_frustum zoom_frustum;

	stereo = halo_stereo_frame();
	/* the zoom (halo_stereo.h): while zoomed in HEAD mode's full view, one
	mono pass of the zoomed camera fills the view in place of the eyes. Its
	cameras are the game's before the head turns them below
	(halo_stereo_zoom_orient turns them on foot) */
	zoom_camera = *camera;
	zoom_rasterizer_camera = window->rasterizer_camera;
	halo_stereo_zoom_orient(&zoom_camera.forward.i, &zoom_camera.up.i);
	halo_stereo_zoom_orient(&zoom_rasterizer_camera.forward.i, &zoom_rasterizer_camera.up.i);
	/* head-tracked stereo: the cameras turn by the head's turn the look takes
	in next frame, and tilt by its roll (the render's alone; the aim has no
	roll), so the picture matches the pose the presenter hands the
	Compositor. Both cameras, so culling and the HUD's projections match */
	halo_stereo_head_orient(&camera->forward.i, &camera->up.i);
	halo_stereo_head_orient(&window->rasterizer_camera.forward.i, &window->rasterizer_camera.up.i);
	/* (after the head's turn, which sets where a seat's reticle points) */
	zoomed = halo_stereo_zoom_begin(player_control_get_zoom_level(window->local_player_index) != NONE);
	/* the eyes (passes 0 and 1), or while zoomed the zoomed pass alone (2) */
	first_pass = zoomed ? 2 : 0;
	pass_end = zoomed ? 3 : 2;
	/* the presenter's depth range (d3d8_device.c): the eyes' planes */
	halo_stereo_set_depth_range(window->rasterizer_camera.z_near, window->rasterizer_camera.z_far);
	aspect = (real)(window->rasterizer_camera.viewport_bounds.x1 - window->rasterizer_camera.viewport_bounds.x0) /
		(real)(window->rasterizer_camera.viewport_bounds.y1 - window->rasterizer_camera.viewport_bounds.y0);
	field_of_view_tangent = tangent(window->rasterizer_camera.vertical_field_of_view * 0.5f);
	/* the zoomed pass's frustum: symmetric over the view the eyes span
	(halo_stereo_zoom_view), each tangent over the zoom's magnification, so
	its picture shown over the view is that many times the world's angular
	scale. It culls with its own (render_player_frame does), so a seat's
	zoom, along the gun, sees what the head has turned away from */
	{
		real view[2];
		real magnification = halo_zoom_magnification(window->local_player_index);

		halo_stereo_zoom_view(view);
		if (!(magnification >= 1.0f))
			magnification = 1.0f;
		zoom_bounds.x1 = view[0] / magnification / (aspect * field_of_view_tangent);
		zoom_bounds.x0 = -zoom_bounds.x1;
		zoom_bounds.y1 = view[1] / magnification / field_of_view_tangent;
		zoom_bounds.y0 = -zoom_bounds.y1;
		render_camera_build_frustum(&zoom_camera, &zoom_bounds, &zoom_frustum, TRUE);
	}
	/* the screen's eyes (the 3D film's, SCREEN gameplay's) take their
	frusta from the camera's field of view (stereo.c); nothing otherwise */
	halo_stereo_screen_frusta(field_of_view_tangent);
	/* debug.gpu_stats: the film camera's field of view, each film frame */
	halo_stereo_log_film_camera(&camera->position.x, &camera->forward.i,
		window->rasterizer_camera.vertical_field_of_view);

	/* culling: one frustum that contains both eyes' exactly. Its bounds are
	the union of the eyes' tangents; its apex is the center camera moved back
	along forward until every eye's apex is inside it. An eye offset by x to
	the side sits inside a plane of tangent t once the apex is x / t behind it,
	and the narrowest tangent is the worst case, so back by the larger of
	max|x| / min(horizontal tangent) and max|y| / min(vertical tangent), plus
	any eye's own offset back. render.camera stays the center camera (level of
	detail, sprites); only the planes move (the pixel scale is mono's, below). */
	cull_bounds.x0 = -MAX(stereo->eyes[0].left, stereo->eyes[1].left) / (aspect * field_of_view_tangent);
	cull_bounds.x1 = MAX(stereo->eyes[0].right, stereo->eyes[1].right) / (aspect * field_of_view_tangent);
	cull_bounds.y0 = -MAX(stereo->eyes[0].down, stereo->eyes[1].down) / field_of_view_tangent;
	cull_bounds.y1 = MAX(stereo->eyes[0].up, stereo->eyes[1].up) / field_of_view_tangent;
	minimum_horizontal_tangent = MIN(
		MIN(stereo->eyes[0].left, stereo->eyes[1].left),
		MIN(stereo->eyes[0].right, stereo->eyes[1].right));
	minimum_vertical_tangent = MIN(
		MIN(stereo->eyes[0].down, stereo->eyes[1].down),
		MIN(stereo->eyes[0].up, stereo->eyes[1].up));
	maximum_offset_x = MAX(ABS(stereo->eyes[0].offset[0]), ABS(stereo->eyes[1].offset[0]));
	maximum_offset_y = MAX(ABS(stereo->eyes[0].offset[1]), ABS(stereo->eyes[1].offset[1]));
	maximum_offset_back = MAX(0.0f, MAX(stereo->eyes[0].offset[2], stereo->eyes[1].offset[2]));
	cull_distance_back = maximum_offset_back + MAX(
		minimum_horizontal_tangent > 0.0f ? maximum_offset_x / minimum_horizontal_tangent : 0.0f,
		minimum_vertical_tangent > 0.0f ? maximum_offset_y / minimum_vertical_tangent : 0.0f);
	cull_camera = *camera;
	cull_camera.position.x -= cull_camera.forward.i * cull_distance_back;
	cull_camera.position.y -= cull_camera.forward.j * cull_distance_back;
	cull_camera.position.z -= cull_camera.forward.k * cull_distance_back;
	cull_camera.z_far += cull_distance_back;
	render_camera_build_frustum(&cull_camera, &cull_bounds, &cull_frustum, TRUE);
	/* port: what the game sizes in pixels (model detail, the model cull,
	particles, sprites) reads the culling frustum's pixel scale, which the
	union's bounds would make about half mono's: keep mono's, times
	display.lod_scale (stereo_lod.c). The planes stay the union's. The
	zoomed pass, which culls with its own frustum, keeps mono's zoomed scale
	the same way */
	if (zoomed)
		halo_stereo_lod_projection(&zoom_camera, &zoom_frustum);
	else
		halo_stereo_lod_projection(&cull_camera, &cull_frustum);
	halo_stereo_log_culling(cull_distance_back);

	cross_product3d(&window->rasterizer_camera.forward, &window->rasterizer_camera.up, &right);
	normalize3d(&right);
	cross_product3d(&right, &window->rasterizer_camera.forward, &up);
	normalize3d(&up);

	/* the frame's screen flash, once: both eyes and the HUD pass draw it.
	The game's function ends a damage flash's time by the frame's at each
	call. While a script fade is up, its intensity is taken at the picture's
	time (the tick fraction), which the host's room tint takes as well, so
	the picture's fade and the room's stay in step */
	memset(&screen_flash, 0, sizeof(screen_flash));
	player_effect_get_screen_flash(window->local_player_index, &screen_flash);
	if (halo_screen_fade(render_interpolation_fraction(), fade))
		screen_flash.intensity = fade[3];
	/* port: a level's frames before its first tick (halo_stereo.h): black,
	drawn as the script's fade to black is (type 1, lighten toward the color) */
	if (halo_stereo_level_start_hold())
	{
		fade[0] = fade[1] = fade[2] = 0.0f;
		fade[3] = 1.0f;
		screen_flash.type = 1;
		screen_flash.color.alpha = 1.0f;
		screen_flash.color.red = screen_flash.color.green = screen_flash.color.blue = 0.0f;
		screen_flash.intensity = 1.0f;
	}
	halo_stereo_set_fade(fade);

	time_delta_since_tick_sec = render.time_delta_since_tick_sec;
	/* the eyes, or the zoomed pass (pass 2) alone */
	for (eye = first_pass; eye < pass_end; eye++)
	{
		struct render_camera eye_camera;
		real_rectangle2d eye_bounds;
		struct render_frustum eye_frustum;
		struct render_mirror mirror;
		boolean has_mirror = FALSE;

		if (eye < 2)
		{
			const struct halo_stereo_eye *stereo_eye = &stereo->eyes[eye];

			eye_camera = window->rasterizer_camera;
			/* the offset is right, up and back in the camera's frame */
			eye_camera.position.x += right.i * stereo_eye->offset[0] + up.i * stereo_eye->offset[1] -
				eye_camera.forward.i * stereo_eye->offset[2];
			eye_camera.position.y += right.j * stereo_eye->offset[0] + up.j * stereo_eye->offset[1] -
				eye_camera.forward.j * stereo_eye->offset[2];
			eye_camera.position.z += right.k * stereo_eye->offset[0] + up.k * stereo_eye->offset[1] -
				eye_camera.forward.k * stereo_eye->offset[2];
			eye_bounds.x0 = -stereo_eye->left / (aspect * field_of_view_tangent);
			eye_bounds.x1 = stereo_eye->right / (aspect * field_of_view_tangent);
			eye_bounds.y0 = -stereo_eye->down / field_of_view_tangent;
			eye_bounds.y1 = stereo_eye->up / field_of_view_tangent;
		}
		else
		{
			/* the zoomed pass: the zoomed camera over the view */
			eye_camera = zoom_rasterizer_camera;
			eye_bounds = zoom_bounds;
		}
		render_camera_build_frustum(&eye_camera, &eye_bounds, &eye_frustum, TRUE);

		/* the layer is set before the mirror, so a screen-sized target the
		mirror pass touches is this eye's */
		halo_stereo_layer(eye < 2 ? eye : HALO_STEREO_LAYER_ZOOM);
		/* port: once per frame in stereo: what advances by the frame's time
		while it renders (glow, the sky's animation, weather) advances in eye
		0 only, and eye 1 (its mirror too) draws the same moment
		(halo_stereo_repeat_pass); the zoomed pass is a zoomed frame's only
		one, and advances it */
		if (eye == 1)
			render.time_delta_since_tick_sec = 0.0f;
		/* port: and what renders from the local random seed (a lightning
		bolt's shape and jitter) draws the same numbers in every pass, so eye
		1 sees the bolt eye 0 does. That holds only for draws made before the
		first consumer that runs in eye 0 alone: weather's spawns (by the
		time delta, zero in later passes) and the fog screen's wind take
		numbers in eye 0 only, so anything drawn after them gets different
		numbers in a later pass. Lightning draws during the object pass,
		before both. The fog screen's own new layer offsets don't matter: a
		repeat pass's fog screen state is put back when its window ends
		(rasterizer_xbox_environment_fog.c). A consumer added later than
		those needs its own per-frame seed, as Task 4's review suggested. The
		zoomed pass, a frame's only one, takes the seed as mono does */
		if (!zoomed)
			halo_render_random_stereo_pass(eye); /* render_random.c */
		/* the mirror's render_window runs in the eye's layer, so it skips
		render_ui_widgets, deliberately: they draw once, in the HUD pass. It
		takes the flash as mono's mirror does (the game's function, for no
		player: the script fade alone) */
		if (structure_visibility_find_mirror(&eye_camera, &eye_frustum, &mirror))
		{
			short saved_cluster_index;
			struct render_camera mirror_camera;
			struct render_frustum mirror_frustum;
			real_rectangle2d mirror_bounds;

			saved_cluster_index = (short)render.cluster_index;
			render_camera_mirror(&eye_camera, &mirror, &mirror_camera);
			/* port: render_camera_mirror's reflecting branch negates the
			reflected up (right' = reflect(right), up' = -reflect(up)); its
			refracting branch (index_of_refraction != 0) doesn't negate up,
			so what follows holds for reflections. The eye's ray at vertical
			tangent y leaves the mirror at -y, and the floor's lookup of the
			target (by the eye's screen position) is flipped to match. Mono's
			bounds are symmetric, so reusing them was exact; an eye's need not
			be (the headset's up and down differ), and then the reflection
			sits a fixed angle off and slides against the floor as the head
			moves. The vertical pair flips; the horizontal pair stays */
			mirror_bounds = eye_bounds;
			mirror_bounds.y0 = -eye_bounds.y1;
			mirror_bounds.y1 = -eye_bounds.y0;
			render_camera_build_frustum(
				&mirror_camera,
				&mirror_bounds,
				&mirror_frustum,
				TRUE);

			rasterizer_profile_enable(FALSE);
			render.cluster_index = mirror.cluster_index;
			render_window(
				NONE,
				&mirror_camera,
				&mirror_frustum,
				&mirror_camera,
				&mirror_frustum,
				_render_target_secondary,
				FALSE);
			render.cluster_index = saved_cluster_index;
			rasterizer_profile_enable(TRUE);
			has_mirror = TRUE;
		}

		render_stereo_visibility_camera = eye < 2 ? &cull_camera : NULL;
		render_stereo_screen_flash = &screen_flash;
		/* port: the eye's position, for what sits at infinity in each eye
		(the sky, render_sky); not the zoomed mono pass, and set only now,
		so the mirror's window above keeps its own camera */
		halo_stereo_set_eye_position(eye < 2 ? &eye_camera.position : NULL);
		render_window(
			window->local_player_index,
			eye < 2 ? camera : &zoom_camera,
			eye < 2 ? &cull_frustum : &zoom_frustum,
			&eye_camera,
			&eye_frustum,
			_render_target_primary,
			has_mirror);
		halo_stereo_set_eye_position(NULL);
		render_stereo_screen_flash = NULL;
		render_stereo_visibility_camera = NULL;
	}
	render.time_delta_since_tick_sec = time_delta_since_tick_sec;
	/* port: the frame leaves the seed where eye 0 left it (render_random.c) */
	if (!zoomed)
		halo_render_random_stereo_end();

	/* the HUD pass: the presenter blends its layer over each eye by its
	alpha, the picture's transmittance (d3d8_device.c, hud_layer_blend). The
	empty clear relies on the zeroed fog color (black) and on
	real_rgb_color_to_pixel32 leaving alpha at 0, which the layer takes as
	nothing drawn. While a movie plays, it
	draws here, once, as a flat picture for both eyes (as render_window
	draws it in mono: in place of the HUD) */
	halo_stereo_layer(HALO_STEREO_LAYER_HUD);
	/* port: each HUD group's rectangle starts empty (halo_stereo.h) */
	halo_hud_group_frame_begin();
	/* port: the HUD pass's projection (render_camera_build_frustum without
	bounds: symmetric, the camera's vertical field of view and the
	viewport's shape), for the presenter's catch-all quad */
	halo_stereo_set_hud_tangents(aspect * field_of_view_tangent, field_of_view_tangent);
	memset(&parameters, 0, sizeof(parameters));
	render.local_player_index = window->local_player_index;
	render.camera = *camera;
	render.frustum = cull_frustum;
	parameters.camera = window->rasterizer_camera;
	render_camera_build_frustum(&parameters.camera, NULL, &parameters.frustum, TRUE);
	parameters.rasterizer_target = _render_target_primary;
	parameters.window_index = render.window_index;
	parameters.screen_flash = screen_flash;
	rasterizer_window_begin(&parameters);
	if (!bink_playback_in_progress())
	{
		/* (while zoomed, the zoomed view's elements draw into the zoomed
		picture, halo_stereo_zoom_overlay, and the crosshairs into the
		reticle's layer as over the eyes) */
		interface_draw_hud();
		/* port: the flash over the HUD layer, and over each HUD group's
		target and the reticle's layer that drew this frame, as mono's is
		over the one HUD; it covers the whole layer, so it doesn't count
		toward a group's rectangle (halo_stereo.h) */
		halo_hud_group_measure(FALSE);
		rasterizer_screen_flash();
		if (halo_stereo_hud_split())
		{
			int group;

			for (group = 0; group < HALO_HUD_GROUP_COUNT; group++)
			{
				float rectangle[4];

				if (!halo_hud_group_rectangle(group, rectangle))
					continue;
				halo_hud_group_begin(group);
				rasterizer_screen_flash();
				halo_hud_group_end();
			}
		}
		if (halo_stereo_reticle_drawn())
		{
			halo_stereo_layer(HALO_STEREO_LAYER_RETICLE);
			rasterizer_screen_flash();
			halo_stereo_layer(HALO_STEREO_LAYER_HUD);
		}
		halo_hud_group_measure(TRUE);
		/* the zoomed picture's flash, over the zoomed view's elements as
		mono's is over the HUD */
		if (zoomed)
		{
			halo_stereo_layer(HALO_STEREO_LAYER_ZOOM);
			rasterizer_screen_flash();
			halo_stereo_layer(HALO_STEREO_LAYER_HUD);
		}
		halo_screen_ui_offset(TRUE);
		/* port: a menu drawn here goes on the UI's quad (halo_stereo_set_ui_span) */
		halo_stereo_set_ui_span(TRUE);
		render_ui_widgets(window->local_player_index, &window->rasterizer_camera.viewport_bounds);
		halo_stereo_set_ui_span(FALSE);
		halo_screen_ui_offset(FALSE);
	}
	/* port: a movie (the intro, the credits) goes on the UI's quad, as a menu does */
	halo_stereo_set_ui_span(TRUE);
	bink_playback_render();
	halo_stereo_set_ui_span(FALSE);
	rasterizer_window_end();
	halo_stereo_layer(HALO_STEREO_LAYER_MONO);
	render_stereo_eyes_drawn = TRUE;

	return;
}

static void render_player_frame(
	struct render_window *window,
	const point2d *screenshot_combined_index)
{
	struct render_camera *camera;
	boolean has_mirror;
	real_rectangle2d frustum_bounds;
	struct render_frustum frustum;
	struct render_frustum rasterizer_frustum;
	struct render_mirror mirror;

	camera = &window->render_camera;
	has_mirror = FALSE;

	structure_visibility_find_camera(camera);
	render.fog.runtime_flags = 0;
	scenario_get_atmospheric_fog(
		window->local_player_index,
		(word)render.visible_sky_index,
		&camera->position,
		&render.fog);
	structure_get_planar_fog((short)render.cluster_index, &render.fog);

	if (render.fog.atmospheric_maximum_distance != 0.0f &&
		render.visible_sky_index == NONE &&
		render.fog.planar_maximum_distance > render.fog.atmospheric_maximum_distance)
	{
		render.fog.planar_maximum_distance = render.fog.atmospheric_maximum_distance;
	}

	if (render.fog.atmospheric_maximum_density == 1.0f &&
		render.fog.atmospheric_maximum_distance != 0.0f)
	{
		window->render_camera.z_far = MIN(
			window->render_camera.z_far,
			render.fog.atmospheric_maximum_distance);
	}

	if (render.fog.planar_mode == _render_planar_fog_mode_fully_fogged &&
		render.fog.planar_maximum_distance != 0.0f)
	{
		window->render_camera.z_far = MIN(
			window->render_camera.z_far,
			render.fog.planar_maximum_distance);
	}

	if (window->render_camera.z_far <= window->render_camera.z_near)
	{
		if (!render_invalid_fog_warning_displayed)
		{
			error(2, "### ERROR something is wrong with the fog in the sky tag or the fog tag");
			render_invalid_fog_warning_displayed = TRUE;
		}

		window->render_camera.z_far = window->render_camera.z_near + 0.01f;
	}

	match_assert(
		"c:\\halo\\SOURCE\\render\\render.c",
		187,
		!memcmp(&window->render_camera.viewport_bounds,
			&window->rasterizer_camera.viewport_bounds,
			sizeof(rectangle2d)));
	match_assert(
		"c:\\halo\\SOURCE\\render\\render.c",
		188,
		!memcmp(&window->render_camera.window_bounds,
			&window->rasterizer_camera.window_bounds,
			sizeof(rectangle2d)));

	render_camera_build_frustum_bounds(camera, &frustum_bounds);

	if (screenshot_combined_index != NULL)
	{
		long tile_count;

		tile_count = global_screenshot_count * global_screenshot_size;
		if (tile_count > 0)
		{
			real tile_width;
			real tile_height;
			real_rectangle2d adjusted_bounds;

			tile_width = (frustum_bounds.x1 - frustum_bounds.x0) / (real)tile_count;
			tile_height = (frustum_bounds.y1 - frustum_bounds.y0) / (real)tile_count;
			adjusted_bounds.x0 =
				(real)screenshot_combined_index->x * tile_width + frustum_bounds.x0;
			adjusted_bounds.y0 =
				(real)(tile_count - screenshot_combined_index->y - 1) * tile_height + frustum_bounds.y0;
			adjusted_bounds.x1 = adjusted_bounds.x0 + tile_width;
			adjusted_bounds.y1 = adjusted_bounds.y0 + tile_height;
			frustum_bounds = adjusted_bounds;
		}
	}

	render_camera_build_frustum(camera, &frustum_bounds, &frustum, TRUE);
	render_camera_build_frustum(
		&window->rasterizer_camera,
		&frustum_bounds,
		&rasterizer_frustum,
		TRUE);

	/* port: stereo (port/linux/game/stereo.c): each eye renders from its own
	camera, culled with the center camera and the union of both eyes' bounds;
	the HUD renders once into its own layer */
	if (halo_stereo_frame()->eye_count == 2 && main_get_window_count() == 1 && !screenshot_combined_index)
	{
		render_player_frame_stereo(window, camera);
		return;
	}

	if (main_get_window_count() == 1)
	{
		if (structure_visibility_find_mirror(camera, &frustum, &mirror))
		{
			short saved_cluster_index;
			struct render_camera mirror_camera;
			struct render_frustum mirror_frustum;

			saved_cluster_index = (short)render.cluster_index;
			match_assert("c:\\halo\\SOURCE\\render\\render.c", 225, window->render_camera.viewport_bounds.x0==0);
			match_assert("c:\\halo\\SOURCE\\render\\render.c", 226, window->render_camera.viewport_bounds.y0==0);
			match_assert("c:\\halo\\SOURCE\\render\\render.c", 227, window->render_camera.viewport_bounds.x1==RASTERIZER_TARGET_RENDER_PRIMARY_WIDTH);
			match_assert("c:\\halo\\SOURCE\\render\\render.c", 228, window->render_camera.viewport_bounds.y1==RASTERIZER_TARGET_RENDER_PRIMARY_HEIGHT);

			render_camera_mirror(camera, &mirror, &mirror_camera);
			render_camera_build_frustum(
				&mirror_camera,
				&frustum_bounds,
				&mirror_frustum,
				TRUE);

			rasterizer_profile_enable(FALSE);
			render.cluster_index = mirror.cluster_index;
			render_window(
				NONE,
				&mirror_camera,
				&mirror_frustum,
				&mirror_camera,
				&mirror_frustum,
				_render_target_secondary,
				FALSE);
			render.cluster_index = saved_cluster_index;
			rasterizer_profile_enable(TRUE);
			has_mirror = TRUE;
		}
	}

	render_window(
		window->local_player_index,
		camera,
		&frustum,
		&window->rasterizer_camera,
		&rasterizer_frustum,
		_render_target_primary,
		has_mirror);

	return;
}

void render_frame(
	struct render_window *windoze,
	short window_count,
	const point2d *screenshot_page_index,
	const point2d *screenshot_index,
	struct bitmap_data *screenshot_bitmap,
	real time_delta_since_tick_sec)
{
	struct rasterizer_frame_begin_parameters parameters;
	short window_index;
	struct render_window *window;
	point2d screenshot_combined_index;

	render.frame_index++;
	render.time_delta_since_tick_sec = time_delta_since_tick_sec;
	render_stereo_eyes_drawn = FALSE;
	memset(&parameters, 0, sizeof(parameters));
	/* continuous between ticks (render_interpolation.c) */
	parameters.game_time_sec = render_interpolation_game_time_sec(game_time_get());
	rasterizer_frame_begin(&parameters);
	rasterizer_windows_begin();

	for (window_index = 0; window_index < window_count; window_index++)
	{
		long window_type;

		window = &windoze[window_index];
		render.window_index = window_index;
		if (window->console_window)
		{
			window_type = 0;
		}
		else if (window->local_player_index != NONE)
		{
			if (screenshot_index != NULL && screenshot_page_index != NULL)
			{
				screenshot_combined_index.x =
					screenshot_page_index->x * global_screenshot_size + screenshot_index->x;
				screenshot_combined_index.y =
					screenshot_page_index->y * global_screenshot_size + screenshot_index->y;
			}

			render_player_frame(
				window,
				screenshot_index != NULL ? &screenshot_combined_index : NULL);
			continue;
		}
		else
		{
			window_type = 1;
		}

		/* port: in a stereo frame the console (and the letterbox it draws)
		goes into the HUD layer, over the HUD pass */
		if (window_type == 0 && render_stereo_eyes_drawn)
			halo_stereo_layer(HALO_STEREO_LAYER_HUD);
		/* the open console goes on the UI's quad (halo_stereo_set_ui_span) */
		halo_stereo_set_ui_span(window_type == 0 && console_is_active());
		render_nonplayer_frame(window, window_type);
		halo_stereo_set_ui_span(FALSE);
		halo_stereo_layer(HALO_STEREO_LAYER_MONO);
	}

	/* port: the progress bar too, on the UI's quad */
	if (render_stereo_eyes_drawn)
		halo_stereo_layer(HALO_STEREO_LAYER_HUD);
	halo_screen_ui_offset(TRUE);
	halo_stereo_set_ui_span(TRUE);
	progress_bar_eachframe();
	halo_stereo_set_ui_span(FALSE);
	halo_screen_ui_offset(FALSE);
	halo_stereo_layer(HALO_STEREO_LAYER_MONO);
	rasterizer_windows_end();
	rasterizer_frame_end();

	return;
}

/* ---------- private code */
