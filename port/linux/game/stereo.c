/*
STEREO.C

The frame's stereo state (port/linux/src/halo_stereo.h): which stereo mode
display.stereo asks for, and, in stereo, where the two eyes are. In HEAD
mode the host's Compositor frame supplies the eyes and the head's turn
(port/ios/host/host_stereo.m), which drives the player's look: the head turns
the view, the right stick turns the body. In SCREEN mode the look stays the
stick's, as in mono, and the game is a 3D TV on the theater screen.

Everything on the screen is a 3D TV: the game's camera rendered twice, the
eyes a short way apart along its right at the camera itself (so the game's
own near plane and culling apply), with frusta skewed so a chosen distance
lies on the screen's surface. One mapping sets the depth of all of it
(screen_mapping_eyes, the stereo spec's "One mapping for the film and the
3D TV"): how far behind the screen infinity sits, as a share of the viewer's
eye separation, and the convergence, the distance ahead of the camera that
lies on the surface. The host reports the viewer's eyes against the screen
(host_stereo.m's screen_eyes); the guest takes from them only the eye
separation, the screen's half width and the head's offset, and maps them.

Cutscenes, in any stereo mode, are the 3D film while the letterbox is in
(halo_stereo_film), and so are moments with a director's scripted camera
(camera_control, the scripted perspective). In HEAD mode so is the
player's own camera while a script holds its look: a camera the head doesn't
steer is easier to watch on a screen. But in HEAD mode (and the side-by-side
view) only a camera that isn't first person goes on the film: a cutscene
stays immersive until its first such camera, then on the film until it ends
(the stereo spec's session 4 "Cutscenes"). A first-person camera without
the look (halo_look_disabled_first_person) turns its picture by the head, as
a third-person one does. At a cutscene's end the film holds until the camera
reaches the player's eyes, eases a second into a window onto the world (the
viewer's own eyes through the screen, portal_eyes), so it shows the world
where the full view will, then its rectangle expands out to the full view
(halo_stereo_window.h). HEAD mode asks the host for SCREEN
eyes meanwhile, so the frame is SCREEN for everyone downstream: the host
draws it on the screen, and the head's turn stays out of the look and the
camera. The film has display.film_depth_share and display.film_convergence,
and doesn't lean.

A third-person camera (a vehicle seat's chase camera) stays immersive in
HEAD mode: the head turns the game's camera for the picture only, relative
to its pose when the camera began, and never the player's facing, so the
sticks drive and aim as in mono (halo_stereo_head_orient). With
display.stereo_vehicle_screen it goes on the screen as the film instead.

SCREEN mode's gameplay is the same picture from the player's camera, with
display.screen_depth_share and display.screen_convergence, and the head's
offset from the screen's axis moves the eyes, as through a small window
(the lean). The first-person weapon has its own nearly flat mapping, its
nearest point on the screen's surface (halo_stereo_first_person_eye, for
rasterizer_set_frustum_z). In SCREEN mode the film's scripted reason counts
only the director's camera, so the player's own camera under a script and a
third-person camera stay gameplay; when the frame's mapping changes, it
eases over MAPPING_EASE_SECONDS rather than popping (no cut covers it).

The screen shows the game's horizontal view across 16:9: main.c narrows
the view by 0.75 while halo_stereo_screen_framing says so, which is the
letterbox's film (held through the film's hold) and, with
display.screen_framing = "band", everything in SCREEN mode. The letterbox
brings bars in for titles (cinematics.c), following the held film.
*/

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../src/halo_stereo.h"
#include "../src/halo_stereo_window.h"
#include "../src/halo_stereo_cutscene.h"

/* port/linux/src/port_config.c */
const char *config_string(const char *name);
double config_real(const char *name);
int config_boolean(const char *name);

/* port/linux/src/sdl_platform.c (the host's port/ios/host/host_gpu.c on iOS);
   d3d8_device.c declares it the same way */
void platform_video_drawable_size(int *width, int *height);
void platform_log(const char *format, ...);
/* port/linux/src/xbox_kernel.c: the frame clock, for the mapping's ease
(with debug.fixed_timestep, presented frames at 1/30 s each) */
int platform_fixed_timestep(void);
unsigned long platform_clock_frames(void);
double halo_frame_trace_milliseconds(void);
/* port/linux/src/d3d8_device.c */
void halo_screen_commit_stereo_scale(void);
/* source/main/main.c: the player windows, more than one in split screen */
short main_get_window_count(void);
#ifdef HALO_IOS
/* port/ios/host/host_stereo.m, imported by the guest (guest_host.h): opens the
Compositor's next frame and fills the eyes and the head's turn from it */
void host_stereo_frame(struct halo_stereo_frame *frame);
#endif

/* the debug side-by-side eyes: fixed frusta and a typical eye separation
(halo_stereo.h, which the side-by-side presenter shares) */
#define SIDE_BY_SIDE_TANGENT HALO_STEREO_SIDE_BY_SIDE_TANGENT
#define SIDE_BY_SIDE_OFFSET HALO_STEREO_SIDE_BY_SIDE_OFFSET

/* one world unit in meters */
#define METERS_PER_UNIT 3.048f
/* the mappings (screen_mapping_eyes): infinity's depth behind the screen as
a share of the viewer's eye separation, and the convergence, the meters
ahead of the camera on the screen's surface. The film's and SCREEN
gameplay's are settings (display.film_depth_share, display.film_convergence,
display.screen_depth_share, display.screen_convergence), whose defaults in
port_config.c are these; the first-person weapon's are fixed, its
convergence the nearest point of its arms and weapon on the surface: Task
7g measured the animation nodes in view along the camera's forward, 0.233 m
for the assault rifle and 0.315 m for the sniper rifle (the pistol wasn't
measured), less 0.05 m for the geometry around the nodes, rounded down */
#define FILM_DEPTH_SHARE 0.25f
#define FILM_CONVERGENCE_METERS 1.75f
#define SCREEN_DEPTH_SHARE 0.3f
#define SCREEN_CONVERGENCE_METERS 1.0f
#define FIRST_PERSON_DEPTH_SHARE 0.05f
#define FIRST_PERSON_CONVERGENCE_METERS 0.15f
/* the settings' ranges: infinity never splits wider than the eyes */
#define DEPTH_SHARE_MIN 0.05f
#define DEPTH_SHARE_MAX 0.9f
#define CONVERGENCE_MIN 0.3f
#define CONVERGENCE_MAX 10.0f
/* the head's offset moves gameplay's eyes by this times the stereo's own
scale (the eyes' separation over the viewer's), each axis clamped to this
many meters */
#define SCREEN_LEAN_SCALE 1.0f
#define SCREEN_LEAN_LIMIT_METERS 0.25f
/* a change of mapping (film to gameplay or back) eases over this long */
#define MAPPING_EASE_SECONDS 0.3f
/* the eyes' separation when the views don't give one (the simulator has
one view, the side-by-side view none), in meters */
#define FILM_DEFAULT_SEPARATION 0.064f
/* the film's shape: the letterbox's inside, 640x360 */
#define FILM_ASPECT (16.0f / 9.0f)
/* the screen's half width in meters where there is no screen (side by
side): the theater's default, 60 degrees across at 4 m */
#define FILM_DEFAULT_HALF_WIDTH 2.309f
/* the default 70-degree camera's vertical half tangent on the screen,
narrowed to 16:9: 0.75 * 0.75 * 0.85 * tan(35 degrees). A placeholder for
the frame's begin only: render.c's eye loop gives the camera's own tangent
(halo_stereo_screen_frusta) before anything reads the eyes */
#define FILM_DEFAULT_VERTICAL_TANGENT 0.335f

static struct halo_stereo_frame stereo_frame;
static int stereo_layer = HALO_STEREO_LAYER_MONO;
static int stereo_mode = -1; /* read once, on the first frame */

static const char *const mode_names[] = {"off", "head", "screen", "side_by_side"};

/* HEAD mode's look: the head's yaw the look hasn't taken in yet (all of it
since the look last ran: none runs while the game is paused), the head's
pitch (the look's pitch follows it), and the stick's turn: its snaps
(input.turn = "snap"), or this frame's share of a smooth turn ("smooth") */
static float head_pending_yaw, head_pitch_now;
static int head_pitch_known;
static float snap_pending, smooth_yaw;
static int snap_armed = 1;

/* how the right stick turns the look in HEAD mode (input.turn), read once */
enum turn_mode { TURN_SNAP, TURN_SMOOTH, TURN_OFF };
static const char *const turn_names[] = {"snap", "smooth", "off"};
static int turn_mode = -1;
static float snap_degrees, smooth_degrees_per_second;
static int comfort_vignette;
/* input.snap_angle's and input.smooth_turn_speed's ranges, in degrees and
degrees per second */
#define SNAP_ANGLE_MIN 5.0f
#define SNAP_ANGLE_MAX 180.0f
#define SMOOTH_SPEED_MIN 10.0f
#define SMOOTH_SPEED_MAX 720.0f
/* the comfort vignette's strength now (0 to 1), and whether the stick was
read since the last frame began (no read, no turn: no vignette) */
static float vignette_strength;
static int vignette_stick_read;
/* the comfort vignette eases in to full over this long, and out from full
over this long, in seconds */
#define VIGNETTE_EASE_IN 0.1f
#define VIGNETTE_EASE_OUT 0.2f
/* the comfort vignette is full at this turn rate and up, in degrees per
second (input.smooth_turn_speed's default): a slower speed setting, a
fainter vignette */
#define VIGNETTE_FULL_RATE 120.0f
/* the stick past this turns one snap; it must come back inside the release
before the next */
#define SNAP_FLICK 0.7f
#define SNAP_RELEASE 0.3f
/* the game's own pitch limit (player_control_modify_desired_angles) */
#define PITCH_LIMIT (85.5f * 3.14159265f / 180.0f)
/* the smallest change of pitch the head hands the look: 0.03 degrees */
#define HEAD_PITCH_DEADBAND 0.0005f
#define TWO_PI (2.0f * 3.14159265f)
#define RADIANS_TO_DEGREES (180.0f / 3.14159265f)

/* HEAD mode's head yaw, all told (radians, wrapped): the head's turns summed
over the head-tracked frames (head_yaw_now), and the part of them the look has
taken (head_yaw_taken, halo_stereo_head_look) */
static float head_yaw_now, head_yaw_taken;
/* a seat's yaw limit (halo_stereo_seat_yaw_clamp): the head's yaw the look
handed the game on this call of player_control_update (once a frame), which
the seat's clamp consumes. And the yaw the eye cameras show that the look
hasn't given the facing (radians, held to -pi..pi): what a seat's limit
refused, and the head's turn through a third-person camera
(third_person_begin), held until the camera holds the facing
(camera_held_facing). The eye cameras add it, so the view follows the head
past the limit while the aim stays at it, and the view doesn't move when a
vehicle's camera ends; every look asks for it again once the camera holds
the facing, until the aim can take it */
static float head_request, head_seat_leftover;
/* the last report of halo_stereo_camera_posed: the previous frame's camera
held the facing with the look's yaw as of then (the look runs before the
render), so a leftover the look gives the facing now turns that camera with
it. 0 until a report. And the frames the eye cameras have carried a
leftover the look didn't ask for (the camera's end, and each look that
waited since), for the fold's log line */
static int camera_held_facing, fold_wait_frames;
/* the leftover the look folded into the facing this frame that the seat's
clamp didn't take back (in the seat it refuses it again): the look folds
only after a frame whose camera held the facing, but a fold landing on a
camera that doesn't (one changed in the fold's tick) would step the view
back by it, so the full view goes black until the camera settles
(halo_stereo_camera_posed, halo_stereo_cut_requested); the cut's seconds so
far */
static float head_fold;
static int seat_cut_requested;
static float seat_cut_elapsed;
/* a fold larger than this is covered by the cut; a smaller one's step is
under the stick's deadband: 2 degrees */
#define SEAT_CUT_FOLD (2.0f * 3.14159265f / 180.0f)
/* what render_interpolation.c said of this frame's camera
(halo_stereo_camera_head_yaw): the head yaw it holds, its blend and how it was
made; camera_noted is 0 until it says */
static float camera_head_yaw, camera_fraction;
static const char *camera_source;
static int camera_noted;
/* debug.head_yaw_log (mapping_settings): a line each frame the head turns;
the frames counted, the eye cameras' yaw less the head's on the last line
logged (radians; head_log_last_known 0 before one), and whether this frame
logged yet (halo_stereo_head_orient runs for each camera) */
static int head_yaw_log;
static unsigned long head_log_frame;
static float head_log_last_offset;
static int head_log_last_known, head_log_done;

static float z_near_world, z_far_world;

/* One mapping (the stereo spec's "One mapping for the film and the 3D
TV"): the film's, SCREEN gameplay's or the first-person weapon's */
struct screen_mapping
{
	float depth_share;         /* sigma: infinity's on-screen parallax over the viewer's separation */
	float convergence_meters;  /* C: the game-world distance that lies on the screen's surface */
	float lean;                /* the lean's scale: 0, the head's offset doesn't move the eyes; SCREEN_LEAN_SCALE */
};
/* a mapping on its way from one to another over MAPPING_EASE_SECONDS
(screen_mapping_ease): now is this frame's */
struct screen_mapping_easing
{
	struct screen_mapping from, to, now;
	float elapsed;             /* seconds since the target changed */
};

/* this frame (and the last) is the 3D film; this frame (and the last) is
SCREEN gameplay's 3D TV */
static int film_frame, film_last;
/* Task 12k's spike (debug.cutscene_immersive, halo_stereo_cutscene.h): a
cutscene's third-person film frame in HEAD mode or the side-by-side view
renders immersive instead, unless its camera's horizontal field of view is
under debug.cutscene_immersive_min_fov (a telephoto shot stays on the film).
The settings (read once, mapping_settings): on, the threshold (radians), the
outside's dim, and the side-by-side view's simulated head turn (amplitude in
radians, period in seconds; 0: none). This frame and the last are immersive;
whether this cutscene (one letterbox or scripted-camera stretch, as the
third-person latch's) has anchored its frame, and the head's yaw in the room
it anchored it at, as its first immersive frame began (a telephoto shot on
the film between keeps it); the side-by-side head's clock (seconds); the frame's axes in
the head's frame (x right, y up, z back) and its half tangents, as
halo_stereo_head_orient last found them */
static int cutscene_immersive_setting;
static float cutscene_immersive_min_fov = 0.6981317f, cutscene_outside_dim = 0.6f;
static float side_by_side_head_amplitude, side_by_side_head_period;
static int cutscene_immersive, cutscene_immersive_last, cutscene_anchored;
static float cutscene_anchor_yaw, side_by_side_head_clock;
static float cutscene_frame_forward[3], cutscene_frame_up[3], cutscene_frame_tangents[2];
static int gameplay_frame, gameplay_last;
/* what this frame's mapping takes: the viewer's eye separation and the
screen's half width (meters), the head's offset from the screen's axis
(meters, right and up, not yet clamped) and the picture's vertical half
tangent (halo_stereo_screen_frusta) */
static float screen_viewer_separation, screen_half_width, screen_head_offset[2], screen_vertical_tangent;
/* the mapping in effect, easing toward the frame's reason's; whether the
last frame was on the screen at all (film or gameplay: a frame that wasn't
takes its mapping at once); which mapping the ease was last logged going to
(0 the film, 1 gameplay; -1 none yet) */
static struct screen_mapping_easing mapping_easing;
static int mapping_on_screen_last, mapping_target_logged = -1;
/* the frame clock's last reading, in seconds (negative: none yet) */
static double mapping_clock = -1.0;
/* the settings, read once (mapping_settings): the film's and gameplay's
mappings; display.screen_framing = "band"; debug.side_by_side_screen and
debug.screen_lean (meters); debug.gpu_stats. film_mapping.depth_share is
negative until read */
static struct screen_mapping film_mapping = { -1.0f, 0.0f, 0.0f }, gameplay_mapping;
static int screen_framing_band = 1, side_by_side_screen, stereo_stats;
static float side_by_side_lean;
/* debug.side_by_side_tangents, read once (mapping_settings): eye 0's left,
right, up and down tangents in the side-by-side view, eye 1 taking left and
right swapped; all four at SIDE_BY_SIDE_TANGENT when the setting is empty.
Asymmetric bounds, as the headset's are, show what symmetric ones hide */
static float side_by_side_tangents[4] = {
	SIDE_BY_SIDE_TANGENT, SIDE_BY_SIDE_TANGENT, SIDE_BY_SIDE_TANGENT, SIDE_BY_SIDE_TANGENT
};
/* display.lod_scale, read once (mapping_settings): HEAD mode's multiplier
on the pixel size the game picks model detail by (halo_stereo_lod_scale) */
#define LOD_SCALE_MIN 0.5f
#define LOD_SCALE_MAX 4.0f
static float lod_scale_setting = 1.0f;
/* display.hud_resolution, read once (mapping_settings): the HUD's targets'
pixels per axis against the view's, in HEAD mode and the side-by-side view
(halo_stereo_hud_resolution) */
#define HUD_RESOLUTION_MIN 0.5f
#define HUD_RESOLUTION_MAX 1.0f
static float hud_resolution_setting = 1.0f;
/* display.weapon_offset_down, display.weapon_offset_back and
display.eye_height_offset, read once (mapping_settings), world units: the
first-person weapon moved down and back in the full view's eye passes
(halo_stereo_weapon_offset), and the full view's eyes raised (the eye-height
A/B). The headset's taller view shows the arms' cut edge, which the Xbox's
kept below the frame (the stereo spec's "First-person scale and the body") */
#define WEAPON_OFFSET_MIN 0.0f
#define WEAPON_OFFSET_MAX 0.2f
#define EYE_HEIGHT_OFFSET_MIN (-0.1f)
#define EYE_HEIGHT_OFFSET_MAX 0.1f
static float weapon_offset_down_setting, weapon_offset_back_setting, eye_height_offset_setting;
/* the culling camera's distance back, logged under debug.gpu_stats once
for each mode and mapping (halo_stereo_log_culling) */
static unsigned culling_logged;
/* the script fade the eyes draw and the room takes this frame
(halo_stereo_set_fade) */
static float frame_fade[4];
/* the level start's hold (halo_stereo_level_start_hold): this frame's, and
how many frames it has run */
static int level_start_hold;
static int level_start_hold_frames;
static int level_start_taken;
/* display.stereo_vehicle_screen, read once (mapping_settings): in HEAD mode
a third-person camera (a vehicle seat's) goes on the screen as the film
does, the stick still driving it as in mono. Off, it stays head-tracked
(third_person_head). In SCREEN mode it's gameplay either way */
static int vehicle_screen;
/* HEAD mode's third person, without display.stereo_vehicle_screen: this
frame's camera is the game's third-person one, which the head turns for the
picture only (halo_stereo_head_orient); the look takes none of the head's
turn. The turn is the head's since the camera began: its yaw since then
(radians, left positive) and its pitch then, so taking a seat doesn't move
the view by however far the head was turned */
static int third_person_head;
static float third_person_yaw, third_person_pitch_from;
/* why the view is on the screen: none, the letterbox, a scripted camera, a
third-person camera; logged as it changes */
static const char *const film_reasons[] = {"none", "a cutscene", "a scripted camera", "a third-person camera"};
static int film_reason, film_reason_logged;
/* the film is held through this many frames with nothing putting the view
on the screen, and leaves on the next: a10 drops its letterbox for two
ticks between two cutscenes (under a white fade), which would otherwise flip
the view to the full view and back */
#define FILM_HOLD_FRAMES 10
static int film_hold;
/* HEAD mode's cutscene (one letterbox or scripted-camera stretch, through
the film's hold) has shown a camera that isn't first person: it stays on the
film from that camera to its end (the stereo spec's session 4 "Cutscenes"),
except while released: while the camera is the player's own first person
with the look enabled (halo_player_camera_first_person; b30's ramp shot on
the Pelican bench), the frame is gameplay, reached through the film's own
end (the hold, the settle, the ease into a window, the expansion); any other
camera in the same stretch clears it, and the film comes back through the
cut */
static int cutscene_third_person, cutscene_released;
/* after a cutscene's film in HEAD mode, the film also holds until the
camera reaches the player's eyes (halo_cutscene_camera_settled), at most
FILM_SETTLE_SECONDS from the cutscene's end: the observer glides from the
cutscene camera's last pose for at most 2 s (observer_update_command), so a
camera still away by then isn't coming. The seconds since the end, and
whether the last wait gave up (the film then ends through black, without
the expansion) */
#define FILM_SETTLE_SECONDS 2.5f
static float settle_elapsed;
static int settle_gave_up;
/* HEAD mode's film becomes a window onto the world before the cutscene
window expands: once the camera has reached the eyes, the film's mapping
eases over PORTAL_SECONDS (the bars' rate) to the eyes the viewer has
against the screen, at their real separation, with frusta through the
screen's edges (portal_eyes), and the camera levels, so the screen at its
end shows exactly what the full view will show through the same rectangle
(halo_stereo_window.h). The seconds eased (counted from the first settled
frame of the hold; 0 while it hasn't begun), whether it has begun, its share
now, the eyes it eases to this frame, and the head's yaw off the screen's
axis (radians, left positive), which the look takes once on the first
full-view frame (film_handover), so the world through the screen stays put */
#define PORTAL_SECONDS 1.0f
/* the theater's default screen's distance, for the side-by-side view's
window and model: 60 degrees across at 4 m (host_theater.m's
display.theater_* defaults), in meters */
#define SIDE_BY_SIDE_SCREEN_DISTANCE 4.0f
static float portal_elapsed, portal_share, portal_yaw, film_handover;
static int portal_begun, portal_full_shown;
static struct halo_stereo_eye portal_eyes[2];
/* the cutscene window's expansion (halo_stereo_window.h): running, its
seconds so far, and the bars it began with */
static int expansion_on;
static float expansion_elapsed, expansion_bars;
/* the bars cinematics.c drew this frame, and on the last frame of the film */
static float title_bars_now, title_bars_film;
/* the side-by-side view's model of the expansion (halo_stereo_side_by_side_window):
each eye's window last frame */
static float side_by_side_last_window[2][4];
/* the zoom this frame (halo_stereo_zoom_begin): whether the frame is
zoomed in the full view, which keeps the zoom's screen effects out of the
eyes and routes the zoomed view's elements out of the HUD layer, and
whether the zoomed pass runs in place of the eyes, which waits under a
menu; the last state logged (-1: none yet); the HUD's draws routed into it
(halo_stereo_zoom_overlay) */
static int zoom_frame, zoom_pass, zoom_logged = -1, zoom_overlay_on;
/* the crosshairs routed out of the HUD layer (halo_stereo_reticle_overlay),
and whether any drew into the reticle's layer this frame */
static int reticle_overlay_on, reticle_drawn;
/* the last stereo frame's HUD layer went whole on the UI's quad */
static int ui_shown_last;

/* a setting clamped to its range, logged once if it wasn't in it */
static float clamped_setting(const char *name, float value, float minimum, float maximum, const char *unit)
{
	float clamped = fmaxf(minimum, fminf(maximum, value));

	if (clamped != value || value != value) {
		if (value != value)
			clamped = minimum;
		platform_log("stereo: %s %g is outside %g to %g %s; using %g", name, value, minimum, maximum,
			unit, clamped);
	}
	return clamped;
}

/* debug.gpu_stats: a line each frame while a cutscene, a scripted camera or
the film is up, and for CUTSCENE_LOG_SECONDS after, with what decides the
film (halo_cutscene_state) and how the film stands; the seconds still to log */
#define CUTSCENE_LOG_SECONDS 3.0f
static float cutscene_log_left;
/* the last frame's script fade intensity (halo_stereo_set_fade) */
static float cutscene_log_fade;

static void cutscene_log(float time_delta)
{
	static const char *const perspectives[] = {"first person", "third person", "scripted", "neutral"};
	static const char *const script_modes[] = {"point", "animation", "first person", "dead"};
	struct halo_cutscene_state state;

	if (!stereo_stats || stereo_mode == HALO_STEREO_OFF)
		return;
	halo_cutscene_state(&state);
	if (state.letterbox || state.director_scripted || state.look_disabled ||
		state.perspective == 2 || film_reason != 0 || expansion_on)
		cutscene_log_left = CUTSCENE_LOG_SECONDS;
	else if (cutscene_log_left > 0.0f)
		cutscene_log_left -= time_delta;
	else
		return;
	platform_log("stereo: cutscene: frame %lu: letterbox %d, director scripted %d, perspective %s, script mode %s, "
		"look %s, the player's first person %d, last frame's fade %.2f, title bars %.2f; film %s, hold %d, waited %.2f s, "
		"a third-person shot %d, released %d, expanding %d at %.2f; observer finished %d, orientation settled %d, "
		"%.3f units from the eyes",
		head_log_frame, state.letterbox, state.director_scripted,
		state.perspective >= 0 && state.perspective < 4 ? perspectives[state.perspective] : "?",
		state.script_mode >= 0 && state.script_mode < 4 ? script_modes[state.script_mode] : "none",
		state.look_disabled ? "disabled" : "enabled", state.player_first_person, cutscene_log_fade, title_bars_film,
		film_reasons[film_reason], film_hold, settle_elapsed,
		cutscene_third_person, cutscene_released, expansion_on, expansion_elapsed / HALO_STEREO_EXPANSION_SECONDS,
		state.observer_finished, state.orientation_settled, state.distance);
}

/* reads input.turn and its companions, once */
static void turn_settings(void)
{
	const char *turn;
	int mode;

	if (turn_mode >= 0)
		return;
	turn = config_string("input.turn");
	turn_mode = TURN_SNAP;
	for (mode = 0; mode < 3; mode++) {
		if (turn && strcmp(turn, turn_names[mode]) == 0)
			break;
	}
	if (mode < 3)
		turn_mode = mode;
	else
		platform_log("stereo: input.turn \"%s\" is not recognized; using snap", turn ? turn : "(none)");
	snap_degrees = clamped_setting("input.snap_angle", (float)config_real("input.snap_angle"), SNAP_ANGLE_MIN,
		SNAP_ANGLE_MAX, "degrees");
	smooth_degrees_per_second = clamped_setting("input.smooth_turn_speed",
		(float)config_real("input.smooth_turn_speed"), SMOOTH_SPEED_MIN, SMOOTH_SPEED_MAX, "degrees per second");
	comfort_vignette = config_boolean("input.comfort_vignette") != 0;
}

/* reads the mappings' settings and the screen's framing, once */
static void mapping_settings(void)
{
	const char *framing;

	if (film_mapping.depth_share >= 0.0f)
		return;
	film_mapping.depth_share = clamped_setting("display.film_depth_share",
		(float)config_real("display.film_depth_share"), DEPTH_SHARE_MIN, DEPTH_SHARE_MAX, "of the eye separation");
	film_mapping.convergence_meters = clamped_setting("display.film_convergence",
		(float)config_real("display.film_convergence"), CONVERGENCE_MIN, CONVERGENCE_MAX, "meters");
	film_mapping.lean = 0.0f;
	gameplay_mapping.depth_share = clamped_setting("display.screen_depth_share",
		(float)config_real("display.screen_depth_share"), DEPTH_SHARE_MIN, DEPTH_SHARE_MAX, "of the eye separation");
	gameplay_mapping.convergence_meters = clamped_setting("display.screen_convergence",
		(float)config_real("display.screen_convergence"), CONVERGENCE_MIN, CONVERGENCE_MAX, "meters");
	gameplay_mapping.lean = SCREEN_LEAN_SCALE;
	framing = config_string("display.screen_framing");
	screen_framing_band = 1;
	if (framing && strcmp(framing, "wide") == 0)
		screen_framing_band = 0;
	else if (!framing || strcmp(framing, "band") != 0)
		platform_log("stereo: display.screen_framing \"%s\" is not recognized; using band", framing ? framing : "(none)");
	side_by_side_screen = config_boolean("debug.side_by_side_screen") != 0;
	vehicle_screen = config_boolean("display.stereo_vehicle_screen") != 0;
	side_by_side_lean = (float)config_real("debug.screen_lean");
	if (side_by_side_lean != side_by_side_lean)
		side_by_side_lean = 0.0f;
	{
		const char *tangents = config_string("debug.side_by_side_tangents");
		float parsed[4];
		int consumed = -1;

		if (tangents && tangents[0] != '\0') {
			/* %n notes how far the four tangents reached: anything after
			them but whitespace ("1,1,0.6,1.4,9") is rejected */
			if (sscanf(tangents, "%f,%f,%f,%f%n", &parsed[0], &parsed[1], &parsed[2], &parsed[3], &consumed) == 4 &&
				consumed >= 0 && tangents[consumed + strspn(tangents + consumed, " \t\r\n")] == '\0' &&
				parsed[0] > 0.0f && parsed[1] > 0.0f && parsed[2] > 0.0f && parsed[3] > 0.0f &&
				parsed[0] <= 4.0f && parsed[1] <= 4.0f && parsed[2] <= 4.0f && parsed[3] <= 4.0f) {
				memcpy(side_by_side_tangents, parsed, sizeof(parsed));
				platform_log("stereo: debug.side_by_side_tangents left %.3f, right %.3f, up %.3f, down %.3f",
					parsed[0], parsed[1], parsed[2], parsed[3]);
			} else
				platform_log("stereo: debug.side_by_side_tangents \"%s\" is not four tangents in (0, 4] "
					"(\"left,right,up,down\"); using %.1f", tangents, SIDE_BY_SIDE_TANGENT);
		}
	}
	cutscene_immersive_setting = config_boolean("debug.cutscene_immersive") != 0;
	cutscene_immersive_min_fov = clamped_setting("debug.cutscene_immersive_min_fov",
		(float)config_real("debug.cutscene_immersive_min_fov"), 1.0f, 120.0f, "degrees") * TWO_PI / 360.0f;
	cutscene_outside_dim = clamped_setting("debug.cutscene_outside_dim", (float)config_real("debug.cutscene_outside_dim"),
		0.0f, 1.0f, "of the outside's brightness");
	{
		const char *yaw = config_string("debug.side_by_side_head_yaw");
		float amplitude, period;

		if (yaw && yaw[0] != '\0') {
			if (sscanf(yaw, "%f,%f", &amplitude, &period) == 2 && amplitude >= 0.0f && amplitude <= 90.0f &&
				period > 0.0f) {
				side_by_side_head_amplitude = amplitude * TWO_PI / 360.0f;
				side_by_side_head_period = period;
			} else
				platform_log("stereo: debug.side_by_side_head_yaw \"%s\" is not \"amplitude,period\" (0 to 90 "
					"degrees, positive seconds); no head turn", yaw);
		}
	}
	if (cutscene_immersive_setting)
		platform_log("stereo: debug.cutscene_immersive: a cutscene's third-person film immersive, telephoto under "
			"%.1f degrees across on the film, the outside dimmed by %.2f", cutscene_immersive_min_fov * 360.0f / TWO_PI,
			cutscene_outside_dim);
	stereo_stats = config_boolean("debug.gpu_stats") != 0;
	head_yaw_log = config_boolean("debug.head_yaw_log") != 0;
	lod_scale_setting = clamped_setting("display.lod_scale", (float)config_real("display.lod_scale"),
		LOD_SCALE_MIN, LOD_SCALE_MAX, "times the Xbox's pixel scale");
	hud_resolution_setting = clamped_setting("display.hud_resolution", (float)config_real("display.hud_resolution"),
		HUD_RESOLUTION_MIN, HUD_RESOLUTION_MAX, "of the view's pixels per axis");
	weapon_offset_down_setting = clamped_setting("display.weapon_offset_down",
		(float)config_real("display.weapon_offset_down"), WEAPON_OFFSET_MIN, WEAPON_OFFSET_MAX, "units");
	weapon_offset_back_setting = clamped_setting("display.weapon_offset_back",
		(float)config_real("display.weapon_offset_back"), WEAPON_OFFSET_MIN, WEAPON_OFFSET_MAX, "units");
	eye_height_offset_setting = clamped_setting("display.eye_height_offset",
		(float)config_real("display.eye_height_offset"), EYE_HEIGHT_OFFSET_MIN, EYE_HEIGHT_OFFSET_MAX, "units");
	if (weapon_offset_down_setting != 0.0f || weapon_offset_back_setting != 0.0f || eye_height_offset_setting != 0.0f)
		platform_log("stereo: display.weapon_offset_down %.3f, display.weapon_offset_back %.3f, "
			"display.eye_height_offset %.3f units", weapon_offset_down_setting, weapon_offset_back_setting,
			eye_height_offset_setting);
}

/* SCREEN gameplay's eyes rather than HEAD-like ones: SCREEN mode, or the
side-by-side view with debug.side_by_side_screen */
static int screen_mode(void)
{
	return stereo_mode == HALO_STEREO_SCREEN || (stereo_mode == HALO_STEREO_SIDE_BY_SIDE && side_by_side_screen);
}

/* the frame clock's time since its last reading, in seconds, at most a
tenth of a second (a load or a pause doesn't run a whole ease in one frame) */
static float mapping_time_delta(void)
{
	double now = platform_fixed_timestep() ? (double)platform_clock_frames() / 30.0 :
		halo_frame_trace_milliseconds() / 1000.0;
	double delta = mapping_clock < 0.0 ? 0.0 : now - mapping_clock;

	mapping_clock = now;
	return (float)fmax(0.0, fmin(0.1, delta));
}

/* the mapping a mapping's ease takes this frame: linear on the screen,
since sigma and sigma C (and sigma times the lean's scale) move linearly, so
every point's parallax moves at a steady rate, the same at any frame rate,
and stops at the target after MAPPING_EASE_SECONDS. A new target starts
over from where the mapping is now */
static void screen_mapping_ease(struct screen_mapping_easing *easing, const struct screen_mapping *target,
	float time_delta)
{
	const struct screen_mapping *from = &easing->from, *to = &easing->to;
	float t, depth_share;

	if (target->depth_share != to->depth_share || target->convergence_meters != to->convergence_meters ||
		target->lean != to->lean) {
		easing->from = easing->now;
		easing->to = *target;
		easing->elapsed = 0.0f;
	}
	easing->elapsed = fminf(MAPPING_EASE_SECONDS, easing->elapsed + fmaxf(0.0f, time_delta));
	t = easing->elapsed / MAPPING_EASE_SECONDS;
	/* (frame times summed in floats fall a hair short of the whole) */
	if (t >= 0.9999f) {
		easing->now = *to;
		return;
	}
	depth_share = from->depth_share + (to->depth_share - from->depth_share) * t;
	easing->now.depth_share = depth_share;
	easing->now.convergence_meters = (from->depth_share * from->convergence_meters +
		(to->depth_share * to->convergence_meters - from->depth_share * from->convergence_meters) * t) / depth_share;
	easing->now.lean = (from->depth_share * from->lean +
		(to->depth_share * to->lean - from->depth_share * from->lean) * t) / depth_share;
}

/* a mapping's eyes (halo_stereo_tv_eyes has the math) for a picture of
vertical half tangent V, the viewer's eye separation e and the screen's half
width w (meters), with the head's offset h (meters, right and up; NULL for
none): the lean moves each eye by clamp(h, +-SCREEN_LEAN_LIMIT_METERS) times
the mapping's lean scale times s / e, the stereo's own scale, so what lies
at C stays put on the screen, infinity moves with the head by sigma h, and
nearer things against it */
static void screen_mapping_eyes(const struct screen_mapping *mapping, float vertical_tangent,
	float viewer_separation_meters, float screen_half_width_meters, const float head_offset_meters[2],
	struct halo_stereo_eye eyes[2])
{
	float lean[2] = { 0.0f, 0.0f };

	if (head_offset_meters && mapping->lean != 0.0f && viewer_separation_meters > 0.0f &&
		screen_half_width_meters > 0.0f) {
		float separation = mapping->depth_share * viewer_separation_meters * vertical_tangent * FILM_ASPECT *
			(mapping->convergence_meters / METERS_PER_UNIT) / screen_half_width_meters;
		int axis;

		for (axis = 0; axis < 2; axis++) {
			float h = fmaxf(-SCREEN_LEAN_LIMIT_METERS, fminf(SCREEN_LEAN_LIMIT_METERS, head_offset_meters[axis]));

			lean[axis] = h * mapping->lean * separation / viewer_separation_meters;
		}
	}
	halo_stereo_tv_eyes(mapping->depth_share, mapping->convergence_meters, viewer_separation_meters,
		screen_half_width_meters, vertical_tangent, lean, eyes);
}

/* the frame's eyes at a vertical half tangent, by the mapping in effect */
static void screen_frusta(float vertical_tangent)
{
	screen_vertical_tangent = vertical_tangent;
	screen_mapping_eyes(&mapping_easing.now, vertical_tangent, screen_viewer_separation, screen_half_width,
		screen_head_offset, stereo_frame.eyes);
	/* the film easing into a window onto the world (portal_eyes): each
	eye's offset and frustum, linearly */
	if (film_frame && portal_share > 0.0f) {
		float t = portal_share;
		int eye, axis;

		for (eye = 0; eye < 2; eye++) {
			struct halo_stereo_eye *e = &stereo_frame.eyes[eye];
			const struct halo_stereo_eye *to = &portal_eyes[eye];

			for (axis = 0; axis < 3; axis++)
				e->offset[axis] += (to->offset[axis] - e->offset[axis]) * t;
			e->left += (to->left - e->left) * t;
			e->right += (to->right - e->right) * t;
			e->up += (to->up - e->up) * t;
			e->down += (to->down - e->down) * t;
		}
	}
}

/* the eyes a window onto the world takes (portal_eyes), from the viewer's
eyes against the screen (the screen's frame: x right, y up, z toward the
viewer, its center the origin, world units) and the screen's half extents:
each eye offset from the eyes' midpoint, where the camera is, with a
frustum through the screen's edges; and the head's yaw off the screen's
axis, from the line between the eyes */
static void portal_eyes_from(const float positions[2][3], float half_width, float half_height)
{
	float middle[3];
	int eye, axis;

	for (axis = 0; axis < 3; axis++)
		middle[axis] = 0.5f * (positions[0][axis] + positions[1][axis]);
	for (eye = 0; eye < 2; eye++) {
		struct halo_stereo_eye *e = &portal_eyes[eye];
		float distance = fmaxf(positions[eye][2], 1e-3f);

		for (axis = 0; axis < 3; axis++)
			e->offset[axis] = positions[eye][axis] - middle[axis];
		e->left = (half_width + positions[eye][0]) / distance;
		e->right = (half_width - positions[eye][0]) / distance;
		e->up = (half_height - positions[eye][1]) / distance;
		e->down = (half_height + positions[eye][1]) / distance;
	}
	portal_yaw = atan2f(-(positions[1][2] - positions[0][2]), positions[1][0] - positions[0][0]);
}

/* the screen's eyes for this frame, the film's or SCREEN gameplay's, from
the viewer's separation and the screen's half width (meters) and the head's
offset (meters, or NULL): level, at the camera, by the mapping, which eases
from the last frame's if that was on the screen too. The camera's own
tangent sets them at the render (halo_stereo_screen_frusta), the default
camera's until then */
static void screen_begin(int gameplay, float viewer_separation, float half_width, const float head_offset[2],
	float time_delta)
{
	static const char *const mapping_names[] = {"film", "gameplay"};
	const struct screen_mapping *target = gameplay ? &gameplay_mapping : &film_mapping;
	struct halo_stereo_eye target_eyes[2];
	float separation_mm, infinity_mm;

	screen_viewer_separation = viewer_separation > 0.001f ? viewer_separation : FILM_DEFAULT_SEPARATION;
	screen_half_width = half_width > 0.0f ? half_width : FILM_DEFAULT_HALF_WIDTH;
	screen_head_offset[0] = head_offset ? head_offset[0] : 0.0f;
	screen_head_offset[1] = head_offset ? head_offset[1] : 0.0f;
	if (!mapping_on_screen_last) {
		mapping_easing.from = mapping_easing.to = mapping_easing.now = *target;
		mapping_easing.elapsed = MAPPING_EASE_SECONDS;
	} else {
		if (mapping_target_logged >= 0 && mapping_target_logged != gameplay)
			platform_log("stereo: the screen's mapping easing from %s to %s", mapping_names[mapping_target_logged],
				mapping_names[gameplay]);
		screen_mapping_ease(&mapping_easing, target, time_delta);
	}
	mapping_target_logged = gameplay;
	film_frame = !gameplay;
	gameplay_frame = gameplay;
	screen_frusta(FILM_DEFAULT_VERTICAL_TANGENT);
	/* the logs give the target's eyes at the default camera, not a moment
	of the ease */
	screen_mapping_eyes(target, FILM_DEFAULT_VERTICAL_TANGENT, screen_viewer_separation, screen_half_width, NULL,
		target_eyes);
	separation_mm = (target_eyes[1].offset[0] - target_eyes[0].offset[0]) * METERS_PER_UNIT * 1000.0f;
	infinity_mm = target->depth_share * screen_viewer_separation * 1000.0f;
	if (!gameplay && (!film_last || film_reason != film_reason_logged))
		platform_log("stereo: %s, as a 3D film on the screen: eyes %.1f mm apart (of the viewer's %.1f mm), "
			"converged at %.2f m, infinity %.1f mm behind the screen; the screen %.2f m wide",
			film_reasons[film_reason], separation_mm, screen_viewer_separation * 1000.0f, target->convergence_meters,
			infinity_mm, 2.0f * screen_half_width);
	if (gameplay && !gameplay_last)
		platform_log("stereo: SCREEN gameplay as a 3D TV: eyes %.1f mm apart (of the viewer's %.1f mm), converged "
			"at %.2f m, infinity %.1f mm behind the screen; the weapon converged at %.2f m; framing %s",
			separation_mm, screen_viewer_separation * 1000.0f, target->convergence_meters, infinity_mm,
			FIRST_PERSON_CONVERGENCE_METERS, screen_framing_band ? "band" : "wide");
	if (!gameplay)
		film_reason_logged = film_reason;
}

/* the mode a display.stereo string names; sets *recognized to 0 for an unknown string */
static int mode_from_name(const char *name, int *recognized)
{
	int mode;

	*recognized = 1;
	for (mode = 0; mode < 4; mode++) {
		if (name && strcmp(name, mode_names[mode]) == 0)
			return mode;
	}
	*recognized = 0;
	return HALO_STEREO_OFF;
}

/* HEAD mode's third person, after the host's frame: whether this frame is
one, and the head's turn since its camera began. The look last took the
head's pose of the last frame if that frame was head-tracked first person
(head_pitch_known), so the turn starts from there, this frame's yaw already
in it; otherwise (after the film) from the head's pose now. The next
first-person frame keeps the view where it was: the eye cameras keep the
head's turn in the seat (head_seat_leftover) until the camera holds the
facing (the game glides it from the boom to the eyes, or to the seat's own
camera, over camera_change_pause and the observer's settle), and then the
look asks for it; meanwhile the look follows the head */
static void third_person_begin(void)
{
	int head_frame = stereo_frame.mode == HALO_STEREO_HEAD && stereo_frame.eye_count == 2;
	/* a first-person camera the player can't look around in (the player's
	own with the script holding its look, or a scripted one in first-person
	mode) takes the same path: the head turns its picture, never the look */
	int look_disabled = head_frame && halo_look_disabled_first_person();
	int third_person = head_frame && ((!vehicle_screen && halo_third_person_camera()) || look_disabled);

	if (third_person && !third_person_head) {
		/* (with the yaw a seat's limit refused, which the eye cameras had
		added: the picture doesn't jump) */
		third_person_yaw = head_pitch_known ? head_pending_yaw + head_seat_leftover + stereo_frame.head_yaw : 0.0f;
		third_person_pitch_from = fmaxf(-PITCH_LIMIT, fminf(PITCH_LIMIT,
			head_pitch_known ? head_pitch_now : stereo_frame.head_pitch));
		platform_log(look_disabled ? "stereo: a first-person camera with the look taken away, head-tracked: the "
			"head turns the picture only" : "stereo: a third-person camera, head-tracked: the head turns the picture, "
			"the sticks drive");
	} else if (third_person) {
		third_person_yaw = remainderf(third_person_yaw + stereo_frame.head_yaw, TWO_PI);
	}
	if (!third_person && third_person_head) {
		/* (the leftover is zero through a third-person camera: its start
		folded it in, and halo_stereo_frame_begin zeroes it each frame. Into
		the film or a closed space nothing is handed over) */
		float kept = 0.0f;

		if (stereo_frame.mode == HALO_STEREO_HEAD && stereo_frame.eye_count == 2) {
			kept = third_person_yaw;
			head_seat_leftover = kept;
			fold_wait_frames = 1;
		}
		platform_log("stereo: the head-tracked third-person or look-less camera ended; the eyes keep the head's %.1f "
			"degrees until the camera holds the facing",
			kept * 180.0f / 3.14159265f);
	}
	/* a snap armed in third person (the stick swings the boom there) doesn't
	fire when the look is the head's again: the stick comes back first */
	if (third_person)
		snap_armed = 0;
	third_person_head = third_person;
}

/* render.c's UI spans this frame (halo_stereo_set_ui_span) */
static int ui_span;

void halo_stereo_set_ui_span(int on)
{
	ui_span = on;
}

int halo_stereo_ui_span(void)
{
	return ui_span;
}

int halo_stereo_ui_dim_active(void)
{
	return ui_span && stereo_layer == HALO_STEREO_LAYER_HUD && halo_stereo_hud_split();
}

/* where the game's crosshair points in the eye cameras' frame (x right, y up,
z back): straight ahead unless halo_stereo_head_orient turned a third-person
camera */
static float reticle_direction[3] = { 0.0f, 0.0f, -1.0f };

/* the game camera's forward (aim) in the frame of the eye camera whose forward
and up are given; NULLs: straight ahead */
static void reticle_set(const float aim[3], const float forward[3], const float up[3])
{
	float right[3];

	if (!aim) {
		reticle_direction[0] = 0.0f;
		reticle_direction[1] = 0.0f;
		reticle_direction[2] = -1.0f;
		return;
	}
	/* the world's z is up and its yaw is left positive, so right is forward
	cross up */
	right[0] = forward[1] * up[2] - forward[2] * up[1];
	right[1] = forward[2] * up[0] - forward[0] * up[2];
	right[2] = forward[0] * up[1] - forward[1] * up[0];
	reticle_direction[0] = aim[0] * right[0] + aim[1] * right[1] + aim[2] * right[2];
	reticle_direction[1] = aim[0] * up[0] + aim[1] * up[1] + aim[2] * up[2];
	reticle_direction[2] = -(aim[0] * forward[0] + aim[1] * forward[1] + aim[2] * forward[2]);
}

/* the HUD pass's projection: half tangents across and up (render.c), 0
until it runs this frame */
static float hud_tangents[2];

void halo_stereo_set_hud_tangents(float across, float up)
{
	hud_tangents[0] = across;
	hud_tangents[1] = up;
}

void halo_stereo_hud_tangents(float tangents[2])
{
	tangents[0] = hud_tangents[0];
	tangents[1] = hud_tangents[1];
}

void halo_stereo_reticle(float direction[3])
{
	direction[0] = reticle_direction[0];
	direction[1] = reticle_direction[1];
	direction[2] = reticle_direction[2];
}

void halo_stereo_frame_begin(void)
{
	int film, screen, on_screen, head_look_frame, cutscene, live_reason, settle_frame = 0;
	/* split screen keeps the mono path: render.c's eye loop runs only with
	one window, so the frame has no eyes for any other hook to act on (the
	cutscene's bars, the screen's framing, the head's look) */
	int one_window = main_get_window_count() == 1;
	float time_delta;

	if (stereo_mode < 0) {
		const char *name = config_string("display.stereo");
		int recognized;

		stereo_mode = mode_from_name(name, &recognized);
		turn_settings();
		mapping_settings();
		/* the settings in effect (input.* act only in HEAD mode) */
		platform_log("stereo: display.stereo %s, input.turn %s, input.snap_angle %.1f, "
			"input.smooth_turn_speed %.1f, input.comfort_vignette %s", mode_names[stereo_mode],
			turn_names[turn_mode], snap_degrees, smooth_degrees_per_second, comfort_vignette ? "true" : "false");
		if (!recognized)
			platform_log("stereo: display.stereo \"%s\" is not recognized; using off", name ? name : "(none)");
	}
	/* the stick wasn't read since the last frame (no gamepad): no turn, no
	vignette */
	if (!vignette_stick_read)
		vignette_strength = 0.0f;
	vignette_stick_read = 0;
	time_delta = mapping_time_delta();

	memset(&stereo_frame, 0, sizeof(stereo_frame));
	stereo_frame.mode = stereo_mode;
	stereo_layer = HALO_STEREO_LAYER_MONO;
	camera_noted = 0;
	head_log_done = 0;
	head_log_frame++;
	ui_span = 0;
	zoom_frame = 0;
	zoom_pass = 0;
	zoom_overlay_on = 0;
	reticle_overlay_on = 0;
	reticle_drawn = 0;
	reticle_set(NULL, NULL, NULL);
	hud_tangents[0] = hud_tangents[1] = 0.0f;
	cutscene_log_fade = frame_fade[3];
	memset(frame_fade, 0, sizeof(frame_fade));
	/* why the view is the film. In SCREEN mode everything is on the screen
	already, so only the director's own cameras are the film: the player's
	camera with its look taken away (a10's pod) and a third-person camera
	stay gameplay, which no change of mapping interrupts */
	screen = screen_mode();
	film_reason = 0;
	if (stereo_mode != HALO_STEREO_OFF) {
		if (halo_cinematic_screen())
			film_reason = 1;
		else if (screen ? halo_scripted_director_camera() : halo_scripted_camera())
			film_reason = 2;
		else if (!screen && vehicle_screen && halo_third_person_camera())
			film_reason = 3;
	}
	cutscene = film_reason == 1 || film_reason == 2;
	/* a level's first frames, before its opening script has had its ticks: black
	in the full view (and on a flat screen: a campaign level only), unless the
	film has them already */
	level_start_taken = film_reason != 0;
	if ((stereo_mode == HALO_STEREO_HEAD || stereo_mode == HALO_STEREO_SIDE_BY_SIDE ||
		(stereo_mode == HALO_STEREO_OFF && halo_level_start_campaign())) &&
		!level_start_taken && halo_level_first_tick_pending()) {
		level_start_hold = level_start_hold_frames < HALO_STEREO_LEVEL_START_HOLD_FRAMES;
		if (level_start_hold_frames++ == HALO_STEREO_LEVEL_START_HOLD_FRAMES)
			platform_log("stereo: the level's first ticks haven't run after %d frames; showing the view",
				HALO_STEREO_LEVEL_START_HOLD_FRAMES);
	} else {
		if (level_start_hold_frames > 0 && stereo_stats)
			platform_log("stereo: level start: the view was held black for %d frame%s at the level's start; "
				"this frame's view: %s", level_start_hold_frames, level_start_hold_frames == 1 ? "" : "s",
				film_reason ? film_reasons[film_reason] : level_start_taken ? "a cutscene's" : "the player's");
		level_start_hold = 0;
		level_start_hold_frames = 0;
	}
	/* HEAD mode (and the side-by-side view): a cutscene's camera is the
	film only if it isn't first person, from the cutscene's first such camera
	to its end; a cutscene first person throughout stays immersive. After
	that camera, the player's own first-person camera with the look enabled
	releases the film (gameplay, through the film's end path), and any other
	camera puts it back. SCREEN mode has everything on the screen already */
	if (!screen && cutscene) {
		if (!cutscene_third_person && !halo_cutscene_camera_first_person()) {
			cutscene_third_person = 1;
			if (stereo_stats)
				platform_log("stereo: the cutscene's first third-person camera: on the film");
		}
		if (cutscene_third_person) {
			int released = halo_player_camera_first_person();

			if (released != cutscene_released && stereo_stats)
				platform_log("stereo: the cutscene's camera %s", released ?
					"is the player's own first person with the look: released from the film" :
					"isn't the player's own first person with the look: on the film");
			cutscene_released = released;
		}
		if (!cutscene_third_person || cutscene_released)
			film_reason = 0;
	}
	/* the cutscene's own reason this frame, before any hold (Task 12k's
	immersive cutscene reads it) */
	live_reason = film_reason;
	/* the title bars the last frame drew, if it was the film's */
	if (film_frame)
		title_bars_film = title_bars_now;
	title_bars_now = 0.0f;
	film_handover = 0.0f;
	if (film_reason != 0) {
		film_hold = FILM_HOLD_FRAMES;
		settle_elapsed = 0.0f;
		settle_gave_up = 0;
		portal_elapsed = 0.0f;
		portal_begun = portal_full_shown = 0;
	} else if (film_frame && !screen && (film_reason_logged == 1 || film_reason_logged == 2)) {
		/* a cutscene's film holds its hold, then until the camera reaches
		the eyes (otherwise the full view would begin on a camera still
		gliding from the cutscene's: the jump at a cutscene's end), then
		while it eases into a window onto the world (portal_eyes) */
		int settled = halo_cutscene_camera_settled();

		settle_frame = 1;
		settle_elapsed += time_delta;
		if (settled && portal_begun)
			portal_elapsed += time_delta;
		portal_begun |= settled;
		/* (frame times summed in floats fall a hair short of the whole) */
		if (!settled && settle_elapsed >= FILM_SETTLE_SECONDS - 1e-4f) {
			film_hold = 0;
			settle_gave_up = 1;
			portal_elapsed = 0.0f;
			portal_begun = portal_full_shown = 0;
			platform_log("stereo: the camera didn't reach the player's eyes within %.1f s of the cutscene; the film "
				"ends through black", FILM_SETTLE_SECONDS);
		} else if (film_hold > 0 || !settled || !portal_full_shown) {
			if (film_hold > 0)
				film_hold--;
			film_reason = film_reason_logged;
		}
	} else if (film_hold > 0) {
		film_hold--;
		film_reason = film_reason_logged;
	}
	if (!cutscene && film_reason != 1 && film_reason != 2)
		cutscene_third_person = cutscene_released = cutscene_anchored = 0;
	/* the film ends the frame after one shows the window whole */
	portal_share = film_reason != 0 && portal_begun ? fminf(1.0f, portal_elapsed / PORTAL_SECONDS) : 0.0f;
	if (portal_share >= 1.0f - 1e-4f)
		portal_share = 1.0f;
	portal_full_shown = portal_share >= 1.0f;
	film = film_reason != 0;
	/* Task 12k's spike: a cutscene's film frame (the third-person latch
	holds) in HEAD mode or the side-by-side view is immersive instead, if
	its camera is at least debug.cutscene_immersive_min_fov across; below it,
	the film, through the existing cut. Its reason stands as the film's would
	(the holds read it), but nothing goes on the screen. Once the cutscene's
	own reason drops, the hold stays immersive only while the camera is at
	the player's eyes (a10's two-tick gap in its letterbox); a camera gliding
	back to them, the hold's end, or a released camera (cutscene_released: the
	head drives the look there, so it never turns a framed picture) goes to
	the film, through the cut, which
	holds until the camera has settled and then expands out to the full view
	as after any cutscene's film: the observer's glide never shows immersive */
	cutscene_immersive = 0;
	if (cutscene_immersive_setting && film && !screen && (film_reason == 1 || film_reason == 2) &&
		(stereo_mode == HALO_STEREO_HEAD || stereo_mode == HALO_STEREO_SIDE_BY_SIDE) &&
		(live_reason != 0 || (cutscene_immersive_last && !settle_frame && !cutscene_released &&
		halo_cutscene_camera_settled()))) {
		float field_of_view = halo_cutscene_camera_field_of_view();

		if (field_of_view >= cutscene_immersive_min_fov) {
			cutscene_immersive = 1;
			film = 0;
			film_reason_logged = film_reason;
			/* the director's frame: the camera's 4:3 frame's width (the 0.85
			the game shrinks every field of view by, render_cameras.c), 16:9 */
			cutscene_frame_tangents[0] = 0.85f * tanf(0.5f * field_of_view);
			cutscene_frame_tangents[1] = cutscene_frame_tangents[0] / HALO_STEREO_CUTSCENE_ASPECT;
		}
		if (cutscene_immersive != cutscene_immersive_last || (stereo_stats && !cutscene_immersive &&
			film_reason != film_reason_logged))
			platform_log("stereo: the cutscene's camera, %.1f degrees across: %s", field_of_view * 360.0f / TWO_PI,
				cutscene_immersive ? "immersive, its frame anchored in the room" : "telephoto: on the film");
	} else if (cutscene_immersive_setting && cutscene_immersive_last && live_reason == 0 && !screen &&
		(film_reason_logged == 1 || film_reason_logged == 2)) {
		/* the immersive cutscene ends: on the film (through the cut) until
		the camera has settled */
		film_reason = film_reason_logged;
		film = 1;
		film_hold = 0;
		settle_elapsed = 0.0f;
		settle_gave_up = 0;
		portal_elapsed = 0.0f;
		portal_begun = portal_full_shown = 0;
		portal_share = 0.0f;
		platform_log("stereo: the immersive cutscene ends on the film, until the camera reaches the player's eyes");
	}
	/* the cutscene window expands from the film's rectangle out to the full
	view once a cutscene's film has held until the camera reached the eyes,
	at the letterbox bars' rate (halo_stereo_window.h); the film again, or
	no full view with eyes (below), stops it. A film shot cutting to an
	immersive cutscene's isn't a cutscene's end (Task 12k) */
	if (film || cutscene_immersive) {
		expansion_on = 0;
	} else if (film_frame && !screen && (film_reason_logged == 1 || film_reason_logged == 2) && !settle_gave_up &&
		(stereo_mode == HALO_STEREO_HEAD || stereo_mode == HALO_STEREO_SIDE_BY_SIDE)) {
		expansion_on = 1;
		expansion_elapsed = 0.0f;
		expansion_bars = title_bars_film;
		/* the world through the screen stays put: the full view puts the
		facing straight ahead of the head, so the look takes the head's yaw
		off the screen's axis once */
		film_handover = portal_yaw;
		memset(side_by_side_last_window, 0, sizeof(side_by_side_last_window));
		platform_log("stereo: the cutscene's film expands out to the full view over %.1f s, its bars at %.2f",
			HALO_STEREO_EXPANSION_SECONDS, expansion_bars);
	} else if (expansion_on) {
		expansion_elapsed += time_delta;
		if (expansion_elapsed >= HALO_STEREO_EXPANSION_SECONDS - 1e-4f) {
			expansion_on = 0;
			if (stereo_stats)
				platform_log("stereo: the cutscene window has expanded to the full view");
		}
	}
	cutscene_log(time_delta);
	film_last = film_frame;
	gameplay_last = gameplay_frame;
	film_frame = gameplay_frame = 0;
	/* the film's on the screen: in HEAD mode the frame asks for SCREEN eyes */
	if (film && stereo_mode == HALO_STEREO_HEAD)
		stereo_frame.mode = HALO_STEREO_SCREEN;

	if (stereo_mode == HALO_STEREO_SIDE_BY_SIDE && one_window) {
		int width = 0, height = 0;
		int eye;

		/* the side-by-side view's simulated head (debug.side_by_side_head_yaw):
		its clock runs while a cutscene is immersive, from 0 at its start */
		if (cutscene_immersive && !cutscene_anchored) {
			cutscene_anchor_yaw = side_by_side_head_clock = 0.0f;
			cutscene_anchored = 1;
		} else if (cutscene_anchored)
			side_by_side_head_clock += time_delta;

		platform_video_drawable_size(&width, &height);
		if (width <= 0 || height <= 0) {
			mapping_on_screen_last = 0;
			return; /* no drawable yet: mono this frame */
		}
		stereo_frame.eye_count = 2;
		stereo_frame.eye_width = width / 2;
		stereo_frame.eye_height = height;
		for (eye = 0; eye < 2; eye++) {
			struct halo_stereo_eye *e = &stereo_frame.eyes[eye];

			e->offset[0] = eye == 0 ? -SIDE_BY_SIDE_OFFSET : SIDE_BY_SIDE_OFFSET;
			/* eye 1 mirrors eye 0 left to right */
			e->left = side_by_side_tangents[eye == 0 ? 0 : 1];
			e->right = side_by_side_tangents[eye == 0 ? 1 : 0];
			e->up = side_by_side_tangents[2];
			e->down = side_by_side_tangents[3];
		}
		/* the default screen and typical eyes; debug.screen_lean leans */
		if (film || screen) {
			const float lean[2] = { side_by_side_lean, 0.0f };
			/* the viewer's eyes against the theater's default screen, for a
			window onto the world (portal_eyes) */
			float positions[2][3] = {
				{ -SIDE_BY_SIDE_OFFSET, 0.0f, SIDE_BY_SIDE_SCREEN_DISTANCE / METERS_PER_UNIT },
				{ SIDE_BY_SIDE_OFFSET, 0.0f, SIDE_BY_SIDE_SCREEN_DISTANCE / METERS_PER_UNIT },
			};

			portal_eyes_from(positions, FILM_DEFAULT_HALF_WIDTH / METERS_PER_UNIT,
				FILM_DEFAULT_HALF_WIDTH / FILM_ASPECT / METERS_PER_UNIT);

			screen_begin(!film, FILM_DEFAULT_SEPARATION, FILM_DEFAULT_HALF_WIDTH, lean, time_delta);
		}
	} else if (stereo_mode == HALO_STEREO_HEAD || stereo_mode == HALO_STEREO_SCREEN) {
		int mode = stereo_frame.mode;

#ifdef HALO_IOS
		host_stereo_frame(&stereo_frame);
#endif
		/* no Compositor frame (the space isn't open): mono, in the window.
		Split screen: mono too, though the frame is still opened (its picture
		goes on the UI's quad, as for any frame without eyes) */
		if (stereo_frame.eye_count != 2 || !one_window) {
			memset(&stereo_frame, 0, sizeof(stereo_frame));
			stereo_frame.mode = mode;
		} else if (film || screen) {
			/* the viewer's eyes as the host found them (SCREEN eyes: where they
			are against the screen, and frusta through its edges): their
			separation, the screen's half width from the frusta (the tangents
			add to the width over the distance) and the head's offset from
			the screen's axis, the eyes' midpoint */
			const struct halo_stereo_eye *eyes = stereo_frame.eyes;
			float dx = eyes[1].offset[0] - eyes[0].offset[0];
			float dy = eyes[1].offset[1] - eyes[0].offset[1];
			float dz = eyes[1].offset[2] - eyes[0].offset[2];
			float head_offset[2] = {
				0.5f * (eyes[0].offset[0] + eyes[1].offset[0]) * METERS_PER_UNIT,
				0.5f * (eyes[0].offset[1] + eyes[1].offset[1]) * METERS_PER_UNIT,
			};
			/* (and a window onto the world through the same screen, portal_eyes) */
			float positions[2][3] = {
				{ eyes[0].offset[0], eyes[0].offset[1], eyes[0].offset[2] },
				{ eyes[1].offset[0], eyes[1].offset[1], eyes[1].offset[2] },
			};

			portal_eyes_from(positions, eyes[0].offset[2] * (eyes[0].left + eyes[0].right) * 0.5f,
				eyes[0].offset[2] * (eyes[0].up + eyes[0].down) * 0.5f);
			screen_begin(!film, sqrtf(dx * dx + dy * dy + dz * dz) * METERS_PER_UNIT,
				eyes[0].offset[2] * (eyes[0].left + eyes[0].right) * 0.5f * METERS_PER_UNIT, head_offset, time_delta);
		}
		third_person_begin();
		/* (the immersive cutscene's head turns the picture only, as the film
		does nothing to the look) */
		head_look_frame = stereo_frame.mode == HALO_STEREO_HEAD && stereo_frame.eye_count == 2 && !third_person_head &&
			!cutscene_immersive;
		if (stereo_frame.mode == HALO_STEREO_HEAD && stereo_frame.eye_count == 2)
			head_yaw_now = remainderf(head_yaw_now + stereo_frame.head_yaw, TWO_PI);
		else
			head_log_last_known = 0;
		/* the immersive cutscene's frame is anchored where the head pointed
		as it began */
		if (cutscene_immersive && !cutscene_anchored) {
			cutscene_anchor_yaw = head_yaw_now;
			cutscene_anchored = 1;
		}
		/* HEAD mode: the look takes this frame's yaw in next frame
		(player_control runs before the render), with any the paused frames
		before it gathered (the look doesn't run while the game is paused,
		and the eye cameras turn by it meanwhile, halo_stereo_head_orient);
		the look's pitch follows the head's. SCREEN mode, the film and HEAD
		mode's third person have neither: a turn the look never took there is
		dropped, and so is the yaw the eye cameras kept (head_seat_leftover,
		which third_person_begin folded into a third-person camera's turn) */
		head_pending_yaw = head_look_frame ?
			remainderf(head_pending_yaw + stereo_frame.head_yaw + film_handover, TWO_PI) :
			0.0f;
		if (!head_look_frame) {
			head_seat_leftover = 0.0f;
			fold_wait_frames = 0;
		}
		head_pitch_known = head_look_frame;
		head_pitch_now = stereo_frame.head_pitch;
		/* a seat's exit cut (halo_stereo_camera_posed) holds until the camera
		has reached the eyes, the film's own end test, and no longer than the
		film waits for it (the observer's glide lasts 2 s at most), nor past
		a frame without the full view */
		if (seat_cut_requested) {
			int settled = halo_cutscene_camera_settled();

			seat_cut_elapsed += time_delta;
			if (settled || !head_look_frame || seat_cut_elapsed >= FILM_SETTLE_SECONDS - 1e-4f) {
				seat_cut_requested = 0;
				platform_log("stereo: the seat's exit cut ends after %.2f s: %s", seat_cut_elapsed,
					settled ? "the camera reached the eyes" : !head_look_frame ? "the full view ended" :
					"the camera didn't settle in time");
			}
		}
	} else
		third_person_head = 0;
	/* display.eye_height_offset: the full view's eyes raised along the
	camera's up (the film and SCREEN gameplay keep theirs) */
	if (stereo_frame.eye_count == 2 && !film && !screen &&
		(stereo_frame.mode == HALO_STEREO_HEAD || stereo_frame.mode == HALO_STEREO_SIDE_BY_SIDE)) {
		stereo_frame.eyes[0].offset[1] += eye_height_offset_setting;
		stereo_frame.eyes[1].offset[1] += eye_height_offset_setting;
	}
	/* without eyes (the space closed, a load) the next zoom logs again, and
	no menu holds the zoomed pass back */
	if (stereo_frame.eye_count != 2) {
		zoom_logged = -1;
		ui_shown_last = 0;
	}
	/* without eyes, nothing is immersive */
	if (stereo_frame.eye_count != 2)
		cutscene_immersive = 0;
	cutscene_immersive_last = cutscene_immersive;
	if (expansion_on && (stereo_frame.eye_count != 2 ||
		(stereo_frame.mode != HALO_STEREO_HEAD && stereo_frame.mode != HALO_STEREO_SIDE_BY_SIDE)))
		expansion_on = 0;
	on_screen = film_frame || gameplay_frame;
	mapping_on_screen_last = on_screen;
	if (!on_screen)
		mapping_target_logged = -1;
	/* the eyes' picture size from this frame on, not the next */
	if (stereo_mode != HALO_STEREO_OFF)
		halo_screen_commit_stereo_scale();
}

int halo_stereo_film(void)
{
	return film_frame && stereo_frame.eye_count == 2;
}

int halo_stereo_film_letterbox(void)
{
	return halo_stereo_film() && film_reason == 1;
}

int halo_stereo_screen_gameplay(void)
{
	return gameplay_frame && stereo_frame.eye_count == 2;
}

int halo_stereo_hud_split(void)
{
	/* (the immersive cutscene's HUD layer, its titles and bars, goes whole
	on the director's frame, as on the film's screen) */
	return stereo_frame.eye_count == 2 && (stereo_frame.mode == HALO_STEREO_HEAD ||
		stereo_frame.mode == HALO_STEREO_SIDE_BY_SIDE) && !halo_stereo_film() && !halo_stereo_screen_gameplay() &&
		!cutscene_immersive;
}

int halo_stereo_cutscene_immersive(void)
{
	return cutscene_immersive && stereo_frame.eye_count == 2;
}

int halo_stereo_cutscene_immersive_letterbox(void)
{
	return halo_stereo_cutscene_immersive() && film_reason == 1;
}

int halo_stereo_cutscene_frame(float forward[3], float up[3], float tangents[2], float *dim)
{
	if (!halo_stereo_cutscene_immersive())
		return 0;
	memcpy(forward, cutscene_frame_forward, sizeof(cutscene_frame_forward));
	memcpy(up, cutscene_frame_up, sizeof(cutscene_frame_up));
	memcpy(tangents, cutscene_frame_tangents, sizeof(cutscene_frame_tangents));
	*dim = cutscene_outside_dim;
	return 1;
}

int halo_stereo_side_by_side_cutscene(int eye, float tangents[4])
{
	if (stereo_mode != HALO_STEREO_SIDE_BY_SIDE || eye < 0 || eye > 1 || !halo_stereo_cutscene_immersive())
		return 0;
	tangents[0] = side_by_side_tangents[eye == 0 ? 0 : 1];
	tangents[1] = side_by_side_tangents[eye == 0 ? 1 : 0];
	tangents[2] = side_by_side_tangents[2];
	tangents[3] = side_by_side_tangents[3];
	return 1;
}

int halo_stereo_screen_framing(void)
{
	if (stereo_frame.eye_count != 2)
		return 0;
	return halo_stereo_film_letterbox() || (screen_mode() && screen_framing_band);
}

void halo_stereo_screen_frusta(float vertical_tangent)
{
	if ((halo_stereo_film() || halo_stereo_screen_gameplay()) && vertical_tangent > 0.0f)
		screen_frusta(vertical_tangent);
}

int halo_stereo_first_person_eye(struct halo_stereo_eye *eye)
{
	struct screen_mapping weapon;
	struct halo_stereo_eye eyes[2];

	/* every frame on the screen in SCREEN mode, the film's too: the film's
	hold after a cutscene shows the player's first-person camera, weapon and
	all, and the weapon must not lunge out of the screen under the film's
	mapping and snap back when gameplay's begins. Under a true cutscene or a
	director's camera the weapon isn't drawn anyway */
	if (!screen_mode() || !(film_frame || gameplay_frame) || stereo_frame.eye_count != 2 ||
		(stereo_layer != 0 && stereo_layer != 1))
		return 0;
	/* the weapon's mapping, leaning with the scene's (which eases) */
	weapon.depth_share = FIRST_PERSON_DEPTH_SHARE;
	weapon.convergence_meters = FIRST_PERSON_CONVERGENCE_METERS;
	weapon.lean = mapping_easing.now.lean;
	screen_mapping_eyes(&weapon, screen_vertical_tangent, screen_viewer_separation, screen_half_width,
		screen_head_offset, eyes);
	*eye = eyes[stereo_layer];
	return 1;
}

void halo_stereo_log_culling(float distance_back)
{
	unsigned key;

	if (!stereo_stats || stereo_frame.eye_count != 2)
		return;
	key = 1u << ((unsigned)stereo_frame.mode * 3u + (halo_stereo_film() ? 1u : halo_stereo_screen_gameplay() ? 2u : 0u));
	if (culling_logged & key)
		return;
	culling_logged |= key;
	platform_log("stereo: the culling camera %.4f units (%.1f mm) behind the camera, display.stereo %s, %s",
		distance_back, distance_back * METERS_PER_UNIT * 1000.0f, mode_names[stereo_frame.mode],
		halo_stereo_film() ? "the film" : halo_stereo_screen_gameplay() ? "SCREEN gameplay" : "the full view");
}

void halo_stereo_log_film_camera(const float position[3], const float forward[3], float vertical_field_of_view)
{
	static const char *const perspectives[] = {"first person", "third person", "scripted", "neutral"};
	struct halo_cutscene_state state;

	if (!stereo_stats || !(halo_stereo_film() || halo_stereo_cutscene_immersive()))
		return;
	halo_cutscene_state(&state);
	platform_log("stereo: film camera: frame %lu: vertical field of view %.3f deg, at %.4f %.4f %.4f, forward "
		"%.4f %.4f %.4f, perspective %s, first person %d, camera %s, %s %s",
		head_log_frame, vertical_field_of_view * 360.0f / TWO_PI, position[0], position[1], position[2],
		forward[0], forward[1], forward[2],
		state.perspective >= 0 && state.perspective < 4 ? perspectives[state.perspective] : "?",
		halo_cutscene_camera_first_person(), camera_noted && camera_source ? camera_source : "unnoted",
		halo_stereo_cutscene_immersive() ? "immersive" : "film", film_reasons[film_reason]);
}

int halo_stereo_head_tracked_view(void)
{
	return stereo_mode == HALO_STEREO_HEAD || stereo_mode == HALO_STEREO_SCREEN;
}

int halo_stereo_level_start_hold(void)
{
	return level_start_hold && stereo_frame.eye_count == 2;
}

int halo_level_start_hold_flat(void)
{
	return level_start_hold && stereo_frame.eye_count != 2;
}

int halo_stereo_cut_covered(void)
{
	return frame_fade[3] > 0.0f;
}

void halo_stereo_set_fade(const float rgb_intensity[4])
{
	memcpy(frame_fade, rgb_intensity, sizeof(frame_fade));
}

void halo_stereo_fade(float rgb_intensity[4])
{
	memcpy(rgb_intensity, frame_fade, sizeof(frame_fade));
}

/* The 3D film's and the 3D TV's mapping (the stereo spec's "One mapping
for the film and the 3D TV"). The camera is the center of two eyes s apart
along its right vector, level whatever the head does, each with a frustum
skewed so the picture's rectangle C ahead of the camera (C_w world units)
is the same for both: that distance lies on the screen's surface.
	s     = sigma e T C_w / w
	left  = T + x / C_w      right = T - x / C_w
	up    = V - y / C_w      down  = V + y / C_w
for the eye at x = -+s/2 (plus the lean) and y (the lean), the picture's
half tangents V and T = 16/9 V, the viewer's eye separation e and the
screen's half width w (any one unit for both). On the screen, something z
ahead has a parallax of sigma e (1 - C_w / z): infinity sigma e behind the
screen, at any field of view (s narrows with T), nearer than C in front.
The eyes sit at the camera (no offset back), so the render keeps the
camera's near and far planes. */
void halo_stereo_tv_eyes(float depth_share, float convergence_meters, float viewer_separation, float half_width,
	float vertical_tangent, const float lean[2], struct halo_stereo_eye eyes[2])
{
	float horizontal_tangent = vertical_tangent * FILM_ASPECT;
	float convergence = convergence_meters / METERS_PER_UNIT;
	float separation = half_width > 0.0f ?
		depth_share * viewer_separation * horizontal_tangent * convergence / half_width : 0.0f;
	int eye;

	for (eye = 0; eye < 2; eye++) {
		struct halo_stereo_eye *e = &eyes[eye];
		float x = (eye == 0 ? -0.5f : 0.5f) * separation + (lean ? lean[0] : 0.0f);
		float y = lean ? lean[1] : 0.0f;

		e->offset[0] = x;
		e->offset[1] = y;
		e->offset[2] = 0.0f;
		e->left = horizontal_tangent + x / convergence;
		e->right = horizontal_tangent - x / convergence;
		e->up = vertical_tangent - y / convergence;
		e->down = vertical_tangent + y / convergence;
	}
}

const struct halo_stereo_frame *halo_stereo_frame(void)
{
	return &stereo_frame;
}

void halo_stereo_layer(int layer)
{
	stereo_layer = layer;
}

int halo_stereo_current_layer(void)
{
	return stereo_layer;
}

int halo_stereo_repeat_pass(void)
{
	return stereo_layer == 1;
}

/* the zoom: HEAD mode's full view and the side-by-side view (the Mac's
stand-in for it), never the screen (SCREEN gameplay, the film), where the
game's own zoom shows as in mono */
int halo_stereo_zoom_begin(int zoomed)
{
	int mode = stereo_frame.mode;

	zoom_frame = zoomed && stereo_frame.eye_count == 2 && !halo_stereo_film() && !halo_stereo_screen_gameplay() &&
		(mode == HALO_STEREO_HEAD || mode == HALO_STEREO_SIDE_BY_SIDE);
	/* the pass alone waits under the last frame's menu, where the presenter
	shows the eyes under the UI's quad. The eyes still leave the zoom out,
	so they don't blink a masked view around a menu */
	zoom_pass = zoom_frame && !ui_shown_last;
	/* the first time, and each change under debug.gpu_stats */
	if (zoom_frame != zoom_logged && (zoom_logged < 0 ? zoom_frame : stereo_stats)) {
		float view[2];

		halo_stereo_zoom_view(view);
		if (zoom_frame)
			platform_log("stereo: zoomed: one mono zoomed pass fills the view, the eyes' passes skipped: the view's "
				"half tangents %.3f by %.3f, each over the zoom's magnification, shown %.1f m ahead", view[0], view[1],
				HALO_STEREO_ZOOM_DISTANCE_METERS);
		else
			platform_log("stereo: unzoomed: the eyes' passes");
	}
	if (zoom_frame || zoom_logged >= 0)
		zoom_logged = zoom_frame;
	return zoom_pass;
}

void halo_stereo_set_ui_shown(int shown)
{
	ui_shown_last = shown != 0;
}

void halo_stereo_zoom_view(float tangents[2])
{
	float distance = HALO_STEREO_ZOOM_DISTANCE_METERS;
	int eye;

	tangents[0] = tangents[1] = 0.0f;
	for (eye = 0; eye < 2; eye++) {
		const struct halo_stereo_eye *e = &stereo_frame.eyes[eye];
		/* where the eye's frustum meets the HUD's plane, from the head */
		float x = e->offset[0] * METERS_PER_UNIT, y = e->offset[1] * METERS_PER_UNIT;
		float across = fmaxf(e->right * distance + x, e->left * distance - x) / distance;
		float rise = fmaxf(e->up * distance + y, e->down * distance - y) / distance;

		tangents[0] = fmaxf(tangents[0], across);
		tangents[1] = fmaxf(tangents[1], rise);
	}
}

void halo_stereo_zoom_density(float scale[2])
{
	float view[2], across = 0.0f, rise = 0.0f;
	int eye;

	halo_stereo_zoom_view(view);
	/* the narrowest eye has the most pixels per tangent */
	for (eye = 0; eye < 2; eye++) {
		const struct halo_stereo_eye *e = &stereo_frame.eyes[eye];

		if (eye == 0 || e->left + e->right < across)
			across = e->left + e->right;
		if (eye == 0 || e->up + e->down < rise)
			rise = e->up + e->down;
	}
	scale[0] = across > 0.0f ? 2.0f * view[0] / across : 1.0f;
	scale[1] = rise > 0.0f ? 2.0f * view[1] / rise : 1.0f;
}

float halo_stereo_zoom_fit(float layout_aspect)
{
	float view[2];

	halo_stereo_zoom_view(view);
	if (!(layout_aspect > 0.0f) || !(view[0] > 0.0f) || !(view[1] > 0.0f))
		return 1.0f;
	return layout_aspect * view[1] / view[0];
}

int halo_stereo_zoom(void)
{
	return zoom_pass;
}

int halo_stereo_eye_unzoomed(void)
{
	return zoom_frame && (stereo_layer == 0 || stereo_layer == 1);
}

void halo_stereo_zoom_overlay(int on)
{
	if (on) {
		if (zoom_frame && stereo_layer == HALO_STEREO_LAYER_HUD) {
			stereo_layer = HALO_STEREO_LAYER_ZOOM;
			zoom_overlay_on = 1;
		}
	} else if (zoom_overlay_on) {
		stereo_layer = HALO_STEREO_LAYER_HUD;
		zoom_overlay_on = 0;
	}
}

int halo_stereo_zoom_overlay_on(void)
{
	return zoom_overlay_on;
}

void halo_stereo_reticle_overlay(int on)
{
	if (on) {
		if (stereo_layer == HALO_STEREO_LAYER_HUD && halo_stereo_hud_split()) {
			stereo_layer = HALO_STEREO_LAYER_RETICLE;
			reticle_overlay_on = 1;
		}
	} else if (reticle_overlay_on) {
		stereo_layer = HALO_STEREO_LAYER_HUD;
		reticle_overlay_on = 0;
	}
}

void halo_stereo_reticle_drew(void)
{
	reticle_drawn = 1;
}

int halo_stereo_reticle_drawn(void)
{
	return reticle_drawn;
}

/* whether the head drives the look: HEAD mode with the Compositor's eyes,
not in third person, where the stick has the look as in mono (the last
frame's state, since the look runs before the frame begins) */
static int head_tracking(short gamepad_index)
{
	return gamepad_index == 0 && stereo_frame.mode == HALO_STEREO_HEAD && stereo_frame.eye_count == 2 &&
		!third_person_head;
}

float halo_stereo_vignette_ease(float strength, float turn_fraction, float time_delta)
{
	float target = fmaxf(0.0f, fminf(1.0f, turn_fraction));

	if (time_delta <= 0.0f)
		return strength;
	if (strength < target)
		return fminf(target, strength + time_delta / VIGNETTE_EASE_IN);
	return fmaxf(target, strength - time_delta / VIGNETTE_EASE_OUT);
}

void halo_stereo_stick_look(short gamepad_index, float yaw_response, float time_delta, float *yaw, float *pitch)
{
	float magnitude, turn_fraction = 0.0f;

	if (gamepad_index != 0)
		return;
	vignette_stick_read = 1;
	smooth_yaw = 0.0f;
	/* not head-tracked, or HEAD mode's third person: the game's own turn and
	pitch, which swing a vehicle's chase camera and aim its guns; no snaps,
	no vignette */
	if (!head_tracking(gamepad_index)) {
		vignette_strength = 0.0f;
		return;
	}
	*pitch = 0.0f;
	turn_settings();
	if (turn_mode == TURN_SMOOTH) {
		/* the game's response curve, at the speed asked for, steady and for
		this frame's time: the game's own turn would speed up to three times
		as fast while the stick is held over */
		smooth_yaw = yaw_response * smooth_degrees_per_second * (3.14159265f / 180.0f) * time_delta;
		turn_fraction = fabsf(yaw_response) * smooth_degrees_per_second / VIGNETTE_FULL_RATE;
	} else if (turn_mode == TURN_SNAP) {
		/* a snap turns the way the stick would turn the look smoothly */
		float snap_radians = snap_degrees * 3.14159265f / 180.0f;

		magnitude = fabsf(*yaw);
		if (snap_armed && magnitude > SNAP_FLICK) {
			snap_pending += *yaw > 0.0f ? snap_radians : -snap_radians;
			snap_armed = 0;
		} else if (magnitude < SNAP_RELEASE) {
			snap_armed = 1;
		}
	}
	/* the turn, if any, comes in with the head's (halo_stereo_head_look) */
	*yaw = 0.0f;
	vignette_strength = comfort_vignette && turn_mode == TURN_SMOOTH ?
		halo_stereo_vignette_ease(vignette_strength, turn_fraction, time_delta) : 0.0f;
}

/* the eye pass's camera position (halo_stereo.h): render.c's eye loop sets
it around each eye's render_window */
static const union real_point3d *eye_position;

void halo_stereo_set_eye_position(const union real_point3d *position)
{
	eye_position = position;
}

const union real_point3d *halo_stereo_eye_position(void)
{
	return eye_position;
}

float halo_stereo_lod_scale(void)
{
	return stereo_mode == HALO_STEREO_HEAD || stereo_mode == HALO_STEREO_SIDE_BY_SIDE ? lod_scale_setting : 1.0f;
}

int halo_stereo_weapon_offset(float *down, float *back)
{
	int layer = halo_stereo_current_layer();

	*down = *back = 0.0f;
	if ((weapon_offset_down_setting == 0.0f && weapon_offset_back_setting == 0.0f) || !halo_stereo_hud_split() ||
		(layer != 0 && layer != 1))
		return 0;
	*down = weapon_offset_down_setting;
	*back = weapon_offset_back_setting;
	return 1;
}

float halo_stereo_hud_resolution(void)
{
	return stereo_mode == HALO_STEREO_HEAD || stereo_mode == HALO_STEREO_SIDE_BY_SIDE ? hud_resolution_setting : 1.0f;
}

float halo_stereo_vignette(void)
{
	return stereo_frame.mode == HALO_STEREO_HEAD && stereo_frame.eye_count == 2 ? vignette_strength : 0.0f;
}

int halo_stereo_head_drives_look(short gamepad_index)
{
	return head_tracking(gamepad_index);
}

/* the head's pitch inside the game's limit */
static float head_pitch_limited(void)
{
	return fmaxf(-PITCH_LIMIT, fminf(PITCH_LIMIT, head_pitch_now));
}

int halo_stereo_head_look(short gamepad_index, float current_pitch, float *yaw, float *pitch)
{
	*yaw = 0.0f;
	*pitch = 0.0f;
	if (!head_tracking(gamepad_index))
	{
		/* a snap armed in the frame the space closed doesn't wait to fire
		when it opens again */
		if (gamepad_index == 0)
			snap_pending = 0.0f;
		return 0;
	}
	/* the head's yaw since the look last ran, and all the eye cameras kept
	(head_seat_leftover): in a seat the clamp refuses what the limit still
	holds back (halo_stereo_seat_yaw_clamp); elsewhere the game takes it all.
	The leftover only once the last frame's camera held the facing
	(camera_held_facing), so the camera that shows the fold turns with it: a
	glide to the eyes or a seat's camera, a chase camera or a blend with one
	of them, never turned by the facing, would step the view back by it. And
	not while the director holds the facing (the debug camera's controls,
	director_update_controls): the game drops the frame's turn there, so the
	leftover stays the eyes' until a look the game takes */
	if (!camera_held_facing || halo_director_inhibited_facing()) {
		head_request = head_pending_yaw;
		head_fold = 0.0f;
		if (head_seat_leftover != 0.0f)
			fold_wait_frames++;
	} else {
		/* (logged when the leftover waited for the camera: the request; in
		a seat the clamp may let only part of it through, which the yaw
		log's left column shows) */
		if (head_seat_leftover != 0.0f && fold_wait_frames > 0)
			platform_log("stereo: the look asks for the %.1f degrees the eyes kept, after %d frames: the camera holds "
				"the facing", head_seat_leftover * RADIANS_TO_DEGREES, fold_wait_frames);
		fold_wait_frames = 0;
		head_request = head_pending_yaw + head_seat_leftover;
		head_fold = head_seat_leftover;
		head_seat_leftover = 0.0f;
	}
	*yaw = head_request + snap_pending + smooth_yaw;
	head_yaw_taken = remainderf(head_yaw_taken + head_request, TWO_PI);
	/* the look's pitch is the head's, whatever moved it meanwhile (the game
	levels it as the player walks, a script sets it) */
	/* changes under a few hundredths of a degree are noise: they would set
	the look's up and down action flags every frame, which scripts test
	(player_action_test_look_relative_up, a10's look lesson) */
	if (head_pitch_known)
	{
		*pitch = head_pitch_limited() - current_pitch;
		if (fabsf(*pitch) < HEAD_PITCH_DEADBAND)
			*pitch = 0.0f;
	}
	head_pending_yaw = 0.0f;
	snap_pending = 0.0f;
	smooth_yaw = 0.0f;
	return *yaw != 0.0f || *pitch != 0.0f;
}

/* the game's signed_angular_difference (source/math/real_math.h): from one
angle to another, -pi..pi */
static float signed_difference(float from, float to)
{
	float result = to - from;

	if (result >= 3.14159265f)
		result -= TWO_PI;
	if (result <= -3.14159265f)
		result += TWO_PI;
	return result;
}

/* the game's seat clamp (player_control_modify_desired_angles) on a yaw off
the seat's marker: outside the arc from minimum to maximum, the nearer bound */
static float seat_nearest_bound(float yaw, float minimum, float maximum)
{
	float arc = signed_difference(minimum, maximum);
	float to_maximum = signed_difference(yaw, maximum);
	float to_minimum = signed_difference(minimum, yaw);

	if (arc < 0.0f)
		arc += TWO_PI;
	if (!(to_maximum >= 0.0f && to_maximum < arc) && !(to_minimum >= 0.0f && to_minimum < arc))
		return fabsf(to_minimum) < fabsf(to_maximum) ? minimum : maximum;
	return yaw;
}

int halo_stereo_seat_yaw_clamp(short local_player_index, float *desired_yaw, float yaw_before, float delta_yaw,
	float marker_yaw, float yaw_minimum, float yaw_maximum)
{
	float yaw, head, target, applied, refused;

	if (local_player_index != 0 || !head_tracking(0) || yaw_minimum > yaw_maximum)
		return 0;
	/* the look before this turn, off the marker (the game's yaw is 0..2 pi,
	the marker's -pi..pi), dragged by the seat's own motion as in mono */
	yaw = seat_nearest_bound(remainderf(yaw_before - marker_yaw, TWO_PI), yaw_minimum, yaw_maximum);
	/* the head's share first, inside the bounds on the unwrapped angle: what
	they hold back stays the head's, for the eye cameras and the next look */
	head = head_request;
	head_request = 0.0f;
	/* (inside them, all of it, exactly: no leftover from rounding) */
	target = yaw + head;
	applied = target > yaw_maximum ? yaw_maximum - yaw : target < yaw_minimum ? yaw_minimum - yaw : head;
	yaw += applied;
	/* then the rest (the stick's snap or smooth turn, a script's impulse):
	what the bounds refuse of it is dropped, as in mono */
	yaw = fmaxf(yaw_minimum, fminf(yaw_maximum, yaw + (delta_yaw - head)));
	refused = head - applied;
	/* (added to: a second call this frame, a damage effect's camera impulse
	through player_control_permanent_impulse, carries no head yaw and must
	keep what the first held back) */
	head_seat_leftover = remainderf(head_seat_leftover + refused, TWO_PI);
	head_yaw_taken = remainderf(head_yaw_taken - refused, TWO_PI);
	/* (the leftover the look folded in, taken back: no fold landed) */
	head_fold = 0.0f;
	*desired_yaw = marker_yaw + yaw;
	return 1;
}

void halo_stereo_camera_posed(int holds_facing)
{
	/* (for the next frame's look) */
	camera_held_facing = holds_facing != 0;
	/* the look folded a leftover into the facing and the camera doesn't hold
	it (the camera changed in the fold's tick: a death, a cutscene, the
	glide of a seat left that tick): the full view goes black until the
	camera reaches the eyes */
	if (!holds_facing && !seat_cut_requested && fabsf(head_fold) > SEAT_CUT_FOLD &&
		stereo_frame.mode == HALO_STEREO_HEAD && stereo_frame.eye_count == 2 && !third_person_head) {
		seat_cut_requested = 1;
		seat_cut_elapsed = 0.0f;
		platform_log("stereo: the look took the %.1f degrees a seat's limit held back into a camera not posed from "
			"the facing (the seat's exit glide): the full view goes black until the camera reaches the eyes",
			head_fold * RADIANS_TO_DEGREES);
	}
	head_fold = 0.0f;
}

int halo_stereo_cut_requested(void)
{
	return seat_cut_requested && stereo_frame.mode == HALO_STEREO_HEAD && stereo_frame.eye_count == 2;
}

/* v turned by angle about the unit axis (Rodrigues) */
static void rotate(float v[3], const float axis[3], float angle)
{
	float c = cosf(angle), s = sinf(angle);
	float d = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
	float cross[3] = {
		axis[1] * v[2] - axis[2] * v[1],
		axis[2] * v[0] - axis[0] * v[2],
		axis[0] * v[1] - axis[1] * v[0],
	};
	int i;

	for (i = 0; i < 3; i++)
		v[i] = v[i] * c + cross[i] * s + axis[i] * d * (1.0f - c);
}

static void normalize(float v[3])
{
	float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (length > 0.0f) {
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}
}

float halo_stereo_head_yaw_taken(void)
{
	return head_yaw_taken;
}

void halo_stereo_camera_head_yaw(float head_yaw, float fraction, const char *source)
{
	camera_head_yaw = head_yaw;
	camera_fraction = fraction;
	camera_source = source;
	camera_noted = 1;
}

/* debug.head_yaw_log: on the frame's first orientation, while the head
turns (or the world moved against it), a line with the head's yaw all told
(the frame's predicted pose), the yaw the eye cameras use (their forward's,
the game camera's turned) and the second less the first: the body's yaw,
which a still body keeps, so its change from the last line (step) is how far
the world moved in the room. With the game camera's yaw before the turn, the
head yaw it holds and how render_interpolation.c made it, and the head yaw
the look hasn't taken (pending), in degrees */
static void head_yaw_log_frame(float camera_yaw, float eye_yaw)
{
	float offset, step;

	if (!head_yaw_log || head_log_done)
		return;
	offset = remainderf(eye_yaw - head_yaw_now, TWO_PI);
	step = head_log_last_known ? remainderf(offset - head_log_last_offset, TWO_PI) : 0.0f;
	head_log_done = 1;
	if (stereo_frame.head_yaw == 0.0f && fabsf(step) < 1e-6f && head_log_last_known)
		return;
	platform_log("stereo: head yaw: frame %lu head %.3f eye %.3f body %.3f step %+.4f; camera %.3f holds %.3f "
		"(taken %.3f, pending %.3f, left %.3f), %s t %.3f%s", head_log_frame, head_yaw_now * RADIANS_TO_DEGREES,
		eye_yaw * RADIANS_TO_DEGREES, offset * RADIANS_TO_DEGREES, step * RADIANS_TO_DEGREES,
		camera_yaw * RADIANS_TO_DEGREES, (camera_noted ? camera_head_yaw : head_yaw_taken) * RADIANS_TO_DEGREES,
		head_yaw_taken * RADIANS_TO_DEGREES, head_pending_yaw * RADIANS_TO_DEGREES,
		head_seat_leftover * RADIANS_TO_DEGREES,
		camera_noted && camera_source ? camera_source : "unnoted", camera_noted ? camera_fraction : 1.0f,
		third_person_head ? ", third person" : "");
	head_log_last_offset = offset;
	head_log_last_known = 1;
}

/* Task 12k's immersive cutscene: the cutscene camera turned by the head,
as the third-person head path turns a seat's camera (the stereo spec's "How
it would work"): its yaw about the world's up by the head's yaw since the
cutscene began (from cutscene_anchor_yaw, so each cut re-aims the new
camera at the same direction in the room), its pitch by the head's pitch
from level, inside the game's limit, and the head's roll. The world's
vertical stays the room's; the director's frame pitches with the camera.
HEAD mode's head, or the side-by-side view's simulated one. The frame's
axes go to cutscene_frame_forward and _up, in the eye camera's frame (x
right, y up, z back), for the presenter's mask */
static void cutscene_orient(float forward[3], float up[3])
{
	float camera_forward[3] = { forward[0], forward[1], forward[2] };
	float camera_up[3] = { up[0], up[1], up[2] };
	float eye_right[3];
	float yaw = 0.0f, pitch = 0.0f, roll = 0.0f, eye_yaw, eye_pitch;
	int i;

	normalize(camera_forward);
	/* the camera's up, square to its forward; its left (z up: up x forward) */
	{
		float along = camera_up[0] * camera_forward[0] + camera_up[1] * camera_forward[1] +
			camera_up[2] * camera_forward[2];

		for (i = 0; i < 3; i++)
			camera_up[i] -= along * camera_forward[i];
		normalize(camera_up);
	}
	if (stereo_frame.mode == HALO_STEREO_HEAD) {
		yaw = remainderf(head_yaw_now - cutscene_anchor_yaw, TWO_PI);
		pitch = stereo_frame.head_pitch;
		roll = stereo_frame.head_roll;
	} else if (side_by_side_head_period > 0.0f)
		yaw = side_by_side_head_amplitude * sinf(TWO_PI * side_by_side_head_clock / side_by_side_head_period);
	eye_yaw = atan2f(camera_forward[1], camera_forward[0]) + yaw;
	eye_pitch = fmaxf(-PITCH_LIMIT, fminf(PITCH_LIMIT, asinf(fmaxf(-1.0f, fminf(1.0f, camera_forward[2]))) + pitch));
	forward[0] = cosf(eye_pitch) * cosf(eye_yaw);
	forward[1] = cosf(eye_pitch) * sinf(eye_yaw);
	forward[2] = sinf(eye_pitch);
	up[0] = -sinf(eye_pitch) * cosf(eye_yaw);
	up[1] = -sinf(eye_pitch) * sinf(eye_yaw);
	up[2] = cosf(eye_pitch);
	rotate(up, forward, -roll);
	normalize(forward);
	normalize(up);
	/* the eye camera's right (z up: forward x up), and the frame's axes in
	the eye camera's frame: x right, y up, z back */
	eye_right[0] = forward[1] * up[2] - forward[2] * up[1];
	eye_right[1] = forward[2] * up[0] - forward[0] * up[2];
	eye_right[2] = forward[0] * up[1] - forward[1] * up[0];
	cutscene_frame_forward[0] = camera_forward[0] * eye_right[0] + camera_forward[1] * eye_right[1] +
		camera_forward[2] * eye_right[2];
	cutscene_frame_forward[1] = camera_forward[0] * up[0] + camera_forward[1] * up[1] + camera_forward[2] * up[2];
	cutscene_frame_forward[2] = -(camera_forward[0] * forward[0] + camera_forward[1] * forward[1] +
		camera_forward[2] * forward[2]);
	cutscene_frame_up[0] = camera_up[0] * eye_right[0] + camera_up[1] * eye_right[1] + camera_up[2] * eye_right[2];
	cutscene_frame_up[1] = camera_up[0] * up[0] + camera_up[1] * up[1] + camera_up[2] * up[2];
	cutscene_frame_up[2] = -(camera_up[0] * forward[0] + camera_up[1] * forward[1] + camera_up[2] * forward[2]);
	reticle_set(NULL, NULL, NULL);
}

void halo_stereo_head_orient(float forward[3], float up[3])
{
	float yaw, pitch, camera_yaw;
	float aim[3] = { forward[0], forward[1], forward[2] };

	/* HEAD mode's film easing into a window onto the world (portal_eyes):
	the camera levels, as the screen is, since the full view after it puts
	the room's level on the world's (the look's pitch is the head's) */
	if (stereo_mode == HALO_STEREO_HEAD && stereo_frame.mode == HALO_STEREO_SCREEN && film_frame &&
		portal_share > 0.0f) {
		yaw = atan2f(forward[1], forward[0]);
		pitch = asinf(fmaxf(-1.0f, fminf(1.0f, forward[2]))) * (1.0f - portal_share);
		forward[0] = cosf(pitch) * cosf(yaw);
		forward[1] = cosf(pitch) * sinf(yaw);
		forward[2] = sinf(pitch);
		up[0] = -sinf(pitch) * cosf(yaw);
		up[1] = -sinf(pitch) * sinf(yaw);
		up[2] = cosf(pitch);
		return;
	}

	if (halo_stereo_cutscene_immersive()) {
		cutscene_orient(forward, up);
		return;
	}
	if (stereo_frame.mode != HALO_STEREO_HEAD || stereo_frame.eye_count != 2)
		return;
	camera_yaw = atan2f(forward[1], forward[0]);
	if (third_person_head) {
		/* the game's third-person camera turned by the head since it began:
		its yaw about the world's up, and its pitch by the head's change of
		pitch, inside the game's limit. The roll is the head's own, as in
		first person, so the camera's up is the room's whenever the camera
		is level */
		yaw = atan2f(forward[1], forward[0]) + third_person_yaw;
		pitch = asinf(fmaxf(-1.0f, fminf(1.0f, forward[2]))) + head_pitch_limited() - third_person_pitch_from;
		pitch = fmaxf(-PITCH_LIMIT, fminf(PITCH_LIMIT, pitch));
	} else {
		/* the head's yaw at render time: the game camera's yaw, less the
		head yaw it holds, plus the head's yaw now, so the eye cameras are
		exactly the head's every frame, whatever the game's camera lags by.
		The camera holds the head yaw the look had taken when it was posed
		(render_interpolation.c says how much, halo_stereo_camera_head_yaw:
		as of the later tick it blends, or as of now); since then the look
		took more (taken less held: none for a camera posed this frame), and
		it hasn't taken the rest yet (pending: this frame's and the paused
		frames'), nor what the eye cameras keep (head_seat_leftover: what a
		seat's limit holds back, the head's turn through a vehicle's camera
		until the camera holds the facing). Left positive, as the
		game's yaw. Then the head's own pitch, inside the game's limit, so
		the camera is level when the head is, whatever the look's pitch was */
		float held = camera_noted ? camera_head_yaw : head_yaw_taken;

		yaw = atan2f(forward[1], forward[0]) + remainderf(head_yaw_taken - held, TWO_PI) + head_pending_yaw +
			head_seat_leftover;
		pitch = head_pitch_limited();
	}
	forward[0] = cosf(pitch) * cosf(yaw);
	forward[1] = cosf(pitch) * sinf(yaw);
	forward[2] = sinf(pitch);
	up[0] = -sinf(pitch) * cosf(yaw);
	up[1] = -sinf(pitch) * sinf(yaw);
	up[2] = cosf(pitch);
	/* the roll tilts up about forward; left ear down tilts it to the left,
	which is a turn the other way about forward */
	rotate(up, forward, -stereo_frame.head_roll);
	normalize(forward);
	normalize(up);
	head_yaw_log_frame(camera_yaw, yaw);
	/* a seat's gun aims along the game's camera, which only the stick turns:
	the crosshair goes where that points in the picture the head turned
	(halo_stereo_reticle). So does a first-person seat's while its limit
	holds the aim back from the head (head_seat_leftover). On foot the game's
	look is the head's, and the crosshair is straight ahead */
	if (third_person_head || head_seat_leftover != 0.0f) {
		normalize(aim);
		reticle_set(aim, forward, up);
	} else
		reticle_set(NULL, NULL, NULL);
}

void halo_stereo_zoom_orient(float forward[3], float up[3])
{
	/* (the zoom looks along the gun: in a seat whose limit holds the aim
	back from the head, the game camera's forward, as in third person) */
	if (!third_person_head && head_seat_leftover == 0.0f)
		halo_stereo_head_orient(forward, up);
}

void halo_stereo_set_depth_range(float z_near, float z_far)
{
	z_near_world = z_near;
	z_far_world = z_far;
}

void halo_stereo_depth_range(float *z_near, float *z_far)
{
	*z_near = z_near_world;
	*z_far = z_far_world;
}

void halo_stereo_set_title_bars(float bars)
{
	title_bars_now = bars;
}

/* the film's title bars come in at the Xbox letterbox's rate
(cinematics.c's cinematic_render): one letterbox amount a second */
#define TITLE_BARS_RATE 1.0f

float halo_stereo_title_bars_ease(float bars, float limit, float fade, float seconds)
{
	if (!(limit > 0.0f))
		return 0.0f;
	/* (they never rise while the fade shows; they fall with the title) */
	if (!(fade > 0.0f))
		bars = fminf(bars + fmaxf(0.0f, seconds) * TITLE_BARS_RATE, 1.0f);
	return fminf(bars, limit);
}

int halo_stereo_expansion(float *progress, float *bars)
{
	if (!expansion_on || stereo_frame.eye_count != 2) {
		*progress = *bars = 0.0f;
		return 0;
	}
	*progress = expansion_elapsed / HALO_STEREO_EXPANSION_SECONDS;
	*bars = halo_stereo_expansion_bars(expansion_bars, expansion_elapsed);
	return 1;
}

int halo_stereo_side_by_side_window(int eye, struct halo_stereo_window_model *model)
{
	static const float identity[3][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
	float position[3], half_height = FILM_DEFAULT_HALF_WIDTH / FILM_ASPECT, start[4], end[4];
	float progress, bars;
	int film = halo_stereo_film(), expanding = halo_stereo_expansion(&progress, &bars);

	memset(model, 0, sizeof(*model));
	if (stereo_mode != HALO_STEREO_SIDE_BY_SIDE || side_by_side_screen || eye < 0 || eye > 1 ||
		stereo_frame.eye_count != 2 || (!film && !expanding))
		return 0;
	/* the eye's fixed frustum (eye 1 mirrors eye 0), and where it sits
	before the screen: its offset to the side, the screen's center level
	with it */
	model->tangents[0] = side_by_side_tangents[eye == 0 ? 0 : 1];
	model->tangents[1] = side_by_side_tangents[eye == 0 ? 1 : 0];
	model->tangents[2] = side_by_side_tangents[2];
	model->tangents[3] = side_by_side_tangents[3];
	position[0] = (eye == 0 ? -SIDE_BY_SIDE_OFFSET : SIDE_BY_SIDE_OFFSET) * METERS_PER_UNIT;
	position[1] = 0.0f;
	position[2] = SIDE_BY_SIDE_SCREEN_DISTANCE;
	model->screen[0] = (-FILM_DEFAULT_HALF_WIDTH - position[0]) / position[2];
	model->screen[1] = (FILM_DEFAULT_HALF_WIDTH - position[0]) / position[2];
	model->screen[2] = (-half_height - position[1]) / position[2];
	model->screen[3] = (half_height - position[1]) / position[2];
	if (film) {
		model->kind = 1;
		return 1;
	}
	halo_stereo_screen_window(position, FILM_DEFAULT_HALF_WIDTH, half_height, start);
	halo_stereo_window_empty(end);
	halo_stereo_window_include_view(end, model->tangents, identity);
	halo_stereo_window_at(start, end, progress, progress > 0.0f ? side_by_side_last_window[eye] : NULL,
		model->window);
	memcpy(side_by_side_last_window[eye], model->window, sizeof(model->window));
	model->bars = bars;
	model->kind = 2;
	return 2;
}
