#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEAD_LOCKED_RETICLE_METERS 2.0f
#define HALO_STEREO_ZOOM_DISTANCE_METERS 2.0f

struct halo_controller_draw {
	int active, dim, laser;
	float blend, reticle_distance, laser_opacity;
	float sightline[3], barrel_origin[3], eye[3];
};
struct gpu_stereo_present {
	int zoom, reticle_world, reticle_dim, laser;
	float reticle[3], reticle_distance, laser_opacity, laser_from[3];
};

static struct halo_controller_draw draw;
static int zoom_shown;
static const float zoom_ray[3] = { 0.3492f, 0.12f, -1.0f };

static const struct halo_controller_draw *halo_controller_draw_state(void) { return &draw; }
static int halo_controller_zoom_reticle(float out[3])
{
	memcpy(out, zoom_ray, sizeof(zoom_ray));
	return zoom_shown;
}
static void halo_stereo_eye_frame(float forward[3], float up[3])
{
	forward[0] = forward[1] = up[0] = up[2] = 0.0f;
	forward[2] = -1.0f;
	up[1] = 1.0f;
}
static void halo_stereo_eyes_frame_vector(const float forward[3], const float up[3], const float vector[3], float out[3])
{
	(void)forward;
	(void)up;
	memcpy(out, vector, sizeof(float) * 3);
}

#include "stereo_reticle_aimed.inc"

static void check(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

int main(void)
{
	struct gpu_stereo_present present = { 0 };

	draw.active = 1;
	draw.blend = 1.0f;
	draw.reticle_distance = 6.0f;
	present.zoom = 1;
	present.reticle[2] = -1.0f;
	zoom_shown = 1;
	stereo_reticle_aimed(&present);
	check(present.reticle_world && present.reticle_distance == 1.0f,
		"visible scoped aim sends a positive marker, leaving live plane distance to the host");
	check(fabsf(present.reticle[0] - zoom_ray[0]) < 1e-6f &&
		fabsf(present.reticle[1] - zoom_ray[1]) < 1e-6f,
		"scoped aim keeps the ray for the host's plane intersection");
	memset(&present, 0, sizeof(present));
	present.zoom = 1;
	zoom_shown = 2;
	stereo_reticle_aimed(&present);
	check(present.reticle_world && present.reticle_distance == 0.0f,
		"out-of-picture scoped aim sends the hidden marker");
	memset(&present, 0, sizeof(present));
	present.reticle[2] = -1.0f;
	draw.sightline[2] = -1.0f;
	stereo_reticle_aimed(&present);
	check(fabsf(present.reticle_distance - draw.reticle_distance * 3.048f) < 1e-5f,
		"unzoomed tracked aim keeps its world distance");
	puts("scope guest presenter: PASS");
	return 0;
}
