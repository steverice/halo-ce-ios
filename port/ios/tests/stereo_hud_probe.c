/* HEAD mode's HUD and UI layout (port/ios/host/host_stereo_hud.c): the
reticle's layer whole at its natural size, centered or along a seat's aim;
one quad per HUD group that drew, showing its whole rectangle, in the
periphery, each group's outer corner at its CE corner's angles from the
display.hud_* settings (clamped to the eyes' shared view), facing the eyes
from 2 m on the level, yaw-only frame; the UI inside foveation's sharp
region; the catch-all, the HUD layer whole where the HUD pass projected it;
the zoomed picture's quad, the level frame's yaw. With --quads WIDTH UI X Y
Z it prints the layout's quads for sample group rectangles instead (one per
line: frame, catch-all, source u0 v0 u1 v1, center, x axis, y axis, layer). */
#include "host_stereo_hud.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEGREES ((float)M_PI / 180.0f)

static int failures;

static void check(int condition, const char *what)
{
	printf("%s: %s\n", condition ? "ok" : "FAILED", what);
	if (!condition)
		failures++;
}

/* a level quad's corners, in degrees from straight ahead, across and up
(the plane is at z = center z) */
static void level_extent(const struct host_stereo_hud_quad *quad, float degrees[4])
{
	float distance = -quad->center[2];
	float x0 = quad->center[0] - fabsf(quad->x_axis[0]), x1 = quad->center[0] + fabsf(quad->x_axis[0]);
	float y0 = quad->center[1] - fabsf(quad->y_axis[1]), y1 = quad->center[1] + fabsf(quad->y_axis[1]);

	degrees[0] = atanf(x0 / distance) / DEGREES;
	degrees[1] = atanf(y0 / distance) / DEGREES;
	degrees[2] = atanf(x1 / distance) / DEGREES;
	degrees[3] = atanf(y1 / distance) / DEGREES;
}

/* a 105 by 90 degree view's half tangents (Vision Pro-like; the HUD pass
takes its projection from the eyes') for the --quads output and the checks */
static const float hud_tangents[2] = { 1.303f, 1.0f };

/* the groups' rectangles as the Mac's 640-line captures measured the
elements (Task 7c's t7c/ folder): the counters, the meters, the tracker, a
prompt and two messages under it; at another width, where the game's rule
puts them (48 lines in at 640 and floor(48 * width / 640) at any width,
rasterizer_xbox.c), the meters anchored right, the rest left */
static void sample_extents(float width, float extents[HALO_HUD_GROUP_COUNT][4])
{
	static const float measured[HALO_HUD_GROUP_COUNT][4] = {
		{ 48, 37, 168, 82 }, { 464, 37, 584, 66 }, { 48, 362, 131, 444 }, { 48, 82.5f, 386, 117 },
		{ 48, 117, 300, 146 } };
	float inset = floorf(48.0f * width / 640.0f);
	int group, axis;

	for (group = 0; group < HALO_HUD_GROUP_COUNT; group++)
		for (axis = 0; axis < 4; axis++)
		{
			float value = measured[group][axis];

			if (axis == 0 || axis == 2)
				value = group == HALO_HUD_GROUP_UNIT ? width - inset - (592.0f - value) : inset + (value - 48.0f);
			extents[group][axis] = value;
		}
	/* on foot there are no seat labels */
	memset(extents[HALO_HUD_GROUP_SEATS], 0, sizeof(extents[0]));
}

static void print_quads(float width, int ui, const float reticle[3])
{
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
	float extents[HALO_HUD_GROUP_COUNT][4];
	int count, index;

	sample_extents(width, extents);
	count = host_stereo_hud_layout(width, ui, reticle, hud_tangents, (const float (*)[4])extents, NULL, NULL, quads);
	for (index = 0; index < count; index++)
	{
		const struct host_stereo_hud_quad *q = &quads[index];

		printf("%d %d %g %g %g %g %g %g %g %g %g %g %g %g %g %d\n", q->frame, q->catch_all, q->source[0], q->source[1],
			q->source[2], q->source[3], q->center[0], q->center[1], q->center[2], q->x_axis[0], q->x_axis[1],
			q->x_axis[2], q->y_axis[0], q->y_axis[1], q->y_axis[2], q->layer);
	}
}

/* the quad showing a layer, or NULL */
static const struct host_stereo_hud_quad *quad_of(const struct host_stereo_hud_quad *quads, int count, int layer)
{
	int index;

	for (index = 0; index < count; index++)
		if (quads[index].layer == layer)
			return &quads[index];
	return NULL;
}

/* whether a quad's source holds the whole rectangle (layout lines) */
static int shows_whole(const struct host_stereo_hud_quad *quad, float width, const float rectangle[4])
{
	return quad && quad->source[0] * width <= rectangle[0] + 1e-3f && quad->source[1] * 480.0f <= rectangle[1] + 1e-3f &&
		quad->source[2] * width >= rectangle[2] - 1e-3f && quad->source[3] * 480.0f >= rectangle[3] - 1e-3f;
}

/* the catch-all: the HUD layer whole, where the HUD pass projected it, so a
layer point (a nav point, the multiplayer score) shows at the direction
the game put it */
static void catch_all_checks(float width)
{
	const float ahead[3] = { 0.0f, 0.0f, -1.0f };
	/* a seat aiming 60 degrees right: the reticle's quad is there */
	const float seat[3] = { 0.866f, 0.0f, -0.5f };
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
	float extents[HALO_HUD_GROUP_COUNT][4];
	int count;
	const struct host_stereo_hud_quad *all;
	/* a nav point drawn at three quarters across and a third down the layer */
	float u = 0.75f, v = 0.33f, x, y, across, up, game_across, game_up;
	char what[200];

	sample_extents(width, extents);
	count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])extents, NULL, NULL, quads);
	all = &quads[count - 1];
	x = all->center[0] + (2.0f * u - 1.0f) * all->x_axis[0];
	y = all->center[1] + (1.0f - 2.0f * v) * all->y_axis[1];
	across = atanf(x / HOST_STEREO_HUD_DISTANCE_DEFAULT);
	up = atanf(y / HOST_STEREO_HUD_DISTANCE_DEFAULT);
	game_across = atanf((2.0f * u - 1.0f) * hud_tangents[0]);
	game_up = atanf((1.0f - 2.0f * v) * hud_tangents[1]);
	snprintf(what, sizeof(what), "a nav point in the HUD layer shows at %.2f, %.2f degrees, where the HUD pass "
		"projected it (%.2f, %.2f)", across / DEGREES, up / DEGREES, game_across / DEGREES, game_up / DEGREES);
	check(all->catch_all && all->layer == HOST_STEREO_HUD_LAYER_HUD && all->source[0] == 0.0f &&
		all->source[1] == 0.0f && all->source[2] == 1.0f && all->source[3] == 1.0f &&
		fabsf(across - game_across) < 1e-5f && fabsf(up - game_up) < 1e-5f, what);
	count = host_stereo_hud_layout(width, 0, seat, hud_tangents, (const float (*)[4])extents, NULL, NULL, quads);
	check(quads[0].layer == HOST_STEREO_HUD_LAYER_RETICLE &&
		fabsf(atan2f(quads[0].center[0], -quads[0].center[2]) / DEGREES - 60.0f) < 0.01f,
		"in a seat the crosshairs' layer is centered along the aim");
	count = host_stereo_hud_layout(width, 0, (const float[3]){ 0.0f, 0.0f, 1.0f }, hud_tangents,
		(const float (*)[4])extents, NULL, NULL, quads);
	check(!quad_of(quads, count, HOST_STEREO_HUD_LAYER_RETICLE) && quads[count - 1].catch_all,
		"with the aim behind, there's no reticle quad, and the rest stays");
	check(host_stereo_hud_layout(width, 0, ahead, NULL, (const float (*)[4])extents, NULL, NULL, quads) == count,
		"without the HUD pass's projection there's no catch-all");
}

/* the brief's starting angles, which the defaults are */
static const struct host_stereo_hud_placement defaults = HOST_STEREO_HUD_PLACEMENT_DEFAULT;

static float length3(const float v[3])
{
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/* a point's angles in the level frame, in degrees: across (right positive)
and up */
static void angles_of(const float point[3], float angles[2])
{
	angles[0] = atan2f(point[0], -point[2]) / DEGREES;
	angles[1] = atan2f(point[1], hypotf(point[0], point[2])) / DEGREES;
}

/* the corner of a group quad's drawn content (the quad less its margin,
HOST_STEREO_HUD_GROUP_MARGIN_LINES at scale times the natural meters a
line) on side x (-1 left, 1 right) and y (1 top, -1 bottom) */
static void content_corner(const struct host_stereo_hud_quad *quad, float scale, int x, int y, float point[3])
{
	float margin = HOST_STEREO_HUD_GROUP_MARGIN_LINES * HOST_STEREO_HUD_METERS_PER_LINE * scale;
	float x_length = length3(quad->x_axis), y_length = length3(quad->y_axis);
	int axis;

	for (axis = 0; axis < 3; axis++)
		point[axis] = quad->center[axis] + (float)x * quad->x_axis[axis] * (x_length - margin) / x_length +
			(float)y * quad->y_axis[axis] * (y_length - margin) / y_length;
}

/* a group quad's drawn content as the box of its corners' angles: across
from, up from, across to, up to, in degrees */
static void content_box(const struct host_stereo_hud_quad *quad, float scale, float box[4])
{
	int x, y;

	box[0] = box[1] = 1e9f;
	box[2] = box[3] = -1e9f;
	for (x = -1; x <= 1; x += 2)
		for (y = -1; y <= 1; y += 2)
		{
			float point[3], angles[2];

			content_corner(quad, scale, x, y, point);
			angles_of(point, angles);
			box[0] = fminf(box[0], angles[0]);
			box[1] = fminf(box[1], angles[1]);
			box[2] = fmaxf(box[2], angles[0]);
			box[3] = fmaxf(box[3], angles[1]);
		}
}

/* whether two group quads' drawn contents overlap: on one plane (a
corner's groups share theirs), as rectangles on it; else as the boxes of
their corners' angles */
static int contents_overlap(const struct host_stereo_hud_quad *a, const struct host_stereo_hud_quad *b, float scale)
{
	float right[3], up[3], a_box[4], b_box[4];
	float margin = HOST_STEREO_HUD_GROUP_MARGIN_LINES * HOST_STEREO_HUD_METERS_PER_LINE * scale;
	int axis, coplanar = 1;

	for (axis = 0; axis < 3; axis++)
	{
		right[axis] = a->x_axis[axis] / length3(a->x_axis);
		up[axis] = a->y_axis[axis] / length3(a->y_axis);
		coplanar &= fabsf(right[axis] - b->x_axis[axis] / length3(b->x_axis)) < 1e-6f &&
			fabsf(up[axis] - b->y_axis[axis] / length3(b->y_axis)) < 1e-6f;
	}
	if (coplanar)
	{
		float a_x = a->center[0] * right[0] + a->center[1] * right[1] + a->center[2] * right[2];
		float a_y = a->center[0] * up[0] + a->center[1] * up[1] + a->center[2] * up[2];
		float b_x = b->center[0] * right[0] + b->center[1] * right[1] + b->center[2] * right[2];
		float b_y = b->center[0] * up[0] + b->center[1] * up[1] + b->center[2] * up[2];
		float a_w = length3(a->x_axis) - margin, a_h = length3(a->y_axis) - margin;
		float b_w = length3(b->x_axis) - margin, b_h = length3(b->y_axis) - margin;

		return fabsf(a_x - b_x) < a_w + b_w - 1e-6f && fabsf(a_y - b_y) < a_h + b_h - 1e-6f;
	}
	content_box(a, scale, a_box);
	content_box(b, scale, b_box);
	return a_box[0] < b_box[2] - 1e-4f && b_box[0] < a_box[2] - 1e-4f && a_box[1] < b_box[3] - 1e-4f &&
		b_box[1] < a_box[3] - 1e-4f;
}

/* whether a group's content corner on side x, y sits at across, up
degrees, within a thousandth of a degree; prints where it is */
static int corner_at(const struct host_stereo_hud_quad *quad, float scale, int x, int y, float across, float up,
	const char *name)
{
	float point[3], angles[2];

	if (!quad)
		return 0;
	content_corner(quad, scale, x, y, point);
	angles_of(point, angles);
	printf("  %s: its %s %s corner at %.4f across, %.4f up (wanted %.1f, %.1f)\n", name, y > 0 ? "top" : "bottom",
		x < 0 ? "left" : "right", angles[0], angles[1], across, up);
	return fabsf(angles[0] - across) < 1e-3f && fabsf(angles[1] - up) < 1e-3f;
}

/* whether a group's quad faces the eyes' midpoint from a plane
HOST_STEREO_HUD_DISTANCE_DEFAULT away, upright (its width level), turning with the
head's yaw only, at scale times the natural size, showing its rectangle */
static int on_the_sphere(const struct host_stereo_hud_quad *quad, float width, float scale)
{
	float normal[3], normal_length, plane, facing;
	float natural_width, natural_height;

	normal[0] = quad->x_axis[1] * quad->y_axis[2] - quad->x_axis[2] * quad->y_axis[1];
	normal[1] = quad->x_axis[2] * quad->y_axis[0] - quad->x_axis[0] * quad->y_axis[2];
	normal[2] = quad->x_axis[0] * quad->y_axis[1] - quad->x_axis[1] * quad->y_axis[0];
	normal_length = length3(normal);
	/* the plane's distance from the eyes, and whether its front (the
	texture's, u right and v up) faces them */
	plane = fabsf(normal[0] * quad->center[0] + normal[1] * quad->center[1] + normal[2] * quad->center[2]) /
		normal_length;
	facing = normal[0] * quad->center[0] + normal[1] * quad->center[1] + normal[2] * quad->center[2];
	natural_width = (quad->source[2] - quad->source[0]) * width * HOST_STEREO_HUD_METERS_PER_LINE * scale;
	natural_height = (quad->source[3] - quad->source[1]) * HOST_STEREO_HUD_LINES * HOST_STEREO_HUD_METERS_PER_LINE *
		scale;
	return quad->frame == HOST_STEREO_HUD_LEVEL && fabsf(plane - HOST_STEREO_HUD_DISTANCE_DEFAULT) < 1e-4f && facing < 0.0f &&
		fabsf(quad->x_axis[1]) < 1e-6f && fabsf(2.0f * length3(quad->x_axis) - natural_width) < 1e-5f &&
		fabsf(2.0f * length3(quad->y_axis) - natural_height) < 1e-5f;
}

/* the placement in "The HUD in the periphery": each group's outer corner
(the edge nearest its CE corner) at the table's angles on the level,
yaw-only frame, the weapon top left, the unit top right, the tracker bottom
left, the prompt and the messages top left from display.hud_messages_up
down; a wider element grows toward the center */
static void layout_checks(float width)
{
	const float ahead[3] = { 0.0f, 0.0f, -1.0f };
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
	float extents[HALO_HUD_GROUP_COUNT][4];
	const struct host_stereo_hud_quad *weapon, *unit, *tracker, *prompt, *messages;
	int count, index, group, sphere = 1, sources = 1, whole = 1, inside = 1;
	float reach = 0.0f;
	char what[200];

	sample_extents(width, extents);
	count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])extents, NULL, NULL, quads);
	printf("layout %.0f lines across (%.3f:1)\n", width, width / 480.0f);
	check(count == 1 + HALO_HUD_GROUP_COUNT, "a quad for the reticle, each group that drew (all but the seats) and the catch-all");
	check(quads[0].frame == HOST_STEREO_HUD_HEAD && quads[0].layer == HOST_STEREO_HUD_LAYER_RETICLE,
		"the reticle's layer comes first, head-locked");
	check(fabsf(quads[0].center[0]) < 1e-6f && fabsf(quads[0].center[1]) < 1e-6f &&
		fabsf(quads[0].center[2] + HOST_STEREO_HUD_DISTANCE_DEFAULT) < 1e-6f, "the reticle is centered, 2 m ahead");
	check(quads[0].source[0] == 0.0f && quads[0].source[1] == 0.0f && quads[0].source[2] == 1.0f &&
		quads[0].source[3] == 1.0f, "the reticle's quad shows its layer whole, no square cut from anything");
	check(fabsf(quads[0].x_axis[0] * 2.0f - width * HOST_STEREO_HUD_METERS_PER_LINE) < 1e-6f &&
		fabsf(quads[0].y_axis[1] * 2.0f - 480.0f * HOST_STEREO_HUD_METERS_PER_LINE) < 1e-6f,
		"the reticle's layer is at its natural size");
	check(quads[count - 1].catch_all && quads[count - 1].frame == HOST_STEREO_HUD_HEAD,
		"the catch-all comes last, head-locked");
	for (group = 0; group < HALO_HUD_GROUP_COUNT; group++)
		if (group != HALO_HUD_GROUP_SEATS)
			whole &= shows_whole(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + group), width, extents[group]);
	check(whole, "each group's quad shows its whole rectangle, from its own target");
	/* the groups' quads: all but the reticle and the catch-all */
	for (index = 1; index < count - 1; index++)
	{
		float box[4];
		int axis;

		sphere &= on_the_sphere(&quads[index], width, 1.0f);
		content_box(&quads[index], 1.0f, box);
		for (axis = 0; axis < 4; axis++)
		{
			reach = fmaxf(reach, fabsf(box[axis]));
			if (quads[index].source[axis] < 0.0f || quads[index].source[axis] > 1.0f)
				sources = 0;
		}
		printf("  group %d: %.2f to %.2f degrees across, %.2f to %.2f up\n",
			quads[index].layer - HOST_STEREO_HUD_LAYER_GROUP, box[0], box[2], box[1], box[3]);
	}
	check(sphere, "every group's quad faces the eyes' midpoint from 2 m, upright in the level, yaw-only frame, at "
		"0.072 degrees a line");
	check(sources, "the groups are cut from inside their targets");
	snprintf(what, sizeof(what), "every group stays inside the eyes' shared view, 40 degrees across and up (the "
		"farthest %.2f)", reach);
	inside = reach <= 40.0f;
	check(inside, what);
	weapon = quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_WEAPON);
	unit = quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_UNIT);
	tracker = quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_TRACKER);
	prompt = quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_PROMPT);
	messages = quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_MESSAGES);
	check(corner_at(weapon, 1.0f, -1, 1, -defaults.across, defaults.up, "weapon"),
		"the weapon's top left corner is at -28 across, +20 up");
	check(corner_at(unit, 1.0f, 1, 1, defaults.across, defaults.up, "unit"),
		"the unit's top right corner is at +28 across, +20 up");
	check(corner_at(tracker, 1.0f, -1, -1, -defaults.across, -defaults.tracker_down, "tracker"),
		"the tracker's bottom left corner is at -28 across, -22 up");
	check(corner_at(prompt, 1.0f, -1, 1, -defaults.across, defaults.messages_up, "prompt"),
		"the prompt's top left corner is at -28 across, +12 up");
	/* the messages hang under the prompt, left edges together, as CE draws
	them (the sample's prompt ends where the messages begin) */
	{
		float prompt_corner[3], messages_corner[3];
		int axis, together = 1;

		content_corner(prompt, 1.0f, -1, -1, prompt_corner);
		content_corner(messages, 1.0f, -1, 1, messages_corner);
		for (axis = 0; axis < 3; axis++)
			together &= fabsf(prompt_corner[axis] - messages_corner[axis]) < 1e-5f;
		check(together, "the messages hang under the prompt, their left edges together, as CE draws them");
	}
	/* no two groups' drawn content overlaps */
	{
		int a, b, overlap = 0;

		for (a = 1; a < count - 1; a++)
			for (b = a + 1; b < count - 1; b++)
				overlap |= contents_overlap(&quads[a], &quads[b], 1.0f);
		check(!overlap, "no two groups' rectangles overlap");
	}
	/* a wider element, such as the needler's icon, or a longer prompt:
	its group's quad grows with it, toward the center, and still shows all
	of it, its outer corner where it was */
	{
		float wide[HALO_HUD_GROUP_COUNT][4], narrow_box[4], wide_box[4];
		const struct host_stereo_hud_quad *wide_weapon, *wide_prompt;
		int n;

		content_box(weapon, 1.0f, narrow_box);
		memcpy(wide, extents, sizeof(wide));
		wide[HALO_HUD_GROUP_WEAPON][2] += 80.0f;
		wide[HALO_HUD_GROUP_PROMPT][2] = fminf(width - 10.0f, wide[HALO_HUD_GROUP_PROMPT][2] + 160.0f);
		n = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])wide, NULL, NULL, quads);
		wide_weapon = quad_of(quads, n, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_WEAPON);
		wide_prompt = quad_of(quads, n, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_PROMPT);
		content_box(wide_weapon, 1.0f, wide_box);
		check(shows_whole(wide_weapon, width, wide[HALO_HUD_GROUP_WEAPON]) &&
			shows_whole(wide_prompt, width, wide[HALO_HUD_GROUP_PROMPT]),
			"a wider weapon icon and a longer prompt show whole, each on its group's quad");
		check(corner_at(wide_weapon, 1.0f, -1, 1, -defaults.across, defaults.up, "wider weapon") &&
			corner_at(wide_prompt, 1.0f, -1, 1, -defaults.across, defaults.messages_up, "longer prompt") &&
			wide_box[2] > narrow_box[2] + 5.0f, "they grow toward the center; their outer corners stay");
	}
	/* a vehicle's driver seat (the Mac's b30 Warthog: the passenger's label
	and bar at 60 to 140 lines across, 37 to 90 down, the Warthog's bar at
	463 to 597, 36 to 70), and a Scorpion's, which adds the cannon's
	counters top left (CE puts the rider labels under them): the labels are
	the seats' group, in the weapon's corner (CE anchors them top left), the
	bars the unit's, top right; next to the counters the labels keep CE's
	place relative to them, and no two quads overlap */
	{
		float inset = floorf(48.0f * width / 640.0f);
		float driver[HALO_HUD_GROUP_COUNT][4] = { { 0 } };
		int scorpion;

		driver[HALO_HUD_GROUP_SEATS][0] = inset + 12.0f;
		driver[HALO_HUD_GROUP_SEATS][1] = 37.0f;
		driver[HALO_HUD_GROUP_SEATS][2] = inset + 92.0f;
		driver[HALO_HUD_GROUP_SEATS][3] = 90.0f;
		driver[HALO_HUD_GROUP_UNIT][0] = width - inset - 129.0f;
		driver[HALO_HUD_GROUP_UNIT][1] = 36.0f;
		driver[HALO_HUD_GROUP_UNIT][2] = width - inset + 5.0f;
		driver[HALO_HUD_GROUP_UNIT][3] = 70.0f;
		for (scorpion = 0; scorpion < 2; scorpion++)
		{
			const struct host_stereo_hud_quad *seat_unit, *seats, *cannon;
			int n, a, b, overlap = 0, placed;
			char name[160];

			if (scorpion)
			{
				/* the cannon's counters, and the rider labels moved under them */
				driver[HALO_HUD_GROUP_WEAPON][0] = inset - 1.0f;
				driver[HALO_HUD_GROUP_WEAPON][1] = 37.0f;
				driver[HALO_HUD_GROUP_WEAPON][2] = inset + 210.0f;
				driver[HALO_HUD_GROUP_WEAPON][3] = 88.0f;
				driver[HALO_HUD_GROUP_SEATS][1] = 92.0f;
				driver[HALO_HUD_GROUP_SEATS][3] = 200.0f;
			}
			n = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])driver, NULL, NULL, quads);
			seat_unit = quad_of(quads, n, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_UNIT);
			seats = quad_of(quads, n, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_SEATS);
			cannon = quad_of(quads, n, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_WEAPON);
			for (a = 1; a < n - 1; a++)
				for (b = a + 1; b < n - 1; b++)
					overlap |= contents_overlap(&quads[a], &quads[b], 1.0f);
			if (!scorpion)
				placed = corner_at(seats, 1.0f, -1, 1, -defaults.across, defaults.up, "Warthog seats");
			else
			{
				/* the labels 13 lines right of the counters' left edge and 55
				below their top, on the counters' plane */
				float from[3], to[3], offset[3], right, down, k = HOST_STEREO_HUD_METERS_PER_LINE;
				int axis;

				content_corner(cannon, 1.0f, -1, 1, from);
				content_corner(seats, 1.0f, -1, 1, to);
				for (axis = 0; axis < 3; axis++)
					offset[axis] = to[axis] - from[axis];
				right = (offset[0] * cannon->x_axis[0] + offset[1] * cannon->x_axis[1] + offset[2] * cannon->x_axis[2]) /
					length3(cannon->x_axis);
				down = -(offset[0] * cannon->y_axis[0] + offset[1] * cannon->y_axis[1] + offset[2] * cannon->y_axis[2]) /
					length3(cannon->y_axis);
				printf("  Scorpion seats: %.2f lines right of the counters, %.2f lines below them\n", right / k, down / k);
				placed = corner_at(cannon, 1.0f, -1, 1, -defaults.across, defaults.up, "Scorpion counters") &&
					fabsf(right - 13.0f * k) < 1e-5f && fabsf(down - 55.0f * k) < 1e-5f;
			}
			snprintf(name, sizeof(name), "a %s driver's seat labels sit in the top left corner%s, the bars top right, "
				"no quads overlapping", scorpion ? "Scorpion" : "Warthog", scorpion ? ", under the cannon's counters" : "");
			check(shows_whole(seats, width, driver[HALO_HUD_GROUP_SEATS]) &&
				shows_whole(seat_unit, width, driver[HALO_HUD_GROUP_UNIT]) && placed &&
				corner_at(seat_unit, 1.0f, 1, 1, defaults.across, defaults.up, "seat unit") && !overlap, name);
		}
	}
	/* a group that didn't draw has no quad; with no rectangles at all only
	the reticle and the catch-all are left */
	{
		float some[HALO_HUD_GROUP_COUNT][4];

		memcpy(some, extents, sizeof(some));
		memset(some[HALO_HUD_GROUP_PROMPT], 0, sizeof(some[0]));
		count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])some, NULL, NULL, quads);
		check(count == HALO_HUD_GROUP_COUNT && !quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_PROMPT),
			"an empty group has no quad");
		check(corner_at(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_MESSAGES), 1.0f, -1, 1,
			-defaults.across, defaults.messages_up, "messages alone"),
			"without a prompt, the messages' top left corner is at -28 across, +12 up");
		check(host_stereo_hud_layout(width, 0, ahead, hud_tangents, NULL, NULL, NULL, quads) == 2,
			"without the groups' rectangles, only the reticle and the catch-all");
	}
	catch_all_checks(width);
}

/* the settings (display.hud_corner_across, _corner_up, _tracker_down,
_messages_up, _scale): the angles move the corners, the scale the size,
each angle clamped to the eyes' shared view, the edge included; a head
pitch or roll moves no piece, a body yaw moves them all */
static void placement_checks(void)
{
	const float ahead[3] = { 0.0f, 0.0f, -1.0f };
	const float width = 854.0f;
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS], natural[HOST_STEREO_HUD_MAXIMUM_QUADS];
	float extents[HALO_HUD_GROUP_COUNT][4];
	int count, index;

	sample_extents(width, extents);
	/* a bracket from the headset's checklist */
	{
		struct host_stereo_hud_placement placement = { 20.0f, 25.0f, 18.0f, 10.0f, 1.0f, 2.0f };

		count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])extents, &placement, NULL, quads);
		check(corner_at(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_WEAPON), 1.0f, -1, 1, -20.0f,
			25.0f, "weapon") &&
			corner_at(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_UNIT), 1.0f, 1, 1, 20.0f, 25.0f,
			"unit") &&
			corner_at(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_TRACKER), 1.0f, -1, -1, -20.0f,
			-18.0f, "tracker") &&
			corner_at(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_PROMPT), 1.0f, -1, 1, -20.0f,
			10.0f, "prompt"),
			"the settings' angles move each corner: across 20, up 25, tracker 18 down, messages 10 up");
	}
	/* display.hud_scale: twice the size, the corners where they were */
	{
		struct host_stereo_hud_placement placement = HOST_STEREO_HUD_PLACEMENT_DEFAULT;
		int natural_count, sized = 1, corners;

		placement.scale = 2.0f;
		natural_count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])extents, NULL, NULL, natural);
		count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])extents, &placement, NULL, quads);
		for (index = 1; index < count - 1 && count == natural_count; index++)
			sized &= on_the_sphere(&quads[index], width, 2.0f) &&
				fabsf(length3(quads[index].x_axis) - 2.0f * length3(natural[index].x_axis)) < 1e-5f;
		corners = corner_at(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_WEAPON), 2.0f, -1, 1,
			-defaults.across, defaults.up, "weapon at scale 2") &&
			corner_at(quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_TRACKER), 2.0f, -1, -1,
			-defaults.across, -defaults.tracker_down, "tracker at scale 2");
		check(count == natural_count && sized && corners && quads[0].x_axis[0] == natural[0].x_axis[0],
			"display.hud_scale 2 doubles every group's size, keeps its outer corner, and leaves the reticle alone");
	}
	/* the clamp: each angle 0 to 40 degrees, the edge included; the scale
	0.5 to 2; not a number takes the default */
	{
		struct host_stereo_hud_placement placement = { 90.0f, -5.0f, 41.0f, 40.0f, 0.0f, 2.0f };
		struct host_stereo_hud_placement unchanged = HOST_STEREO_HUD_PLACEMENT_DEFAULT;
		struct host_stereo_hud_placement odd = { NAN, 20.0f, 22.0f, 12.0f, NAN, 2.0f };
		float reach = 0.0f;

		check(host_stereo_hud_placement_clamp(&placement) && placement.across == 40.0f && placement.up == 0.0f &&
			placement.tracker_down == 40.0f && placement.messages_up == 40.0f && placement.scale == 0.5f,
			"the angles clamp to 0 to 40 degrees, 40 itself kept, and the scale to 0.5");
		check(!host_stereo_hud_placement_clamp(&unchanged) && !memcmp(&unchanged, &defaults, sizeof(defaults)),
			"the defaults pass unchanged");
		check(host_stereo_hud_placement_clamp(&odd) && odd.across == defaults.across && odd.scale == defaults.scale,
			"a setting that isn't a number takes its default");
		/* at the edge, every corner of every group stays inside 40 degrees */
		placement = (struct host_stereo_hud_placement){ 40.0f, 40.0f, 40.0f, 40.0f, 2.0f, 2.0f };
		count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])extents, &placement, NULL, quads);
		for (index = 1; index < count - 1; index++)
		{
			float box[4];
			int axis;

			content_box(&quads[index], 2.0f, box);
			for (axis = 0; axis < 4; axis++)
				reach = fmaxf(reach, fabsf(box[axis]));
		}
		{
			char what[160];

			snprintf(what, sizeof(what), "at the clamp's edge and twice the size every group stays inside the shared "
				"view, 40 degrees (the farthest %.4f)", reach);
			check(reach <= 40.0f + 1e-3f, what);
		}
	}
	/* a head pitch or roll moves none of them; a body yaw moves all of them
	(the level frame, host_stereo_hud_level_yaw, at the device: x' = x cos
	yaw + z sin yaw, z' = z cos yaw - x sin yaw) */
	{
		float pitches[] = { 0.0f, 35.0f, -45.0f }, rolls[] = { 0.0f, 25.0f }, yaws[] = { 0.0f, 30.0f };
		float rooms[2][3][2][HOST_STEREO_HUD_MAXIMUM_QUADS][3];
		size_t y, p, r;
		float still = 0.0f, turned = 1e9f;

		count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])extents, NULL, NULL, quads);
		for (y = 0; y < 2; y++)
			for (p = 0; p < 3; p++)
				for (r = 0; r < 2; r++)
				{
					float a = yaws[y] * DEGREES, b = pitches[p] * DEGREES, c = rolls[r] * DEGREES;
					float forward[3] = { -sinf(a) * cosf(b), sinf(b), -cosf(a) * cosf(b) };
					float level_right[3] = { cosf(a), 0.0f, -sinf(a) };
					float up0[3] = { sinf(a) * sinf(b), cosf(b), cosf(a) * sinf(b) };
					float right[3], back[3], yaw;
					int k;

					for (k = 0; k < 3; k++)
					{
						right[k] = level_right[k] * cosf(c) - up0[k] * sinf(c);
						back[k] = -forward[k];
					}
					yaw = host_stereo_hud_level_yaw(right, back);
					for (index = 1; index < count - 1; index++)
					{
						const float *center = quads[index].center;

						rooms[y][p][r][index][0] = center[0] * cosf(yaw) + center[2] * sinf(yaw);
						rooms[y][p][r][index][1] = center[1];
						rooms[y][p][r][index][2] = center[2] * cosf(yaw) - center[0] * sinf(yaw);
					}
				}
		for (y = 0; y < 2; y++)
			for (p = 0; p < 3; p++)
				for (r = 0; r < 2; r++)
					for (index = 1; index < count - 1; index++)
					{
						int k;
						float moved = 0.0f;

						for (k = 0; k < 3; k++)
							still = fmaxf(still, fabsf(rooms[y][p][r][index][k] - rooms[y][0][0][index][k]));
						if (y == 1)
						{
							for (k = 0; k < 3; k++)
								moved += (rooms[1][p][r][index][k] - rooms[0][p][r][index][k]) *
									(rooms[1][p][r][index][k] - rooms[0][p][r][index][k]);
							turned = fminf(turned, sqrtf(moved));
						}
					}
		check(still < 1e-5f, "a head pitch or roll moves no group in the room");
		check(turned > 0.5f, "a body yaw of 30 degrees moves every group in the room");
	}
}

/* a quad's four corners' angles, across and up in degrees (angles_of),
in the order bottom left, bottom right, top left, top right */
static void corner_angles(const struct host_stereo_hud_quad *quad, float angles[4][2])
{
	int corner, axis;

	for (corner = 0; corner < 4; corner++)
	{
		float x = corner & 1 ? 1.0f : -1.0f, y = corner & 2 ? 1.0f : -1.0f, point[3];

		for (axis = 0; axis < 3; axis++)
			point[axis] = quad->center[axis] + x * quad->x_axis[axis] + y * quad->y_axis[axis];
		angles_of(point, angles[corner]);
	}
}

/* the worst difference, in degrees, between two quads' corners' angles */
static float corner_miss(const struct host_stereo_hud_quad *a, const struct host_stereo_hud_quad *b)
{
	float angles_a[4][2], angles_b[4][2], worst = 0.0f;
	int corner;

	corner_angles(a, angles_a);
	corner_angles(b, angles_b);
	for (corner = 0; corner < 4; corner++)
		worst = fmaxf(worst, fmaxf(fabsf(angles_a[corner][0] - angles_b[corner][0]),
			fabsf(angles_a[corner][1] - angles_b[corner][1])));
	return worst;
}

/* display.hud_distance (the placement's distance): the whole layout (the
reticle, the groups, the catch-all, the UI's quad) and the zoom's quad at
1.5, 1.75 and 2 m keep every quad's angular extents, and each quad sits
that distance out (a quad centered on its plane at it; one offset on a
shared plane, the prompt's and the messages', its 2 m center scaled); the
clamp takes 1 to 4 m, and a value that isn't a number the default */
static void distance_checks(void)
{
	const float ahead[3] = { 0.0f, 0.0f, -1.0f };
	const float zoom_tangents[2] = { 1.776f, 1.05f };
	const float distances[] = { 1.5f, 1.75f, 2.0f };
	const float width = 854.0f;
	struct host_stereo_hud_quad natural[HOST_STEREO_HUD_MAXIMUM_QUADS], quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
	struct host_stereo_hud_quad natural_zoom, zoom;
	float extents[HALO_HUD_GROUP_COUNT][4];
	int natural_count, count, index;
	size_t d;
	char what[240];

	sample_extents(width, extents);
	natural_count = host_stereo_hud_layout(width, 1, ahead, hud_tangents, (const float (*)[4])extents, NULL, NULL, natural);
	host_stereo_hud_zoom(zoom_tangents, HOST_STEREO_HUD_DISTANCE_DEFAULT, &natural_zoom);
	for (d = 0; d < sizeof(distances) / sizeof(distances[0]); d++)
	{
		struct host_stereo_hud_placement placement = HOST_STEREO_HUD_PLACEMENT_DEFAULT;
		float distance = distances[d], worst_angle = 0.0f, worst_length = 0.0f;

		placement.distance = distance;
		count = host_stereo_hud_layout(width, 1, ahead, hud_tangents, (const float (*)[4])extents, &placement, NULL, quads);
		host_stereo_hud_zoom(zoom_tangents, distance, &zoom);
		for (index = 0; index < count && count == natural_count; index++)
		{
			float wanted = length3(natural[index].center) * distance / HOST_STEREO_HUD_DISTANCE_DEFAULT;
			int layer = quads[index].layer;

			/* a quad centered on its plane: its center is the distance out */
			if (layer == HOST_STEREO_HUD_LAYER_RETICLE || layer == HOST_STEREO_HUD_LAYER_HUD ||
				layer == HOST_STEREO_HUD_LAYER_UI || layer == HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_WEAPON ||
				layer == HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_UNIT ||
				layer == HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_TRACKER)
				wanted = distance;
			worst_angle = fmaxf(worst_angle, corner_miss(&quads[index], &natural[index]));
			worst_length = fmaxf(worst_length, fabsf(length3(quads[index].center) - wanted));
		}
		worst_angle = fmaxf(worst_angle, corner_miss(&zoom, &natural_zoom));
		worst_length = fmaxf(worst_length, fabsf(length3(zoom.center) - distance));
		snprintf(what, sizeof(what), "display.hud_distance %.2f m: every quad (the reticle, %d groups, the catch-all, "
			"the UI, the zoom) keeps its angular extents (worst %.5f degrees) and sits %.2f m out (worst %.6f m)",
			distance, count - 3, worst_angle, distance, worst_length);
		check(count == natural_count && worst_angle < 0.01f && worst_length < 1e-4f, what);
		if (distance == HOST_STEREO_HUD_DISTANCE_DEFAULT)
			check(!memcmp(quads, natural, (size_t)count * sizeof(quads[0])) &&
				!memcmp(&zoom, &natural_zoom, sizeof(zoom)), "at 2 m the layout is byte for byte the default's");
	}
	{
		struct host_stereo_hud_placement near = HOST_STEREO_HUD_PLACEMENT_DEFAULT, far = near, odd = near;

		near.distance = 0.5f;
		far.distance = 9.0f;
		odd.distance = NAN;
		check(host_stereo_hud_placement_clamp(&near) && near.distance == 1.0f &&
			host_stereo_hud_placement_clamp(&far) && far.distance == 4.0f &&
			host_stereo_hud_placement_clamp(&odd) && odd.distance == HOST_STEREO_HUD_DISTANCE_DEFAULT,
			"display.hud_distance clamps 0.5 m to 1, 9 m to 4, and a value that isn't a number to 2");
	}
}

/* depth-adaptive placement (host_stereo_hud_depth_ease): with the
defaults at 90 Hz, each piece rests at the distance, pulls in to 0.85 of
what's nearest under it within 0.1 s, never nearer than that or the floor,
and relaxes back over 1 s after holding 0.5 s */
#define DEPTH_FRAME (1.0f / 90.0f)
static void depth_ease_checks(void)
{
	struct host_stereo_hud_depth_settings settings = HOST_STEREO_HUD_DEPTH_SETTINGS_DEFAULT;
	struct host_stereo_hud_depth_state state = HOST_STEREO_HUD_DEPTH_STATE_INITIAL, before;
	float distance = 0.0f, last_diopters, t, reached, held, back;
	int frame, monotone = 1, never_nearer = 1, floor_kept = 1;
	char what[240];

	check(!host_stereo_hud_depth_settings_clamp(&settings), "the depth settings' defaults pass their clamp unchanged");
	for (frame = 0; frame < 90; frame++)
		distance = host_stereo_hud_depth_ease(&state, INFINITY, DEPTH_FRAME, &settings);
	check(distance == 2.0f, "with nothing near under a piece it rests at 2.00 m");
	/* something 1 m away under it: 0.85 m, in diopters at once */
	last_diopters = 1.0f / distance;
	reached = -1.0f;
	for (frame = 1, t = DEPTH_FRAME; frame <= 45; frame++, t += DEPTH_FRAME)
	{
		distance = host_stereo_hud_depth_ease(&state, 1.0f, DEPTH_FRAME, &settings);
		monotone &= 1.0f / distance >= last_diopters - 1e-6f;
		never_nearer &= distance >= 0.85f - 1e-5f;
		last_diopters = 1.0f / distance;
		if (reached < 0.0f && fabsf(distance - 0.85f) <= 0.01f)
			reached = t;
	}
	snprintf(what, sizeof(what), "something 1 m under it: 0.85 +- 0.01 m after %.3f s (within 0.1 s and a frame), "
		"monotone in diopters, never nearer than 0.85 m", reached);
	check(reached >= 0.0f && reached <= 0.1f + DEPTH_FRAME + 1e-4f && monotone && never_nearer, what);
	/* 0.2 m: the floor */
	for (frame = 0; frame < 45; frame++)
	{
		distance = host_stereo_hud_depth_ease(&state, 0.2f, DEPTH_FRAME, &settings);
		floor_kept &= distance >= 0.3f - 1e-5f;
	}
	check(fabsf(distance - 0.3f) < 1e-4f && floor_kept, "something 0.2 m under it: the floor, 0.30 m, and never nearer");
	/* back to far: holds, then relaxes */
	held = -1.0f;
	back = -1.0f;
	for (frame = 1, t = DEPTH_FRAME; frame <= 250; frame++, t += DEPTH_FRAME)
	{
		float now = host_stereo_hud_depth_ease(&state, INFINITY, DEPTH_FRAME, &settings);

		if (held < 0.0f && now > 0.3f + 1e-4f)
			held = t - DEPTH_FRAME;
		if (back < 0.0f && now >= 2.0f - 1e-4f)
			back = t;
		distance = now;
	}
	snprintf(what, sizeof(what), "back to far: it holds %.3f s (0.5 +- a frame), then is at 2.00 m %.3f s later "
		"(within 1.0 s and a frame)", held, back - held);
	check(fabsf(held - 0.5f) <= DEPTH_FRAME + 1e-4f && back > 0.0f && back - held <= 1.0f + DEPTH_FRAME + 1e-4f &&
		distance == 2.0f, what);
	/* one frame of something near pulls in at once */
	distance = host_stereo_hud_depth_ease(&state, 1.0f, DEPTH_FRAME, &settings);
	check(distance < 2.0f - 1e-3f, "one frame of something 1 m under it pulls the piece in at once");
	before = state;
	check(host_stereo_hud_depth_ease(&state, 0.4f, 0.0f, &settings) == distance && !memcmp(&state, &before,
		sizeof(state)), "a frame of 0 s changes nothing");
	{
		struct host_stereo_hud_depth_state far_state = state, odd_state = state, rest = HOST_STEREO_HUD_DEPTH_STATE_INITIAL;
		float far_distance = host_stereo_hud_depth_ease(&far_state, INFINITY, DEPTH_FRAME, &settings);
		float odd_distance = host_stereo_hud_depth_ease(&odd_state, NAN, DEPTH_FRAME, &settings);

		check(odd_distance == far_distance && !memcmp(&odd_state, &far_state, sizeof(far_state)) &&
			host_stereo_hud_depth_ease(&rest, NAN, DEPTH_FRAME, &settings) == 2.0f &&
			host_stereo_hud_depth_ease(&rest, 0.0f, DEPTH_FRAME, &settings) == 2.0f,
			"a nearest that isn't a number (or isn't positive) is far");
	}
	{
		struct host_stereo_hud_depth_settings off = settings;
		struct host_stereo_hud_depth_state pulled = state;

		off.enabled = 0;
		check(host_stereo_hud_depth_ease(&pulled, 0.5f, DEPTH_FRAME, &off) == 2.0f,
			"with display.hud_depth off every piece rests at the distance");
	}
	/* the clamp: the floor below the distance, the share 0.5 to 1, the
	seconds 0 to 5, not a number the default */
	{
		struct host_stereo_hud_depth_settings odd = settings, high = settings;

		high.distance = 1.0f;
		high.floor = 1.5f;
		odd.share = 2.0f;
		odd.pull_in = -1.0f;
		odd.relax = 9.0f;
		odd.relax_delay = NAN;
		odd.floor = NAN;
		check(host_stereo_hud_depth_settings_clamp(&high) && high.floor < high.distance && high.floor > 0.0f,
			"a floor above the distance clamps below it");
		check(host_stereo_hud_depth_settings_clamp(&odd) && odd.share == 1.0f && odd.pull_in == 0.0f &&
			odd.relax == 5.0f && odd.relax_delay == 0.5f && odd.floor == 0.3f,
			"the share clamps to 0.5 to 1, the seconds to 0 to 5, and a value that isn't a number takes its default");
	}
}

/* the ease's deadband (HOST_STEREO_HUD_DEPTH_DEADBAND_DIOPTERS): b30's
Warthog ride, a passenger, had the meters over the frame whose nearest
swung between 0.61 and 0.71 m (0.27 diopters of target) for 0.7 s or so at
a time, which relaxed and pulled the piece back in about every 1.1 s. Held
in the band, the piece stays put; a real approach still pulls in at once,
a real recession still relaxes, and nothing under it still rests */
static float depth_run(struct host_stereo_hud_depth_state *state, float nearest, float seconds,
	const struct host_stereo_hud_depth_settings *settings, float *lowest, float *highest)
{
	float distance = 0.0f, t;

	for (t = 0.0f; t < seconds - 1e-4f; t += DEPTH_FRAME)
	{
		distance = host_stereo_hud_depth_ease(state, nearest, DEPTH_FRAME, settings);
		if (lowest)
			*lowest = fminf(*lowest, distance);
		if (highest)
			*highest = fmaxf(*highest, distance);
	}
	return distance;
}

static void depth_deadband_checks(void)
{
	struct host_stereo_hud_depth_settings settings = HOST_STEREO_HUD_DEPTH_SETTINGS_DEFAULT;
	struct host_stereo_hud_depth_state state = HOST_STEREO_HUD_DEPTH_STATE_INITIAL;
	float near_target = 1.0f / (0.85f * 0.61f), far_target = 1.0f / (0.85f * 0.71f);
	float held, lowest = INFINITY, highest = 0.0f, distance, t, back;
	int cycle, frame;
	char what[240];

	check(near_target - far_target < HOST_STEREO_HUD_DEPTH_DEADBAND_DIOPTERS,
		"the ride's 0.61 to 0.71 m swing is inside the deadband");
	held = depth_run(&state, 0.61f, 0.2f, &settings, NULL, NULL);
	for (cycle = 0; cycle < 4; cycle++)
	{
		depth_run(&state, 0.71f, 0.7f, &settings, &lowest, &highest);
		depth_run(&state, 0.61f, 0.7f, &settings, &lowest, &highest);
	}
	snprintf(what, sizeof(what), "a nearest swinging 0.61 to 0.71 m every 0.7 s holds the piece at %.3f m for 5.6 s "
		"(it went %.3f to %.3f m)", held, lowest, highest);
	check(fabsf(held - 0.85f * 0.61f) < 1e-3f && highest - lowest < 1e-4f && fabsf(highest - held) < 1e-4f, what);
	/* something comes to 0.4 m: in at once */
	back = -1.0f;
	for (frame = 1, t = DEPTH_FRAME; frame <= 18; frame++, t += DEPTH_FRAME)
	{
		distance = host_stereo_hud_depth_ease(&state, 0.4f, DEPTH_FRAME, &settings);
		if (back < 0.0f && fabsf(distance - 0.34f) <= 0.01f)
			back = t;
	}
	snprintf(what, sizeof(what), "then something at 0.4 m pulls it in to 0.34 m after %.3f s (within 0.1 s and a frame)",
		back);
	check(back >= 0.0f && back <= 0.1f + DEPTH_FRAME + 1e-4f, what);
	/* it goes back to 1.5 m, well past the band: holds, then relaxes */
	distance = depth_run(&state, 1.5f, 0.5f + 1.0f + 2.0f * DEPTH_FRAME, &settings, NULL, NULL);
	snprintf(what, sizeof(what), "then at 1.5 m it relaxes out to 1.275 m (at %.3f m 1.5 s later)", distance);
	check(fabsf(distance - 0.85f * 1.5f) < 1e-3f, what);
	/* nothing under it, though 1.275 m is inside the band of the rest */
	distance = depth_run(&state, INFINITY, 0.5f + 1.0f + 2.0f * DEPTH_FRAME, &settings, NULL, NULL);
	snprintf(what, sizeof(what), "and with nothing under it, it rests at 2.00 m again (at %.3f m 1.5 s later)",
		distance);
	check(distance == 2.0f, what);
}

/* the layout with a distance for each piece: every quad keeps its angular
extents, each piece's quads sit at its distance, and without distances the
quads are byte for byte the resting layout's */
static void depth_layout_checks(void)
{
	const float ahead[3] = { 0.0f, 0.0f, -1.0f };
	const float pieces[HOST_STEREO_HUD_PIECE_COUNT] = { 0.5f, 0.4f, 1.0f, 2.0f, 0.3f };
	const float rest[HOST_STEREO_HUD_PIECE_COUNT] = { 2.0f, 2.0f, 2.0f, 2.0f, 2.0f };
	const float width = 854.0f;
	struct host_stereo_hud_quad natural[HOST_STEREO_HUD_MAXIMUM_QUADS], quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
	float extents[HALO_HUD_GROUP_COUNT][4], worst_angle = 0.0f, worst_length = 0.0f;
	int natural_count, count, index, piece_seen[HOST_STEREO_HUD_PIECE_COUNT] = { 0 }, all_seen = 1;
	char what[240];

	sample_extents(width, extents);
	natural_count = host_stereo_hud_layout(width, 1, ahead, hud_tangents, (const float (*)[4])extents, NULL, NULL,
		natural);
	count = host_stereo_hud_layout(width, 1, ahead, hud_tangents, (const float (*)[4])extents, NULL, pieces, quads);
	for (index = 0; index < count && count == natural_count; index++)
	{
		int piece = host_stereo_hud_piece(&quads[index]), layer = quads[index].layer;
		float distance = piece >= 0 ? pieces[piece] : HOST_STEREO_HUD_DISTANCE_DEFAULT;
		float wanted = length3(natural[index].center) * distance / HOST_STEREO_HUD_DISTANCE_DEFAULT;

		if (piece >= 0)
			piece_seen[piece] = 1;
		if (layer != HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_PROMPT &&
			layer != HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_MESSAGES)
			wanted = distance;
		worst_angle = fmaxf(worst_angle, corner_miss(&quads[index], &natural[index]));
		worst_length = fmaxf(worst_length, fabsf(length3(quads[index].center) - wanted));
	}
	for (index = 0; index < HOST_STEREO_HUD_PIECE_COUNT; index++)
		all_seen &= piece_seen[index];
	snprintf(what, sizeof(what), "pieces at 0.5, 0.4, 1.0, 2.0 and 0.3 m: every quad keeps its angular extents "
		"(worst %.5f degrees) and its piece's distance (worst %.6f m); the catch-all and the UI stay at 2 m",
		worst_angle, worst_length);
	check(count == natural_count && all_seen && worst_angle < 0.01f && worst_length < 1e-4f &&
		host_stereo_hud_piece(&quads[count - 1]) == HOST_STEREO_HUD_PIECE_NONE &&
		host_stereo_hud_piece(&quads[count - 2]) == HOST_STEREO_HUD_PIECE_NONE, what);
	count = host_stereo_hud_layout(width, 1, ahead, hud_tangents, (const float (*)[4])extents, NULL, rest, quads);
	check(count == natural_count && !memcmp(quads, natural, (size_t)count * sizeof(quads[0])),
		"every piece at the resting distance, or no distances, is byte for byte the resting layout");
}

/* a view's projection from its half tangents (left, right, up, down, as
view_tangents in host_stereo.m), column-major, w = -z */
static void projection_of(const float tangents[4], float clip[16])
{
	float l = tangents[0], r = tangents[1], u = tangents[2], d = tangents[3];

	memset(clip, 0, 16 * sizeof(float));
	clip[0] = 2.0f / (l + r);
	clip[5] = 2.0f / (u + d);
	clip[8] = (r - l) / (l + r);
	clip[9] = (u - d) / (u + d);
	clip[11] = -1.0f;
	clip[14] = 0.1f;
}

/* the footprint (host_stereo_hud_footprint): a quad's rectangle in the
view's normalized picture coordinates (u right, v down), grown by
HOST_STEREO_HUD_DEPTH_MARGIN_DEGREES through the view's tangents */
static void footprint_checks(void)
{
	const float tangents[4] = { 1.76f, 1.01f, 1.0f, 1.05f };
	const float margin = HOST_STEREO_HUD_DEPTH_MARGIN_DEGREES * DEGREES;
	float clip[16], rectangle[4], expected[4];
	float half = 2.0f * tanf(3.5f * DEGREES);
	struct host_stereo_hud_quad reticle;
	char what[240];

	projection_of(tangents, clip);
	memset(&reticle, 0, sizeof(reticle));
	reticle.center[2] = -2.0f;
	reticle.x_axis[0] = half;
	reticle.y_axis[1] = half;
	reticle.frame = HOST_STEREO_HUD_HEAD;
	expected[0] = (tanf(-3.5f * DEGREES - margin) + tangents[0]) / (tangents[0] + tangents[1]);
	expected[2] = (tanf(3.5f * DEGREES + margin) + tangents[0]) / (tangents[0] + tangents[1]);
	expected[1] = (tangents[2] - tanf(3.5f * DEGREES + margin)) / (tangents[2] + tangents[3]);
	expected[3] = (tangents[2] + tanf(3.5f * DEGREES + margin)) / (tangents[2] + tangents[3]);
	check(host_stereo_hud_footprint(&reticle, clip, NULL, tangents, rectangle), "a 7 degree reticle ahead has a footprint");
	snprintf(what, sizeof(what), "its footprint is %.5f..%.5f by %.5f..%.5f (expected %.5f..%.5f by %.5f..%.5f: "
		"7 degrees and a degree each side)", rectangle[0], rectangle[2], rectangle[1], rectangle[3], expected[0],
		expected[2], expected[1], expected[3]);
	check(fabsf(rectangle[0] - expected[0]) < 1e-5f && fabsf(rectangle[1] - expected[1]) < 1e-5f &&
		fabsf(rectangle[2] - expected[2]) < 1e-5f && fabsf(rectangle[3] - expected[3]) < 1e-5f, what);
	/* the weapon's group, its top left corner at -28 across and 20 up on
	the level frame, with the head turned 10 degrees right of the level
	frame's yaw: the footprint's left edge at -18 less the margin */
	{
		const float ahead[3] = { 0.0f, 0.0f, -1.0f };
		struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
		float extents[HALO_HUD_GROUP_COUNT][4], level[16] = { 0 }, yaw = 10.0f * DEGREES;
		const struct host_stereo_hud_quad *weapon;
		float left0, left10;
		int count;

		sample_extents(640.0f, extents);
		count = host_stereo_hud_layout(640.0f, 0, ahead, hud_tangents, (const float (*)[4])extents, NULL, NULL, quads);
		weapon = quad_of(quads, count, HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_WEAPON);
		/* the level frame in the device's: a direction at across a there is
		at a + yaw in the device's */
		level[0] = cosf(yaw);
		level[2] = sinf(yaw);
		level[5] = 1.0f;
		level[8] = -sinf(yaw);
		level[10] = cosf(yaw);
		level[15] = 1.0f;
		host_stereo_hud_footprint(weapon, clip, NULL, tangents, rectangle);
		/* the quad's outer edge (its margin's lines in) is a little out of
		the content's -28 degree corner */
		left0 = atanf(rectangle[0] * (tangents[0] + tangents[1]) - tangents[0]) / DEGREES;
		host_stereo_hud_footprint(weapon, clip, level, tangents, rectangle);
		left10 = atanf(rectangle[0] * (tangents[0] + tangents[1]) - tangents[0]) / DEGREES;
		{
			/* the quad's own outer edge, from its corners */
			float angles[4][2], edge;

			corner_angles(weapon, angles);
			edge = fminf(angles[0][0], angles[2][0]);
			snprintf(what, sizeof(what), "a level quad lands where corner_plane put it: its footprint's left edge at "
				"%.4f degrees (its quad's %.4f less the margin), %.4f with the level frame turned 10 degrees", left0,
				edge, left10);
			check(fabsf(left0 - (edge - HOST_STEREO_HUD_DEPTH_MARGIN_DEGREES)) < 1e-3f &&
				fabsf(left10 - (edge + 10.0f - HOST_STEREO_HUD_DEPTH_MARGIN_DEGREES)) < 1e-3f, what);
		}
	}
	{
		struct host_stereo_hud_quad behind = reticle;

		behind.center[2] = 2.0f;
		check(!host_stereo_hud_footprint(&behind, clip, NULL, tangents, rectangle), "a quad behind the view has none");
	}
	{
		float screen[4];

		host_stereo_hud_footprint(&reticle, clip, NULL, tangents, rectangle);
		host_stereo_hud_footprint_screen(rectangle, 1888.0f, 1792.0f, screen);
		check(screen[0] == rectangle[0] * 1888.0f && screen[1] == rectangle[1] * 1792.0f &&
			screen[2] == rectangle[2] * 1888.0f && screen[3] == rectangle[3] * 1792.0f,
			"the screen rectangle the host maps to physical texels is the footprint times the screen, unchanged");
	}
}

/* whether a direction (across, up in degrees, from the view's center)
lands inside a footprint in the view of projection_of(tangents) */
static int footprint_holds(const float rectangle[4], const float tangents[4], float across, float up)
{
	float u = (tanf(across * DEGREES) + tangents[0]) / (tangents[0] + tangents[1]);
	float v = (tangents[2] - tanf(up * DEGREES) / cosf(across * DEGREES)) / (tangents[2] + tangents[3]);

	return u >= rectangle[0] && u <= rectangle[2] && v >= rectangle[1] && v <= rectangle[3];
}

/* the reticle's depth is measured under the crosshair only
(host_stereo_hud_depth_quad): a square HOST_STEREO_HUD_RETICLE_DEPTH_LINES
across, about 7 degrees, centered on the crosshair wherever the reticle is
drawn; a wall at the layer's edge isn't under it, something under the
crosshair is; every other piece is measured as drawn */
static void reticle_depth_checks(void)
{
	const float tangents[4] = { 1.76f, 1.01f, 1.0f, 1.05f };
	const float ahead[3] = { 0.0f, 0.0f, -1.0f };
	const float width = 854.0f;
	float clip[16], rectangle[4], extents[HALO_HUD_GROUP_COUNT][4], degrees, half;
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS], measured;
	int count, index, others = 1;
	char what[240];

	projection_of(tangents, clip);
	sample_extents(width, extents);
	count = host_stereo_hud_layout(width, 0, ahead, hud_tangents, (const float (*)[4])extents, NULL, NULL, quads);
	host_stereo_hud_depth_quad(&quads[0], width, &measured);
	host_stereo_hud_footprint(&measured, clip, NULL, tangents, rectangle);
	half = atanf(length3(measured.x_axis) / -measured.center[2]) / DEGREES;
	degrees = 2.0f * half;
	snprintf(what, sizeof(what), "the reticle's depth square is %.2f degrees across (%.0f lines), centered on the "
		"crosshair", degrees, HOST_STEREO_HUD_RETICLE_DEPTH_LINES);
	check(fabsf(degrees - 6.87f) < 0.05f && fabsf(length3(measured.x_axis) - length3(measured.y_axis)) < 1e-6f &&
		!memcmp(measured.center, quads[0].center, sizeof(measured.center)), what);
	/* the layer reaches about 21 degrees each side at 854 lines */
	check(!footprint_holds(rectangle, tangents, 18.0f, 0.0f) && !footprint_holds(rectangle, tangents, -18.0f, 0.0f) &&
		!footprint_holds(rectangle, tangents, 0.0f, -14.0f) && !footprint_holds(rectangle, tangents, half + 1.5f, 0.0f),
		"a near wall at the layer's edge (18 degrees out, or 14 down) is outside it: it doesn't pull the crosshair in");
	check(footprint_holds(rectangle, tangents, 0.0f, 0.0f) && footprint_holds(rectangle, tangents, 2.0f, -2.0f) &&
		footprint_holds(rectangle, tangents, half + 0.5f, 0.0f),
		"something under the crosshair (and within the margin) is inside it: it pulls the crosshair in");
	for (index = 1; index < count; index++)
	{
		host_stereo_hud_depth_quad(&quads[index], width, &measured);
		others &= !memcmp(&measured, &quads[index], sizeof(measured));
	}
	check(others, "every other piece's depth is measured under its whole quad");
	/* a seat's reticle 40 degrees right: the square follows it */
	{
		float yaw = 40.0f * DEGREES, seat[3] = { sinf(yaw), 0.0f, -cosf(yaw) };

		count = host_stereo_hud_layout(width, 0, seat, hud_tangents, (const float (*)[4])extents, NULL, NULL, quads);
		host_stereo_hud_depth_quad(&quads[0], width, &measured);
		host_stereo_hud_footprint(&measured, clip, NULL, tangents, rectangle);
		check(quads[0].layer == HOST_STEREO_HUD_LAYER_RETICLE && footprint_holds(rectangle, tangents, 40.0f, 0.0f) &&
			!footprint_holds(rectangle, tangents, 0.0f, 0.0f) && !footprint_holds(rectangle, tangents, 50.0f, 0.0f),
			"a seat's reticle 40 degrees right: the square follows the crosshair there");
	}
}

static void ui_checks(float aspect)
{
	struct host_stereo_hud_quad quad;
	float degrees[4];
	char what[160];
	const float ahead[3] = { 0.0f, 0.0f, -1.0f };
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];

	host_stereo_hud_ui(aspect, HOST_STEREO_HUD_DISTANCE_DEFAULT, &quad);
	level_extent(&quad, degrees);
	snprintf(what, sizeof(what), "the UI at %.3f:1 is %.1f by %.1f degrees, inside the sharp region, with its shape",
		aspect, degrees[2] - degrees[0], degrees[3] - degrees[1]);
	check(quad.frame == HOST_STEREO_HUD_LEVEL && fmaxf(degrees[2], degrees[3]) <= HUD_SHARP_RADIUS_DEGREES + 1e-3f &&
		fmaxf(degrees[2], degrees[3]) >= HUD_SHARP_RADIUS_DEGREES - 1e-3f &&
		fabsf(quad.x_axis[0] / quad.y_axis[1] - aspect) < 1e-4f && quad.source[2] == 1.0f && quad.source[3] == 1.0f,
		what);
	{
		float extents[HALO_HUD_GROUP_COUNT][4];
		struct host_stereo_hud_quad pieces[HOST_STEREO_HUD_MAXIMUM_QUADS];
		int count, piece_count;

		sample_extents(aspect * 480.0f, extents);
		piece_count = host_stereo_hud_layout(aspect * 480.0f, 0, ahead, hud_tangents, (const float (*)[4])extents,
			NULL, NULL, pieces);
		count = host_stereo_hud_layout(aspect * 480.0f, 1, ahead, hud_tangents, (const float (*)[4])extents, NULL, NULL, quads);
		check(count == piece_count + 1 && count <= HOST_STEREO_HUD_MAXIMUM_QUADS &&
			!memcmp(quads, pieces, (size_t)piece_count * sizeof(quads[0])) &&
			quads[count - 1].layer == HOST_STEREO_HUD_LAYER_UI && quads[count - 1].frame == HOST_STEREO_HUD_LEVEL &&
			!memcmp(quads[count - 1].center, quad.center, sizeof(quad.center)) &&
			!memcmp(quads[count - 1].x_axis, quad.x_axis, sizeof(quad.x_axis)) &&
			!memcmp(quads[count - 1].y_axis, quad.y_axis, sizeof(quad.y_axis)),
			"a menu: the HUD stays in its pieces, and only the UI layer goes whole on the UI's quad, over them");
	}
}

static void reticle_checks(void)
{
	struct host_stereo_hud_quad quad;
	/* a seat: the game camera aims 30 degrees right of where the head
	turned the picture, and 10 degrees up */
	float yaw = -30.0f * DEGREES, pitch = 10.0f * DEGREES;
	float aim[3] = { -sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch) };
	float behind[3] = { 0.3f, 0.0f, 1.0f };
	float facing;

	check(host_stereo_hud_reticle(640.0f, NULL, aim, HOST_STEREO_HUD_DISTANCE_DEFAULT, &quad) == 1, "a seat's reticle shows while its aim is ahead");
	check(fabsf(atan2f(quad.center[0], -quad.center[2]) - 30.0f * DEGREES) < 1e-4f &&
		fabsf(asinf(quad.center[1] / HOST_STEREO_HUD_DISTANCE_DEFAULT) - 10.0f * DEGREES) < 1e-4f,
		"a seat's reticle sits along the game camera's aim, 30 degrees right and 10 up");
	facing = (quad.x_axis[1] * quad.y_axis[2] - quad.x_axis[2] * quad.y_axis[1]) * quad.center[0] +
		(quad.x_axis[2] * quad.y_axis[0] - quad.x_axis[0] * quad.y_axis[2]) * quad.center[1] +
		(quad.x_axis[0] * quad.y_axis[1] - quad.x_axis[1] * quad.y_axis[0]) * quad.center[2];
	check(facing < 0.0f && fabsf(quad.x_axis[1]) < 1e-6f, "it faces the eyes, upright");
	check(host_stereo_hud_reticle(640.0f, NULL, behind, HOST_STEREO_HUD_DISTANCE_DEFAULT, &quad) == 0, "it leaves the view when the head turns far from the aim");
	/* Task 11: at a point (where a controller's aim hits), facing back
	along the aim */
	{
		float point[3] = { 0.3f, -0.2f, -4.0f };

		check(host_stereo_hud_reticle(640.0f, point, aim, HOST_STEREO_HUD_DISTANCE_DEFAULT, &quad) == 1 && quad.center[0] == 0.3f &&
			quad.center[1] == -0.2f && quad.center[2] == -4.0f && fabsf(quad.x_axis[1]) < 1e-6f,
			"given a point, the reticle is centered there, upright, facing back along the aim");
	}
}

/* the zoom (halo_stereo.h, "Zoom fills the view"): the zoomed picture whole
on an opaque, head-locked quad on the HUD's plane, as wide and tall as the
view's half tangents there */
static void zoom_checks(void)
{
	struct host_stereo_hud_quad quad;
	const float tangents[2] = { 1.776f, 1.05f };
	const float none[2] = { 0.0f, 1.0f };
	char what[200];

	check(host_stereo_hud_zoom(tangents, HOST_STEREO_HUD_DISTANCE_DEFAULT, &quad) == 1 && quad.opaque && quad.frame == HOST_STEREO_HUD_HEAD &&
		!quad.catch_all, "the zoomed picture is an opaque, head-locked quad");
	check(fabsf(quad.center[0]) < 1e-6f && fabsf(quad.center[1]) < 1e-6f &&
		fabsf(quad.center[2] + HOST_STEREO_HUD_DISTANCE_DEFAULT) < 1e-6f && HOST_STEREO_HUD_DISTANCE_DEFAULT ==
		HALO_STEREO_ZOOM_DISTANCE_METERS, "straight ahead on the HUD's plane, 2 m away: its depth is the HUD's");
	snprintf(what, sizeof(what), "it spans the view: %.3f by %.3f m there (%.1f by %.1f degrees)",
		2.0f * quad.x_axis[0], 2.0f * quad.y_axis[1], 2.0f * atanf(tangents[0]) / DEGREES,
		2.0f * atanf(tangents[1]) / DEGREES);
	check(fabsf(quad.x_axis[0] - tangents[0] * HOST_STEREO_HUD_DISTANCE_DEFAULT) < 1e-5f &&
		fabsf(quad.y_axis[1] - tangents[1] * HOST_STEREO_HUD_DISTANCE_DEFAULT) < 1e-5f && quad.x_axis[1] == 0.0f &&
		quad.x_axis[2] == 0.0f && quad.y_axis[0] == 0.0f && quad.y_axis[2] == 0.0f, what);
	check(quad.source[0] == 0.0f && quad.source[1] == 0.0f && quad.source[2] == 1.0f && quad.source[3] == 1.0f,
		"and shows the zoomed picture whole: its frustum is the view's");
	check(host_stereo_hud_zoom(none, HOST_STEREO_HUD_DISTANCE_DEFAULT, &quad) == 0, "without a view's tangents there's none");
}

static void scoped_reticle_plane_checks(void)
{
	const float ray[3] = { 0.3492f, 0.12f, -1.0f };
	const float distances[] = { 1.5f, 1.75f, 2.0f };
	const float eye_offsets[2][2] = { { -0.032f, 0.004f }, { 0.029f, -0.003f } };
	const float eye_frusta[2][4] = { { 0.71f, 0.91f, 0.75f, 0.81f },
		{ 0.86f, 0.76f, 0.78f, 0.74f } };
	struct host_stereo_hud_placement placement = HOST_STEREO_HUD_PLACEMENT_DEFAULT;
	struct host_stereo_hud_aim aim = { 0 };
	struct host_stereo_hud_quad quads[HOST_STEREO_HUD_MAXIMUM_QUADS];
	size_t i;

	for (i = 0; i < sizeof(distances) / sizeof(distances[0]); i++)
	{
		float distance = distances[i];
		int eye;

		placement.distance = distance;
		aim.zoomed = 1;
		aim.world = 0;
		check(host_stereo_hud_layout_aimed(640.0f, 0, ray, &aim, hud_tangents, NULL, &placement, NULL,
			quads) > 0, "ordinary scoped reticle is laid out");
		check(fabsf(quads[0].center[2] + distance) < 1e-4f &&
			fabsf(quads[0].center[0] - ray[0] * distance) < 1e-4f &&
			fabsf(quads[0].center[1] - ray[1] * distance) < 1e-4f,
			"ordinary scoped ray meets the live zoom plane");
		for (eye = 0; eye < 2; eye++)
		{
			float left = eye_frusta[eye][0], right = eye_frusta[eye][1];
			float up = eye_frusta[eye][2], down = eye_frusta[eye][3];
			float expected_u = (ray[0] - eye_offsets[eye][0] / distance + left) / (left + right);
			float expected_v = (up - ray[1] + eye_offsets[eye][1] / distance) / (up + down);
			float actual_u = ((quads[0].center[0] - eye_offsets[eye][0]) / -quads[0].center[2] + left) /
				(left + right);
			float actual_v = (up - (quads[0].center[1] - eye_offsets[eye][1]) / -quads[0].center[2]) /
				(up + down);
			check(fabsf(actual_u - expected_u) < 1e-4f && fabsf(actual_v - expected_v) < 1e-4f,
				"each asymmetric eye sees the ordinary reticle on the matching zoom pixel");
		}

		aim.world = 1;
		/* The old guest assumes a fixed 2 m zoom plane. The host's live
		placement must win even if that old distance reaches this boundary. */
		aim.distance = 2.0f * sqrtf(ray[0] * ray[0] + ray[1] * ray[1] + 1.0f);
		check(host_stereo_hud_layout_aimed(640.0f, 0, ray, &aim, hud_tangents, NULL, &placement, NULL,
			quads) > 0, "tracked scoped reticle is laid out");
		check(fabsf(quads[0].center[2] + distance) < 1e-4f &&
			fabsf(quads[0].center[0] - ray[0] * distance) < 1e-4f &&
			fabsf(quads[0].center[1] - ray[1] * distance) < 1e-4f,
			"tracked scoped ray meets the same live zoom plane");
		for (eye = 0; eye < 2; eye++)
		{
			float left = eye_frusta[eye][0], right = eye_frusta[eye][1];
			float up = eye_frusta[eye][2], down = eye_frusta[eye][3];
			float expected_u = (ray[0] - eye_offsets[eye][0] / distance + left) / (left + right);
			float expected_v = (up - ray[1] + eye_offsets[eye][1] / distance) / (up + down);
			float actual_u = ((quads[0].center[0] - eye_offsets[eye][0]) / -quads[0].center[2] + left) /
				(left + right);
			float actual_v = (up - (quads[0].center[1] - eye_offsets[eye][1]) / -quads[0].center[2]) /
				(up + down);
			check(fabsf(actual_u - expected_u) < 1e-4f && fabsf(actual_v - expected_v) < 1e-4f,
				"each asymmetric eye sees the tracked reticle on the matching zoom pixel");
		}
	}
}

/* The presenter maps the guest's zero-distance hidden sentinel before
near/far clipping. Exercise the same clamp that the host calls. */
static void scoped_hidden_clamp_checks(void)
{
	struct host_stereo_hud_aim hidden = { 0 };
	struct host_stereo_hud_aim world = { 0 };
	struct host_stereo_hud_aim scoped = { 0 };

	hidden.world = hidden.zoomed = 1;
	host_stereo_hud_aim_depth_clamp(&hidden, 1.0f, 10.0f);
	check(hidden.distance == 0.0f, "hidden scoped reticle stays hidden through the host depth clamp");
	world.world = 1;
	world.distance = 0.5f;
	host_stereo_hud_aim_depth_clamp(&world, 1.0f, 10.0f);
	check(fabsf(world.distance - 1.05f) < 1e-6f, "visible world reticle stays inside the near plane");
	scoped.world = scoped.zoomed = 1;
	scoped.distance = 2.0f;
	host_stereo_hud_aim_depth_clamp(&scoped, 1.0f, 1.5f);
	check(scoped.distance == 2.0f, "visible scoped reticle keeps its plane marker");
}

static void level_checks(void)
{
	float yaws[] = { 0.0f, 40.0f, -120.0f, 179.0f };
	float pitches[] = { 0.0f, 30.0f, -60.0f, 89.0f };
	float rolls[] = { 0.0f, 20.0f };
	size_t y, p, r;
	float worst = 0.0f;
	char what[120];

	for (y = 0; y < 4; y++)
		for (p = 0; p < 4; p++)
			for (r = 0; r < 2; r++)
			{
				/* the device's axes in the room for yaw (left positive), then
				pitch, then roll */
				float a = yaws[y] * DEGREES, b = pitches[p] * DEGREES, c = rolls[r] * DEGREES;
				float forward[3] = { -sinf(a) * cosf(b), sinf(b), -cosf(a) * cosf(b) };
				float level_right[3] = { cosf(a), 0.0f, -sinf(a) };
				float up0[3] = { sinf(a) * sinf(b), cosf(b), cosf(a) * sinf(b) };
				float right[3], back[3], difference;
				int k;

				for (k = 0; k < 3; k++)
				{
					right[k] = level_right[k] * cosf(c) - up0[k] * sinf(c);
					back[k] = -forward[k];
				}
				difference = host_stereo_hud_level_yaw(right, back) - a;
				while (difference > (float)M_PI)
					difference -= 2.0f * (float)M_PI;
				while (difference < -(float)M_PI)
					difference += 2.0f * (float)M_PI;
				/* past 85 degrees the right's yaw is off by up to the roll's
				share; below it, exact */
				if (fabsf(pitches[p]) < 85.0f || rolls[r] == 0.0f)
					worst = fmaxf(worst, fabsf(difference));
			}
	snprintf(what, sizeof(what), "the level frame turns with the head's yaw at any pitch and roll (worst %.5f degrees)",
		worst / DEGREES);
	check(worst < 1e-3f * DEGREES, what);
}

/* the fade through black between the full view, the screen and the UI's
quad (host_stereo_cut_brightness) */
static void cut_checks(void)
{
	const float refresh = 1.0f / 90.0f;
	struct host_stereo_cut cut = HOST_STEREO_CUT_INITIAL;
	int switched, frame, repeat;
	float brightness, last;

	brightness = host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, 0, refresh, &switched);
	check(brightness == 1.0f && !switched, "the first view after none shows at once");
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, 0, refresh, &switched) == 1.0f && !switched,
		"the same view again doesn't fade");
	/* a load ends: the UI's quad to the full view, at 90 and at 45 frames a
	second (display.frame_repeat 0 and 1): the same duration, so 6 frames
	and 3 */
	for (repeat = 0; repeat <= 1; repeat++)
	{
		float frame_seconds = refresh * (float)(repeat + 1);
		int frames = repeat ? 3 : 6;
		char what[160];

		cut.shown = HOST_STEREO_VIEW_UI;
		cut.elapsed = HOST_STEREO_CUT_SECONDS;
		brightness = host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 0, frame_seconds, &switched);
		check(switched && brightness == 0.0f, "the UI's quad to the full view starts black");
		last = brightness;
		for (frame = 1; frame < frames; frame++)
		{
			brightness = host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 0, frame_seconds, &switched);
			check(!switched && brightness > last && brightness < 1.0f, "the full view comes up over the cut");
			last = brightness;
		}
		snprintf(what, sizeof(what), "the full view is at full brightness after %d frames at %d frames a second "
			"(%.0f ms)", frames, repeat ? 45 : 90, 1000.0f * HOST_STEREO_CUT_SECONDS);
		check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 0, frame_seconds, NULL) == 1.0f, what);
	}
	/* the full view to the UI's quad (a load starts), and the UI's quad to
	the screen */
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, 0, refresh, &switched) == 0.0f && switched,
		"the full view to the UI's quad starts black");
	cut.elapsed = HOST_STEREO_CUT_SECONDS;
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_SCREEN, 0, 0, refresh, &switched) == 0.0f && switched,
		"the UI's quad to the screen starts black");
	cut.elapsed = HOST_STEREO_CUT_SECONDS;
	/* a script fade covers the cut */
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 1, 0, refresh, &switched) == 1.0f && !switched,
		"a cut under a script fade doesn't go through black");
	/* no view (mono) forgets the last: the next view shows at once */
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_NONE, 0, 0, refresh, &switched) == 1.0f && !switched &&
		cut.shown == HOST_STEREO_VIEW_NONE, "a frame without a view forgets the last");
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, 0, refresh, &switched) == 1.0f && !switched,
		"a view after none shows at once");
	/* a switch in the middle of a fade starts it over */
	host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 0, refresh, NULL);
	host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 0, refresh, NULL);
	check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_UI, 0, 0, refresh, &switched) == 0.0f && switched,
		"a cut during a fade starts again from black");
	/* stereo's cut over a seat's exit glide (halo_stereo_cut_requested): the
	full view held black while requested, its fade not advancing, then up
	over the cut's time as after any cut */
	{
		int held = 1, rising = 1;

		cut.shown = HOST_STEREO_VIEW_FULL;
		cut.elapsed = HOST_STEREO_CUT_SECONDS;
		for (frame = 0; frame < 20; frame++)
			held &= host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 1, refresh, &switched) == 0.0f &&
				cut.elapsed == 0.0f && !switched;
		check(held, "a requested cut holds the full view black for as long as it's requested");
		last = host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 0, refresh, &switched);
		check(last == 0.0f && !switched, "when the request drops the view starts up from black");
		for (frame = 1; frame < 6; frame++) {
			brightness = host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 0, refresh, &switched);
			rising &= brightness > last && brightness < 1.0f;
			last = brightness;
		}
		check(rising && host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 0, 0, refresh, NULL) == 1.0f,
			"then comes up over HOST_STEREO_CUT_SECONDS (6 frames at 90 Hz)");
		/* a request under a script fade still holds black */
		check(host_stereo_cut_brightness(&cut, HOST_STEREO_VIEW_FULL, 1, 1, refresh, NULL) == 0.0f,
			"a requested cut holds black under a script fade too");
	}
}

int main(int argc, char **argv)
{
	if (argc == 7 && !strcmp(argv[1], "--quads"))
	{
		float reticle[3] = { (float)atof(argv[4]), (float)atof(argv[5]), (float)atof(argv[6]) };

		print_quads((float)atof(argv[2]), atoi(argv[3]), reticle);
		return 0;
	}
	/* 4:3 (the Mac runner), 16:9 and the widest the game lays out */
	layout_checks(640.0f);
	layout_checks(854.0f);
	layout_checks(1600.0f);
	ui_checks(4.0f / 3.0f);
	ui_checks(16.0f / 9.0f);
	ui_checks(0.9f);
	reticle_checks();
	placement_checks();
	zoom_checks();
	scoped_reticle_plane_checks();
	scoped_hidden_clamp_checks();
	distance_checks();
	depth_ease_checks();
	depth_deadband_checks();
	depth_layout_checks();
	footprint_checks();
	reticle_depth_checks();
	level_checks();
	cut_checks();
	printf("%s\n", failures ? "stereo_hud_probe: FAILED" : "stereo_hud_probe: PASS");
	return failures ? 1 : 0;
}
