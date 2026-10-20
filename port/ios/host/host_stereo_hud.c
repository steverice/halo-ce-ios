/* HEAD mode's HUD and UI layout (host_stereo_hud.h). */
#include "host_stereo_hud.h"
#include "halo_stereo.h"
#include <math.h>
#include <string.h>

#define DEGREES ((float)M_PI / 180.0f)

/* past this pitch the level frame's yaw comes from the head's right */
#define LEVEL_YAW_PITCH_LIMIT (85.0f * DEGREES)

/* The corners: where each HUD group's quad goes (the stereo spec's "The
HUD in the periphery"), in the corner where CE puts it, moved outward. The
weapon's counters and icon (with the grenades) top left, with a vehicle
driver's seat labels, which CE anchors top left too, each where CE puts it
relative to the other, so a Scorpion driver's rider labels stay under the
cannon's counters as on the Xbox; the unit's meters top right; the prompt
and the messages, which CE draws from the top left under the counters, from
display.hud_messages_up down, the prompt over the messages as CE draws
them; the motion tracker bottom left. A corner's groups share one plane, so
they keep CE's arrangement exactly. */
enum
{
	HEIGHT_CORNER_UP,                  /* display.hud_corner_up */
	HEIGHT_MESSAGES_UP,                /* display.hud_messages_up */
	HEIGHT_TRACKER_DOWN                /* display.hud_tracker_down, below the center */
};
struct slot
{
	/* its outer corner's side, across (-1 left, 1 right) and up (1 the top,
	-1 the bottom), and which setting sets that corner's height */
	int x, y, height;
	/* the groups in it, which keep their places relative to each other */
	int groups[2], group_count;
};

static const struct slot slots[] = {
	{ -1, 1, HEIGHT_CORNER_UP, { HALO_HUD_GROUP_WEAPON, HALO_HUD_GROUP_SEATS }, 2 },
	{ 1, 1, HEIGHT_CORNER_UP, { HALO_HUD_GROUP_UNIT }, 1 },
	{ -1, 1, HEIGHT_MESSAGES_UP, { HALO_HUD_GROUP_PROMPT, HALO_HUD_GROUP_MESSAGES }, 2 },
	{ -1, -1, HEIGHT_TRACKER_DOWN, { HALO_HUD_GROUP_TRACKER }, 1 },
};
#define SLOT_COUNT (sizeof(slots) / sizeof(slots[0]))

static const struct host_stereo_hud_placement default_placement = HOST_STEREO_HUD_PLACEMENT_DEFAULT;

/* a group's drawn rectangle in the layout, clipped to it: x0, y0, x1, y1
in lines; 0 if it has none */
static int group_content(const float (*group_extent)[4], int group, float layout_width, float content[4])
{
	const float *extent;

	if (!group_extent || group < 0 || group >= HALO_HUD_GROUP_COUNT)
		return 0;
	extent = group_extent[group];
	content[0] = fmaxf(extent[0], 0.0f);
	content[1] = fmaxf(extent[1], 0.0f);
	content[2] = fminf(extent[2], layout_width);
	content[3] = fminf(extent[3], HOST_STEREO_HUD_LINES);
	return content[2] > content[0] && content[3] > content[1];
}

/* a group's rectangle in the layout with its margin, clipped to the
layout: x0, y0, x1, y1 in lines; 0 if it has none */
static int group_rectangle(const float (*group_extent)[4], int group, float layout_width, float rectangle[4])
{
	float content[4];

	if (!group_content(group_extent, group, layout_width, content))
		return 0;
	rectangle[0] = fmaxf(content[0] - HOST_STEREO_HUD_GROUP_MARGIN_LINES, 0.0f);
	rectangle[1] = fmaxf(content[1] - HOST_STEREO_HUD_GROUP_MARGIN_LINES, 0.0f);
	rectangle[2] = fminf(content[2] + HOST_STEREO_HUD_GROUP_MARGIN_LINES, layout_width);
	rectangle[3] = fminf(content[3] + HOST_STEREO_HUD_GROUP_MARGIN_LINES, HOST_STEREO_HUD_LINES);
	return 1;
}

/* a slot's drawn rectangle in the layout: the union of its groups'; 0 if
none of them drew */
static int slot_content(const struct slot *slot, const float (*group_extent)[4], float layout_width,
	float content[4])
{
	int index, found = 0;

	for (index = 0; index < slot->group_count; index++)
	{
		float group[4];

		if (!group_content(group_extent, slot->groups[index], layout_width, group))
			continue;
		if (!found)
			memcpy(content, group, sizeof(group));
		else
		{
			content[0] = fminf(content[0], group[0]);
			content[1] = fminf(content[1], group[1]);
			content[2] = fmaxf(content[2], group[2]);
			content[3] = fmaxf(content[3], group[3]);
		}
		found = 1;
	}
	return found;
}

static void source_of(const float rectangle[4], float layout_width, float source[4])
{
	source[0] = rectangle[0] / layout_width;
	source[1] = rectangle[1] / HOST_STEREO_HUD_LINES;
	source[2] = rectangle[2] / layout_width;
	source[3] = rectangle[3] / HOST_STEREO_HUD_LINES;
}

/* the plane distance out along the direction at yaw (right positive) and
pitch (up positive), facing the eyes, upright: its center on the sphere,
and its right (level) and up, unit length */
static void plane_at(float distance, float yaw, float pitch, float center[3], float right[3], float up[3])
{
	right[0] = cosf(yaw);
	right[1] = 0.0f;
	right[2] = sinf(yaw);
	/* right across forward */
	up[0] = -sinf(yaw) * sinf(pitch);
	up[1] = cosf(pitch);
	up[2] = cosf(yaw) * sinf(pitch);
	center[0] = distance * sinf(yaw) * cosf(pitch);
	center[1] = distance * sinf(pitch);
	center[2] = -distance * cosf(yaw) * cosf(pitch);
}

/* The plane of a rectangle half_width by half_height meters whose corner on
side x, y (as struct slot's) lies along the direction across, up (radians)
in the level frame: the plane facing the eyes from the sphere distance
out, its center found by stepping the center's angles by the corner's miss
until the corner is there (each step's miss is a small fraction of the
last). */
static void corner_plane(float distance, float across, float up, int x, int y, float half_width, float half_height,
	float center[3], float right[3], float upward[3])
{
	float yaw = across - (float)x * atanf(half_width / distance);
	float pitch = up - (float)y * atanf(half_height / distance);
	int step, axis;

	for (step = 0; step < 32; step++)
	{
		float corner[3], miss_across, miss_up;

		plane_at(distance, yaw, pitch, center, right, upward);
		for (axis = 0; axis < 3; axis++)
			corner[axis] = center[axis] + (float)x * half_width * right[axis] + (float)y * half_height * upward[axis];
		miss_across = across - atan2f(corner[0], -corner[2]);
		miss_up = up - atan2f(corner[1], hypotf(corner[0], corner[2]));
		if (fabsf(miss_across) < 1e-7f && fabsf(miss_up) < 1e-7f)
			return;
		yaw += miss_across;
		pitch += miss_up;
	}
	plane_at(distance, yaw, pitch, center, right, upward);
}

static int clamp_setting(float *value, float minimum, float maximum, float fallback)
{
	float clamped = *value != *value ? fallback : fmaxf(minimum, fminf(maximum, *value));

	if (clamped == *value)
		return 0;
	*value = clamped;
	return 1;
}

int host_stereo_hud_placement_clamp(struct host_stereo_hud_placement *placement)
{
	int changed = 0;

	changed |= clamp_setting(&placement->across, 0.0f, HOST_STEREO_HUD_SHARED_DEGREES, default_placement.across);
	changed |= clamp_setting(&placement->up, 0.0f, HOST_STEREO_HUD_SHARED_DEGREES, default_placement.up);
	changed |= clamp_setting(&placement->tracker_down, 0.0f, HOST_STEREO_HUD_SHARED_DEGREES,
		default_placement.tracker_down);
	changed |= clamp_setting(&placement->messages_up, 0.0f, HOST_STEREO_HUD_SHARED_DEGREES,
		default_placement.messages_up);
	changed |= clamp_setting(&placement->scale, HOST_STEREO_HUD_SCALE_MIN, HOST_STEREO_HUD_SCALE_MAX,
		default_placement.scale);
	changed |= clamp_setting(&placement->distance, HOST_STEREO_HUD_DISTANCE_MIN, HOST_STEREO_HUD_DISTANCE_MAX,
		default_placement.distance);
	return changed;
}

/* a quad showing the layer's rectangle (lines) at the given half extents
(meters), facing back along direction, upright, at distance along it or
centered at position (host_stereo_hud_reticle's rule); 0 if none */
static int facing_rectangle(float layout_width, const float rectangle[4], float half_width, float half_height,
	float distance, const float position[3], const float direction[3], struct host_stereo_hud_quad *quad)
{
	float d[3] = { direction[0], direction[1], direction[2] };
	float length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	float right[3], up[3], right_length;
	int axis;

	if (!(length > 1e-6f))
		return 0;
	for (axis = 0; axis < 3; axis++)
		d[axis] /= length;
	/* not ahead of the eyes: off the view (a seat's gun aiming where the
	head has turned away from) */
	if (d[2] > -0.1f && !position)
		return 0;
	/* facing straight up or down there's no upright: none */
	if (d[0] * d[0] + d[2] * d[2] < 1e-6f)
		return 0;
	/* facing the eyes, upright in their frame: right is the direction
	across the eyes' up, up is right across the direction */
	right[0] = -d[2];
	right[1] = 0.0f;
	right[2] = d[0];
	right_length = sqrtf(right[0] * right[0] + right[2] * right[2]);
	for (axis = 0; axis < 3; axis++)
		right[axis] /= right_length;
	up[0] = right[1] * d[2] - right[2] * d[1];
	up[1] = right[2] * d[0] - right[0] * d[2];
	up[2] = right[0] * d[1] - right[1] * d[0];
	memset(quad, 0, sizeof(*quad));
	source_of(rectangle, layout_width, quad->source);
	for (axis = 0; axis < 3; axis++)
	{
		quad->center[axis] = position ? position[axis] : d[axis] * distance;
		quad->x_axis[axis] = right[axis] * half_width;
		quad->y_axis[axis] = up[axis] * half_height;
	}
	quad->frame = HOST_STEREO_HUD_HEAD;
	return 1;
}

int host_stereo_hud_reticle(float layout_width, const float position[3], const float direction[3], float distance,
	struct host_stereo_hud_quad *quad)
{
	float whole[4] = { 0.0f, 0.0f, layout_width, HOST_STEREO_HUD_LINES };
	/* a line's meters at the distance (exactly 1 at HOST_STEREO_HUD_LINE_DISTANCE) */
	float line_scale = distance / HOST_STEREO_HUD_LINE_DISTANCE;

	if (!(layout_width > 0.0f))
		return 0;
	if (!facing_rectangle(layout_width, whole, layout_width / 2.0f * HOST_STEREO_HUD_METERS_PER_LINE * line_scale,
		HOST_STEREO_HUD_LINES / 2.0f * HOST_STEREO_HUD_METERS_PER_LINE * line_scale, distance, position, direction,
		quad))
		return 0;
	quad->layer = HOST_STEREO_HUD_LAYER_RETICLE;
	return 1;
}

int host_stereo_hud_reticle_world(float layout_width, const float direction[3], float distance,
	struct host_stereo_hud_quad *quad, int *pinned)
{
	return host_stereo_hud_reticle_world_pin(layout_width, direction, distance, 1, quad, pinned);
}

int host_stereo_hud_reticle_world_pin(float layout_width, const float direction[3], float distance, int pin,
	struct host_stereo_hud_quad *quad, int *pinned)
{
	float whole[4] = { 0.0f, 0.0f, layout_width, HOST_STEREO_HUD_LINES };
	float d[3] = { direction[0], direction[1], direction[2] };
	float length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	float edge = HOST_STEREO_HUD_SHARED_DEGREES * DEGREES, scale, position[3];
	int axis;

	*pinned = 0;
	if (!(layout_width > 0.0f) || !(distance > 0.0f) || !(length > 1e-6f))
		return 0;
	for (axis = 0; axis < 3; axis++)
		d[axis] /= length;
	if (pin && acosf(fmaxf(-1.0f, fminf(1.0f, -d[2]))) > edge)
	{
		/* outside the eyes' shared view: on its edge, in the direction it
		lies from the view's center (straight behind has none: the right),
		a marker of which way to move the hand rather than a place */
		float across = d[0], up = d[1], off_center = sqrtf(across * across + up * up);

		if (off_center < 1e-6f)
		{
			across = 1.0f;
			up = 0.0f;
			off_center = 1.0f;
		}
		d[0] = sinf(edge) * across / off_center;
		d[1] = sinf(edge) * up / off_center;
		d[2] = -cosf(edge);
		distance = HOST_STEREO_HUD_DISTANCE_DEFAULT;
		scale = 0.5f * distance / HOST_STEREO_HUD_LINE_DISTANCE;
		*pinned = 1;
	}
	else
		scale = distance / HOST_STEREO_HUD_LINE_DISTANCE;
	for (axis = 0; axis < 3; axis++)
		position[axis] = d[axis] * distance;
	if (!facing_rectangle(layout_width, whole, layout_width / 2.0f * HOST_STEREO_HUD_METERS_PER_LINE * scale,
		HOST_STEREO_HUD_LINES / 2.0f * HOST_STEREO_HUD_METERS_PER_LINE * scale, distance, position, d, quad))
		return 0;
	quad->layer = HOST_STEREO_HUD_LAYER_RETICLE;
	return 1;
}

int host_stereo_hud_laser(const float from[3], const float to[3], struct host_stereo_hud_quad *quad)
{
	float along[3], middle[3], side[3], side_length, length;
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		along[axis] = to[axis] - from[axis];
		middle[axis] = (from[axis] + to[axis]) / 2.0f;
	}
	length = sqrtf(along[0] * along[0] + along[1] * along[1] + along[2] * along[2]);
	if (!(length > 1e-4f))
		return 0;
	/* square to the plane through the eyes' origin and the line, so the
	eyes see it broadside all along it */
	side[0] = along[1] * middle[2] - along[2] * middle[1];
	side[1] = along[2] * middle[0] - along[0] * middle[2];
	side[2] = along[0] * middle[1] - along[1] * middle[0];
	side_length = sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
	if (!(side_length > 1e-6f * length * sqrtf(middle[0] * middle[0] + middle[1] * middle[1] + middle[2] * middle[2])))
		return 0;
	memset(quad, 0, sizeof(*quad));
	quad->source[2] = quad->source[3] = 1.0f;
	for (axis = 0; axis < 3; axis++)
	{
		quad->center[axis] = middle[axis];
		quad->x_axis[axis] = side[axis] / side_length * HOST_STEREO_HUD_LASER_HALF_WIDTH;
		quad->y_axis[axis] = along[axis] / 2.0f;
	}
	quad->frame = HOST_STEREO_HUD_HEAD;
	quad->layer = HOST_STEREO_HUD_LAYER_RETICLE;
	quad->laser = 1;
	quad->laser_opacity = 1.0f;
	return 1;
}

int host_stereo_hud_zoom(const float tangents[2], float distance, struct host_stereo_hud_quad *quad)
{
	if (!(tangents[0] > 0.0f) || !(tangents[1] > 0.0f))
		return 0;
	memset(quad, 0, sizeof(*quad));
	quad->source[2] = quad->source[3] = 1.0f;
	quad->center[2] = -distance;
	quad->x_axis[0] = tangents[0] * distance;
	quad->y_axis[1] = tangents[1] * distance;
	quad->frame = HOST_STEREO_HUD_HEAD;
	/* its picture covers what it's over */
	quad->opaque = 1;
	return 1;
}

void host_stereo_hud_ui(float aspect, float distance, struct host_stereo_hud_quad *quad)
{
	float half = distance * tanf(HUD_SHARP_RADIUS_DEGREES * DEGREES);
	float half_width = half, half_height = half;

	if (!(aspect > 0.0f))
		aspect = 4.0f / 3.0f;
	if (aspect >= 1.0f)
		half_height = half / aspect;
	else
		half_width = half * aspect;
	memset(quad, 0, sizeof(*quad));
	quad->source[2] = 1.0f;
	quad->source[3] = 1.0f;
	quad->center[2] = -distance;
	quad->x_axis[0] = half_width;
	quad->y_axis[1] = half_height;
	quad->frame = HOST_STEREO_HUD_LEVEL;
}

int host_stereo_hud_piece(const struct host_stereo_hud_quad *quad)
{
	size_t index;
	int member;

	if (quad->catch_all || quad->laser)
		return HOST_STEREO_HUD_PIECE_NONE;
	if (quad->layer == HOST_STEREO_HUD_LAYER_RETICLE)
		return HOST_STEREO_HUD_PIECE_RETICLE;
	/* a group's: its slot's (the pieces after the reticle are the slots,
	in order) */
	for (index = 0; index < SLOT_COUNT; index++)
		for (member = 0; member < slots[index].group_count; member++)
			if (quad->layer == HOST_STEREO_HUD_LAYER_GROUP + slots[index].groups[member])
				return HOST_STEREO_HUD_PIECE_RETICLE + 1 + (int)index;
	return HOST_STEREO_HUD_PIECE_NONE;
}

int host_stereo_hud_layout(float layout_width, int ui, const float reticle[3], const float hud_tangents[2],
	const float (*group_extent)[4], const struct host_stereo_hud_placement *placement,
	const float piece_distance[HOST_STEREO_HUD_PIECE_COUNT], struct host_stereo_hud_quad *quads)
{
	return host_stereo_hud_layout_aimed(layout_width, ui, reticle, NULL, hud_tangents, group_extent, placement,
		piece_distance, quads);
}

void host_stereo_hud_aim_depth_clamp(struct host_stereo_hud_aim *aim, float near_meters, float far_meters)
{
	if (!aim || !aim->world || aim->zoomed || !(aim->distance > 0.0f) || !(far_meters > 0.0f))
		return;
	aim->distance = fmaxf(near_meters * 1.05f, fminf(far_meters * 0.95f, aim->distance));
}

int host_stereo_hud_layout_aimed(float layout_width, int ui, const float reticle[3],
	const struct host_stereo_hud_aim *aim, const float hud_tangents[2], const float (*group_extent)[4],
	const struct host_stereo_hud_placement *placement, const float piece_distance[HOST_STEREO_HUD_PIECE_COUNT],
	struct host_stereo_hud_quad *quads)
{
	size_t index;
	int count = 0;
	float scoped_distance = 0.0f;

	if (!(layout_width > 0.0f))
		layout_width = 640.0f;
	if (!placement)
		placement = &default_placement;
	/* The scope image is on the HUD plane. A ray's distance to that plane
	depends on its angle; the same point then projects for both eyes. */
	if (aim && aim->zoomed && reticle && reticle[2] < -1e-4f && placement->distance > 0.0f)
	{
		float length = sqrtf(reticle[0] * reticle[0] + reticle[1] * reticle[1] + reticle[2] * reticle[2]);
		if (isfinite(length) && length > 0.0f)
			scoped_distance = length * placement->distance / -reticle[2];
	}
	if (aim && aim->world)
	{
		int pinned;

		/* the laser under the reticle, to the aim point itself (a pinned
		reticle's laser still shows where the gun points) */
		if (aim->laser)
		{
			float length = sqrtf(reticle[0] * reticle[0] + reticle[1] * reticle[1] + reticle[2] * reticle[2]);

			if (length > 1e-6f && aim->distance > 0.0f)
			{
				float point[3];
				int axis;

				for (axis = 0; axis < 3; axis++)
					point[axis] = reticle[axis] / length * aim->distance;
				if (host_stereo_hud_laser(aim->laser_from, point, &quads[count]))
				{
					quads[count].laser_opacity = aim->laser_opacity < 0.0f ? 0.0f : aim->laser_opacity > 1.0f ? 1.0f :
						aim->laser_opacity;
					count++;
				}
			}
		}
		if (host_stereo_hud_reticle_world_pin(layout_width, reticle,
			aim->zoomed ? (aim->distance > 0.0f ? scoped_distance : 0.0f) : aim->distance,
			!aim->zoomed, &quads[count],
			&pinned))
		{
			/* off the view's edge it's small and dim (the spec's "Out of view") */
			quads[count].dim = aim->dim || pinned;
			count++;
		}
	}
	else if (host_stereo_hud_reticle(layout_width, NULL, reticle,
		aim && aim->zoomed ? scoped_distance :
		piece_distance ? piece_distance[HOST_STEREO_HUD_PIECE_RETICLE] : placement->distance, &quads[count]))
		count++;
	for (index = 0; index < SLOT_COUNT; index++)
	{
		const struct slot *slot = &slots[index];
		float content[4], center[3], right[3], upward[3], across, up, scale;
		float distance = piece_distance ? piece_distance[HOST_STEREO_HUD_PIECE_RETICLE + 1 + index] :
			placement->distance;
		int member;

		if (!slot_content(slot, group_extent, layout_width, content))
			continue;
		/* meters a line on the slot's sphere (the line scale exactly 1 at
		HOST_STEREO_HUD_LINE_DISTANCE) */
		scale = HOST_STEREO_HUD_METERS_PER_LINE * placement->scale * (distance / HOST_STEREO_HUD_LINE_DISTANCE);
		across = (float)slot->x * placement->across;
		up = slot->height == HEIGHT_CORNER_UP ? placement->up : slot->height == HEIGHT_MESSAGES_UP ?
			placement->messages_up : -placement->tracker_down;
		/* the slot's drawn rectangle with its outer corner at the angles */
		corner_plane(distance, across * DEGREES, up * DEGREES, slot->x, slot->y, (content[2] - content[0]) / 2.0f * scale,
			(content[3] - content[1]) / 2.0f * scale, center, right, upward);
		/* each group, with its margin, where it is in the slot's rectangle,
		on the slot's plane */
		for (member = 0; member < slot->group_count && count < HOST_STEREO_HUD_MAXIMUM_QUADS; member++)
		{
			struct host_stereo_hud_quad *quad = &quads[count];
			int group = slot->groups[member], axis;
			float rectangle[4], offset_right, offset_up, half_width, half_height;

			if (!group_rectangle(group_extent, group, layout_width, rectangle))
				continue;
			offset_right = ((rectangle[0] + rectangle[2]) - (content[0] + content[2])) / 2.0f * scale;
			offset_up = -((rectangle[1] + rectangle[3]) - (content[1] + content[3])) / 2.0f * scale;
			half_width = (rectangle[2] - rectangle[0]) / 2.0f * scale;
			half_height = (rectangle[3] - rectangle[1]) / 2.0f * scale;
			memset(quad, 0, sizeof(*quad));
			source_of(rectangle, layout_width, quad->source);
			for (axis = 0; axis < 3; axis++)
			{
				quad->center[axis] = center[axis] + offset_right * right[axis] + offset_up * upward[axis];
				quad->x_axis[axis] = half_width * right[axis];
				quad->y_axis[axis] = half_height * upward[axis];
			}
			quad->frame = HOST_STEREO_HUD_LEVEL;
			quad->layer = HOST_STEREO_HUD_LAYER_GROUP + group;
			count++;
		}
	}
	/* the HUD layer, which holds what no group drew: whole, head-locked, at
	the HUD pass's own projection the distance ahead, so a nav point or the
	multiplayer score shows where the game projected it */
	if (hud_tangents && hud_tangents[0] > 0.0f && hud_tangents[1] > 0.0f && count < HOST_STEREO_HUD_MAXIMUM_QUADS)
	{
		struct host_stereo_hud_quad *quad = &quads[count];

		memset(quad, 0, sizeof(*quad));
		quad->source[2] = 1.0f;
		quad->source[3] = 1.0f;
		quad->center[2] = -placement->distance;
		quad->x_axis[0] = placement->distance * hud_tangents[0];
		quad->y_axis[1] = placement->distance * hud_tangents[1];
		quad->frame = HOST_STEREO_HUD_HEAD;
		quad->layer = HOST_STEREO_HUD_LAYER_HUD;
		quad->catch_all = 1;
		count++;
	}
	/* a menu, a help panel, the console or a progress bar: the UI layer
	whole on the UI's quad, over the HUD's pieces as the game draws it over
	the HUD */
	if (ui && count < HOST_STEREO_HUD_MAXIMUM_QUADS)
	{
		host_stereo_hud_ui(layout_width / HOST_STEREO_HUD_LINES, placement->distance, &quads[count]);
		quads[count++].layer = HOST_STEREO_HUD_LAYER_UI;
	}
	return count;
}

float host_stereo_hud_level_yaw(const float right[3], const float back[3])
{
	float forward[3] = { -back[0], -back[1], -back[2] };
	float pitch = asinf(fmaxf(-1.0f, fminf(1.0f, forward[1])));

	/* near straight up or down forward's horizontal part vanishes; the
	head's right stays level there */
	if (fabsf(pitch) >= LEVEL_YAW_PITCH_LIMIT)
		return atan2f(-right[2], right[0]);
	return atan2f(-forward[0], -forward[2]);
}

float host_stereo_cut_brightness(struct host_stereo_cut *cut, int shown, int covered, int requested,
	float frame_seconds, int *switched)
{
	float brightness;

	if (switched)
		*switched = 0;
	if (shown == HOST_STEREO_VIEW_NONE)
	{
		cut->shown = HOST_STEREO_VIEW_NONE;
		cut->elapsed = HOST_STEREO_CUT_SECONDS;
		return 1.0f;
	}
	if (cut->shown != HOST_STEREO_VIEW_NONE && shown != cut->shown && !covered)
	{
		cut->elapsed = 0.0f;
		if (switched)
			*switched = 1;
	}
	cut->shown = shown;
	/* held black: the fade starts when the request drops */
	if (requested)
	{
		cut->elapsed = 0.0f;
		return 0.0f;
	}
	if (cut->elapsed >= HOST_STEREO_CUT_SECONDS)
		return 1.0f;
	brightness = cut->elapsed / HOST_STEREO_CUT_SECONDS;
	cut->elapsed += frame_seconds;
	return brightness;
}

static const struct host_stereo_hud_depth_settings default_depth_settings = HOST_STEREO_HUD_DEPTH_SETTINGS_DEFAULT;

int host_stereo_hud_depth_settings_clamp(struct host_stereo_hud_depth_settings *settings)
{
	int changed = 0;
	float distance = settings->distance;

	if (!(distance >= HOST_STEREO_HUD_DISTANCE_MIN && distance <= HOST_STEREO_HUD_DISTANCE_MAX))
	{
		settings->distance = distance != distance ? default_depth_settings.distance :
			fmaxf(HOST_STEREO_HUD_DISTANCE_MIN, fminf(HOST_STEREO_HUD_DISTANCE_MAX, distance));
		changed = 1;
	}
	changed |= clamp_setting(&settings->share, 0.5f, 1.0f, default_depth_settings.share);
	changed |= clamp_setting(&settings->floor, HOST_STEREO_HUD_DEPTH_FLOOR_MIN,
		HOST_STEREO_HUD_DEPTH_FLOOR_SHARE_MAX * settings->distance, default_depth_settings.floor);
	changed |= clamp_setting(&settings->pull_in, 0.0f, 5.0f, default_depth_settings.pull_in);
	changed |= clamp_setting(&settings->relax, 0.0f, 5.0f, default_depth_settings.relax);
	changed |= clamp_setting(&settings->relax_delay, 0.0f, 5.0f, default_depth_settings.relax_delay);
	return changed;
}

/* a rate in diopters a second covering span diopters in seconds (at once
when seconds is 0) */
static float diopter_rate(float span, float seconds)
{
	return seconds > 0.0f ? span / seconds : INFINITY;
}

float host_stereo_hud_depth_ease(struct host_stereo_hud_depth_state *state, float nearest, float dt,
	const struct host_stereo_hud_depth_settings *settings)
{
	float rest = 1.0f / settings->distance, span = 1.0f / settings->floor - rest;
	float current = fmaxf(state->diopters, rest), target;

	if (!settings->enabled)
	{
		state->diopters = 0.0f;
		state->waited = 0.0f;
		return settings->distance;
	}
	if (!(dt > 0.0f))
		return 1.0f / current;
	/* nothing near: rest */
	target = nearest > 0.0f && nearest < INFINITY ? 1.0f / fmaxf(settings->floor, fminf(settings->distance,
		settings->share * nearest)) : rest;
	if (target >= current)
	{
		/* in front of what's under it, at once */
		state->waited = 0.0f;
		current = fminf(target, current + diopter_rate(span, settings->pull_in) * dt);
	}
	else if (target > rest && target > current - HOST_STEREO_HUD_DEPTH_DEADBAND_DIOPTERS &&
		state->waited <= settings->relax_delay)
	{
		/* a little farther than it is, with something near under it: it
		holds, so a nearest that wanders in a small range doesn't pump (a
		relax under way goes on to its target) */
		state->waited = 0.0f;
	}
	else
	{
		state->waited += dt;
		if (state->waited > settings->relax_delay)
			current = fmaxf(target, current - diopter_rate(span, settings->relax) * dt);
	}
	state->diopters = current;
	return current <= rest ? settings->distance : 1.0f / current;
}

/* a column-major 4x4 times a point (w 1) or another matrix */
static void transform_point(const float m[16], const float p[3], float out[4])
{
	int row;

	for (row = 0; row < 4; row++)
		out[row] = m[row] * p[0] + m[4 + row] * p[1] + m[8 + row] * p[2] + m[12 + row];
}

static void multiply(const float a[16], const float b[16], float out[16])
{
	int row, column;

	for (column = 0; column < 4; column++)
		for (row = 0; row < 4; row++)
			out[column * 4 + row] = a[row] * b[column * 4] + a[4 + row] * b[column * 4 + 1] +
				a[8 + row] * b[column * 4 + 2] + a[12 + row] * b[column * 4 + 3];
}

int host_stereo_hud_footprint(const struct host_stereo_hud_quad *quad, const float clip_from_device[16],
	const float device_from_level[16], const float tangents[4], float rectangle[4])
{
	float clip_from_frame[16], margin = HOST_STEREO_HUD_DEPTH_MARGIN_DEGREES * DEGREES;
	float across = tangents[0] + tangents[1], down = tangents[2] + tangents[3];
	float left, right, top, bottom;
	int corner, axis;

	if (quad->frame == HOST_STEREO_HUD_LEVEL && device_from_level)
		multiply(clip_from_device, device_from_level, clip_from_frame);
	else
		memcpy(clip_from_frame, clip_from_device, sizeof(clip_from_frame));
	rectangle[0] = rectangle[1] = INFINITY;
	rectangle[2] = rectangle[3] = -INFINITY;
	for (corner = 0; corner < 4; corner++)
	{
		float x = corner & 1 ? 1.0f : -1.0f, y = corner & 2 ? 1.0f : -1.0f, point[3], clip[4], u, v;

		for (axis = 0; axis < 3; axis++)
			point[axis] = quad->center[axis] + x * quad->x_axis[axis] + y * quad->y_axis[axis];
		transform_point(clip_from_frame, point, clip);
		if (!(clip[3] > 1e-6f))
			return 0;
		u = (clip[0] / clip[3] + 1.0f) / 2.0f;
		v = (1.0f - clip[1] / clip[3]) / 2.0f;
		rectangle[0] = fminf(rectangle[0], u);
		rectangle[1] = fminf(rectangle[1], v);
		rectangle[2] = fmaxf(rectangle[2], u);
		rectangle[3] = fmaxf(rectangle[3], v);
	}
	/* the margin, as angles through the view's tangents: u at tangent t
	right of center is (t + left) / (left + right), v at t up is
	(up - t) / (up + down) */
	if (!(across > 0.0f) || !(down > 0.0f))
		return 0;
	left = atanf(rectangle[0] * across - tangents[0]) - margin;
	right = atanf(rectangle[2] * across - tangents[0]) + margin;
	top = atanf(tangents[2] - rectangle[1] * down) + margin;
	bottom = atanf(tangents[2] - rectangle[3] * down) - margin;
	rectangle[0] = fmaxf(0.0f, (tanf(fmaxf(left, -1.5f)) + tangents[0]) / across);
	rectangle[2] = fminf(1.0f, (tanf(fminf(right, 1.5f)) + tangents[0]) / across);
	rectangle[1] = fmaxf(0.0f, (tangents[2] - tanf(fminf(top, 1.5f))) / down);
	rectangle[3] = fminf(1.0f, (tangents[2] - tanf(fmaxf(bottom, -1.5f))) / down);
	return rectangle[2] > rectangle[0] && rectangle[3] > rectangle[1];
}

void host_stereo_hud_footprint_screen(const float rectangle[4], float width, float height, float screen[4])
{
	screen[0] = rectangle[0] * width;
	screen[1] = rectangle[1] * height;
	screen[2] = rectangle[2] * width;
	screen[3] = rectangle[3] * height;
}

void host_stereo_hud_depth_quad(const struct host_stereo_hud_quad *quad, float layout_width,
	struct host_stereo_hud_quad *depth_quad)
{
	float across, up;
	int axis;

	*depth_quad = *quad;
	if (quad->layer != HOST_STEREO_HUD_LAYER_RETICLE || quad->catch_all)
		return;
	if (!(layout_width > 0.0f))
		layout_width = 640.0f;
	/* the central square's share of the layer's half extents */
	across = fminf(1.0f, HOST_STEREO_HUD_RETICLE_DEPTH_LINES / layout_width);
	up = fminf(1.0f, HOST_STEREO_HUD_RETICLE_DEPTH_LINES / HOST_STEREO_HUD_LINES);
	for (axis = 0; axis < 3; axis++)
	{
		depth_quad->x_axis[axis] = quad->x_axis[axis] * across;
		depth_quad->y_axis[axis] = quad->y_axis[axis] * up;
	}
	depth_quad->source[0] = 0.5f - across / 2.0f;
	depth_quad->source[2] = 0.5f + across / 2.0f;
	depth_quad->source[1] = 0.5f - up / 2.0f;
	depth_quad->source[3] = 0.5f + up / 2.0f;
}
