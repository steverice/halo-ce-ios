/* HEAD mode's HUD and UI layout (host_stereo.m): which of the HUD's layers
go where in the room. Plain C, so a host-side test can check it
(port/ios/tests/stereo_hud_probe.c).

The game draws its HUD laid out as always: 480 lines tall, its width the
window's shape (the HUD's aspect times 480), elements at the corners and the
center. In HEAD mode the guest splits it by the function that draws each
element (halo_stereo.h, the stereo spec's "The reticle split, by draw"):
the crosshairs into the reticle's layer, each HUD group (the weapon with
its grenades, the unit, the motion tracker, the prompts, the messages) into
a target of its own with the rectangle its draws cover, and everything else
into the HUD layer itself. Every one is laid out as the whole HUD. The
presenter puts:
- the reticle's layer whole, at its natural size, centered where the game's
  crosshair is (host_stereo_hud_reticle);
- each group's rectangle of its own target on a quad in the periphery, in
  the corner where CE puts it (the stereo spec's "The HUD in the
  periphery"), turning with the head's yaw only, level with the room: the
  weapon's counters (and a driver's seat labels) top left, the unit's
  meters top right, the motion tracker bottom left, the prompts and the
  messages top left under the counters. Each group's outer corner, its
  edge nearest that corner, sits at the display.hud_* settings' angles
  (struct host_stereo_hud_placement), so a wider element grows toward the
  center;
- the HUD layer whole, head-locked at the HUD pass's projection, the
  catch-all: nav points and the multiplayer score where the game projected
  them;
- while a menu, a help panel, the console or a progress bar drew (the
  guest's hud_ui), the guest's UI layer, which holds only those, whole on
  one level, yaw-following quad over the rest (the guest leaves the widgets'
  dim out of it, and the presenter darkens the eyes by it instead);
- while a frame has no eyes (the main menu, a load), or a menu is over the
  film, the HUD layer whole on that quad instead (host_stereo_present_ui). */
#ifndef HOST_STEREO_HUD_H
#define HOST_STEREO_HUD_H

#include "halo_stereo.h"

/* how far ahead the HUD and the UI rest, in meters: display.hud_distance
(struct host_stereo_hud_placement's distance), its default and its range.
Every piece keeps its angular size at any distance */
#define HOST_STEREO_HUD_DISTANCE_DEFAULT 2.0f
#define HOST_STEREO_HUD_DISTANCE_MIN 1.0f
#define HOST_STEREO_HUD_DISTANCE_MAX 4.0f

/* The radius, in degrees from the view's center with the head level, that
the UI stays inside, horizontally and vertically: foveation's sharp region
(the HUD's groups no longer do: they sit in the periphery). The research's
one published measurement is about 40 degrees across, about 20 either side
of the view's center (douevenknow.us, May 2024), less a 2 degree margin.
Task 10 replaces it with the radius it measures from the rate map. */
#define HUD_SHARP_RADIUS_DEGREES 18.0f

/* the HUD layer's height in the game's layout lines */
#define HOST_STEREO_HUD_LINES 480.0f

/* the HUD's scale in meters per layout line at HOST_STEREO_HUD_LINE_DISTANCE: the
first headset build's head-locked quad (1.6 m wide for a 4:3 layout, 640
lines across, so 480 lines are 1.2 m, about 33 degrees, at 2 m), about 0.072
degrees a line. The reticle keeps it; the groups take it times
display.hud_scale. At another distance a line's meters scale by that
distance over this one, so its angle stays */
#define HOST_STEREO_HUD_METERS_PER_LINE (1.6f / 640.0f)
#define HOST_STEREO_HUD_LINE_DISTANCE 2.0f

/* the margin, in layout lines, a group's quad shows around its rectangle,
so linear filtering at the quad's edge never cuts a drawn texel (the target
holds nothing else) */
#define HOST_STEREO_HUD_GROUP_MARGIN_LINES 2.0f

/* the texture a quad shows (the presenter's array of them) */
enum
{
	/* the HUD layer itself: the catch-all, a menu */
	HOST_STEREO_HUD_LAYER_HUD,
	/* the crosshairs' layer */
	HOST_STEREO_HUD_LAYER_RETICLE,
	/* a HUD group's target: this plus the group (enum halo_hud_group) */
	HOST_STEREO_HUD_LAYER_GROUP,
	/* the UI layer (a menu, a help panel, the console, a progress bar) */
	HOST_STEREO_HUD_LAYER_UI = HOST_STEREO_HUD_LAYER_GROUP + HALO_HUD_GROUP_COUNT,
	HOST_STEREO_HUD_LAYER_COUNT
};

/* what a quad's frame follows */
enum
{
	/* the device: head-locked */
	HOST_STEREO_HUD_HEAD,
	/* at the device's position, turned by the head's yaw only: level with
	the room (host_stereo_hud_level) */
	HOST_STEREO_HUD_LEVEL
};

struct host_stereo_hud_quad
{
	/* its texture's rectangle: u0, v0, u1, v1 (v down) */
	float source[4];
	/* in its frame's meters, x right, y up, z back: its center, and the
	half extents along its width and height (the texture's u and v reversed:
	+y_axis is the top) */
	float center[3], x_axis[3], y_axis[3];
	int frame;
	/* which texture it shows (HOST_STEREO_HUD_LAYER_*) */
	int layer;
	/* the catch-all: the HUD layer whole, head-locked */
	int catch_all;
	/* covers what it's over whatever its texture's alpha (the zoomed
	picture, a picture) */
	int opaque;
	/* drawn at half opacity (the world reticle held, easing, its tracking
	orientation-only for over 2 s, or pinned to the view's edge) */
	int dim;
	/* the laser: a solid line in the laser's color
	(HOST_STEREO_HUD_LASER_OPACITY times laser_opacity), its texture unread */
	int laser;
	float laser_opacity;
};

/* Where the HUD's groups go (the stereo spec's "The HUD in the periphery"):
the angles, in degrees from the view's center on the level, yaw-only frame,
of each group's outer corner, and the groups' size against the natural one.
The weapon's (and a driver's seat labels') top left corner is at -across,
+up; the unit's top right at +across, +up; the motion tracker's bottom left
at -across, -tracker_down; the prompt's and the messages' top left at
-across, +messages_up, growing down. The settings display.hud_corner_across,
display.hud_corner_up, display.hud_tracker_down, display.hud_messages_up and
display.hud_scale, read at start (host_stereo.m); and distance, how far out
the HUD and the UI rest (display.hud_distance, meters) */
struct host_stereo_hud_placement
{
	float across, up, tracker_down, messages_up;
	float scale;
	float distance;
};
#define HOST_STEREO_HUD_PLACEMENT_DEFAULT { 28.0f, 20.0f, 22.0f, 12.0f, 1.0f, HOST_STEREO_HUD_DISTANCE_DEFAULT }
/* the eyes' shared view, either way across and up from its center, in
degrees: a corner at most this far out keeps its group inside both eyes'
views, since the group grows from it toward the center */
#define HOST_STEREO_HUD_SHARED_DEGREES 40.0f
#define HOST_STEREO_HUD_SCALE_MIN 0.5f
#define HOST_STEREO_HUD_SCALE_MAX 2.0f
/* clamps each angle to 0 to HOST_STEREO_HUD_SHARED_DEGREES (the edge
included), the scale to HOST_STEREO_HUD_SCALE_MIN to _MAX and the distance
to HOST_STEREO_HUD_DISTANCE_MIN to _MAX; a value that isn't a number takes
its default. Returns 1 if anything changed */
int host_stereo_hud_placement_clamp(struct host_stereo_hud_placement *placement);

/* most quads a layout makes: the laser, the reticle, the groups, the
catch-all and the UI */
#define HOST_STEREO_HUD_MAXIMUM_QUADS (HOST_STEREO_HUD_LAYER_COUNT + 1)

/* The reticle's quad: the crosshairs' layer whole (layout_width lines by
480) at its natural size (HOST_STEREO_HUD_METERS_PER_LINE), its center on
the crosshair, in the eyes' frame (x right, y up, z back), facing back along
direction, upright. Without a position it sits
distance along direction (the guest's halo_stereo_reticle), its size scaled
by distance over HOST_STEREO_HUD_LINE_DISTANCE so its angle stays:
straight ahead, head-locked, on foot; where the game's camera aims in a
head-tracked third-person seat; and returns 0 (no quad) when that's not
ahead of the eyes. With one (meters; Task 11's point where the controller's
aim hits) it's centered there. This is the one place the reticle is
placed: tracked-controller aiming (Task 11) replaces its caller's
arguments. */
int host_stereo_hud_reticle(float layout_width, const float position[3], const float direction[3], float distance,
	struct host_stereo_hud_quad *quad);

/* The reticle in the world (tracked-controller aiming): the reticle quad
at distance (meters) along direction (the eyes' frame), its half extents
scaled by distance / HOST_STEREO_HUD_LINE_DISTANCE so its angular size is
the 2 m quad's. A direction outside the eyes' shared view (more than
HOST_STEREO_HUD_SHARED_DEGREES off the eyes' forward) is pinned to that
edge in its direction, at half that angular size,
HOST_STEREO_HUD_DISTANCE_DEFAULT away (it marks a direction, not a place,
so it doesn't take the aim point's depth); straight behind pins to the
right edge. *pinned says which. Returns 0 (no quad) without a direction or
a positive distance */
int host_stereo_hud_reticle_world(float layout_width, const float direction[3], float distance,
	struct host_stereo_hud_quad *quad, int *pinned);
/* the same with pin 0: never pinned, at distance along direction however
far off forward (the zoomed picture's reticle, which the guest sets only
while it shows inside the shared view: past it the head has the aim and the
reticle is the head-locked one) */
int host_stereo_hud_reticle_world_pin(float layout_width, const float direction[3], float distance, int pin,
	struct host_stereo_hud_quad *quad, int *pinned);

/* the laser: 2 mm wide, the reticle's color at 60% opacity */
#define HOST_STEREO_HUD_LASER_HALF_WIDTH 0.001f
#define HOST_STEREO_HUD_LASER_OPACITY 0.6f
/* The laser's quad from from to to (meters, the eyes' frame), a line
HOST_STEREO_HUD_LASER_HALF_WIDTH either side, broadside to the eyes'
origin all along it (its width is square to the plane through the eyes and
the line); 0 (no quad) when the line is too short or points at the eyes */
int host_stereo_hud_laser(const float from[3], const float to[3], struct host_stereo_hud_quad *quad);

/* what tracked-controller aiming hands the layout (the guest's
gpu_stereo_present reticle_distance, reticle_world, reticle_dim, laser,
laser_from and laser_opacity) */
struct host_stereo_hud_aim
{
	/* 1 while a hand aims: the reticle sits at distance (meters) along the
	layout's reticle direction (host_stereo_hud_reticle_world) */
	int world;
	float distance;
	/* 1: the reticle at half opacity */
	int dim;
	/* 1: the laser from laser_from (meters, the eyes' frame) to the
	reticle's point, at laser_opacity (0 to 1; the guest's close-range mode
	fades it) of HOST_STEREO_HUD_LASER_OPACITY */
	int laser;
	float laser_from[3];
	float laser_opacity;
	/* 1: the frame shows the zoomed picture, and the reticle is where the
	picture shows the hand's aim (the guest sets world only while that is
	inside the part of the picture both eyes see, within the shared view:
	controller_zoom_project_sight): never pinned to the shared view's edge */
	int zoomed;
};

/* The zoom (halo_stereo.h, "Zoom fills the view"): the zoomed picture
whole on an opaque head-locked quad straight ahead on the HUD's plane,
distance away (the placement's), tangents (the guest's halo_stereo_zoom_view:
half tangents across and up) wide and tall there, so it covers both eyes'
views and its depth is the HUD's. Drawn in place of the eyes' pictures and
before the HUD's pieces and the reticle, which stay over it; 0 (no quad)
without positive tangents */
int host_stereo_hud_zoom(const float tangents[2], float distance, struct host_stereo_hud_quad *quad);

/* The HUD's pieces that step in front of what they cover (the stereo
spec's "The HUD and UI in head-tracked stereo"): the reticle and each
corner's groups, which share a plane. Each rests at the placement's
distance and comes nearer while something under it is nearer
(host_stereo_hud_depth_ease); the catch-all, the UI's quad and the zoom
stay at the resting distance */
enum
{
	HOST_STEREO_HUD_PIECE_NONE = -1,
	HOST_STEREO_HUD_PIECE_RETICLE,
	HOST_STEREO_HUD_PIECE_COUNTERS,    /* the weapon's counters and a driver's seat labels, top left */
	HOST_STEREO_HUD_PIECE_METERS,      /* the unit's meters, top right */
	HOST_STEREO_HUD_PIECE_MESSAGES,    /* the prompt and the messages, left */
	HOST_STEREO_HUD_PIECE_TRACKER,     /* the motion tracker, bottom left */
	HOST_STEREO_HUD_PIECE_COUNT
};
/* which piece a layout's quad belongs to, or HOST_STEREO_HUD_PIECE_NONE
(the catch-all, the UI, the laser) */
int host_stereo_hud_piece(const struct host_stereo_hud_quad *quad);

/* the quads for a HEAD-mode frame's HUD laid out layout_width lines across,
given each HUD group's rectangle (group_extent: x0, y0, x1, y1 in layout
lines, the guest's gpu_stereo_present hud_group_extent; an empty one, or
NULL for all, draws no quad): the reticle's quad (reticle: its direction,
as above; none when it's off the view) first, then one quad per group with
a rectangle, where placement puts it (NULL: the defaults; clamped already,
host_stereo_hud_placement_clamp), then, given the HUD pass's half tangents across
and up (the guest's halo_stereo_hud_tangents; NULL or 0: none), the
catch-all: the HUD layer whole, head-locked at that projection; and last,
with ui, the UI layer whole on the UI's quad, over them. Every quad rests at
the placement's distance, but each piece's at piece_distance[piece] given
them (NULL: all at rest), its size scaled with it so its angles stay.
Returns the count; quads holds HOST_STEREO_HUD_MAXIMUM_QUADS */
int host_stereo_hud_layout(float layout_width, int ui, const float reticle[3], const float hud_tangents[2],
	const float (*group_extent)[4], const struct host_stereo_hud_placement *placement,
	const float piece_distance[HOST_STEREO_HUD_PIECE_COUNT], struct host_stereo_hud_quad *quads);

/* host_stereo_hud_layout with tracked-controller aiming: with aim (NULL:
as host_stereo_hud_layout) and aim->world, the reticle's quad is
host_stereo_hud_reticle_world's at aim->distance along reticle (not the
reticle's piece_distance: it sits where the shot lands), dimmed with
aim->dim or when pinned (never with aim->zoomed), and with aim->laser the
laser's quad comes first, from aim->laser_from to the reticle's point (its
own, not the pinned one) */
int host_stereo_hud_layout_aimed(float layout_width, int ui, const float reticle[3],
	const struct host_stereo_hud_aim *aim, const float hud_tangents[2], const float (*group_extent)[4],
	const struct host_stereo_hud_placement *placement, const float piece_distance[HOST_STEREO_HUD_PIECE_COUNT],
	struct host_stereo_hud_quad *quads);

/* Depth-adaptive placement: each piece's distance is
clamp(share x the nearest depth under it, floor, the resting distance),
eased in diopters: pulling in at (1/floor - 1/distance) / pull_in diopters a
second at once, relaxing out relax_delay after the last frame that asked
for the current distance or nearer, at (1/floor - 1/distance) / relax. A
frame asking for a distance less than HOST_STEREO_HUD_DEPTH_DEADBAND_DIOPTERS
farther counts as asking for the current one, unless nothing is near enough
to move the piece from rest or it is already relaxing.
display.hud_depth, _share, _floor (meters), _pull_in, _relax and
_relax_delay (seconds), read at start (host_stereo.m); distance is the
placement's. host_stereo_hud_depth_settings_clamp clamps the floor below
the distance (HOST_STEREO_HUD_DEPTH_FLOOR_MIN up), the share to 0.5 to 1
and the seconds to 0 to 5; a value that isn't a number takes its default.
Returns 1 if anything changed */
struct host_stereo_hud_depth_settings
{
	int enabled;
	float share, floor, pull_in, relax, relax_delay;
	float distance;
};
#define HOST_STEREO_HUD_DEPTH_SETTINGS_DEFAULT { 1, 0.85f, 0.3f, 0.1f, 1.0f, 0.5f, HOST_STEREO_HUD_DISTANCE_DEFAULT }
#define HOST_STEREO_HUD_DEPTH_FLOOR_MIN 0.1f
/* how much farther, in diopters, what's under a piece may ask it to be
without starting a relax. b30's Warthog ride swung the targets of a piece
held over something near by 0.22 to 0.35 diopters (the frame's edge entering
and leaving the footprint), a relax and a new pull-in about every 1.1 s;
the smallest real move in that ride asked for 0.59 diopters */
#define HOST_STEREO_HUD_DEPTH_DEADBAND_DIOPTERS 0.45f
/* the floor's most, as a share of the distance */
#define HOST_STEREO_HUD_DEPTH_FLOOR_SHARE_MAX 0.9f
int host_stereo_hud_depth_settings_clamp(struct host_stereo_hud_depth_settings *settings);

/* a piece's ease: its distance in diopters (0: at rest) and the seconds
since a frame last asked for its distance or nearer */
struct host_stereo_hud_depth_state
{
	float diopters, waited;
};
#define HOST_STEREO_HUD_DEPTH_STATE_INITIAL { 0.0f, 0.0f }
/* a frame dt seconds long with nearest (meters; not a number, 0 or less, or
infinite: nothing near) under the piece: its distance (meters). A frame of
0 seconds changes nothing; with the settings off it's the distance */
float host_stereo_hud_depth_ease(struct host_stereo_hud_depth_state *state, float nearest, float dt,
	const struct host_stereo_hud_depth_settings *settings);

/* how far, in degrees, a piece's footprint reaches past its quad */
#define HOST_STEREO_HUD_DEPTH_MARGIN_DEGREES 1.0f
/* A quad's footprint in a view: its corners through clip_from_device (the
view's projection from the device's frame, column-major; for a
HOST_STEREO_HUD_LEVEL quad times device_from_level, NULL: the same frame),
their rectangle in the view's normalized picture coordinates (u0, v0, u1,
v1; u right, v down: the eye's picture fills its view, so this is where the
game's picture and depth are under it), grown by
HOST_STEREO_HUD_DEPTH_MARGIN_DEGREES through the view's half tangents
(left, right, up, down) and kept inside the view. 0 (none) when a corner is
behind the eyes or the rectangle is outside the view */
int host_stereo_hud_footprint(const struct host_stereo_hud_quad *quad, const float clip_from_device[16],
	const float device_from_level[16], const float tangents[4], float rectangle[4]);
/* the reticle's depth footprint, in layout lines each way: a square about
7 degrees across centered on the crosshair (its layer is centered there),
not the whole crosshair layer, so only what's under the crosshair brings it
nearer */
#define HOST_STEREO_HUD_RETICLE_DEPTH_LINES 96.0f
/* the quad whose footprint measures a piece's depth: for the reticle (laid
out layout_width lines across) its central HOST_STEREO_HUD_RETICLE_DEPTH_LINES
square, wherever it's drawn; any other quad as it is */
void host_stereo_hud_depth_quad(const struct host_stereo_hud_quad *quad, float layout_width,
	struct host_stereo_hud_quad *depth_quad);
/* a footprint in a screen width by height (the eye's picture's, which a
foveated eye's rate map maps to physical texels): x0, y0, x1, y1 */
void host_stereo_hud_footprint_screen(const float rectangle[4], float width, float height, float screen[4]);

/* the UI's quad for a picture of the given aspect (width over height): the
whole picture, centered ahead distance away (the placement's), as large as
fits inside the sharp region */
void host_stereo_hud_ui(float aspect, float distance, struct host_stereo_hud_quad *quad);

/* The level frame's yaw from the device's axes in the room (ARKit's: x
right, y up, z back), radians, left positive: its forward's, or past 85
degrees of pitch its right's (host_stereo_head.c does the same). The frame
sits at the device's position, turned by that yaw about the room's up */
float host_stereo_hud_level_yaw(const float right[3], const float back[3]);

/* The fade through black when HEAD mode's view changes between the full
view, the screen (the film, SCREEN gameplay) and the UI's quad (a menu over
the film, a load): the new view starts black and comes up over
HOST_STEREO_CUT_SECONDS (6 frames at 90 Hz, 3 at 45), unless a script fade
already covers the cut. The pause menu over gameplay keeps the full view and
doesn't fade. */
#define HOST_STEREO_CUT_SECONDS (6.0f / 90.0f)
enum host_stereo_view
{
	HOST_STEREO_VIEW_NONE = -1,        /* no stereo frame (mono, the space closed) */
	HOST_STEREO_VIEW_FULL,
	HOST_STEREO_VIEW_SCREEN,
	HOST_STEREO_VIEW_UI
};
struct host_stereo_cut
{
	int shown;                         /* enum host_stereo_view: what the last frame showed */
	float elapsed;                     /* seconds since the cut began; HOST_STEREO_CUT_SECONDS or more: none */
};
#define HOST_STEREO_CUT_INITIAL { HOST_STEREO_VIEW_NONE, HOST_STEREO_CUT_SECONDS }
/* this frame's brightness (0 to 1) for a frame showing shown, covered when
a script fade is over the picture, which stays up frame_seconds (the
refresh period times the repeat count plus one); *switched (may be NULL)
says whether this frame began a fade. A frame with no view forgets the
last, so a switch from it doesn't fade. While requested (stereo's cut over
a seat's exit glide, halo_stereo_cut_requested) the view is black and the
fade doesn't advance; when the request drops the view comes up over
HOST_STEREO_CUT_SECONDS, as after any cut */
float host_stereo_cut_brightness(struct host_stereo_cut *cut, int shown, int covered, int requested,
	float frame_seconds, int *switched);

#endif
