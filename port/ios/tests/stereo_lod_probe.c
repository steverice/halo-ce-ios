/* The stereo culling frustum's pixel scale (port/linux/game/stereo_lod.c,
included, not linked, with port/linux/game/stereo.c for display.lod_scale).

The stereo spec's "Pop-in: the game picks model detail for half the Xbox's
pixels": render_player_frame_stereo culls with one frustum over the union of
the eyes' tangents, and the game sizes models, particles and sprites by
that frustum's projection_world_to_screen. The probe builds the culling
frustum as render.c does, from the game's 70 degree camera and the
headset's logged tangents (left 1.76, right 1.01, up 1.0, down 1.05, the
eyes mirrored), and mono's from the same camera with unbounded bounds, then
asks halo_stereo_lod_projection to fix the first: a 0.1-unit sphere 10 units
ahead must then measure mono's pixels times halo_stereo_lod_scale (within
0.1%), sprites' scale (.i) must stay mono's, and nothing else in the frustum (its planes above all) may change.
And display.lod_scale itself: 1.0 by default, clamped to 0.5 to 4, in HEAD
mode and the side-by-side view only; display.hud_resolution likewise, 1.0
by default, clamped to 0.5 to 1.

The game's projection math (render_cameras.c: render_camera_build_frustum's
bounds and scales, render_camera_build_frustum_bounds and
render_frustum_sphere_diameter_in_pixels) is copied below in the shapes
stereo_lod.c reads; the planes the copy builds are a stand-in, since the
check is only that the helper never writes them. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "halo_stereo.h"

/* the game's types stereo_lod.c reads, in the shape it reads them */
typedef float real;
typedef unsigned char boolean;
#define TRUE 1
typedef struct { real x, y, z; } real_point3d;
typedef struct { real i, j, k; } real_vector3d;
typedef struct { real i, j; } real_vector2d;
typedef struct { real x0, x1, y0, y1; } real_rectangle2d;
typedef struct { short y0, x0, y1, x1; } rectangle2d;
typedef struct { real_vector3d n; real d; } real_plane3d;
typedef struct
{
	real scale;
	real_vector3d forward, left, up;
	real_point3d position;
} real_matrix4x3;
struct render_camera
{
	real_point3d position;
	real_vector3d forward;
	real_vector3d up;
	boolean mirrored;
	real vertical_field_of_view;
	rectangle2d viewport_bounds;
	rectangle2d window_bounds;
	real z_near;
	real z_far;
};
struct render_frustum
{
	real_rectangle2d frustum_bounds;
	real_matrix4x3 world_to_view;
	real_plane3d world_planes[6];
	real z_near;
	real z_far;
	boolean projection_valid;
	real projection_matrix[4][4];
	real_vector2d projection_world_to_screen;
};

/* render_cameras.c's render_camera_build_frustum_bounds */
static void render_camera_build_frustum_bounds(const struct render_camera *camera, real_rectangle2d *frustum_bounds)
{
	const rectangle2d *viewport_bounds = &camera->viewport_bounds;
	const rectangle2d *window_bounds = &camera->window_bounds;
	real aspect_ratio = (real)(viewport_bounds->y1 - viewport_bounds->y0) / (viewport_bounds->x1 - viewport_bounds->x0);
	real inverse_window_height = 1.0f / (window_bounds->y1 - window_bounds->y0);
	real temporary_y0;

	frustum_bounds->x0 = (2 * viewport_bounds->x0 - window_bounds->x0 - window_bounds->x1) * inverse_window_height;
	frustum_bounds->x1 = (2 * viewport_bounds->x1 - window_bounds->x0 - window_bounds->x1) * inverse_window_height;
	frustum_bounds->y0 = (2 * viewport_bounds->y0 - window_bounds->y0 - window_bounds->y1) * inverse_window_height;
	frustum_bounds->y1 = (2 * viewport_bounds->y1 - window_bounds->y0 - window_bounds->y1) * inverse_window_height;
	frustum_bounds->x0 *= aspect_ratio;
	frustum_bounds->x1 *= aspect_ratio;
	temporary_y0 = frustum_bounds->y0;
	frustum_bounds->y0 = -frustum_bounds->y1;
	frustum_bounds->y1 = -temporary_y0;
}

/* render_cameras.c's render_camera_build_frustum: the bounds, the view's
depth row and the projection's scales as the game computes them */
static void render_camera_build_frustum(const struct render_camera *camera, const real_rectangle2d *frustum_bounds,
	struct render_frustum *frustum, boolean build_projection)
{
	real viewport_width = (real)(camera->viewport_bounds.x1 - camera->viewport_bounds.x0);
	real viewport_height = (real)(camera->viewport_bounds.y1 - camera->viewport_bounds.y0);
	real half_bounds_width, half_bounds_height, field_of_view_tangent, projection_x_scale, projection_y_scale;
	int plane;

	memset(frustum, 0, sizeof(*frustum));
	if (frustum_bounds)
		frustum->frustum_bounds = *frustum_bounds;
	else
	{
		frustum->frustum_bounds.y0 = -1.0f;
		frustum->frustum_bounds.x0 = -1.0f;
		frustum->frustum_bounds.y1 = 1.0f;
		frustum->frustum_bounds.x1 = 1.0f;
	}
	half_bounds_width = (frustum->frustum_bounds.x1 - frustum->frustum_bounds.x0) * 0.5f;
	half_bounds_height = (frustum->frustum_bounds.y1 - frustum->frustum_bounds.y0) * 0.5f;
	field_of_view_tangent = tanf(camera->vertical_field_of_view * 0.5f);
	projection_x_scale = 1.0f / (half_bounds_width / viewport_height * viewport_width * field_of_view_tangent);
	projection_y_scale = 1.0f / (field_of_view_tangent * half_bounds_height);
	/* the view's depth: minus the distance along the camera's forward */
	frustum->world_to_view.forward.k = -camera->forward.i;
	frustum->world_to_view.left.k = -camera->forward.j;
	frustum->world_to_view.up.k = -camera->forward.k;
	frustum->world_to_view.position.z = camera->position.x * camera->forward.i +
		camera->position.y * camera->forward.j + camera->position.z * camera->forward.k;
	/* a stand-in for the planes, distinct for each bounds and camera */
	for (plane = 0; plane < 6; plane++)
	{
		frustum->world_planes[plane].n.i = frustum->frustum_bounds.x0 + plane;
		frustum->world_planes[plane].n.j = frustum->frustum_bounds.y1 - plane;
		frustum->world_planes[plane].n.k = half_bounds_width * half_bounds_height;
		frustum->world_planes[plane].d = camera->position.x + camera->z_far * plane;
	}
	frustum->z_near = camera->z_near;
	frustum->z_far = camera->z_far;
	if (build_projection)
	{
		frustum->projection_matrix[0][0] = projection_x_scale;
		frustum->projection_matrix[1][1] = projection_y_scale;
		frustum->projection_valid = TRUE;
		frustum->projection_world_to_screen.i = projection_x_scale * viewport_width * 0.5f;
		frustum->projection_world_to_screen.j = projection_y_scale * viewport_height * 0.5f;
	}
}

/* render_cameras.c's render_frustum_sphere_diameter_in_pixels */
static real render_frustum_sphere_diameter_in_pixels(const struct render_frustum *frustum, const real_point3d *point,
	real radius)
{
	real depth = frustum->world_to_view.up.k * point->z + frustum->world_to_view.left.k * point->y +
		frustum->world_to_view.forward.k * point->x + frustum->world_to_view.position.z;
	real clamped_depth = fmaxf(fabsf(depth), 0.1f);

	return (frustum->projection_world_to_screen.j / clamped_depth) * radius * 2.0f;
}

/* stereo.c's imports */
static const char *setting_stereo = "head";
static double setting_lod_scale = 1.0, setting_hud_resolution = 1.0;
static int lod_logs, clamp_logs, hud_resolution_logs;
static char last_log[256];
const char *config_string(const char *name)
{
	if (!strcmp(name, "display.stereo"))
		return setting_stereo;
	if (!strcmp(name, "input.turn"))
		return "snap";
	if (!strcmp(name, "display.screen_framing"))
		return "band";
	return "";
}
double config_real(const char *name)
{
	if (!strcmp(name, "display.lod_scale"))
		return setting_lod_scale;
	if (!strcmp(name, "display.hud_resolution"))
		return setting_hud_resolution;
	if (!strcmp(name, "display.film_depth_share"))
		return 0.25;
	if (!strcmp(name, "display.film_convergence"))
		return 1.75;
	if (!strcmp(name, "display.screen_depth_share"))
		return 0.3;
	if (!strcmp(name, "display.screen_convergence"))
		return 1.0;
	if (!strcmp(name, "input.snap_angle"))
		return 30.0;
	if (!strcmp(name, "input.smooth_turn_speed"))
		return 120.0;
	return 0.0;
}
int config_boolean(const char *name) { (void)name; return 0; }
void platform_video_drawable_size(int *width, int *height) { *width = *height = 0; }
void platform_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(last_log, sizeof(last_log), format, arguments);
	va_end(arguments);
	if (strstr(last_log, "stereo: level of detail scale"))
		lod_logs++;
	if (strstr(last_log, "display.lod_scale"))
		clamp_logs++;
	if (strstr(last_log, "display.hud_resolution"))
		hud_resolution_logs++;
}
int halo_cinematic_screen(void) { return 0; }
int halo_scripted_camera(void) { return 0; }
int halo_scripted_director_camera(void) { return 0; }
int halo_level_first_tick_pending(void) { return 0; }
int halo_level_start_campaign(void) { return 0; }
int halo_seat_cryo_pod(void) { return 0; }
int halo_seat_first_person_passenger(void) { return 0; }
int halo_third_person_camera(void) { return 0; }
int halo_coop_dead_camera(void) { return 0; }
int halo_cutscene_camera_first_person(void) { return 0; }
float halo_cutscene_camera_field_of_view(void) { return 1.2217305f; }
int halo_look_disabled_first_person(void) { return 0; }
int halo_player_camera_first_person(void) { return 0; }
int halo_director_inhibited_facing(void) { return 0; }
int halo_cutscene_camera_settled(void) { return 1; }
void halo_cutscene_state(struct halo_cutscene_state *state) { memset(state, 0, sizeof(*state)); }
int platform_fixed_timestep(void) { return 1; }
unsigned long platform_clock_frames(void) { return 0; }
double halo_frame_trace_milliseconds(void) { return 0.0; }
void halo_screen_commit_stereo_scale(void) {}
/* main.c: one player window (split screen keeps the mono path) */
short main_get_window_count(void) { return 1; }
void halo_controller_sample_draw(void) {}

#include "../../linux/game/stereo.c"
#define STEREO_LOD_PROBE
#include "../../linux/game/stereo_lod.c"

static int failures;

static void check(int ok, const char *what)
{
	printf("  %s: %s\n", ok ? "ok" : "FAIL", what);
	if (!ok)
		failures++;
}

/* the settings read afresh, as if the game had just started */
static void restart(const char *stereo, double lod_scale)
{
	setting_stereo = stereo;
	setting_lod_scale = lod_scale;
	stereo_mode = -1;
	turn_mode = -1;
	film_mapping.depth_share = -1.0f;
	clamp_logs = 0;
	hud_resolution_logs = 0;
	halo_stereo_frame_begin();
}

/* the game's camera: 70 degrees across the logical 640 by 480 screen
(main.c: the vertical tangent is 0.75 tan 35 degrees), at the origin
looking down x */
static void game_camera(struct render_camera *camera)
{
	memset(camera, 0, sizeof(*camera));
	camera->forward.i = 1.0f;
	camera->up.k = 1.0f;
	camera->vertical_field_of_view = 2.0f * atanf(0.75f * tanf(35.0f * 3.14159265f / 180.0f));
	camera->viewport_bounds.x1 = camera->window_bounds.x1 = 640;
	camera->viewport_bounds.y1 = camera->window_bounds.y1 = 480;
	camera->z_near = 0.0078125f;
	camera->z_far = 1024.0f;
}

/* render_player_frame_stereo's culling frustum for these eyes: the union of
their tangents, its apex moved back until both eyes' are inside */
static void culling_frustum(const struct render_camera *camera, const struct halo_stereo_eye eyes[2],
	struct render_camera *cull_camera, struct render_frustum *cull_frustum)
{
	real aspect = (real)(camera->viewport_bounds.x1 - camera->viewport_bounds.x0) /
		(real)(camera->viewport_bounds.y1 - camera->viewport_bounds.y0);
	real field_of_view_tangent = tanf(camera->vertical_field_of_view * 0.5f);
	real_rectangle2d cull_bounds;
	real minimum_horizontal = fminf(fminf(eyes[0].left, eyes[1].left), fminf(eyes[0].right, eyes[1].right));
	real minimum_vertical = fminf(fminf(eyes[0].down, eyes[1].down), fminf(eyes[0].up, eyes[1].up));
	real offset_x = fmaxf(fabsf(eyes[0].offset[0]), fabsf(eyes[1].offset[0]));
	real offset_y = fmaxf(fabsf(eyes[0].offset[1]), fabsf(eyes[1].offset[1]));
	real distance_back = fmaxf(0.0f, fmaxf(eyes[0].offset[2], eyes[1].offset[2])) +
		fmaxf(offset_x / minimum_horizontal, offset_y / minimum_vertical);

	cull_bounds.x0 = -fmaxf(eyes[0].left, eyes[1].left) / (aspect * field_of_view_tangent);
	cull_bounds.x1 = fmaxf(eyes[0].right, eyes[1].right) / (aspect * field_of_view_tangent);
	cull_bounds.y0 = -fmaxf(eyes[0].down, eyes[1].down) / field_of_view_tangent;
	cull_bounds.y1 = fmaxf(eyes[0].up, eyes[1].up) / field_of_view_tangent;
	*cull_camera = *camera;
	cull_camera->position.x -= cull_camera->forward.i * distance_back;
	cull_camera->z_far += distance_back;
	render_camera_build_frustum(cull_camera, &cull_bounds, cull_frustum, TRUE);
}

static void pixel_scale(const char *name, const struct halo_stereo_eye eyes[2])
{
	struct render_camera camera, cull_camera;
	struct render_frustum mono_frustum, cull_frustum, before;
	const real_point3d ahead = { 10.0f, 0.0f, 0.0f };
	real mono, union_pixels, fixed, expected;

	printf("%s, display.lod_scale %.2f:\n", name, (double)halo_stereo_lod_scale());
	game_camera(&camera);
	culling_frustum(&camera, eyes, &cull_camera, &cull_frustum);
	/* mono's from the same camera, the culling one (its apex a hundredth of
	a unit back, which alone moves the sphere's size by 0.1%) */
	render_camera_build_frustum(&cull_camera, NULL, &mono_frustum, TRUE);
	before = cull_frustum;
	union_pixels = render_frustum_sphere_diameter_in_pixels(&cull_frustum, &ahead, 0.05f);
	/* as render.c calls it: the culling camera, after its frustum */
	halo_stereo_lod_projection(&cull_camera, &cull_frustum);
	mono = render_frustum_sphere_diameter_in_pixels(&mono_frustum, &ahead, 0.05f);
	fixed = render_frustum_sphere_diameter_in_pixels(&cull_frustum, &ahead, 0.05f);
	expected = mono * halo_stereo_lod_scale();
	printf("  a 0.1-unit sphere 10 units ahead: mono %.3f px, union %.3f px (%.2f of mono), fixed %.3f px "
		"(j %.1f, mono's %.1f; i %.1f, mono's %.1f)\n", (double)mono, (double)union_pixels,
		(double)(union_pixels / mono), (double)fixed, (double)cull_frustum.projection_world_to_screen.j,
		(double)mono_frustum.projection_world_to_screen.j, (double)cull_frustum.projection_world_to_screen.i,
		(double)mono_frustum.projection_world_to_screen.i);
	check(fabsf(fixed - expected) <= expected * 0.001f, "the sphere measures mono's pixels times display.lod_scale");
	check(fabsf(cull_frustum.projection_world_to_screen.j -
		mono_frustum.projection_world_to_screen.j * halo_stereo_lod_scale()) <=
		mono_frustum.projection_world_to_screen.j * 0.001f, "model detail's scale (j) is mono's times display.lod_scale");
	check(fabsf(cull_frustum.projection_world_to_screen.i - mono_frustum.projection_world_to_screen.i) <=
		mono_frustum.projection_world_to_screen.i * 0.001f, "sprites' scale (i) is mono's, whatever display.lod_scale");
	/* nothing but the pixel scale changes: the planes cull as before */
	cull_frustum.projection_world_to_screen = before.projection_world_to_screen;
	check(!memcmp(&cull_frustum, &before, sizeof(before)), "the planes and the rest of the frustum are unchanged");
}

/* count more stereo frames through the helper, quietly */
static void repeat_projection(const struct halo_stereo_eye eyes[2], int frames)
{
	struct render_camera camera, cull_camera;
	struct render_frustum cull_frustum;

	game_camera(&camera);
	while (frames-- > 0)
	{
		culling_frustum(&camera, eyes, &cull_camera, &cull_frustum);
		halo_stereo_lod_projection(&cull_camera, &cull_frustum);
	}
}

static void lod_scale_setting_check(void)
{
	printf("display.lod_scale:\n");
	restart("head", 1.0);
	check(halo_stereo_lod_scale() == 1.0f && clamp_logs == 0, "1.0 in HEAD mode by default");
	restart("head", 2.0);
	check(halo_stereo_lod_scale() == 2.0f, "HEAD mode takes the setting");
	restart("side_by_side", 1.5);
	check(halo_stereo_lod_scale() == 1.5f, "the side-by-side view takes the setting");
	restart("screen", 2.0);
	check(halo_stereo_lod_scale() == 1.0f, "SCREEN mode keeps 1.0");
	restart("off", 2.0);
	check(halo_stereo_lod_scale() == 1.0f, "mono keeps 1.0");
	restart("head", 10.0);
	check(halo_stereo_lod_scale() == 4.0f && clamp_logs == 1, "clamped to 4, and logged");
	restart("head", 0.1);
	check(halo_stereo_lod_scale() == 0.5f && clamp_logs == 1, "clamped to 0.5, and logged");
}

/* display.hud_resolution: the HUD's targets' pixels per axis against the
view's (d3d8_device.c scales every HUD target by it) */
static void hud_resolution_setting_check(void)
{
	printf("display.hud_resolution:\n");
	restart("head", 1.0);
	check(halo_stereo_hud_resolution() == 1.0f && hud_resolution_logs == 0, "1.0 in HEAD mode by default");
	setting_hud_resolution = 0.5;
	restart("head", 1.0);
	check(halo_stereo_hud_resolution() == 0.5f && hud_resolution_logs == 0, "HEAD mode takes 0.5, the edge");
	setting_hud_resolution = 0.75;
	restart("side_by_side", 1.0);
	check(halo_stereo_hud_resolution() == 0.75f, "the side-by-side view takes the setting");
	restart("screen", 1.0);
	check(halo_stereo_hud_resolution() == 1.0f, "SCREEN mode keeps 1.0");
	restart("off", 1.0);
	check(halo_stereo_hud_resolution() == 1.0f, "mono keeps 1.0");
	setting_hud_resolution = 2.0;
	restart("head", 1.0);
	check(halo_stereo_hud_resolution() == 1.0f && hud_resolution_logs == 1, "clamped to 1, and logged");
	setting_hud_resolution = 0.1;
	restart("head", 1.0);
	check(halo_stereo_hud_resolution() == 0.5f && hud_resolution_logs == 1, "clamped to 0.5, and logged");
	setting_hud_resolution = 1.0;
}

int main(void)
{
	/* the headset's tangents (session-4 log), the eyes mirrored and 0.0105
	units apart, and the side-by-side view's */
	const struct halo_stereo_eye headset[2] = {
		{ { -0.0105f, 0.0f, 0.0f }, 1.76f, 1.01f, 1.0f, 1.05f },
		{ { 0.0105f, 0.0f, 0.0f }, 1.01f, 1.76f, 1.0f, 1.05f },
	};
	const struct halo_stereo_eye side_by_side[2] = {
		{ { -HALO_STEREO_SIDE_BY_SIDE_OFFSET, 0.0f, 0.0f }, HALO_STEREO_SIDE_BY_SIDE_TANGENT,
			HALO_STEREO_SIDE_BY_SIDE_TANGENT, HALO_STEREO_SIDE_BY_SIDE_TANGENT, HALO_STEREO_SIDE_BY_SIDE_TANGENT },
		{ { HALO_STEREO_SIDE_BY_SIDE_OFFSET, 0.0f, 0.0f }, HALO_STEREO_SIDE_BY_SIDE_TANGENT,
			HALO_STEREO_SIDE_BY_SIDE_TANGENT, HALO_STEREO_SIDE_BY_SIDE_TANGENT, HALO_STEREO_SIDE_BY_SIDE_TANGENT },
	};

	lod_scale_setting_check();
	hud_resolution_setting_check();
	restart("head", 1.0);
	pixel_scale("the headset's eyes", headset);
	check(lod_logs == 1, "the scale is logged once a run");
	restart("side_by_side", 1.0);
	pixel_scale("the side-by-side view's eyes", side_by_side);
	restart("head", 2.0);
	pixel_scale("the headset's eyes", headset);
	check(lod_logs == 1, "a changed scale isn't logged again within 600 frames");
	restart("head", 0.5);
	pixel_scale("the headset's eyes", headset);
	/* the first line came on stereo frame 1, and this is frame 4 */
	repeat_projection(headset, 596);
	check(lod_logs == 1, "nor 599 frames after the last line");
	repeat_projection(headset, 1);
	check(lod_logs == 2, "but is 600 frames after it");
	repeat_projection(headset, 1200);
	check(lod_logs == 2, "and an unchanged scale never is");
	if (failures)
	{
		printf("stereo lod probe: %d failed\n", failures);
		return 1;
	}
	printf("stereo lod probe: PASS\n");
	return 0;
}
