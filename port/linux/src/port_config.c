/*
PORT_CONFIG.C

The native ports' settings (port_config.h), parsed with tomlc17
(port/third_party/tomlc17). Every setting is in the table below with its
type, default, the HALO_* environment variable that overrides it and the
comment written into a new file. The file is read once, on the first
question (config_reload_boolean reads one setting again); unknown keys and
values of the wrong type are reported in the log and the defaults used
instead, and a table or key repeated in it keeps its first and is reported
too. Once it exists, the file is only rewritten to add a newer version's
settings or fold such repeats, keeping the player's edits and comments.

Under the file sits the device's profile (profiles/, chosen by the iOS host:
port/ios/host/host_profile.c), which reaches this file as
HALO_DEVICE_PROFILE_VALUES. A setting's device value is its default with the
profile's value over it. config.origins, beside config.toml, records for
each setting whether the value in the file is one the game wrote as a
default or one somebody chose: a written default follows the device value
(its line is rewritten when that changes), a chosen value is in force. See
config_origin_decide.
*/

#include "platform.h"
#include "port_config.h"
#include "port_profile_apply.h"
#include "tomlc17.h"

#include <SDL3/SDL.h>

#ifdef HALO_ILP32
/* Runtime output context supplied by the Apple host. */
int host_external_active(void);
#endif
#include <ctype.h>
#include <math.h>
#include <locale.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- the settings */

enum config_type
{
	_config_boolean,
	_config_integer,
	_config_real,
	_config_string,
};

/* how the setting's environment variable sets it */
enum config_environment
{
	/* the variable's text is the value ("0", "false", "no" and "off" are
	false for a boolean) */
	_environment_value,
	/* the variable being set (not empty, "0", "false", "no" or "off") makes it true */
	_environment_set_is_true,
	/* the variable being set (not empty, "0", "false", "no" or "off") makes it false */
	_environment_set_is_false,
};

/* the builds a setting means something in, and is written for */
enum
{
	_platform_desktop = 1,
	_platform_ios = 2,
	_platform_all = _platform_desktop | _platform_ios,
	/* (of the desktop builds, only Windows) */
	_platform_windows = 4,
};

struct config_setting
{
	const char *name;
	enum config_type type;
	/* as it is written in the file */
	const char *default_value;
	const char *environment;
	enum config_environment environment_style;
	unsigned platforms;
	const char *comment;
};

#ifdef HALO_IOS
#define DEFAULT_INTERNET_PLAY "false"
#define DEFAULT_CLIPBOARD_JOIN "false"
/* (a download of up to about 2.3 GB starts without a question: off on Apple
builds until it asks first) */
#define DEFAULT_MAP_DOWNLOAD "false"
/* (helping send a map opens a listening port, TCP 5153, to any address, and
uploads unasked: off on Apple builds, a player's choice there) */
#define DEFAULT_MAP_SEED "false"
#define DEFAULT_HOST_PUBLIC "false"
#else
#define DEFAULT_MAP_DOWNLOAD "true"
#define DEFAULT_MAP_SEED "true"
#define DEFAULT_INTERNET_PLAY "true"
#define DEFAULT_CLIPBOARD_JOIN "true"
#define DEFAULT_HOST_PUBLIC "true"
#endif
/* (Apple builds' network.online_ask) "true" is internet play on by default,
as far as an Apple build goes: the first internet screen opened asks, and
nothing reaches a server before the player agrees (halo_internet.h) */
#define DEFAULT_ONLINE_ASK "false"

static const struct config_setting config_settings[] =
{
	{ "display.fullscreen", _config_boolean, "true", "HALO_FULLSCREEN", _environment_value, _platform_desktop,
		"Where display.mode is empty: start borderless over the whole display;\n"
		"false starts in a window. F11 switches." },
	{ "display.mode", _config_string, "\"\"", "HALO_DISPLAY_MODE", _environment_value, _platform_desktop,
		"\"fullscreen\" takes the display (at display.resolution's mode),\n"
		"\"borderless\" is a window over the whole desktop, \"windowed\" a window\n"
		"(display.window_size). Empty: display.fullscreen's (true: borderless).\n"
		"F11 switches to the window and back." },
	{ "display.resolution", _config_string, "\"native\"", "HALO_RESOLUTION", _environment_value, _platform_desktop,
		"What fullscreen and borderless draw at: \"native\", the display's own, or\n"
		"\"<width>x<height>\" (\"1920x1080\"), 640x480 or more. Fullscreen sets the\n"
		"display to it; borderless draws it scaled to the display." },
	{ "display.resolution_scaling", _config_string, "\"native\"", "HALO_RESOLUTION_SCALING", _environment_value,
		_platform_desktop,
		"\"native\" draws at the window's resolution (fullscreen, the display's or\n"
		"display.resolution); \"original\" draws the Xbox's 640x480 and scales it\n"
		"up." },
	{ "display.window_size", _config_string, "\"\"", "HALO_WINDOW_SIZE", _environment_value, _platform_desktop,
		"The window's size, \"<width>x<height>\" (\"1920x1080\"), 640x480 or more (it\n"
		"can be resized). Empty: display.window_scale's." },
	{ "display.window_scale", _config_integer, "2", "HALO_WINDOW_SCALE", _environment_value, _platform_desktop,
		"Where display.window_size is empty: the window's size as a multiple of\n"
		"640x480." },
	{ "display.screen_width", _config_integer, "0", "HALO_SCREEN_WIDTH", _environment_value, _platform_ios,
		"Columns of the 480-line picture: 0 for the display's shape, 640 for the\n"
		"Xbox's 4:3." },
	{ "display.render_height", _config_integer, "0", "HALO_RENDER_HEIGHT", _environment_value, _platform_ios,
		"Internal rendering height in physical pixels: 0 (default) uses native\n"
		"display resolution. Try 1080, 720 or 480 for lower GPU/battery use.\n"
		"The selected aspect ratio is preserved; changes apply on relaunch." },
	{ "display.anisotropic_filtering", _config_integer, "16", "HALO_ANISOTROPIC_FILTERING", _environment_value, _platform_all,
		"Anisotropic filtering of the game's textures, 1 (off, as on the Xbox) to 16:\n"
		"sharper floors and terrain at a distance, for a few percent of GPU time." },
	{ "display.decal_batching", _config_boolean, "true", "HALO_DECAL_BATCHING", _environment_value, _platform_all,
		"Draw the decals (bullet holes, blood, scorch marks) that share a texture\n"
		"in one draw each rather than one draw a decal: the same picture for less\n"
		"CPU time. False draws each on its own, to compare." },
	{ "display.shadow_batching", _config_boolean, "true", "HALO_SHADOW_BATCHING", _environment_value, _platform_all,
		"Draw all of a view's character and vehicle shadows before laying any on the\n"
		"world, and in stereo lay one eye's shadows in the other too: the picture's\n"
		"pass is no longer interrupted for each shadow. False keeps the Xbox's\n"
		"order, to compare." },
	{ "display.shadow_minimum_size", _config_integer, "0", "HALO_SHADOW_MINIMUM_SIZE", _environment_value, _platform_all,
		"Device-profile floor on how small a character or vehicle may look and still\n"
		"cast its shadow, in pixels of the 480-line picture, 0 to 240. Shadows fade\n"
		"out as objects shrink toward it. 0 to 30 keep the Xbox's 30. The players'\n"
		"own units, vehicles, fellow riders and bodies keep their shadows." },
	{ "display.dynamic_light_limit", _config_integer, "0", "HALO_DYNAMIC_LIGHT_LIMIT", _environment_value, _platform_all,
		"Device-profile cap on dynamic lights illuminating level surfaces, 0 to 128.\n"
		"Zero keeps all lights; first-person weapon lights are always retained.\n"
		"Other lights rank by radius over camera distance. Object lighting is unchanged." },
	{ "display.specular_light_limit", _config_integer, "0", "HALO_SPECULAR_LIGHT_LIMIT", _environment_value, _platform_all,
		"Device-profile cap on specular lights among the level's selected dynamic\n"
		"lights, 0 to 128. Zero keeps all selected lights' highlights. First-person\n"
		"weapon lights are always retained. Object lighting is unchanged." },
	{ "display.shadow_map_size", _config_integer, "512", "HALO_SHADOW_MAP_SIZE", _environment_value, _platform_all,
		"The size of the textures characters' and vehicles' shadows are drawn into,\n"
		"128 (as on the Xbox) to 2048: larger is smoother." },
	{ "display.effect_resolution", _config_string, "\"half\"", "HALO_EFFECT_RESOLUTION", _environment_value, _platform_all,
		"The resolution of the 320x240 target the reflections in mirror floors and\n"
		"the active camouflage are drawn into: \"xbox\" keeps the Xbox's 320x240,\n"
		"\"half\" is half the screen's resolution each way, and \"full\" the screen's\n"
		"resolution (sharper; up to a whole second render of the scene's pixels\n"
		"where a mirror shows)." },
	{ "display.model_lod", _config_string, "\"max\"", "HALO_MODEL_LOD", _environment_value, _platform_all,
		"Which geometry detail level models draw: \"max\" (default) always the\n"
		"highest they have, so they never pop between levels or drop out with\n"
		"distance; \"auto\" picks by on-screen size, as the Xbox did. The console's\n"
		"rasterizer_debug_model_lod overrides it." },
	{ "display.lod_scale", _config_real, "1.0", "HALO_LOD_SCALE", _environment_value, _platform_ios,
		"HEAD mode and the side-by-side view: a multiplier on the pixel size the game\n"
		"picks model detail and particle distance by; 1.0 is the Xbox's (0.5 to 4).\n"
		"Higher keeps model detail and particles farther out, for the headset's\n"
		"denser pixels, at some frame time. Sprites keep their size. Read at start." },
	{ "display.compressed_textures", _config_boolean, "true", "HALO_COMPRESSED_TEXTURES", _environment_value, _platform_ios,
		"With the Metal renderer, keep the game's DXT textures compressed on the GPU\n"
		"where it supports them (BC1-3): a quarter to an eighth of the memory, and\n"
		"faster loads. false decodes them, as OpenGL ES must." },
	{ "display.render_scale", _config_real, "1.0", "HALO_RENDER_SCALE", _environment_value, _platform_ios,
		"Render at this fraction of the display's pixels each way (0.25 to 1.0;\n"
		"0.67 draws under half the pixels), scaled back up by display.upscaler.\n"
		"display.render_height, when set, caps the result." },
	{ "display.native_hud", _config_boolean, "true", "HALO_NATIVE_HUD", _environment_value, _platform_ios,
		"When the scene renders below the display's size (display.render_height,\n"
		"display.render_scale, or a build that sets a render height), draw the HUD,\n"
		"menus, text and letterbox bars at the display's own resolution over the\n"
		"scaled-up scene. false scales them up with the scene. No effect, and no\n"
		"cost, at native resolution or on Apple Vision Pro." },
	{ "display.dynamic_resolution", _config_boolean, "false", "HALO_DYNAMIC_RESOLUTION", _environment_value, _platform_ios,
		"Metal renderer, in gameplay: lower the 3D view's resolution while the GPU\n"
		"cannot keep up and raise it again when it can, between frames, from\n"
		"display.render_height (the ceiling) down to display.dynamic_resolution_floor.\n"
		"The HUD and menus stay at the display's resolution. Read at launch." },
	{ "display.dynamic_resolution_floor", _config_integer, "540", "HALO_DYNAMIC_RESOLUTION_FLOOR", _environment_value, _platform_ios,
		"The fewest lines display.dynamic_resolution draws the 3D view at; never\n"
		"fewer than half the ceiling's. Read at launch." },
	{ "display.anti_aliasing", _config_string, "\"off\"", "HALO_ANTI_ALIASING", _environment_value, _platform_all,
		"Smoothing of jagged edges, which the Xbox did not have: \"off\", or\n"
		"\"fxaa\" or \"smaa\", a pass over the 3D view once it is drawn, at the size\n"
		"it is drawn at (the HUD and menus are drawn after it and stay sharp).\n"
		"SMAA is sharper and costs more." },
	{ "display.upscaler", _config_string, "\"bilinear\"", "HALO_UPSCALER", _environment_value, _platform_ios,
		"How a picture rendered below the display's size is scaled up: \"bilinear\"\n"
		"(default), or \"metalfx\" (Metal renderer, on a device: MetalFX's spatial\n"
		"scaler, sharper edges and text for a fraction of a millisecond)." },
	{ "display.immersive", _config_boolean, "false", "HALO_IMMERSIVE", _environment_value, _platform_ios,
		"Apple Vision Pro, visionOS 26 and later: show the game on a screen standing\n"
		"in the room (an immersive space) instead of in a window." },
	{ "display.stereo", _config_string, "\"off\"", "HALO_STEREO", _environment_value, _platform_ios,
		"Apple Vision Pro: \"off\" (flat), \"head\" (in stereo, the head turns the view),\n"
		"\"screen\" (in stereo on the theater screen), or \"side_by_side\" (a debug view\n"
		"of both eyes in the window)." },
	{ "display.frame_repeat", _config_integer, "0", "HALO_FRAME_REPEAT", _environment_value, _platform_ios,
		"Apple Vision Pro, display.stereo = \"head\" or \"screen\": how many extra\n"
		"display refreshes each frame stays up, 0 to 3: 0 draws a frame every\n"
		"refresh (90 a second), 1 every other (45 a second) for when 90 costs too\n"
		"much. Set when the immersive space opens." },
	{ "display.foveation", _config_boolean, "true", "HALO_FOVEATION", _environment_value, _platform_ios,
		"Apple Vision Pro, display.stereo = \"head\": render through the\n"
		"Compositor's foveation, sharper at the middle of each view than at its\n"
		"edges, at display.render_quality. False renders each view evenly, for\n"
		"comparisons. Set when the immersive space opens." },
	{ "display.render_quality", _config_real, "0.6", "HALO_RENDER_QUALITY", _environment_value, _platform_ios,
		"With display.foveation: the Compositor's render quality, 0 to 1 (the\n"
		"headset's lowest to its highest): how many pixels the middle of each view\n"
		"gets. Higher is sharper and costs more frame time and memory. In place of\n"
		"display.render_scale for the eyes. The default, 0.6, is the highest\n"
		"quality that held 90 Hz in the 2026-10-07 headset sweep (a10, foveated):\n"
		"0.7 and 0.8 begin to miss refreshes, 0.9 and 1.0 miss 5 to 9%, and\n"
		"unfoveated rendering ran at 45 Hz. Set when the immersive space opens." },
	{ "display.first_person_body", _config_boolean, "true", "HALO_FIRST_PERSON_BODY", _environment_set_is_true, _platform_ios,
		"With display.stereo = \"head\": draw the Master Chief's body below the\n"
		"view (no head or neck, and no third-person arms while you hold a weapon),\n"
		"so looking down shows your chest and feet. None while a script animates\n"
		"him; in a seat, as display.first_person_body_seats says." },
	{ "display.first_person_body_offset", _config_real, "0.08", "HALO_FIRST_PERSON_BODY_OFFSET", _environment_value,
		_platform_ios,
		"With display.first_person_body: how far behind your eyes the body stands,\n"
		"in world units (0 to 0.2; 0.08 is about 24 cm), so the feet show past the\n"
		"chest when you look down. Read at start." },
	{ "display.first_person_body_seats", _config_boolean, "true", "HALO_FIRST_PERSON_BODY_SEATS", _environment_set_is_true,
		_platform_ios,
		"With display.first_person_body: also draw the legs in a vehicle's\n"
		"first-person seat (the Warthog's passenger seat, the Scorpion's rider\n"
		"seats). Seats whose camera is on the gun or behind the vehicle, and a10's\n"
		"cryo pod, show no body." },
	{ "display.first_person_body_seat_offset", _config_real, "0.0", "HALO_FIRST_PERSON_BODY_SEAT_OFFSET",
		_environment_value, _platform_ios,
		"With display.first_person_body_seats: how far the seated legs move back\n"
		"along the seat's facing, in world units (0 to 0.2). A seat's camera is a\n"
		"marker on the vehicle, not a point on the body, so 0 means the legs sit\n"
		"where the game puts them." },
	{ "display.weapon_offset_down", _config_real, "0.0", "HALO_WEAPON_OFFSET_DOWN", _environment_value, _platform_ios,
		"With display.stereo = \"head\": how far the first-person weapon and arms\n"
		"sit below where the game puts them, in world units (0 to 0.2; 0.01 is about\n"
		"3 cm), so the headset's taller view doesn't show where the arms end. Read\n"
		"at start." },
	{ "display.weapon_offset_back", _config_real, "0.0", "HALO_WEAPON_OFFSET_BACK", _environment_value, _platform_ios,
		"With display.stereo = \"head\": how far the first-person weapon and arms\n"
		"sit closer to your eyes than where the game puts them, in world units (0\n"
		"to 0.2). Read at start." },
	{ "display.eye_height_offset", _config_real, "0.0", "HALO_EYE_HEIGHT_OFFSET", _environment_value, _platform_ios,
		"With display.stereo = \"head\": how far your eyes sit above the game's\n"
		"camera, in world units (-0.1 to 0.1; 0.03 is about 9 cm), for trying a\n"
		"taller eye height. Not in cutscenes. Read at start." },
	{ "display.seat_look_fade", _config_boolean, "true", "HALO_SEAT_LOOK_FADE", _environment_value, _platform_ios,
		"With display.stereo = \"head\": in a first-person seat whose aim has a\n"
		"limit (a Warthog's passenger seat, the Pelican's bench, a10's cryo pod),\n"
		"looking past the limit fades the view and the HUD toward black as the\n"
		"head turns on, while the world still moves with the head. False lets the\n"
		"head look all the way around. Read at start." },
	{ "display.seat_look_fade_start", _config_real, "5.0", "HALO_SEAT_LOOK_FADE_START", _environment_value,
		_platform_ios,
		"With display.seat_look_fade: how many degrees past the seat's limit the\n"
		"fade begins (0 to 90). Read at start." },
	{ "display.seat_look_fade_black", _config_real, "45.0", "HALO_SEAT_LOOK_FADE_BLACK", _environment_value,
		_platform_ios,
		"With display.seat_look_fade: how many degrees past the seat's limit the\n"
		"view is black (1 to 180, past display.seat_look_fade_start). Read at\n"
		"start." },
	{ "display.pod_look_fade_start", _config_real, "15.0", "HALO_POD_LOOK_FADE_START", _environment_value,
		_platform_ios,
		"As display.seat_look_fade_start, for a10's cryo pod alone: how many\n"
		"degrees past the pod's limit the fade begins (0 to 90). Read at start." },
	{ "display.pod_look_fade_end", _config_real, "70.0", "HALO_POD_LOOK_FADE_END", _environment_value,
		_platform_ios,
		"In a10's cryo pod: how many degrees past the pod's limit the fade is at\n"
		"its darkest (1 to 180, past display.pod_look_fade_start). Read at start." },
	{ "display.pod_look_fade_dim", _config_real, "0.85", "HALO_POD_LOOK_FADE_DIM", _environment_value,
		_platform_ios,
		"In a10's cryo pod: the most the view darkens past the limit, 0.0 to 1.0\n"
		"(1.0 is black). The blur still reaches full. Read at start." },
	{ "display.seat_look_fade_blur", _config_boolean, "true", "HALO_SEAT_LOOK_FADE_BLUR", _environment_value,
		_platform_ios,
		"With display.seat_look_fade: blur the view as it fades, the blur full\n"
		"half way to black; false only dims it. Read at start." },
	{ "display.film_depth_share", _config_real, "0.25", "HALO_FILM_DEPTH_SHARE", _environment_value, _platform_ios,
		"With display.stereo: how deep a cutscene (a 3D film on the screen) looks:\n"
		"how far behind the screen the far distance sits, as a share of your eye\n"
		"separation (0.05 to 0.9; more is deeper). Read at start." },
	{ "display.film_convergence", _config_real, "1.75", "HALO_FILM_CONVERGENCE", _environment_value, _platform_ios,
		"With display.stereo: in a cutscene, what lies this many meters ahead of\n"
		"the camera sits on the screen's surface; nearer comes out in front of it\n"
		"(0.3 to 10; less pushes faces back behind the screen). Read at start." },
	{ "display.screen_depth_share", _config_real, "0.3", "HALO_SCREEN_DEPTH_SHARE", _environment_value, _platform_ios,
		"With display.stereo = \"screen\": how deep the game looks on the screen (a\n"
		"3D TV): how far behind the screen the far distance sits, as a share of\n"
		"your eye separation (0.05 to 0.9; more is deeper, less keeps the reticle\n"
		"from doubling on far targets). Read at start." },
	{ "display.screen_convergence", _config_real, "1.0", "HALO_SCREEN_CONVERGENCE", _environment_value, _platform_ios,
		"With display.stereo = \"screen\": what lies this many meters ahead of the\n"
		"player sits on the screen's surface; nearer comes out in front of it\n"
		"(0.3 to 10; 0.6 to 0.7 brings only walls you nearly touch in front).\n"
		"Read at start." },
	{ "display.screen_framing", _config_string, "\"band\"", "HALO_SCREEN_FRAMING", _environment_value, _platform_ios,
		"With display.stereo = \"screen\": \"band\" (default) shows the game's\n"
		"horizontal view across the 16:9 screen, so things look their natural\n"
		"size, and the screen's bottom edge cuts the weapon like a window frame;\n"
		"\"wide\" shows the flat game's wider view, everything a quarter smaller\n"
		"and the whole weapon in view. Cutscenes fill the screen either way.\n"
		"Read at start." },
	{ "display.stereo_vehicle_screen", _config_boolean, "false", "HALO_STEREO_VEHICLE_SCREEN", _environment_value,
		_platform_ios,
		"With display.stereo = \"head\": put third-person cameras (a vehicle's chase\n"
		"camera) on the theater screen, as cutscenes are. Off (default), they stay\n"
		"around you: your head looks around from the camera, and the sticks drive\n"
		"and aim. Read at start." },
	{ "display.hud_corner_across", _config_real, "28.0", "HALO_HUD_CORNER_ACROSS", _environment_value, _platform_ios,
		"With display.stereo = \"head\": how far out to the sides the HUD sits, in\n"
		"degrees from the view's center: the outer corners of the weapon's counters\n"
		"(top left), the shields and health (top right), the motion tracker (bottom\n"
		"left) and the prompts and messages (left). Each piece grows toward the\n"
		"center from there. 0 to 40 (both eyes see 40). Read at start." },
	{ "display.hud_corner_up", _config_real, "20.0", "HALO_HUD_CORNER_UP", _environment_value, _platform_ios,
		"With display.stereo = \"head\": how far up the HUD's top corners sit (the\n"
		"weapon's counters, the shields and health), in degrees from the view's\n"
		"center, 0 to 40. Read at start." },
	{ "display.hud_tracker_down", _config_real, "22.0", "HALO_HUD_TRACKER_DOWN", _environment_value, _platform_ios,
		"With display.stereo = \"head\": how far down the motion tracker's bottom\n"
		"edge sits, in degrees below the view's center, 0 to 40. Read at start." },
	{ "display.hud_messages_up", _config_real, "12.0", "HALO_HUD_MESSAGES_UP", _environment_value, _platform_ios,
		"With display.stereo = \"head\": how far up the top of the help text,\n"
		"prompts and messages sits, in degrees from the view's center; they run\n"
		"down from there. 0 to 40. Read at start." },
	{ "display.hud_scale", _config_real, "1.0", "HALO_HUD_SCALE", _environment_value, _platform_ios,
		"With display.stereo = \"head\": the size of the HUD's pieces (not the\n"
		"crosshair) against their natural 0.072 degrees a line, 0.5 to 2. Read at\n"
		"start." },
	{ "display.hud_distance", _config_real, "2.0", "HALO_HUD_DISTANCE", _environment_value, _platform_ios,
		"With display.stereo = \"head\": how far ahead the HUD, the crosshair and the\n"
		"menus rest, in meters, 1 to 4. Every piece keeps its size in your view at\n"
		"any distance; only how near it looks changes. Read at start." },
	{ "display.hud_depth", _config_boolean, "true", "HALO_HUD_DEPTH", _environment_value, _platform_ios,
		"With display.stereo = \"head\": bring each HUD piece (the crosshair, the\n"
		"corners) nearer while something under it is nearer than its resting\n"
		"distance, so it always looks in front of what it covers. Off: the HUD\n"
		"stays at display.hud_distance. Read at start." },
	{ "display.hud_depth_share", _config_real, "0.85", "HALO_HUD_DEPTH_SHARE", _environment_value, _platform_ios,
		"With display.hud_depth: how near a piece comes, as a share of the\n"
		"distance to the nearest thing under it, 0.5 to 1. Read at start." },
	{ "display.hud_depth_floor", _config_real, "0.3", "HALO_HUD_DEPTH_FLOOR", _environment_value, _platform_ios,
		"With display.hud_depth: the nearest a piece comes, in meters, below\n"
		"display.hud_distance. Read at start." },
	{ "display.hud_depth_pull_in", _config_real, "0.1", "HALO_HUD_DEPTH_PULL_IN", _environment_value, _platform_ios,
		"With display.hud_depth: the seconds a piece takes to come from its\n"
		"resting distance to the nearest, 0 to 5. Read at start." },
	{ "display.hud_depth_relax", _config_real, "1.0", "HALO_HUD_DEPTH_RELAX", _environment_value, _platform_ios,
		"With display.hud_depth: the seconds a piece takes to go back from the\n"
		"nearest to its resting distance, 0 to 5. Read at start." },
	{ "display.hud_depth_relax_delay", _config_real, "0.5", "HALO_HUD_DEPTH_RELAX_DELAY", _environment_value,
		_platform_ios,
		"With display.hud_depth: the seconds a piece waits, once nothing near is\n"
		"under it, before it goes back, 0 to 5. Read at start." },
	{ "display.hud_resolution", _config_real, "1.0", "HALO_HUD_RESOLUTION", _environment_value, _platform_ios,
		"HEAD mode and the side-by-side view: the pixels the HUD, the crosshair and\n"
		"the menus are drawn with, as a fraction of the view's along each axis, 0.5\n"
		"to 1. Less takes less memory (0.5: a quarter) and looks softer. Read at\n"
		"start." },
	{ "display.texture_idle_seconds", _config_real, "60.0", "HALO_TEXTURE_IDLE_SECONDS", _environment_value, _platform_all,
		"Seconds a texture goes undrawn before the cache drops it, 10 to 600. The\n"
		"cache sweeps every 10 seconds, so a texture goes 0 to 10 seconds after\n"
		"that. A device profile sets it for the device's memory; it is not a player\n"
		"choice." },
	{ "display.clutter_distance", _config_real, "0.0", "HALO_CLUTTER_DISTANCE", _environment_value, _platform_all,
		"Beyond this many world units from the camera, dead bodies, weapons and\n"
		"equipment on the ground and garbage objects are not drawn; 0 (default) draws\n"
		"them all, 0 to 200. Held weapons, living units, vehicles, scenery and effects\n"
		"are always drawn. Only the drawing changes, never the game. A device profile\n"
		"sets it for the device's speed; it is not a player choice." },
	{ "input.turn", _config_string, "\"snap\"", "HALO_TURN", _environment_value, _platform_ios,
		"With display.stereo = \"head\": how the right stick turns the view: \"snap\"\n"
		"(default; a flick turns input.snap_angle at once), \"smooth\" (steadily, at\n"
		"input.smooth_turn_speed), or \"off\" (it doesn't; turn your body)." },
	{ "input.snap_angle", _config_real, "30.0", "HALO_SNAP_ANGLE", _environment_value, _platform_ios,
		"With input.turn = \"snap\": degrees per snap (5 to 180)." },
	{ "input.smooth_turn_speed", _config_real, "120.0", "HALO_SMOOTH_TURN_SPEED", _environment_value, _platform_ios,
		"With input.turn = \"smooth\": degrees per second with the stick all the way\n"
		"over (10 to 720; 120 is the game's own speed before it speeds up)." },
	{ "input.comfort_vignette", _config_boolean, "false", "HALO_COMFORT_VIGNETTE", _environment_value, _platform_ios,
		"With input.turn = \"smooth\": darken the edges of the view while the stick\n"
		"turns it, for comfort." },
	{ "input.aim", _config_string, "\"controller\"", "HALO_AIM", _environment_value, _platform_ios,
		"With display.stereo = \"head\": what aims, \"head\" (the view's center)\n"
		"or \"controller\" (default; the aiming hand's Sense controller). Aiming\n"
		"uses the head when tracking is unavailable in local play. In network\n"
		"play, aim holds its last valid hand direction through input.aim_loss_grace,\n"
		"then uses the head until the hand is tracked again (and before the\n"
		"first tracked pose); a seat uses stick aim. Anything else aims with the\n"
		"head." },
	{ "input.aim_loss_grace", _config_real, "0.5", "HALO_AIM_LOSS_GRACE", _environment_value, _platform_ios,
		"With input.aim = \"controller\": seconds of lost hand tracking during\n"
		"which the last hand aim is held, in local and network play, before the\n"
		"aim eases to the head over 0.2 seconds (0 to 5). A device profile sets\n"
		"it for the device; it is not a player choice." },
	{ "input.aim_hand", _config_string, "\"right\"", "HALO_AIM_HAND", _environment_value, _platform_ios,
		"With input.aim = \"controller\": the aiming hand, \"right\" (default) or\n"
		"\"left\" (the buttons mirror)." },
	{ "input.aim_pitch_offset", _config_real, "0.0", "HALO_AIM_PITCH_OFFSET", _environment_value, _platform_ios,
		"With input.aim = \"controller\": degrees the barrel ray and gun tilt down\n"
		"from the aim axis (-10 to 45), for aiming from the hip with the forearm\n"
		"low. With debug.sense_tuning_chord, hold Back (Create) and push the right\n"
		"stick or the D-pad up or down to change it by a degree for the session\n"
		"(the stick alone while\n"
		"debug.render_scale_dpad or debug.hud_distance_cycle has the D-pad)." },
	{ "input.aim_assist", _config_real, "1.0", "HALO_AIM_ASSIST", _environment_value, _platform_ios,
		"With input.aim = \"controller\": how much of the game's bullet autoaim a\n"
		"hand-aimed shot keeps, 0.0 to 2.0. It scales the cone around the aim\n"
		"inside which a shot bends onto a target and the reticle turns red: 1.0\n"
		"is the game's own for each weapon, 0.0 leaves every shot where the hand\n"
		"points. Single player only: a network or multiplayer game always plays\n"
		"at 1.0. With debug.sense_tuning_chord, hold Back (Create) and push the right stick right or left to\n"
		"change it by 0.5 for the session." },
	{ "input.aim_steady", _config_real, "0.0", "HALO_AIM_STEADY", _environment_value, _platform_ios,
		"With input.aim = \"controller\": how much a hand's tremor is smoothed\n"
		"while its aim is on a target (inside the autoaim cone, most where the\n"
		"reticle is red) and the weapon is not zoomed, 0.0 to 2.0: 0.0 none\n"
		"(default), 1.0 moderate, 2.0 strong. It lengthens the time a small move\n"
		"of the hand takes to reach the reticle; the aim is never pulled toward\n"
		"the target and a larger move arrives at once. Multiplied by\n"
		"input.aim_assist, so no assist means no steadying. Single player only.\n"
		"Zoomed, input.aim_steady_zoom_2x and input.aim_steady_zoom_8x apply.\n"
		"With debug.sense_tuning_chord, hold Back (Create) and click the right stick to cycle through these\n"
		"settings by zoom, then off, 1.0 and 2.0 at every zoom, for the session." },
	{ "input.aim_steady_zoom_2x", _config_real, "1.0", "HALO_AIM_STEADY_ZOOM_2X", _environment_value, _platform_ios,
		"As input.aim_steady, for a weapon zoomed up to 2x (default 1.0)." },
	{ "input.aim_steady_zoom_8x", _config_real, "2.0", "HALO_AIM_STEADY_ZOOM_8X", _environment_value, _platform_ios,
		"As input.aim_steady, for a weapon zoomed 8x or more (default 2.0);\n"
		"between 2x and 8x the strength is in proportion." },
	{ "input.controller_rumble", _config_real, "1.0", "HALO_CONTROLLER_RUMBLE", _environment_value, _platform_ios,
		"How strongly the game's rumble plays on a gamepad (0 to 2: 1 is the\n"
		"game's own strength, 0 is off). With input.aim = \"controller\" it plays\n"
		"on the Sense controllers: the pad's heavy left motor on the other hand's\n"
		"controller and its light right motor on the aiming hand's." },
	{ "input.blast_rumble", _config_boolean, "true", "HALO_BLAST_RUMBLE", _environment_value, _platform_all,
		"A blast that shakes the view but has no rumble of its own in the game's\n"
		"data (a grenade's shock wave beyond 7 m) rumbles the gamepad's heavy\n"
		"motor, at half the blast's strength at your distance for as long as\n"
		"the shake lasts. False keeps the original: no rumble from such blasts." },
	{ "input.laser", _config_string, "\"off\"", "HALO_LASER", _environment_value, _platform_ios,
		"HEAD mode with controller aim: a line from the gun's muzzle to the\n"
		"reticle. \"off\" (default), \"close\" (only while the reticle's surface\n"
		"is within input.laser_distance, where a hand held below the eyes makes\n"
		"the reticle slide down a surface as you walk up to it) or \"on\"\n"
		"(always). In any mode it shows while the gun's line passes something\n"
		"the eye's line doesn't. With debug.sense_tuning_chord, hold Back (Create) and pull the right trigger\n"
		"to cycle off, close and on for the session; the trigger doesn't fire\n"
		"while Back is held." },
	{ "input.reticle_ease", _config_real, "0.05", "HALO_RETICLE_EASE", _environment_value, _platform_ios,
		"HEAD mode with controller aim: the time, in seconds, over which the\n"
		"reticle moves along the gun's line when that line goes from a far\n"
		"surface onto a near one or back (0.0 to 0.3; default 0.05; 0.0 steps at\n"
		"once, as before). The gun is below your eyes, so its line meets a near\n"
		"enemy lower than the ground behind it, and the reticle stepped as it\n"
		"crossed the enemy's outline. The shots follow the eased reticle. Hold\n"
		"Back (Create) and press Y (Triangle) to switch it off and on." },
	{ "input.weapon_turn_limit", _config_real, "25.0", "HALO_WEAPON_TURN_LIMIT", _environment_value, _platform_ios,
		"input.first_person_arms \"limited\": the most the turned model turns across the\n"
		"view from its center, in degrees (0 to 90; 0 leaves the turn alone)." },
	{ "input.weapon_roll_limit", _config_real, "15.0", "HALO_WEAPON_ROLL_LIMIT", _environment_value, _platform_ios,
		"input.first_person_arms \"limited\": the most the turned model rolls, in degrees\n"
		"(0 to 90; 0 leaves the roll alone)." },
	{ "input.weapon_smoothing", _config_real, "0.08", "HALO_WEAPON_SMOOTHING", _environment_value, _platform_ios,
		"HEAD mode with controller aim: the time, in seconds, over which the\n"
		"first-person gun and arms follow the hand (0.0 to 0.5; default 0.08;\n"
		"0.0 follows it directly, as before). It takes the hand's tremor and the\n"
		"small steps of the aim point out of the model's motion; a larger move\n"
		"catches up faster. Only the drawn model is smoothed: the reticle and\n"
		"the shots follow the hand directly. With debug.sense_tuning_chord, hold Back (Create) and click the\n"
		"left stick to switch it off and on for the session." },
	{ "input.first_person_arms", _config_string, "\"solved\"", "HALO_FIRST_PERSON_ARMS", _environment_value, _platform_ios,
		"HEAD mode with controller aim: how the first-person gun and arms\n"
		"follow the hand. \"turned\": the whole model is turned onto the hand's\n"
		"line at the eyes (as before; a hand pointed to the side brings the\n"
		"arms in from the side of the view). \"limited\": the same, with the\n"
		"turn held within 25 degrees and the roll within 15, the model sinking\n"
		"out of view beyond. \"solved\" (default): the shoulders stay with the\n"
		"view, the gun turns at its grip onto the hand's line within\n"
		"input.arms_turn_limit, input.arms_up_limit, input.arms_down_limit and\n"
		"input.arms_roll_limit, the hands stay on it and the arms bend to\n"
		"follow. The reticle and the shots follow the hand in every mode.\n"
		"With debug.sense_tuning_chord, hold Back (Create) and push the left stick right or left to cycle the\n"
		"modes for the session." },
	{ "input.arms_follow", _config_real, "0.5", "HALO_ARMS_FOLLOW", _environment_value, _platform_ios,
		"HEAD mode with controller aim, input.first_person_arms \"solved\":\n"
		"how much of the hand's movement away from its usual place (its\n"
		"average over the last ten seconds) moves the gun, 0.0 (the gun stays\n"
		"where the animation puts it) to 1.0 (it moves with the hand, as far as\n"
		"the arm reaches); default 0.5. With debug.sense_tuning_chord, hold Back (Create) and press A (Cross)\n"
		"to switch between 0.0 and 0.5." },
	{ "input.arms_turn_limit", _config_real, "35", "HALO_ARMS_TURN_LIMIT", _environment_value, _platform_ios,
		"HEAD mode with controller aim, input.first_person_arms \"solved\":\n"
		"how far across the view, in degrees, the drawn gun turns with the\n"
		"hand (0 to 90; default 35). It follows the hand exactly to 70% of\n"
		"this and eases to it beyond; the reticle and the shots go on. With debug.sense_tuning_chord, hold\n"
		"Back (Create) and press X (Square) to switch between 35 and 45." },
	{ "input.arms_up_limit", _config_real, "40", "HALO_ARMS_UP_LIMIT", _environment_value, _platform_ios,
		"HEAD mode with controller aim, input.first_person_arms \"solved\":\n"
		"how far above the view's center, in degrees, the drawn gun turns\n"
		"(0 to 89; default 40), eased as input.arms_turn_limit is." },
	{ "input.arms_down_limit", _config_real, "50", "HALO_ARMS_DOWN_LIMIT", _environment_value, _platform_ios,
		"HEAD mode with controller aim, input.first_person_arms \"solved\":\n"
		"how far below the view's center, in degrees, the drawn gun turns\n"
		"(0 to 89; default 50), eased as input.arms_turn_limit is." },
	{ "input.arms_roll_limit", _config_real, "60", "HALO_ARMS_ROLL_LIMIT", _environment_value, _platform_ios,
		"HEAD mode with controller aim, input.first_person_arms \"solved\":\n"
		"how far the drawn gun rolls about its barrel with the hand, in degrees\n"
		"either way (0 to 180; default 60), eased as input.arms_turn_limit is;\n"
		"the forearm takes half of the wrist's twist." },
	{ "input.head_magnetism", _config_boolean, "false", "HALO_HEAD_MAGNETISM", _environment_set_is_true, _platform_ios,
		"HEAD mode: false (default), the game's aim magnetism never turns or\n"
		"slows the view (with a stick it drags the look after a target while\n"
		"you move, which in the headset turns the world by itself). true, the\n"
		"game's own magnetism while the head aims. The red reticle and the\n"
		"bullets' bend toward a target are the same either way." },
	{ "input.zoom_takeover", _config_boolean, "false", "HALO_ZOOM_TAKEOVER", _environment_set_is_true, _platform_ios,
		"HEAD mode with controller aim, zoomed: false (default), the reticle\n"
		"stays where the gun points, and when the gun points outside the zoomed\n"
		"picture there is no reticle; the shots follow the gun either way. true,\n"
		"the head takes the aim and the reticle jumps to the center as the gun's\n"
		"line leaves the part of the zoomed picture both eyes see." },
	{ "input.aim_origin_raise", _config_real, "0.0", "HALO_AIM_ORIGIN_RAISE", _environment_value, _platform_ios,
		"HEAD mode with controller aim: where the aim ray starts, from 0.0 (the\n"
		"hand, default) to 1.0 (the eyes), along the line between them; the ray\n"
		"keeps the hand's direction. A hand held below the eyes makes the reticle\n"
		"slide down a surface as you walk up to it; a raised origin slides less,\n"
		"and at 1.0 not at all, but the reticle then sits above where the\n"
		"controller itself points, by the height raised. The reticle, the shots,\n"
		"aim assist, the zoom and the laser all follow it. With debug.sense_tuning_chord, hold Back (Create)\n"
		"and pull the left trigger to raise it by 0.1, or press the left\n"
		"shoulder (the other hand's grip on a Sense controller) to lower it; the\n"
		"value is saved here." },
	{ "input.laser_distance", _config_real, "3.0", "HALO_LASER_DISTANCE", _environment_value, _platform_ios,
		"With input.laser = \"close\": how near the reticle's surface is, in\n"
		"meters from the eyes (1 to 20), for the line to show. It fades in over\n"
		"the last half meter inside this distance." },
	{ "debug.test_controller", _config_string, "\"\"", "HALO_TEST_CONTROLLER", _environment_value, _platform_ios,
		"A synthetic right Sense controller in place of the real ones, to check\n"
		"controller aim on the Mac and in the simulator: \"fixed:<yaw>,<pitch>\"\n"
		"holds it still in the room at those degrees from the room's forward\n"
		"(yaw right, pitch up positive; \"fixed:30,0\"), \"sweep\" turns it 30\n"
		"degrees each way over 4 s, and \"loss:<seconds>\" holds it at fixed:0,0\n"
		"but untracked for that long every 10 s, then orientation-only as long\n"
		"(up to 5). Empty for none; leave it empty on a headset." },
	{ "debug.controller_record", _config_string, "\"\"", "HALO_CONTROLLER_RECORD", _environment_value,
		_platform_ios,
		"Write every pose sample the host hands the game (both hands, the\n"
		"buttons, the head's yaw) to NAME.poses in the data folder (a\n"
		"headset's Documents folder), flushed once a second. Empty for none.\n"
		"Play it back with debug.controller_replay." },
	{ "debug.controller_replay", _config_string, "\"\"", "HALO_CONTROLLER_REPLAY", _environment_value,
		_platform_ios,
		"Answer the game's pose samples from NAME.poses (debug.controller_record)\n"
		"instead of the controllers or debug.test_controller, each kind in its\n"
		"own order, holding the last at the end; works on any host. The side-by-side\n"
		"view then aims with the replayed hand as it does for debug.test_controller.\n"
		"Empty for none." },
	{ "debug.controller_aim_axes", _config_string, "\"\"", "HALO_CONTROLLER_AIM_AXES", _environment_value,
		_platform_ios,
		"The Sense controller's aim axes as measured, \"<forward>,<up>\" (each\n"
		"one of +x, -x, +y, -y, +z and -z, on different lines; \"-z,+y\"), in\n"
		"place of the built-in ones; empty for the built-in ones. Anything else\n"
		"is logged once and the built-in axes stand." },
	{ "debug.controller_correction", _config_string, "\"none\"", "HALO_CONTROLLER_CORRECTION", _environment_value,
		_platform_ios,
		"The Sense controllers' pose correction from ARKit: \"none\" (default)\n"
		"or \"rendered\" (aligned with passthrough), to compare the two for lag\n"
		"and swim." },
	{ "debug.controller_axes", _config_boolean, "false", "HALO_CONTROLLER_AXES", _environment_set_is_true,
		_platform_ios,
		"HEAD mode with Sense controllers (visionOS 27): log the right\n"
		"controller's aim and grip axes in the head's frame twice a second\n"
		"(\"spatial: right aim x (...) y (...) z (...) grip ...\"), to measure\n"
		"which of the aim's axes points along the barrel." },
	{ "debug.aim_trace", _config_boolean, "false", "HALO_AIM_TRACE", _environment_set_is_true, _platform_ios,
		"Controller aim: log a line every drawn frame while a hand aims, with the\n"
		"sightline's and the first-person gun's yaw and pitch, the reticle's\n"
		"distance, the action's blend and gate and the magnification." },
	{ "debug.aim_loss_grace", _config_real, "-1.0", "HALO_AIM_LOSS_GRACE_DEBUG", _environment_value, _platform_ios,
		"Tuning input.aim_loss_grace live: from 0.0 up, the seconds of lost hand\n"
		"tracking that hold the last aim, in place of the device profile's. Negative\n"
		"uses the profile's." },
	{ "debug.texture_idle_seconds", _config_real, "-1.0", "HALO_TEXTURE_IDLE_SECONDS_DEBUG", _environment_value, _platform_all,
		"Tuning display.texture_idle_seconds live: from 10 up, the seconds a\n"
		"texture goes undrawn before the cache drops it, in place of the device\n"
		"profile's. Negative uses the profile's." },
	{ "debug.clutter_distance", _config_real, "-1.0", "HALO_CLUTTER_DISTANCE_DEBUG", _environment_value, _platform_all,
		"Tuning display.clutter_distance live: from 0 up, the distance in world units\n"
		"in place of the device profile's. Negative uses the profile's." },
	{ "debug.clutter_lowest_lod", _config_boolean, "false", "HALO_CLUTTER_LOWEST_LOD", _environment_value, _platform_all,
		"Draw the clutter display.clutter_distance names (dead bodies, loose weapons\n"
		"and equipment, garbage) at its lowest model detail level, wherever it is.\n"
		"Only the drawing changes." },
	{ "debug.offscreen_window_wall_seconds", _config_real, "60.0", "HALO_OFFSCREEN_WINDOW_WALL_SECONDS", _environment_value, _platform_all,
		"How many seconds of wall time an offscreen benchmark window may run before\n"
		"it is ended where it stands (the report's capped and measured_ticks), from 30\n"
		"to 1800. A signed catalog case may set it for itself. A slow device needs\n"
		"more than the default to finish a full window." },
	{ "debug.collision_type_filter", _config_boolean, "true", "HALO_COLLISION_TYPE_FILTER", _environment_value, _platform_all,
		"Collision rays and sphere queries skip an object whose type\n"
		"the query does not test before looking at it, instead of after: the same\n"
		"answers with less work in a crowded cluster. Read again each tick, so it\n"
		"can be flipped while the game runs." },
	{ "debug.sphere_query_type_filter", _config_boolean, "true", "HALO_SPHERE_QUERY_TYPE_FILTER", _environment_value, _platform_all,
		"The nearby-object queries (objects_in_sphere) skip an\n"
		"object whose type the caller did not ask for before collecting it,\n"
		"instead of after: the same answers with less work. Read again each tick,\n"
		"so it can be flipped while the game runs." },
	{ "debug.static_light_relink", _config_boolean, "false", "HALO_STATIC_LIGHT_RELINK", _environment_value, _platform_all,
		"An attached light that has not moved keeps its place in the\n"
		"map's light lists instead of leaving and joining them again every tick.\n"
		"Read again each tick, so it can be flipped while the game runs." },
	{ "debug.immediate_merge", _config_boolean, "true", "HALO_IMMEDIATE_MERGE", _environment_value, _platform_all,
		"Consecutive immediate-mode draws (the HUD's and the text's\n"
		"quads) in the same state are made as one draw, in the same order: the same\n"
		"pictures with fewer draws. Read again each tick, so it can be flipped\n"
		"while the game runs." },
	{ "debug.collision_candidates", _config_boolean, "true", "HALO_COLLISION_CANDIDATES", _environment_value, _platform_all,
		"Each cluster keeps a second list of its vehicles, scenery and other objects\n"
		"that are not bipeds, items or projectiles, and a collision ray that tests\n"
		"none of those three walks only that list: the same answers in the same\n"
		"order without stepping past every body and weapon in a crowded cluster.\n"
		"On by default; false walks every object. Read again each tick, so it can\n"
		"be flipped while the game runs." },
	{ "debug.aim_steady_level", _config_real, "-1.0", "HALO_AIM_STEADY_LEVEL", _environment_value, _platform_ios,
		"Testing input.aim_steady without an enemy: from 0.0 up, how firmly on a\n"
		"target the hand's aim counts as (1.0 a red reticle), in place of the\n"
		"game's own targeting. Negative uses the game's." },
	{ "display.theater_width", _config_real, "60.0", "HALO_THEATER_WIDTH", _environment_value, _platform_ios,
		"With display.immersive: how much of the view the screen spans, in degrees\n"
		"across (the headset's view is about 100; 60 is the front of a cinema).\n"
		"The picture is rendered as sharp as the headset can show at that size." },
	{ "display.theater_distance", _config_real, "4.0", "HALO_THEATER_DISTANCE", _environment_value, _platform_ios,
		"With display.immersive: how far away the screen stands, in meters." },
	{ "display.theater_environment", _config_string, "\"passthrough\"", "HALO_THEATER_ENVIRONMENT", _environment_value,
		_platform_ios,
		"With display.immersive: around the screen, \"passthrough\" (the room) or\n"
		"\"dark\" (black)." },
	{ "display.renderer", _config_string, "\"metal\"", "HALO_RENDERER", _environment_value, _platform_ios,
		"What draws the game: \"metal\" (default) Metal, or \"gl\" OpenGL ES, for\n"
		"debugging only: it lacks the Metal renderer's performance work and\n"
		"measures no GPU time. Changes apply on relaunch." },
	{ "display.frame_deadline", _config_boolean, "true", "HALO_FRAME_DEADLINE", _environment_set_is_true, _platform_ios,
		"Tell the system each frame's deadline (the frame rate's period), so the\n"
		"CPU keeps the clock that finishes frames in time while frames under a\n"
		"frame rate cap wait for the display; without it an A9X's clock fell to\n"
		"its lowest and frames ran late. Not on a Vision Pro, whose frames the\n"
		"system times. false leaves it off." },
	{ "display.frame_pacing", _config_string, "\"off\"", "HALO_FRAME_PACING", _environment_value, _platform_ios,
		"How the Metal renderer times frames: \"off\" (default) shows each frame as\n"
		"soon as it's drawn; \"tick\" shows every frame for the same whole number\n"
		"of display refreshes, at a rate that is a multiple of the game's 30 ticks\n"
		"a second (90 or 30 at 90 Hz, 60 or 30 at 60 Hz), dropping to the next\n"
		"rate when frames take too long; \"refresh\" allows any whole number of\n"
		"refreshes (45 at 90 Hz). Experimental: in a visionOS window the pacing\n"
		"can't yet see when frames show." },
	{ "display.stereo_flare_counting", _config_boolean, "false", "HALO_STEREO_FLARE_COUNTING", _environment_value, _platform_ios,
		"On a Vision Pro in stereo, dim each eye's lens flares by how much of the\n"
		"light that eye can see, as the flat screen does, each eye for itself.\n"
		"Off (default), a light either flares fully or not at all, the same in\n"
		"both eyes. With foveation, flares toward the edge of the view come out\n"
		"dimmer than they should; to be compared in the headset. Changes apply on\n"
		"relaunch." },
	{ "display.late_look", _config_boolean, "false", "HALO_LATE_LOOK", _environment_value, _platform_all,
		"Read the controller's look stick again just before a frame's camera is\n"
		"built and turn the first-person view by what it has done since the game\n"
		"read it, a few milliseconds on the frames that run a game tick. Only the\n"
		"picture moves; aim and shots are the game's as before. Off by default;\n"
		"F9 switches it while playing. Player one's controller, on foot." },
	{ "display.late_start", _config_string, "\"\"", "HALO_LATE_START", _environment_value, _platform_ios,
		"Start each frame's work as late as it can be and still be done for the\n"
		"next refresh of the display, so the controller is read later and what you\n"
		"do reaches the screen sooner (the Metal renderer's frame pacer). It backs\n"
		"off to starting at once whenever frames take too long or vary. \"true\" or\n"
		"\"false\"; empty (default): on in the Mac app, off on an iPhone, iPad or\n"
		"Apple TV. The Mac app's F8 switches it while playing. Not on a Vision Pro,\n"
		"and not when frames don't wait for the display (display.frame_rate)." },
	{ "display.frame_source", _config_string, "\"\"", "HALO_FRAME_SOURCE", _environment_value, _platform_ios,
		"Where the Metal renderer's frames get their place on the display:\n"
		"\"clock\" asks for one when a frame is ready and applies\n"
		"display.frames_in_flight and display.frame_spacing itself; \"system\"\n"
		"takes one from the system for each refresh (iOS 17, tvOS 17, macOS 14\n"
		"and later, and only with display.frame_rate = \"display\"; elsewhere\n"
		"\"clock\" is used). Empty (default): \"clock\". Not on a Vision Pro." },
	{ "display.frames_in_flight", _config_string, "\"\"", "HALO_FRAMES_IN_FLIGHT", _environment_value, _platform_ios,
		"How many frames the Metal renderer may have on their way to the display\n"
		"at once, \"1\" to \"3\": a frame isn't started on its way until fewer than\n"
		"that many are waiting to be shown. \"1\" gives the least delay between the\n"
		"game and the screen, and costs frames when a frame's CPU and GPU work\n"
		"together take longer than a refresh; \"system\" leaves the queue to the\n"
		"system (up to 3). Empty (default): \"2\". Not on a Vision Pro, which paces\n"
		"frames itself, and not for frames that don't wait for the display\n"
		"(display.frame_rate)." },
	{ "display.frame_spacing", _config_boolean, "true", "HALO_FRAME_SPACING", _environment_value, _platform_ios,
		"Keep each frame on screen for its share of the display's time before the\n"
		"next may replace it (the Metal renderer), so frames show evenly spaced:\n"
		"one refresh, or with a display.frame_rate cap under the display's rate,\n"
		"the cap's. Off with \"unlimited\" or a cap above the display's rate, where\n"
		"it would itself be a cap. Not on a Vision Pro." },
	{ "display.frame_rate", _config_string, "\"display\"", "HALO_FRAME_RATE", _environment_value, _platform_ios,
		"The most frames a second: \"display\" (default) draws one frame for each\n"
		"refresh of the display; a number (\"60\", \"240\") holds that rate;\n"
		"\"unlimited\" draws as fast as it can. In the Mac app, above the display's\n"
		"refresh rate and with \"unlimited\", frames no longer wait for the display,\n"
		"so the picture can tear and the Mac uses more power; each frame still\n"
		"waits for a free drawable from the window server. On iPhone, iPad and\n"
		"Apple TV a number under the display's highest rate runs the display at\n"
		"it (\"60\" on a 120 Hz screen); a higher number and \"unlimited\" keep the\n"
		"display's rate. The game still runs at 30 ticks a second whatever is\n"
		"drawn, and with display.interpolation off it draws 30 frames a second at\n"
		"most. A Vision Pro paces frames itself and ignores this." },
	{ "display.async_shaders", _config_string, "\"auto\"", "HALO_ASYNC_SHADERS", _environment_value, _platform_ios,
		"How the Metal renderer compiles shaders: \"auto\" (default) on worker\n"
		"threads, skipping a draw for the frames until its pipeline is ready, so\n"
		"the game never waits on the compiler (on Apple Vision Pro a long wait\n"
		"ends the app); a run that must draw the same frames every time (fixed\n"
		"timestep, an input replay or benchmark, debug.exit_after, screenshots)\n"
		"compiles each at its first draw instead. \"on\" or \"off\" forces either.\n"
		"Changes apply on relaunch." },
	{ "display.movies", _config_string, "\"auto\"", "HALO_MOVIES", _environment_value, _platform_ios,
		"The game's intro and credits movies (bink/intro.bik and bink/credits.bik\n"
		"beside maps/, imported from the disc image with the maps): \"auto\"\n"
		"(default) plays them, except in a run that must draw the same frames\n"
		"every time (fixed timestep, an input replay or benchmark,\n"
		"debug.exit_after, screenshots); \"on\" plays them there too; \"off\" skips\n"
		"them. A build without the movie decoder always skips them." },
	{ "display.vsync", _config_boolean, "true", "HALO_NO_VSYNC", _environment_set_is_false, _platform_all,
		"Wait for the display between frames; false draws as fast as possible." },
	{ "display.max_fps", _config_integer, "0", "HALO_MAX_FPS", _environment_value, _platform_desktop,
		"With vsync off, the most frames a second: 0 for twice the display's\n"
		"refresh rate, -1 for no limit (which can hang some Intel graphics)." },
	{ "display.interpolation", _config_boolean, "true", "HALO_INTERPOLATION", _environment_value, _platform_all,
		"Draw a frame for every display refresh, blending between the game's 30\n"
		"ticks a second; false keeps the original 30 frames a second." },
	{ "display.direct_camera", _config_boolean, "true", "HALO_DIRECT_CAMERA", _environment_value, _platform_all,
		"In first person, point the view where the player aims now instead of\n"
		"where the last tick left it: the view turns the frame the stick or the\n"
		"mouse moves, not up to two ticks (66 ms) later." },
	{ "display.high_res_hud", _config_boolean, "true", "HALO_HIGH_RES_HUD", _environment_value, _platform_all,
		"Draw the HUD (meters, counters, panels, motion sensor, reticles,\n"
		"waypoints, scopes) from the high-res assets (8x the maps' bitmaps);\n"
		"false draws the maps' own bitmaps." },
	{ "display.high_res_text", _config_boolean, "true", "HALO_HIGH_RES_TEXT", _environment_value, _platform_all,
		"Draw the menus' and HUD's text with the fonts in port/assets/fonts\n"
		"(Overpass) at the resolution the game draws at, and the menus' titles\n"
		"from port/assets/titles; false draws the maps' bitmap fonts and titles." },
	{ "display.upscaled_textures", _config_boolean, "true", "HALO_UPSCALED_TEXTURES", _environment_value, _platform_all,
		"Draw upscaled textures (made on this device from your copy of the game)\n"
		"where they exist; false draws the maps' own. A device profile that sets\n"
		"false turns them off whatever this says. Takes effect at the next\n"
		"level load." },
	{ "display.upscaled_textures_memory", _config_integer, "0", "HALO_UPSCALED_TEXTURES_MEMORY", _environment_value,
		_platform_all,
		"The most memory the upscaled textures may take, in MB; 0 (default) picks\n"
		"it from the device's memory. Past it, the ones drawn longest ago make\n"
		"room, and a texture with no room is drawn as the map has it." },
	{ "display.menus", _config_string, "\"pc\"", "HALO_MENUS", _environment_value, _platform_all,
		"The menus: \"pc\" for the PC version's main menu (port/assets/menus,\n"
		"and a menus folder here for your own), \"xbox\" for the Xbox's." },
	{ "display.player_names", _config_string, "\"none\"", "HALO_PLAYER_NAMES", _environment_value, _platform_all,
		"In multiplayer, whose names are drawn above their heads: \"none\" (the\n"
		"Xbox's: the name of the player under the reticle shows, and teammates\n"
		"have the game's marker), \"all\", \"allies\" or \"enemies\". An enemy's shows only within the\n"
		"motion sensor's reach, in sight and not camouflaged; none show if the\n"
		"gametype's motion tracker shows no players, only allies' if it shows\n"
		"only friends." },
	{ "display.player_name_scale", _config_real, "1.0", "HALO_PLAYER_NAME_SCALE", _environment_value, _platform_all,
		"How large the players' names are drawn: 1.0 three quarters of the size of\n"
		"the HUD's text, 0.25 to 4." },
	{ "display.scoreboard_team_layout", _config_string, "\"teams\"", "HALO_SCOREBOARD_TEAM_LAYOUT", _environment_value,
		_platform_all,
		"How the scoreboard lists a team game's players: \"teams\" in a column for\n"
		"each team (red on the left, blue on the right), \"score\" all in order of\n"
		"score." },
	{ "display.scoreboard_background", _config_boolean, "true", "HALO_SCOREBOARD_BACKGROUND", _environment_value,
		_platform_all,
		"Draw a panel behind the multiplayer scoreboard, for clearer text." },
	{ "display.scoreboard_background_color", _config_string, "\"16, 16, 16, 150\"", "HALO_SCOREBOARD_BACKGROUND_COLOR",
		_environment_value, _platform_all,
		"The scoreboard panel's colour: \"red, green, blue, alpha\", each 0 to 255\n"
		"(alpha 0 is see-through, 255 solid)." },
	{ "streamer.enabled", _config_boolean, "false", "HALO_STREAMER_MODE", _environment_value, _platform_all,
		"Hide offensive words in names and text from other players, maps, and mods as stars." },
	{ "streamer.slurs", _config_string, "\"mild\"", "HALO_STREAMER_SLURS", _environment_value, _platform_all,
		"With streamer.enabled, hide slurs at the selected severity: \"off\", \"severe\",\n"
		"\"strong\", or \"mild\" (all)." },
	{ "streamer.swearing", _config_string, "\"strong\"", "HALO_STREAMER_SWEARING", _environment_value, _platform_all,
		"With streamer.enabled, hide swearing at the selected severity: \"off\",\n"
		"\"severe\", \"strong\", or \"mild\" (all)." },
	{ "streamer.insults", _config_string, "\"off\"", "HALO_STREAMER_INSULTS", _environment_value, _platform_all,
		"With streamer.enabled, hide insults at the selected severity: \"off\",\n"
		"\"severe\", \"strong\", or \"mild\" (all)." },

	{ "audio.enabled", _config_boolean, "true", "HALO_NO_AUDIO", _environment_set_is_false, _platform_all,
		"Play sound." },
	{ "audio.volume", _config_real, "1.0", "HALO_VOLUME", _environment_value, _platform_all,
		"The volume of everything, 0.0 to 1.0." },
	{ "audio.music_volume", _config_real, "1.0", "HALO_MUSIC_VOLUME", _environment_value, _platform_all,
		"The music's volume, 0.0 to 1.0 (of audio.volume)." },
	{ "audio.effects_volume", _config_real, "1.0", "HALO_EFFECTS_VOLUME", _environment_value, _platform_all,
		"The volume of every other sound (effects and speech), 0.0 to 1.0 (of\n"
		"audio.volume)." },
	{ "audio.loose_sounds", _config_boolean, "false", "HALO_LOOSE_SOUNDS", _environment_value, _platform_all,
		"For those making sounds: play each of a map's sounds that has a Halo PC\n"
		"sound tag file of its name under the data root's tags folder\n"
		"(tags/sound/.../name.sound) from that file. At the console,\n"
		"loose_sounds_reload reads the files again and loose_sounds false gives\n"
		"the map's sounds back." },

	{ "input.stick_dead_zone", _config_real, "0.27", "HALO_STICK_DEAD_ZONE", _environment_value, _platform_all,
		"How far a controller stick moves before it counts, from 0 to 1. At 0.27\n"
		"(the Xbox's) the game's own square dead zone applies; lower values make a\n"
		"round one of that size (0.1 suits most modern controllers)." },
	{ "input.mouse_sensitivity", _config_real, "1.0", "HALO_MOUSE_SENSITIVITY", _environment_value, _platform_desktop,
		"How far the view turns for the mouse's movement." },
	{ "input.invert_mouse", _config_boolean, "false", "HALO_MOUSE_INVERT", _environment_set_is_true, _platform_desktop,
		"Moving the mouse forward looks down." },
	{ "network.block_mouse_aim_assist", _config_boolean, "false", "HALO_NET_BLOCK_MOUSE_AIM_ASSIST", _environment_value,
		_platform_all, "Host: disable optional mouse magnetism for mouse/keyboard players. Controllers and touch keep their own assist." },
	{ "input.mouse_aim_assist", _config_boolean, "false", "HALO_MOUSE_AIM_ASSIST", _environment_value, _platform_desktop,
		"Magnetism while aiming with the mouse, as with a controller: the view\n"
		"slowed and dragged along by a target. The last of the mouse and the\n"
		"right stick to move decides. The bullets' autoaim (bent toward the\n"
		"target) stays either way." },
	{ "input.mouse_vertical_sensitivity", _config_real, "0.0", "HALO_MOUSE_VERTICAL_SENSITIVITY", _environment_value,
		_platform_desktop,
		"How far the view turns up and down for the mouse's movement; 0 for the\n"
		"same as input.mouse_sensitivity." },

	/* the keyboard and mouse's own controls (port/linux/src/xinput_sdl.c) */
	{ "controls.move_forward", _config_string, "\"W\"", "HALO_KEY_MOVE_FORWARD", _environment_value, _platform_all,
		"The keyboard and mouse's controls, which Settings > Controls Setup\n"
		"changes: up to two keys or buttons each, separated by a comma. Keys by\n"
		"their names (\"W\", \"Space\", \"Left Ctrl\", \"F1\"), and \"Mouse Left\",\n"
		"\"Mouse Right\", \"Mouse Middle\", \"Mouse 4\", \"Mouse 5\", \"Wheel\" (either\n"
		"way), \"Wheel Up\" and \"Wheel Down\"; empty for none. Moving forward:" },
	{ "controls.move_backward", _config_string, "\"S\"", "HALO_KEY_MOVE_BACKWARD", _environment_value, _platform_all,
		"Moving backward." },
	{ "controls.strafe_left", _config_string, "\"A\"", "HALO_KEY_STRAFE_LEFT", _environment_value, _platform_all,
		"Moving left." },
	{ "controls.strafe_right", _config_string, "\"D\"", "HALO_KEY_STRAFE_RIGHT", _environment_value, _platform_all,
		"Moving right." },
	{ "controls.jump", _config_string, "\"Space\"", "HALO_KEY_JUMP", _environment_value, _platform_all,
		"Jumping (and skipping cutscenes)." },
	{ "controls.crouch", _config_string, "\"Left Ctrl, C\"", "HALO_KEY_CROUCH", _environment_value, _platform_all,
		"Crouching." },
	{ "controls.fire", _config_string, "\"Mouse Left\"", "HALO_KEY_FIRE", _environment_value, _platform_all,
		"Firing." },
	{ "controls.throw_grenade", _config_string, "\"Mouse Right, G\"", "HALO_KEY_THROW_GRENADE", _environment_value,
		_platform_all,
		"Throwing a grenade." },
	{ "controls.melee", _config_string, "\"F, Mouse 4\"", "HALO_KEY_MELEE", _environment_value, _platform_all,
		"Melee attack." },
	{ "controls.reload", _config_string, "\"R\"", "HALO_KEY_RELOAD", _environment_value, _platform_all,
		"Reloading." },
	{ "controls.zoom", _config_string, "\"Z, Mouse Middle\"", "HALO_KEY_ZOOM", _environment_value, _platform_all,
		"Zooming the scope." },
	{ "controls.switch_weapon", _config_string, "\"Wheel, 1\"", "HALO_KEY_SWITCH_WEAPON", _environment_value,
		_platform_all,
		"Switching weapons." },
	{ "controls.switch_grenade", _config_string, "\"X\"", "HALO_KEY_SWITCH_GRENADE", _environment_value, _platform_all,
		"Switching grenades." },
	{ "controls.action", _config_string, "\"E\"", "HALO_KEY_ACTION", _environment_value, _platform_all,
		"The action: picking up (held: swapping weapons), entering and leaving\n"
		"vehicles, pressing switches; never reloading (the controller's X does\n"
		"when there is nothing to act on)." },
	{ "controls.flashlight", _config_string, "\"Q\"", "HALO_KEY_FLASHLIGHT", _environment_value, _platform_all,
		"The flashlight." },
	{ "controls.scoreboard", _config_string, "\"Tab\"", "HALO_KEY_SCOREBOARD", _environment_value, _platform_all,
		"Showing the scores (the controller's Back)." },
	{ "controls.pause", _config_string, "\"Escape\"", "HALO_KEY_PAUSE", _environment_value, _platform_all,
		"The pause menu (the controller's Start)." },

	{ "game.console_log", _config_string, "\"important\"", "HALO_CONSOLE_LOG", _environment_value, _platform_all,
		"What the game's console shows on screen of what it logs: \"important\"\n"
		"(bans, players dropped for cheating, what refuses a command, and the\n"
		"asserts that stop the game), \"all\" (every line, the game's own\n"
		"chatter too), or \"none\" (the asserts that stop the game only). What\n"
		"a command prints shows whatever this is, and debug.txt has every line." },

	{ "game.language", _config_string, "\"\"", "HALO_LANGUAGE", _environment_value, _platform_all,
		"The language the game asks the Xbox for: \"ja\", \"de\", \"fr\", \"es\" or \"it\";\n"
		"empty for English. The game data decides what is translated." },
	{ "game.loading_screen", _config_string, "\"auto\"", "HALO_LOADING_SCREEN", _environment_value, _platform_all,
		"The loading screen over a campaign level's load: the last frame blurring\n"
		"away, the ring's picture lit from left to right as the load goes, and\n"
		"its tone, until the level is read and its shaders are compiled. \"auto\"\n"
		"(default) shows it except in a run that must draw the same frames every\n"
		"time (fixed timestep, an input replay or benchmark, debug.exit_after,\n"
		"screenshots) and except in stereo; \"on\" or \"off\" forces either. The\n"
		"picture is your disc's: a maps folder imported before the importer took\n"
		"it gets it from tools/loading_picture.py." },
	{ "game.loading_screen_seconds", _config_real, "1.5", "HALO_LOADING_SCREEN_SECONDS", _environment_value, _platform_all,
		"The least time the loading screen stays up, in seconds (0 to 30), so a\n"
		"level that is ready at once doesn't flash it." },
	{ "game.enhanced_animations", _config_boolean, "true", "HALO_ENHANCED_ANIMATIONS", _environment_value, _platform_all,
		"The player bipeds' grenade throws keep their legs moving (crouched,\n"
		"in the air and in a vehicle's seat too), riders' hands leave the grips\n"
		"to throw and reload, and a player turns with the aim while throwing;\n"
		"false: the original animations, which freeze the legs and stand a\n"
		"rider up." },
	{ "game.custom_edition", _config_boolean, "true", "HALO_CUSTOM_EDITION", _environment_value, _platform_all,
		"Load and run Halo Custom Edition maps (not those that need OpenSauce):\n"
		"put them and Custom Edition's bitmaps.map, sounds.map and loc.map in\n"
		"the custom_maps folder beside the maps folder; the map lists show them\n"
		"as CUSTOM SINGLEPLAYER and CUSTOM MULTIPLAYER. Their tags are checked\n"
		"as the game's own maps' are before they run; false refuses them\n"
		"(docs/custom_edition_caches.md)." },
	{ "display.first_person_legs", _config_boolean, "false", "HALO_FIRST_PERSON_LEGS", _environment_value, _platform_all,
		"Your own legs in first person, seen looking down: your body as others\n"
		"see it, drawn from the waist down, moving as it moves. On a flat view (a\n"
		"phone, a tablet, a window) only: a stereo view ignores it, and\n"
		"display.first_person_body draws the whole body in head-tracked stereo. No\n"
		"menu row yet." },
	{ "display.first_person_legs_seat_shift", _config_real, "1.0", "HALO_FIRST_PERSON_LEGS_SEAT_SHIFT", _environment_value,
		_platform_all,
		"With display.first_person_legs, in a vehicle seat seen through your own\n"
		"eyes (a Warthog's passenger seat): how far your seated body is drawn\n"
		"moved ahead so the waist sits under the view, 0.0 (where the game seats\n"
		"it, well behind the eye) to 1.0 (toward the eye, default), never more\n"
		"than about 0.37 m so the knees stay short of the dash." },

	{ "paths.data", _config_string, "\"\"", "HALO_DATA_ROOT", _environment_value, _platform_desktop,
		"The folder holding the game data's maps folder; empty looks in the\n"
		"working directory and its assets folder. Windows paths are easiest in\n"
		"single quotes: 'C:\\Games\\Halo'." },
	{ "paths.saves", _config_string, "\"\"", "HALO_SAVE_ROOT", _environment_value, _platform_desktop,
		"Where saved games and profiles go; empty for the usual place\n"
		"(~/.local/share/halo-linux, or %APPDATA%\\halo on Windows)." },
	{ "paths.custom_edition", _config_string, "\"\"", "HALO_CUSTOM_EDITION_ROOT", _environment_value, _platform_desktop,
		"A Halo Custom Edition install whose maps folder is looked in after the\n"
		"custom_maps folder for Custom Edition maps and their bitmaps.map,\n"
		"sounds.map and loc.map (game.custom_edition); empty for none." },

	{ "network.address", _config_string, "\"\"", "HALO_NET_ADDRESS", _environment_value, _platform_all,
		"This machine's IPv4 address for system link, for a machine on several\n"
		"networks; empty chooses one." },
	{ "network.broadcast", _config_string, "\"\"", "HALO_NET_BROADCAST", _environment_value, _platform_all,
		"Comma-separated IPv4 addresses system link sends its announcements to\n"
		"instead of the local network's broadcast address (for VPNs); empty for\n"
		"the local network." },
	{ "network.lan_broadcast", _config_string, "\"auto\"", "HALO_NET_LAN_BROADCAST", _environment_value, _platform_ios,
		"System link by broadcast, which finds games of every system: \"auto\" where\n"
		"this device may (the Mac; an iPhone, iPad or Vision Pro only with Apple's\n"
		"multicast entitlement), \"on\" or \"off\". Bonjour finds other Apple devices\n"
		"either way." },
	{ "network.online", _config_boolean, DEFAULT_INTERNET_PLAY, "HALO_NET_ONLINE", _environment_value, _platform_all,
		"Internet play: hosting makes an invite link (logged, and put on the\n"
		"clipboard) that lets whoever has it join over the internet; opening a\n"
		"link (or copying one before switching to the game) joins. Only people\n"
		"with the invite can join. Off keeps system link to the local network." },
	{ "network.online_ask", _config_boolean, DEFAULT_ONLINE_ASK, "HALO_NET_ONLINE_ASK", _environment_value, _platform_ios,
		"While network.online is false: ask whether to turn Internet Play on the\n"
		"first time Create Game > Internet, Join Game > Server Browser or Direct\n"
		"Link is opened, saying which servers it reaches and what they see. Asked\n"
		"once: either answer, and a join link's question, writes false here." },
	{ "network.join_from_clipboard", _config_boolean, DEFAULT_CLIPBOARD_JOIN, "HALO_NET_JOIN_FROM_CLIPBOARD", _environment_value,
		_platform_all,
		"Join the game of an invite link found on the clipboard when the game\n"
		"comes to the front." },
	{ "network.tunnel_port", _config_integer, "0", "HALO_NET_TUNNEL_PORT", _environment_value, _platform_all,
		"The UDP port internet play uses; 0 picks one. A fixed one can be\n"
		"forwarded on the router, for networks whose NAT stops connections." },
	{ "network.allow_upnp", _config_boolean, "true", "HALO_NET_ALLOW_UPNP", _environment_value, _platform_all,
		"Let internet play ask the router (UPnP; on Apple devices NAT-PMP or\n"
		"PCP) to forward its port, for networks whose NAT stops connections:\n"
		"when a player joins this machine's game, and when joining a game takes\n"
		"too long. False never asks." },
	{ "network.background_grace", _config_integer, "60", "HALO_NET_BACKGROUND_GRACE", _environment_value, _platform_all,
		"Seconds the other players of an internet game wait for this machine\n"
		"when its app goes to the background (an Apple device suspends it, its\n"
		"game held) before they drop it." },
	{ "network.public_lobby", _config_boolean, "true", "HALO_NET_PUBLIC_LOBBY", _environment_value, _platform_all,
		"The server browser: public games are listed through the signalling\n"
		"brokers, and Join Game > Server Browser shows them. False lists no\n"
		"game of this machine's and shows none." },
	{ "network.delta_browse", _config_boolean, "true", "HALO_NET_DELTA_BROWSE", _environment_value, _platform_all,
		"The server browser also lists the games of Delta's public list\n"
		"(network.delta_url), run by others, with the network version of each;\n"
		"games of a version this build can't join are shown and not joined. Only\n"
		"while the server browser is open with network.online and\n"
		"network.public_lobby on. It asks the list for its games every 15\n"
		"seconds and sends nothing else: the list's server sees this machine's\n"
		"internet address, as the signalling brokers do." },
	{ "network.delta_announce", _config_boolean, "false", "HALO_NET_DELTA_ANNOUNCE", _environment_value, _platform_all,
		"List the public game this machine hosts for the internet (Create Game >\n"
		"Internet, PUBLIC, no password) in Delta's public list\n"
		"(network.delta_url), so that players using it find it. Off by default:\n"
		"it sends the list's server, run by others, the game's invite (anyone who\n"
		"has it can join), name, map, game type number, player counts, whether\n"
		"it is open and has teams, this build's network version and that it\n"
		"plays the game's own protocol alone, and the server sees this machine's\n"
		"internet address. The game is withdrawn when it ends or goes private." },
	{ "network.delta_url", _config_string, "\"https://halo.milenko.org\"", "HALO_NET_DELTA_URL", _environment_value,
		_platform_all,
		"The address of Delta's public game list, whose /v1/games the server\n"
		"browser reads (network.delta_browse) and whose /v1/announce a hosted\n"
		"game is listed at (network.delta_announce). Empty turns both off." },
	{ "network.host_public", _config_boolean, DEFAULT_HOST_PUBLIC, "HALO_NET_HOST_PUBLIC", _environment_value, _platform_all,
		"Whether a new game of Create Game > Internet starts as PUBLIC (listed\n"
		"in everyone's server browser: anyone can see and join it) or, false,\n"
		"PRIVATE (only players with its invite link can join). Server Setup's\n"
		"LISTING changes it for each game." },
	{ "network.player_capacity", _config_integer, "0", "HALO_NET_PLAYER_CAPACITY", _environment_value, _platform_all,
		"The most players this device holds at 30 frames a second at the lowest\n"
		"settings, 2 to 128; 0 (default) names no limit. Create Game starts\n"
		"MAXIMUM PLAYERS at the largest choice within it (the player can raise it,\n"
		"and Server Setup says the device may run slowly), and the server browser\n"
		"marks the games with more. Joining a game is never refused. A device\n"
		"profile sets it for the device's speed; it is not a player choice." },
	{ "network.map_share", _config_boolean, "true", "HALO_NET_MAP_SHARE", _environment_value, _platform_all,
		"Hosting: send a player who joins without the game's Custom Edition map\n"
		"the map (from custom_maps), and the resource maps it needs that the\n"
		"player lacks (bitmaps.map, sounds.map, loc.map). The game's other\n"
		"players help (network.map_seed)." },
	{ "network.map_share_upload", _config_integer, "1024", "HALO_NET_MAP_SHARE_UPLOAD", _environment_value,
		_platform_all,
		"Hosting: the most kilobytes a second the host sends of maps (16 to\n"
		"65536), so that the game's own traffic keeps the rest of the upload." },
	{ "network.map_seed", _config_boolean, DEFAULT_MAP_SEED, "HALO_NET_MAP_SEED", _environment_value, _platform_all,
		"Playing in another's game: help send its map to players who join\n"
		"without it, as a torrent's peers do (the pieces this machine has, at\n"
		"most network.map_seed_upload). While it does, in a game on a Custom\n"
		"Edition map, this machine takes connections on TCP port 5153. Off by\n"
		"default on Apple builds." },
	{ "network.map_seed_upload", _config_integer, "256", "HALO_NET_MAP_SEED_UPLOAD", _environment_value,
		_platform_all,
		"Playing in another's game: the most kilobytes a second this machine\n"
		"sends of maps to other players (16 to 65536)." },
	{ "network.custom_maps", _config_boolean, "false", "HALO_NET_CUSTOM_MAPS", _environment_value, _platform_all,
		"Experimental: the server browser also shows games on Custom Edition\n"
		"maps, those of a map in custom_maps or, with network.map_download on,\n"
		"any. Off by default: it shows only the games on the game's own maps.\n"
		"Games on a map pack's maps are never shown. Joining still checks that\n"
		"the map is the host's version." },
	{ "network.map_download", _config_boolean, DEFAULT_MAP_DOWNLOAD, "HALO_NET_MAP_DOWNLOAD", _environment_value,
		_platform_all,
		"Joining a game on a Custom Edition map this machine does not have:\n"
		"download it (and the resource maps it needs, up to about 2.3 GB) from\n"
		"the game's players into custom_maps, if its host shares it\n"
		"(network.map_share); false leaves the game, saying which map is missing\n"
		"and that downloads are off. Off by default on Apple builds. It governs\n"
		"receiving only: sending maps to others is network.map_share (a host)\n"
		"and network.map_seed (a player in another's game)." },
	{ "network.coop_public", _config_boolean, "false", "HALO_NET_COOP_PUBLIC", _environment_value, _platform_all,
		"Whether an online co-op game (Create Game > Internet, a SINGLEPLAYER\n"
		"map) starts as PUBLIC or, false, PRIVATE: Server Setup's LISTING in\n"
		"co-op, which writes its choice here." },
	{ "network.coop_friendly_fire", _config_string, "\"on\"", "HALO_NET_COOP_FRIENDLY_FIRE", _environment_value,
		_platform_all,
		"Whether the players of an online co-op game hurt each other: \"off\",\n"
		"\"on\", \"shields_only\" or \"explosives_only\" (Server Setup's FRIENDLY\n"
		"FIRE in co-op, which writes its choice here). Damage to AI allies is\n"
		"controlled separately by network.coop_ally_friendly_fire." },
	{ "network.coop_own_friendly_fire", _config_boolean, "true", "HALO_NET_COOP_OWN_FRIENDLY_FIRE",
		_environment_value, _platform_all,
		"Whether this machine's players hurt, and are hurt by, the other players\n"
		"of an online co-op game: two players hurt each other only if both have\n"
		"it true. A player away from the controls for over 70 seconds is hurt by\n"
		"every player, whatever either chose." },
	{ "network.coop_ally_friendly_fire", _config_boolean, "true", "HALO_NET_COOP_ALLY_FRIENDLY_FIRE", _environment_value,
		_platform_all,
		"Whether players can hurt friendly non-player units and vehicles in\n"
		"online co-op. Server Setup's ALLY FRIENDLY FIRE writes its choice\n"
		"here; false protects allies without changing player-to-player damage,\n"
		"enemy damage, AI-on-AI damage or scripted deaths." },
	{ "network.coop_player_collisions", _config_boolean, "true", "HALO_NET_COOP_PLAYER_COLLISIONS", _environment_value,
		_platform_all,
		"Whether the players of an online co-op game bump into each other;\n"
		"false, they walk through each other (the AI's characters they still\n"
		"bump into). Server Setup's PLAYER COLLISIONS in co-op writes its\n"
		"choice here." },
	{ "network.coop_enemies_mode", _config_string, "\"per_player\"", "HALO_NET_COOP_ENEMIES_MODE", _environment_value,
		_platform_all,
		"Online co-op's extra enemies: \"none\", \"per_player\" (each squad of\n"
		"enemies grows by coop_enemies for each player past the first) or\n"
		"\"multiplier\" (each is coop_enemies_multiplier times as large, for any\n"
		"number of players). Server Setup's EXTRA ENEMIES in co-op writes its\n"
		"choice here." },
	{ "network.coop_enemies", _config_integer, "50", "HALO_NET_COOP_ENEMIES", _environment_value, _platform_all,
		"Online co-op's extra enemies per player, a percentage: for each player\n"
		"past the first, each squad of enemies a level places gets this much of\n"
		"itself more (100: as many again; 25 to 200). Server Setup's PER PLAYER\n"
		"in co-op writes its choice here." },
	{ "network.coop_enemies_multiplier", _config_integer, "2", "HALO_NET_COOP_ENEMIES_MULTIPLIER", _environment_value,
		_platform_all,
		"Online co-op's static multiplier of its enemies: each squad of enemies\n"
		"a level places is this many times as large (2 to 32). Server Setup's\n"
		"MULTIPLIER in co-op writes its choice here." },
	{ "network.brokers_file", _config_string, "\"brokers.txt\"",
		"HALO_NET_BROKERS_FILE", _environment_value, _platform_all,
		"The file of the public MQTT brokers through which the machines of an\n"
		"invite find each other (its messages are encrypted), beside this file\n"
		"unless a full path: one host:port on each line, up to 4. Updates\n"
		"replace brokers.txt: keep a list of your own under another name." },
	{ "network.stun_servers", _config_string, "\"stun.l.google.com:19302,stun.cloudflare.com:3478\"",
		"HALO_NET_STUN", _environment_value, _platform_all,
		"Public STUN servers that tell this machine its internet address;\n"
		"comma-separated host:port." },
	{ "discord.application_id", _config_string, "\"1553978809840050229\"", "HALO_DISCORD_APPLICATION",
		_environment_value, _platform_desktop,
		"The Discord application internet play invites go through while the\n"
		"Discord desktop client runs; empty for none." },

	{ "update.auto", _config_boolean, "true", "HALO_UPDATE_AUTO", _environment_value, _platform_desktop,
		"Look for a new version when the game starts, and offer to update to it;\n"
		"false never looks (the game's \"Do not ask again\" writes false here)." },

	{ "debug.external_mode", _config_string, "\"\"", "HALO_EXTERNAL_MODE", _environment_value, _platform_ios,
		"Force an external display mode for testing: preferred, WIDTHxHEIGHT, or a comma-separated list.\n"
		"Hold Back and press the D-pad's up or down to cycle the list live." },

	{ "debug.output_height", _config_integer, "0", "HALO_OUTPUT_HEIGHT", _environment_value, _platform_ios,
		"Cap the built-in display's drawable at this many lines for measuring (0, the\n"
		"default: the device's own size, or an Apple TV profile's display.output_height);\n"
		"the system scales it to the panel. iPhone, iPad and Apple TV with Metal, read at launch." },
	{ "debug.render_scale_dpad", _config_boolean, "false", "HALO_RENDER_SCALE_DPAD", _environment_value, _platform_ios,
		"While Back is held, the D-pad's left and right step the render scale\n"
		"(display.render_scale) down and up by 0.05 as the game runs, shown on\n"
		"screen for two seconds; the game doesn't see the D-pad meanwhile." },
	{ "debug.knob_cycle", _config_boolean, "false", "HALO_KNOB_CYCLE", _environment_value, _platform_ios,
		"While Back is held, the D-pad's up and down choose a quality setting\n"
		"(render height, shadow map size, anisotropic filtering, model detail,\n"
		"anti-aliasing, effect resolution, mirror resolution) and left and right\n"
		"step its value as the game runs, from the next frame, shown on screen for\n"
		"two seconds and logged as \"knob: <setting>=<value>\". The game doesn't see\n"
		"the D-pad meanwhile, and debug.render_scale_dpad, debug.external_mode and\n"
		"debug.sense_tuning_chord's D-pad step get none of its presses. Not saved." },
	{ "debug.sense_tuning_chord", _config_boolean, "false", "HALO_SENSE_TUNING_CHORD", _environment_value, _platform_ios,
		"While a hand aims and Back (Create) is held, controller buttons change aim and\n"
		"arms tuning as the game runs (laser mode, aim assist, steadying, pitch offset,\n"
		"origin raise, reticle easing, arms, gun smoothing), and the game sees neither\n"
		"stick meanwhile. Off, Back with those buttons is ordinary game input." },
	{ "debug.halt_screen", _config_boolean, "false", "HALO_HALT_SCREEN", _environment_value, _platform_ios,
		"On a fatal game error (a failed assertion or a halt), stop on the game's\n"
		"halt screen, its recent messages on a blue background, instead of crashing.\n"
		"Off, the app crashes, and ios-runtime.log and the crash report have the error\n"
		"and the game's frames (tools/guest_symbolicate.py reads the report)." },
	{ "debug.crash_test", _config_string, "\"\"", "HALO_CRASH_TEST", _environment_value, _platform_ios,
		"Fail on purpose 150 frames after the game starts, to check crash reports:\n"
		"\"halt\" (a failed assertion), \"fault\" (a memory fault in game code), \"trap\"\n"
		"(a trap instruction) or \"abort\" (the game runtime's fatal error); \"map\" fails\n"
		"the first level load instead, which shows a message; empty, never." },
	{ "debug.upscaled_textures_toggle", _config_boolean, "false", "HALO_UPSCALED_TEXTURES_TOGGLE", _environment_value,
		_platform_ios,
		"While Back is held, the D-pad's up switches all of the level's textures\n"
		"between upscaled (and debug.texture_override_directory's) and original,\n"
		"shown on screen for two seconds; each level starts upscaled, and\n"
		"display.upscaled_textures is not changed. The game doesn't see the D-pad\n"
		"meanwhile." },
	{ "debug.upscaled_textures_toggle_every", _config_integer, "0", "HALO_UPSCALED_TEXTURES_TOGGLE_EVERY",
		_environment_value, _platform_ios,
		"With upscaled_textures_toggle: press its toggle every this many frames,\n"
		"for a test run with no controller; 0 never." },
	{ "debug.input_record", _config_string, "\"\"", "HALO_INPUT_RECORD", _environment_value, _platform_all,
		"Record controller 1 while a map plays, to <name>.input in the data folder\n"
		"(port/linux/src/input_replay.c); empty records nothing." },
	{ "debug.input_replay", _config_string, "\"\"", "HALO_INPUT_REPLAY", _environment_value, _platform_all,
		"Replay a recording made with debug.input_record in place of controller 1,\n"
		"at the same moments of the game time." },
	{ "debug.input_replay_route", _config_integer, "0", "HALO_INPUT_REPLAY_ROUTE", _environment_value, _platform_all,
		"With debug.input_replay or debug.input_record: log player 1's position\n"
		"every this many ticks, with the segment, the tick and whether the tick\n"
		"took a recorded action (tools/replay_check.py reads it); 0 never." },
	{ "debug.enemy_multiplier", _config_real, "1.0", "HALO_ENEMY_MULTIPLIER", _environment_value, _platform_all,
		"A single-player campaign's enemies, each squad this many times as large\n"
		"(1.0 off; 1.5 gives every second enemy a companion; up to 8). Extra\n"
		"enemies stand around the squad's starting locations as in online co-op.\n"
		"A take recorded with it carries the value, and its replay uses that one." },
	{ "debug.scenario_help", _config_boolean, "true", "HALO_SCENARIO_HELP", _environment_value, _platform_all,
		"Show the in-game help dialogs a level's scripts raise (taking a Scorpion's or a Banshee's seat), which\n"
		"stop the game until a button is pressed. Automatic takes and the replays of takes recorded that way show\n"
		"none whatever this says. False shows none anywhere." },
	{ "debug.profile_controls", _config_boolean, "false", "HALO_PROFILE_CONTROLS", _environment_value, _platform_all,
		"A level started without the menu (the console's map_name) takes the controls of the last profile\n"
		"used: look sensitivity, inverted look, button and stick layout. The profile file is not written.\n"
		"Off, such a level uses the default controls." },
	{ "debug.benchmark", _config_boolean, "false", "HALO_BENCHMARK", _environment_value, _platform_all,
		"With debug.input_replay: time every frame of the replay, write\n"
		"benchmark-<name>-<time>.txt and quit when the replay ends." },
	{ "debug.benchmark_lip_sync", _config_boolean, "false", "HALO_BENCHMARK_LIP_SYNC", _environment_value, _platform_all,
		"Use simulated, tick-driven mouths while recording or replaying benchmark takes.\n"
		"False keeps the original audio-driven mouths; true alone does not change normal play.\n"
		"Benchmark suites opt in for their replays without saving this setting." },
	{ "debug.benchmark_suite", _config_string, "\"\"", "HALO_BENCHMARK_SUITE", _environment_value, _platform_ios,
		"\"quick\" or \"full\" runs the device benchmark and quits: the suite's\n"
		"scenes, several times over at different settings, with a report in\n"
		"the data folder's benchmark folder (port/linux/src/benchmark_suite.c);\n"
		"empty for none." },
	{ "debug.benchmark_offscreen", _config_boolean, "false", "HALO_BENCHMARK_OFFSCREEN", _environment_set_is_true, _platform_ios,
		"With debug.benchmark_suite set at launch: draw each frame of the suite\n"
		"into a texture that is never shown, with no wait for the display, one\n"
		"1/30 s tick a frame (debug.fixed_timestep), as fast as the device goes.\n"
		"A \"full-unified\" suite then ends with a short presented run of its\n"
		"first campaign and first multiplayer case, in real time, as a check.\n"
		"On in tester builds for \"full-unified\". Ignored without a suite and\n"
		"on Vision Pro." },
	{ "debug.benchmark_offscreen_mode", _config_string, "\"serialized\"", "HALO_BENCHMARK_OFFSCREEN_MODE", _environment_value,
		_platform_ios,
		"With debug.benchmark_offscreen, how the offscreen runs' frames go:\n"
		"\"serialized\" (default) waits for each lens flare's visibility answer, so\n"
		"the CPU and the GPU take turns and the CPU time counts the GPU's;\n"
		"\"pipelined\" takes every answer from three frames before, the same every\n"
		"run, so frames overlap as they do on screen: frames a second near\n"
		"1/max(CPU, GPU), for comparing settings' costs, not for frame rate tiers." },
	{ "debug.benchmark_virtual_display", _config_boolean, "false", "HALO_BENCHMARK_VIRTUAL_DISPLAY", _environment_set_is_true,
		_platform_ios,
		"With debug.benchmark_offscreen: every run of the suite is drawn\n"
		"offscreen in real time against a virtual display: refreshes at the\n"
		"display's rate (or display.frame_rate's cap), three virtual drawables, each\n"
		"held on the virtual screen until the next frame replaces it, and the same\n"
		"frame pacer and frame timing as a shown run. Visibility answers come from\n"
		"three frames before." },
	{ "debug.master_list", _config_boolean, "true", "HALO_MASTER_LIST", _environment_value, _platform_all,
		"Include the official OpenCE master's signed games in the internet browser.\n"
		"Fetch on open or refresh, at most once every five seconds. Listed hosts\n"
		"are contacted only when joining; the browser shows their region." },
	{ "debug.network_test", _config_string, "\"\"", "HALO_NETWORK_TEST", _environment_value, _platform_all,
		"Automated system link sessions for testing (port/linux/game/network_test.c):\n"
		"\"host:<map>\" hosts a game on that map, \"join\" joins the first game found;\n"
		"\"browse-only\" logs public games without joining; empty for none." },
	{ "debug.multiplayer_benchmark_probe", _config_boolean, "false", "HALO_MULTIPLAYER_BENCHMARK_PROBE",
		_environment_value, _platform_all,
		"In a four-local-player automated Chill Out Slayer match, draw only player\n"
		"1's view and drive the controllers with distinct game-tick scripts.\n"
		"This provisional probe is off in ordinary play." },
	{ "debug.network_test_start", _config_real, "15.0", "HALO_NETWORK_TEST_START", _environment_value, _platform_all,
		"Seconds after hosting that an automated test game starts." },
	{ "debug.network_test_players", _config_integer, "1", "HALO_NETWORK_TEST_PLAYERS", _environment_value, _platform_all,
		"The players an automated test host has on its own computer (split screen):\n"
		"controller 1 and the next ones, which debug.test_input plays when no gamepad\n"
		"is connected to them." },
	{ "debug.network_test_kill", _config_real, "0.0", "HALO_NETWORK_TEST_KILL", _environment_value, _platform_all,
		"Every this many seconds an automated test host kills its last player; 0 never." },
	{ "debug.network_test_score", _config_integer, "0", "HALO_NETWORK_TEST_SCORE", _environment_value, _platform_all,
		"The score an automated test host's game type plays to (a short game, to\n"
		"test the next); 0 the game type's own." },
	{ "debug.network_test_shoot", _config_real, "0.0", "HALO_NETWORK_TEST_SHOOT", _environment_value, _platform_all,
		"Every this many seconds each automated test player hits the next with\n"
		"their weapon, within its reach (the host brings far players near the\n"
		"first a second before); 0 never." },
	{ "debug.network_test_vehicle", _config_real, "0.0", "HALO_NETWORK_TEST_VEHICLE", _environment_value, _platform_all,
		"This many seconds into an automated test game the host seats its last\n"
		"player as a vehicle's driver (and out 15 seconds on); 0 never." },
	{ "debug.network_test_pickup", _config_real, "0.0", "HALO_NETWORK_TEST_PICKUP", _environment_value, _platform_all,
		"This many seconds into an automated test game the host stands its last\n"
		"player on a weapon, which a joining player then picks up; 0 never." },
	{ "debug.network_test_pickup_weapon", _config_string, "\"\"", "HALO_NETWORK_TEST_PICKUP_WEAPON", _environment_value,
		_platform_all,
		"The weapon network_test_pickup stands the player on: the first whose tag\n"
		"name has this in it (\"sniper\", say); empty any." },
	{ "debug.network_test_public", _config_boolean, "false", "HALO_NETWORK_TEST_PUBLIC", _environment_value,
		_platform_all,
		"The network test's host lists its game in the server browser; browse joins it." },
	{ "debug.telnet_console", _config_boolean, "false", "HALO_TELNET_CONSOLE", _environment_set_is_true, _platform_all,
		"Listen on 127.0.0.1 (port telnet_console_port) for a script console that\n"
		"runs what it is sent as the game's console does, with no password; false\n"
		"none." },
	{ "debug.telnet_console_port", _config_integer, "2323", "HALO_TELNET_CONSOLE_PORT", _environment_value,
		_platform_all,
		"The port of the script console (telnet_console); the Xbox's was 23, which\n"
		"only the administrator can listen on." },
	{ "debug.network_latency", _config_real, "0.0", "HALO_NETWORK_LATENCY", _environment_value, _platform_all,
		"Milliseconds everything received is held back (a round trip between two\n"
		"machines of twice it), to test the netcode as over the internet; 0 none." },
	{ "debug.network_loss", _config_real, "0.0", "HALO_NETWORK_LOSS", _environment_value, _platform_all,
		"Percent of datagrams received that are dropped, for the same; 0 none." },
	{ "debug.network_corrupt", _config_real, "0.0", "HALO_NETWORK_CORRUPT", _environment_value, _platform_all,
		"Percent of the datagrams received that are damaged at random, to test\n"
		"that nothing a machine sends can crash the game; 0 none." },
	{ "debug.network_corrupt_stream", _config_real, "0.0", "HALO_NETWORK_CORRUPT_STREAM", _environment_value,
		_platform_all,
		"Percent of the reads of streams that are damaged at random, for the\n"
		"same (a damaged stream is closed, so a little goes a long way); 0 none." },
	{ "debug.network_corrupt_after", _config_real, "0.0", "HALO_NETWORK_CORRUPT_AFTER", _environment_value,
		_platform_all,
		"Seconds after the start before anything is damaged, so that a game can\n"
		"be set up and started first (a host's messages to its own client are\n"
		"damaged too)." },
	{ "debug.test_input", _config_string, "\"\"", "HALO_TEST_INPUT", _environment_value, _platform_all,
		"\"bot:<seed>\" plays controller 1 with a scripted pattern (automated\n"
		"network tests); \"look:<seed>\" stands still, only turning and looking\n"
		"up and down; \"walklook:<seed>\" looks down, then walks;\n"
		"\"zoom:<start>,<swaps>,<clicks>[,<lights>[,<unclicks>]]\" stands still and\n"
		"from poll <start> swaps weapons <swaps> times, zooms <clicks> times,\n"
		"presses the flashlight <lights> times (night vision), then clicks the\n"
		"zoom <unclicks> times more (out of it); \"aachord:<start>,<period>\"\n"
		"presses the live anti-aliasing chord (Back and R1) from poll <start> and\n"
		"every <period> polls after; empty for none." },
	{ "debug.side_by_side_screen", _config_boolean, "false", "HALO_SIDE_BY_SIDE_SCREEN", _environment_set_is_true,
		_platform_ios,
		"With display.stereo = \"side_by_side\": show the eyes SCREEN mode's\n"
		"gameplay would have (a 3D TV on the default screen, 64 mm eyes) in place\n"
		"of head-tracked ones." },
	{ "debug.side_by_side_tangents", _config_string, "\"\"", "HALO_SIDE_BY_SIDE_TANGENTS", _environment_value,
		_platform_ios,
		"With display.stereo = \"side_by_side\": the left eye's frustum as\n"
		"\"left,right,up,down\" tangents (the right eye's mirrored left to right),\n"
		"for asymmetric eyes like the headset's; empty for 0.8 on every side." },
	{ "debug.screen_lean", _config_real, "0.0", "HALO_SCREEN_LEAN", _environment_value, _platform_ios,
		"With debug.side_by_side_screen: a fixed lean of the head, in meters to the\n"
		"right (up to 0.25), as if you leaned in front of the screen." },
	{ "debug.cutscene_immersive", _config_boolean, "false", "HALO_CUTSCENE_IMMERSIVE", _environment_set_is_true,
		_platform_ios,
		"A spike: in HEAD mode (and the side-by-side view), a cutscene's third-person\n"
		"film frame renders immersive instead of on the 3D-film screen: the eyes\n"
		"are the cutscene camera turned by the head, the director's 16:9 frame is\n"
		"anchored in the room where the head pointed as the cutscene began, and\n"
		"outside it the picture is blurred, darkened and desaturated." },
	{ "debug.cutscene_immersive_min_fov", _config_real, "40.0", "HALO_CUTSCENE_IMMERSIVE_MIN_FOV", _environment_value,
		_platform_ios,
		"With debug.cutscene_immersive: the narrowest shot that renders immersive,\n"
		"as the camera's horizontal field of view in degrees (70 for the game's\n"
		"default lens; the director's frame spans that across its width). A\n"
		"narrower (telephoto) shot falls back to the 3D-film screen." },
	{ "debug.cutscene_outside_dim", _config_real, "0.6", "HALO_CUTSCENE_OUTSIDE_DIM", _environment_value,
		_platform_ios,
		"With debug.cutscene_immersive: how much the blurred picture outside the\n"
		"director's frame is darkened and desaturated, 0 (not at all) to 1\n"
		"(black)." },
	{ "debug.side_by_side_head_yaw", _config_string, "\"\"", "HALO_SIDE_BY_SIDE_HEAD_YAW", _environment_value,
		_platform_ios,
		"With display.stereo = \"side_by_side\" and debug.cutscene_immersive: a head\n"
		"that turns side to side in the immersive cutscene, as \"amplitude,period\"\n"
		"(degrees, seconds), so the Mac shows the room-anchored frame; empty for\n"
		"none." },
	{ "debug.head_yaw_log", _config_boolean, "false", "HALO_HEAD_YAW_LOG", _environment_set_is_true, _platform_ios,
		"HEAD mode: log a line each frame while the head turns (\"stereo: head\n"
		"yaw\"): the head's yaw from the frame's predicted pose, the yaw the eye\n"
		"cameras used, and how far the world moved against the head since the\n"
		"last line, to see whether the world stays put while the head turns." },
	{ "debug.head_sweep", _config_string, "\"\"", "HALO_HEAD_SWEEP", _environment_value, _platform_ios,
		"HEAD mode: turn the head by itself, left and right in a sine, as\n"
		"\"amplitude_degrees,period_seconds\" (\"30,4\"), in place of the yaw\n"
		"ARKit gives, to check the view's tracking in the simulator; empty for\n"
		"none. It overrides the device's head yaw too, so leave it empty on a\n"
		"headset." },
	{ "debug.foveation_eye_passes", _config_boolean, "true", "HALO_FOVEATION_EYE_PASSES", _environment_value,
		_platform_ios,
		"With display.foveation: false renders the game's eyes unfoveated at the\n"
		"foveated view's full size, and only the last step that fills each view\n"
		"goes through the Compositor's rate map (composite-only foveation, the\n"
		"reference the full path is compared against)." },
	{ "debug.anti_aliasing_folded", _config_boolean, "false", "HALO_ANTI_ALIASING_FOLDED", _environment_value, _platform_all,
		"With display.anti_aliasing = \"fxaa\" and the HUD at the display's size:\n"
		"run FXAA inside the stretch to the display's size (Metal), at each of\n"
		"the display's pixels, rather than as a pass over the scene before it.\n"
		"It saves a copy and a pass but does four times the FXAA work at half\n"
		"scale: on the Apple TV HD it cost 15 ms a frame against the pass's 6." },
	{ "debug.rate_map_test", _config_boolean, "false", "HALO_RATE_MAP_TEST", _environment_value, _platform_ios,
		"Metal renderer on the Mac: give the screen-sized targets a synthetic,\n"
		"lopsided rate map (full rate in the middle third, half outside it, denser\n"
		"at the top than the bottom), resolved at present, to check foveated\n"
		"rendering off the headset." },
	{ "debug.update_answer", _config_string, "\"\"", "HALO_UPDATE_ANSWER", _environment_value, _platform_desktop,
		"The answer to the new version question, for automated tests: \"yes\",\n"
		"\"no\" or \"never\" (do not ask again, confirmed); empty asks." },
	{ "debug.link_consent_answer", _config_string, "\"\"", "HALO_LINK_CONSENT_ANSWER", _environment_value, _platform_ios,
		"The answer to \"Join this game over the internet?\" and to \"Turn on\n"
		"Internet Play?\" for automated tests: \"join\" (Join, Turn On) or\n"
		"\"cancel\"; empty asks." },
	{ "debug.exit_after", _config_real, "0.0", "HALO_EXIT_AFTER", _environment_value, _platform_all,
		"Quit this many seconds after the window opens (of game time under\n"
		"debug.fixed_timestep); 0 never." },
	{ "debug.init_after", _config_real, "0.0", "HALO_INIT_AFTER", _environment_value, _platform_all,
		"Run init.txt's commands this many seconds after start-up instead of at\n"
		"once, so a test can wait at the main menu and then start a level\n"
		"('map_name a10') in the same launch; 0 at once." },
	{ "debug.benchmark_window_marker", _config_boolean, "false", "HALO_BENCHMARK_WINDOW_MARKER", _environment_set_is_true, _platform_all,
		"Show the measured benchmark window and a five-second countdown onscreen." },
	{ "debug.benchmark_review_take", _config_boolean, "false", "HALO_BENCHMARK_REVIEW_TAKE", _environment_set_is_true, _platform_all,
		"In an onscreen benchmark replay, play the rest of the recorded input after the measured window closes. Log take position every ten seconds." },
	{ "debug.fixed_timestep", _config_boolean, "false", "HALO_FIXED_TIMESTEP", _environment_set_is_true, _platform_all,
		"Advance the game's clock 1/30 s per frame, however long the frame took, so\n"
		"two runs show the same frames; debug.exit_after then counts seconds of\n"
		"game time, 30 frames each (600 = 18,000 frames)." },
	{ "debug.hidden_window", _config_boolean, "false", "HALO_HIDDEN_WINDOW", _environment_set_is_true, _platform_desktop,
		"Keep the window hidden (and never fullscreen)." },
	{ "debug.null_renderer", _config_boolean, "false", "HALO_NULL_RENDERER", _environment_set_is_true, _platform_all,
		"Run without a window, drawing nothing." },
	{ "debug.fixed_timestep_paced", _config_boolean, "false", "HALO_FIXED_TIMESTEP_PACED", _environment_set_is_true, _platform_all,
		"With fixed_timestep, hold each frame for at least 1/30 s, so the game\n"
		"plays at its own speed instead of as fast as frames are presented." },
	{ "debug.frame_trace", _config_boolean, "false", "HALO_FRAME_TRACE", _environment_set_is_true, _platform_all,
		"Log every frame's time, the blend between ticks and the camera's yaw,\n"
		"to see how smoothly the view turns." },
	{ "debug.present_times", _config_boolean, "false", "HALO_PRESENT_TIMES", _environment_set_is_true, _platform_ios,
		"Log when each frame reached the display and how long after its work began\n"
		"(the frame pacer, with the Metal renderer; not a Vision Pro or a\n"
		"simulator), to see how evenly and how late frames are shown." },
	{ "debug.frame_cpu_probe", _config_boolean, "false", "HALO_FRAME_CPU_PROBE", _environment_set_is_true, _platform_ios,
		"Benchmark reports also carry, for every frame, the drawing thread's own\n"
		"CPU time, the whole app's, and how long a fixed loop took (which falls\n"
		"as the core's clock rises), to tell a slow clock or another thread's\n"
		"work from waiting." },
	{ "debug.visibility_any_sample", _config_boolean, "false", "HALO_VISIBILITY_ANY_SAMPLE", _environment_set_is_true, _platform_ios,
		"The Metal renderer's visibility tests (lens flares) only say whether\n"
		"anything was visible, as before they counted how much: a light that is\n"
		"mostly hidden then flares at full strength. For comparing." },
	{ "debug.frame_load_cpu", _config_real, "0.0", "HALO_FRAME_LOAD_CPU", _environment_value, _platform_ios,
		"Milliseconds of made-up CPU work added to every frame (the frame pacer),\n"
		"to see what display.frames_in_flight does when frames don't fit in a\n"
		"refresh. 0 (default): none." },
	{ "debug.frame_load_every", _config_real, "0.0", "HALO_FRAME_LOAD_EVERY", _environment_value, _platform_ios,
		"With debug.frame_load_cpu: the made-up work comes and goes, on in every\n"
		"other span of this many seconds, as a fight would. 0 (default): always." },
	{ "debug.low_power_frame_rate", _config_string, "\"true\"", "HALO_LOW_POWER_FRAME_RATE", _environment_value, _platform_ios,
		"In Low Power Mode, gameplay and cutscenes aim for half the frame rate\n"
		"they aim for otherwise (120 to 60, 60 to 30, never under 30) and menus\n"
		"keep the display's pace: \"true\" (default); \"false\" keeps the rate in\n"
		"Low Power Mode; \"always\" acts as if Low Power Mode were on, for testing.\n"
		"Not on a Vision Pro." },
	{ "debug.frame_load_gpu", _config_integer, "0", "HALO_FRAME_LOAD_GPU", _environment_value, _platform_ios,
		"Made-up GPU work added to every frame: that many extra full-size draws\n"
		"(the Metal renderer), with debug.frame_load_cpu. 0 (default): none." },
	{ "debug.frame_counter", _config_boolean, "false", "HALO_FRAME_COUNTER", _environment_set_is_true, _platform_ios,
		"Show the frame number and the game time in a corner of the screen, to\n"
		"point at a moment; with debug.fixed_timestep the same frame is the same\n"
		"moment in every run. Not in screenshots." },
	{ "debug.terminal_on_screen", _config_boolean, "false", "HALO_TERMINAL_ON_SCREEN", _environment_set_is_true, _platform_ios,
		"Draw the console's output (what print and commands print) over the\n"
		"game, as the desktop builds do; false keeps it in the log only." },
	{ "debug.test_extra_scene", _config_boolean, "false", "HALO_TEST_EXTRA_SCENE", _environment_set_is_true, _platform_ios,
		"Ask for a second window scene a few seconds in, to check that the game\n"
		"ignores its start, keeps its event pump and closes the scene (the\n"
		"host's host_main.m). For the simulator." },
	{ "debug.test_reclaim_sockets", _config_boolean, "false", "HALO_TEST_RECLAIM_SOCKETS", _environment_set_is_true,
		_platform_ios,
		"On coming back from the background, make every socket of the game's\n"
		"again and break its TCP connections, as an Apple device does to a\n"
		"suspended app (the host's host_socket_revival.c); for the simulator,\n"
		"whose apps keep their sockets." },
	{ "debug.test_text_answer", _config_string, "\"\"", "HALO_TEST_TEXT_ANSWER", _environment_value, _platform_ios,
		"On an Apple TV, answer the dialog a menu's text field asks with (a join\n"
		"code, a password) with this text, 3 seconds after it comes up, as if it\n"
		"were typed and Done pressed (the host's host_tv.m); empty for none. For\n"
		"the simulator, whose keyboard cannot type into it." },
	{ "debug.test_theater_reopen", _config_boolean, "false", "HALO_TEST_THEATER_REOPEN", _environment_set_is_true, _platform_ios,
		"With display.immersive: close the immersive space a few seconds in, then\n"
		"press the window's button that opens it again (the host's\n"
		"host_theater.m). For the simulator." },
	{ "debug.gl_debug", _config_boolean, "false", "HALO_GL_DEBUG", _environment_set_is_true, _platform_all,
		"Report OpenGL errors in the log." },
	{ "debug.render_validation", _config_string, "\"log\"", "HALO_RENDER_VALIDATION", _environment_value, _platform_ios,
		"In a build made with render validation (tools/ios_build.py\n"
		"--render-validation; no other build has any of it): every call into the\n"
		"GPU backend is checked before it is made, and every place the renderer\n"
		"substitutes, ignores or skips something is named and counted in the log.\n"
		"\"off\", \"log\" (each distinct cause once, with the draw it first happened\n"
		"in, and a summary with counts) or \"strict\" (the first failure or fallback\n"
		"ends the run with exit status 3)." },
	{ "debug.render_capture", _config_string, "\"\"", "HALO_RENDER_CAPTURE", _environment_value, _platform_ios,
		"In a build made with render validation: a folder (in the data folder\n"
		"unless a full path) where the frames named by render_capture_frames are\n"
		"written as the calls the game made into the GPU backend, one file a\n"
		"window, for tools/render_replay.py to replay without the game. The files\n"
		"hold the game's own textures and models: keep them out of any repository." },
	{ "debug.render_capture_frames", _config_string, "\"\"", "HALO_RENDER_CAPTURE_FRAMES", _environment_value, _platform_ios,
		"The windows render_capture writes: \"600+30,1200+30\" is 30 frames from\n"
		"frame 600 and 30 from frame 1200." },
	{ "debug.render_validation_allow", _config_string, "\"\"", "HALO_RENDER_VALIDATION_ALLOW", _environment_value, _platform_ios,
		"With debug.render_validation = \"strict\": reasons the run goes on past,\n"
		"separated by commas (\"fill-mode-ignored,sampler-unbound\")." },
	{ "debug.metal_state_cache", _config_boolean, "true", "HALO_METAL_STATE_CACHE", _environment_value, _platform_ios,
		"With the Metal renderer, skip the encoder calls that set what the\n"
		"encoder already holds; false makes every call, to compare." },
	{ "debug.shader_list_warm", _config_boolean, "true", "HALO_SHADER_LIST_WARM", _environment_value, _platform_ios,
		"Compile the shaders and pipelines in a map's shader list while it loads;\n"
		"false compiles each when a frame first draws with it, to compare." },
	{ "debug.shader_list_record", _config_string, "\"shader-lists-missed\"", "HALO_SHADER_LIST_RECORD", _environment_value, _platform_ios,
		"With the Metal renderer, a folder (in the data folder unless a full path)\n"
		"where the shaders and pipelines made while drawing, those a map's shader\n"
		"list missed, are appended to MAP.txt, each line once; \"\" for none.\n"
		"tools/shader_lists.py merges them into port/shader-lists." },
	{ "debug.metal_pipeline_archive", _config_boolean, "false", "HALO_METAL_PIPELINE_ARCHIVE", _environment_value, _platform_ios,
		"With the Metal renderer, keep the compiled pipelines in an archive in the\n"
		"app's caches between launches. Off until writing it works: in the Mac app\n"
		"it fails with \"cannot create temporary file\" (Metal's own shader cache\n"
		"still keeps compiled shaders between launches)." },
	{ "debug.metal_specialize", _config_boolean, "true", "HALO_METAL_SPECIALIZE", _environment_value, _platform_ios,
		"With the Metal renderer, specialize each pipeline's vertex shader for its\n"
		"attribute formats; false uses the unspecialized shaders, which read the\n"
		"formats per draw (the fallback when specializing fails), to compare." },
	{ "debug.metal_vertex_bgra", _config_boolean, "true", "HALO_METAL_VERTEX_BGRA", _environment_value, _platform_ios,
		"With the Metal renderer, the vertex shaders read vertex colors in the\n"
		"game's own byte order (BGRA); false swaps the bytes of every colored\n"
		"vertex on the CPU as it is uploaded, as the OpenGL ES renderer has to, to\n"
		"compare." },
	{ "debug.metal_base_vertex", _config_boolean, "true", "HALO_METAL_BASE_VERTEX", _environment_value, _platform_ios,
		"With the Metal renderer, indexed draws take a base vertex, so their\n"
		"indices are drawn as the game holds them; false copies and rebases every\n"
		"draw's indices on the CPU, as the OpenGL ES renderer has to, to compare." },
	{ "debug.menu_open", _config_string, "\"\"", "HALO_MENU_OPEN", _environment_value, _platform_all,
		"Start on this screen of the menus (port/assets/menus) instead of the main\n"
		"menu, a player profile being edited; empty for the main menu." },
	{ "debug.gpu_flush_draws", _config_integer, "-1", "HALO_GPU_FLUSH_DRAWS", _environment_value, _platform_desktop,
		"Flush the GPU's pipeline every this many draws: -1 for every 3 on Intel\n"
		"graphics with Mesa's driver (which can hang without), 0 never." },
	{ "debug.dynamic_resolution_shadow", _config_boolean, "false", "HALO_DYNAMIC_RESOLUTION_SHADOW", _environment_value, _platform_ios,
		"With the Metal renderer, log and record the dynamic resolution planner's\n"
		"choices without changing the rendered resolution." },
	{ "debug.dynamic_resolution_lines", _config_integer, "0", "HALO_DYNAMIC_RESOLUTION_LINES", _environment_value, _platform_ios,
		"Turn dynamic resolution on and hold the 3D view at this many lines inside\n"
		"its ceiling's targets (from its floor to its ceiling), for comparisons; 0\n"
		"for the planner's choice. Read at launch." },
	{ "debug.dynamic_resolution_validate", _config_boolean, "false", "HALO_DYNAMIC_RESOLUTION_VALIDATE", _environment_value, _platform_ios,
		"Turn dynamic resolution on, also in benchmark runs, where a case's lines\n"
		"are the ceiling: the per-model validation run. Read at launch." },
	{ "debug.dynamic_resolution_sweep", _config_boolean, "false", "HALO_DYNAMIC_RESOLUTION_SWEEP", _environment_value, _platform_ios,
		"Turn dynamic resolution on and step the 3D view from its ceiling to its\n"
		"floor and back, a level every 30 frames, in place of the planner. Read at\n"
		"launch." },
	{ "debug.gpu_stats", _config_boolean, "false", "HALO_GPU_STATS", _environment_set_is_true, _platform_all,
		"Log the renderer's draw counts once a second." },
	{ "debug.gpu_trace_frame", _config_integer, "-1", "HALO_GPU_TRACE", _environment_value, _platform_all,
		"Log every draw of this frame; -1 none." },
	{ "debug.gpu_trace_constants", _config_boolean, "false", "HALO_GPU_TRACE_CONSTANTS", _environment_set_is_true, _platform_all,
		"With gpu_trace_frame, also the vertex shader constants." },
	{ "debug.gpu_skip_vertex_shaders", _config_string, "\"\"", "HALO_GPU_SKIP_VS", _environment_value, _platform_all,
		"Comma-separated ids of vertex shaders not to draw with." },
	{ "debug.gpu_dump_shaders", _config_string, "\"\"", "HALO_GPU_DUMP_SHADERS", _environment_value, _platform_all,
		"A folder to write the generated shaders (GLSL or MSL) to; empty none." },
	{ "debug.gpu_shader_replay", _config_string, "\"\"", "HALO_GPU_SHADER_REPLAY", _environment_value, _platform_all,
		"A folder of shader inputs (.vsh, .key) that gpu_dump_shaders recorded:\n"
		"translate each at startup into its replay folder; empty none." },
	{ "debug.gpu_shader_replay_dialect", _config_string, "\"\"", "HALO_GPU_SHADER_REPLAY_DIALECT", _environment_value, _platform_all,
		"The dialect gpu_shader_replay translates into: empty the context's, \"450\" desktop GLSL,\n"
		"\"msl\" Metal (which the Metal renderer also compiles)." },
	{ "debug.gpu_debug_expression", _config_string, "\"\"", "HALO_GPU_DEBUG_EXPR", _environment_value, _platform_all,
		"A GLSL expression every pixel shader shows instead of its result." },
	{ "debug.gpu_debug_texture0", _config_boolean, "false", "HALO_GPU_DEBUG_T0", _environment_set_is_true, _platform_all,
		"Pixel shaders show their first texture." },
	{ "debug.gpu_debug_flat", _config_boolean, "false", "HALO_GPU_DEBUG_FLAT", _environment_set_is_true, _platform_all,
		"Pixel shaders show their vertex colour." },
	{ "debug.companion_capture", _config_boolean, "false", "HALO_COMPANION_CAPTURE", _environment_set_is_true, _platform_all,
		"Draw the companion HUD without an external display and save it with screenshots." },
	{ "debug.screenshot_directory", _config_string, "\"\"", "HALO_SCREENSHOT_DIR", _environment_value, _platform_all,
		"A folder to save frames to (with screenshot_every); empty none." },
	{ "debug.shadow_blur", _config_string, "\"footprint\"", "HALO_SHADOW_BLUR", _environment_value, _platform_all,
		"How object shadows larger than 128 texels (display.shadow_map_size) are blurred: \"footprint\" over\n"
		"the area the game's blur covers at 128, \"xbox\" the game's taps at their fixed offsets (at 512 they\n"
		"fall 4 texels apart and show as separate copies). 128-texel shadows always get the game's blur." },
	{ "debug.screenshot_every", _config_integer, "0", "HALO_SCREENSHOT_EVERY", _environment_value, _platform_all,
		"Save every this many frames to screenshot_directory; 0 none." },
	{ "debug.texture_astc", _config_string, "\"auto\"", "HALO_TEXTURE_ASTC", _environment_value, _platform_ios,
		"DXT-to-ASTC texture preparation: \"auto\" (default) converts on GPUs without BC\n"
		"once the release enables it, \"force\" emulates a GPU without BC for validation, \"off\" disables it. Requires compressed_textures." },
	{ "debug.texture_astc_sync", _config_boolean, "false", "HALO_TEXTURE_ASTC_SYNC", _environment_set_is_true, _platform_ios,
		"Wait for ASTC map preparation before loading, for screenshot validation only." },
	{ "debug.texture_upscale_generate", _config_string, "\"auto\"", "HALO_TEXTURE_UPSCALE_GENERATE", _environment_value, _platform_ios,
		"Make upscaled textures on this device: \"auto\" (default) where the device's profile\n"
		"and the display.upscaled_textures switch both say so and Core ML has a Neural Engine\n"
		"(at import, in menus, loading and pauses, never in gameplay), \"force\" whatever the\n"
		"profile says, \"off\" never." },
	{ "debug.texture_upscale_export", _config_boolean, "false", "HALO_TEXTURE_UPSCALE_EXPORT", _environment_set_is_true, _platform_ios,
		"Export the generator's policy table and uncompressed textures for parity checks." },
	{ "debug.texture_dump_directory", _config_string, "\"\"", "HALO_TEXTURE_DUMP", _environment_value, _platform_all,
		"A folder to write every texture to as it is uploaded; empty none." },
	{ "debug.texture_override_directory", _config_string, "\"\"", "HALO_TEXTURE_OVERRIDE", _environment_value, _platform_all,
		"A folder of replacement textures, <hash>.rgba (step-tools/texture-upscale.py),\n"
		"each drawn in place of the texture whose first mip level hashes to <hash>; empty none." },
	{ "debug.texture_compare_directory", _config_string, "\"\"", "HALO_TEXTURE_COMPARE", _environment_value, _platform_all,
		"A second folder of replacement textures to compare texture_override_directory's\n"
		"with. With upscaled_textures_toggle, Back + D-pad up switches between the two sets\n"
		"as the game runs; each level starts on the first. Both stay in memory while\n"
		"display.upscaled_textures_memory has room, else the set left is read again. A\n"
		"texture the second set has no file for is drawn from the first, and the readout\n"
		"says so; empty keeps the original/upscaled comparison." },
	{ "debug.texture_override_name", _config_string, "\"\"", "HALO_TEXTURE_OVERRIDE_NAME", _environment_value, _platform_all,
		"What the comparison's readout calls texture_override_directory's set; empty \"SET A\"." },
	{ "debug.texture_compare_name", _config_string, "\"\"", "HALO_TEXTURE_COMPARE_NAME", _environment_value, _platform_all,
		"What the comparison's readout calls texture_compare_directory's set; empty \"SET B\"." },
	{ "debug.texture_log", _config_boolean, "false", "HALO_TEXTURE_LOG", _environment_set_is_true, _platform_all,
		"Log texture uploads." },
	{ "debug.texture_no_cache", _config_boolean, "false", "HALO_TEXTURE_NO_CACHE", _environment_set_is_true, _platform_all,
		"Upload textures again every time they are used." },
	{ "debug.texture_replacement_stats", _config_boolean, "false", "HALO_TEXTURE_REPLACEMENT_STATS", _environment_value,
		_platform_all,
		"Log the upscaled textures' memory (\"texture replacements, now: ...\") every 300\n"
		"frames, as well as at each level load." },
	{ "debug.device_profile", _config_string, "\"\"", "HALO_DEVICE_PROFILE", _environment_value, _platform_ios,
		"Which device profile (profiles/ in the app) sets this device's graphics\n"
		"settings: empty chooses by the device; \"none\" uses the built-in\n"
		"defaults; a model identifier (\"AppleTV5,3\") or a Mac's chip name chooses\n"
		"as that device, for trying its profile elsewhere." },
	{ "debug.frame_log", _config_string, "\"auto\"", "HALO_FRAME_LOG", _environment_value, _platform_ios,
		"With the Metal renderer: append to frame-log.txt, in this folder, a line\n"
		"every 5 seconds and at each change of scene class with the frames drawn,\n"
		"frames a second, the 95th and 99th percentile frame times, late frames,\n"
		"the render size, the scene class, the map, memory and thermal state, so a\n"
		"session played by hand can be read afterwards (tools/frame_log.py).\n"
		"\"auto\" (default) logs on an Apple TV only; \"on\" and \"off\" force it." },
	{ "debug.test_win_level", _config_real, "0.0", "HALO_TEST_WIN_LEVEL", _environment_value, _platform_ios,
		"Win the level after this many seconds of its game time, once, as when\n"
		"the player reaches its end: a run nobody plays then goes through the\n"
		"change to the next level. 0 (default) does nothing." },
	{ "debug.settings_dump", _config_boolean, "false", "HALO_SETTINGS_DUMP", _environment_set_is_true, _platform_ios,
		"Log every setting a device profile may set at start, with the layer each\n"
		"value came from; false logs only those something set." },
#ifdef HALO_PROFILE
	{ "debug.profile_record", _config_boolean, "false", "HALO_PROFILE_RECORD", _environment_value, _platform_all,
		"Record a profile with no command (configure.py --profile and\n"
		"tools/ios_build.py --profile builds; docs/profiling.md): from\n"
		"when profile_record_when says until profile_stop, a map change or the\n"
		"end, into numbered part files in the data folder's profiles folder\n"
		"(tools/net_report.py reads it)." },
	{ "debug.profile_record_when", _config_string, "\"start\"", "HALO_PROFILE_RECORD_WHEN", _environment_value,
		_platform_all,
		"\"start\": from the first frame until the first map change; \"game\":\n"
		"each game that is not the main menu, a recording each, until its map\n"
		"goes." },
	{ "debug.profile_memory", _config_integer, "256", "HALO_PROFILE_MEMORY", _environment_value, _platform_all,
		"Megabytes a recording keeps in memory, 4 to 1024: two halves, each\n"
		"written out as a part when it fills." },
#endif

};

#define NUMBER_OF_CONFIG_SETTINGS (sizeof(config_settings) / sizeof(config_settings[0]))

#ifdef HALO_ILP32
#define CONFIG_PLATFORM _platform_ios
#elif defined(_WIN32)
#define CONFIG_PLATFORM (_platform_desktop | _platform_windows)
#else
#define CONFIG_PLATFORM _platform_desktop
#endif

struct config_value
{
	int boolean;
	long integer;
	double real;
	char *string;
};

static struct config_value config_values[NUMBER_OF_CONFIG_SETTINGS];
static int config_loaded = 0;
static pthread_mutex_t config_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------- the device's profile, and where each value came from */

/* the settings a device profile may set, and those it may set for a scene
class */
static const char *const config_profile_keys[] =
{
#define PROFILE_BOOLEAN(name) name,
#define PROFILE_INTEGER(name, minimum, maximum) name,
#define PROFILE_REAL(name, minimum, maximum) name,
#define PROFILE_STRING(name, choices) name,
#define PROFILE_SCENE(name)
#define PROFILE_VARIABLE(name)
#define PROFILE_EXTERNAL(name)
#include "port_profile_keys.h"
#undef PROFILE_BOOLEAN
#undef PROFILE_INTEGER
#undef PROFILE_REAL
#undef PROFILE_STRING
#undef PROFILE_SCENE
#undef PROFILE_VARIABLE
#undef PROFILE_EXTERNAL
};

static const char *const config_scene_keys[] =
{
#define PROFILE_BOOLEAN(name)
#define PROFILE_INTEGER(name, minimum, maximum)
#define PROFILE_REAL(name, minimum, maximum)
#define PROFILE_STRING(name, choices)
#define PROFILE_SCENE(name) name,
#define PROFILE_VARIABLE(name)
#define PROFILE_EXTERNAL(name)
#include "port_profile_keys.h"
#undef PROFILE_BOOLEAN
#undef PROFILE_INTEGER
#undef PROFILE_REAL
#undef PROFILE_STRING
#undef PROFILE_SCENE
#undef PROFILE_VARIABLE
#undef PROFILE_EXTERNAL
};

static const char *const config_external_keys[] =
{
#define PROFILE_BOOLEAN(name)
#define PROFILE_INTEGER(name, minimum, maximum)
#define PROFILE_REAL(name, minimum, maximum)
#define PROFILE_STRING(name, choices)
#define PROFILE_SCENE(name)
#define PROFILE_VARIABLE(name)
#define PROFILE_EXTERNAL(name) name,
#include "port_profile_keys.h"
#undef PROFILE_BOOLEAN
#undef PROFILE_INTEGER
#undef PROFILE_REAL
#undef PROFILE_STRING
#undef PROFILE_SCENE
#undef PROFILE_VARIABLE
#undef PROFILE_EXTERNAL
};

/* those it may set for the variable-workload context ([variable.display]), which changes only at a map load */
static const char *const config_variable_keys[] =
{
#define PROFILE_BOOLEAN(name)
#define PROFILE_INTEGER(name, minimum, maximum)
#define PROFILE_REAL(name, minimum, maximum)
#define PROFILE_STRING(name, choices)
#define PROFILE_SCENE(name)
#define PROFILE_VARIABLE(name) name,
#define PROFILE_EXTERNAL(name)
#include "port_profile_keys.h"
#undef PROFILE_BOOLEAN
#undef PROFILE_INTEGER
#undef PROFILE_REAL
#undef PROFILE_STRING
#undef PROFILE_SCENE
#undef PROFILE_VARIABLE
#undef PROFILE_EXTERNAL
};

static const struct
{
	const char *name;
	/* as the file held it */
	const char *value;
} config_former_defaults[] =
{
#define FORMER_DEFAULT(name, value) { name, value },
#include "port_config_former_defaults.h"
#undef FORMER_DEFAULT
};

static const char *const config_scene_names[NUMBER_OF_CONFIG_SCENES] = { "gameplay", "menu", "cutscene" };

enum
{
	/* a class profile, a model profile and a drop-in file */
	MAXIMUM_PROFILE_LAYERS = 3,
};

/* one profile layer's value for a setting: as text (true, 540, metal) and
the layer's name for the log (class "Apple TV") */
struct config_profile_value
{
	char *text;
	char *source;
};

enum config_source
{
	_source_default,
	_source_profile,
	_source_file,
	_source_environment,
};

enum config_origin
{
	/* config.origins has no line for the setting */
	_origin_none,
	/* the game wrote the file's value as the setting's default */
	_origin_default,
	/* somebody set it: the menus, the Settings app's switch, a tool, a hand edit */
	_origin_chosen,
};

static struct config_setting_state
{
	/* the profile's layers that set it, lowest first */
	struct config_profile_value profile[MAXIMUM_PROFILE_LAYERS];
	int profile_count;
	/* a scene class's value, from the highest layer that has one (text NULL: none) */
	struct config_profile_value scene[NUMBER_OF_CONFIG_SCENES];
	struct config_value scene_value[NUMBER_OF_CONFIG_SCENES];
	/* the variable-workload context's value ([variable.display]), from the highest layer that has one */
	struct config_profile_value variable;
	struct config_value variable_value;
	/* Index 0 is the external base, then menu and cutscene. */
	struct config_profile_value external[NUMBER_OF_CONFIG_SCENES];
	struct config_value external_value[NUMBER_OF_CONFIG_SCENES];
	/* the default with the profile over it */
	struct config_value device;
	/* that as config.toml holds a value (strings quoted) */
	char *device_literal;
	/* where the value in force came from */
	enum config_source source;
	/* config.origins' line: for _origin_default, the value written */
	enum config_origin origin;
	char *origin_literal;
} config_states[NUMBER_OF_CONFIG_SETTINGS];

static int config_origins_changed;

/* ---------- the file */

static void config_path(char *path, size_t size)
{
#ifdef HALO_ILP32
	/* the data folder, which the app names (port/ios/host/host_main.m) */
	const char *root = getenv("HALO_DATA_ROOT");

	snprintf(path, size, "%s/config.toml", root && *root ? root : ".");
#else
	/* the executable's folder, with its separator */
	const char *base = SDL_GetBasePath();

	snprintf(path, size, "%sconfig.toml", base ? base : "");
#endif
}

/* the whole file, NUL terminated, or NULL; free() it */
static char *config_read_file(const char *path, size_t *size)
{
#ifdef HALO_ILP32
	FILE *file = fopen(path, "rb");
	char *text = NULL;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) >= 0 && fseek(file, 0, SEEK_SET) == 0)
	{
		text = malloc((size_t)length + 1);
		if (text && fread(text, 1, (size_t)length, file) == (size_t)length)
		{
			text[length] = 0;
			*size = (size_t)length;
		}
		else
		{
			free(text);
			text = NULL;
		}
	}
	fclose(file);
	return text;
#else
	/* SDL's, for UTF-8 paths on Windows */
	void *data = SDL_LoadFile(path, size);
	char *text;

	if (!data)
		return NULL;
	text = malloc(*size + 1);
	if (text)
	{
		memcpy(text, data, *size);
		text[*size] = 0;
	}
	SDL_free(data);
	return text;
#endif
}

int config_benchmark_isolated(void)
{
	const char *suite = getenv("HALO_BENCHMARK_SUITE");

	return suite && *suite;
}

static int config_write_file(const char *path, const char *text)
{
	/* port: a benchmark suite never changes the player's settings (its own come from the environment) */
	if (config_benchmark_isolated())
		return 1;
#ifdef HALO_ILP32
	FILE *file = fopen(path, "wb");
	int written;

	if (!file)
		return 0;
	written = fwrite(text, 1, strlen(text), file) == strlen(text);
	return fclose(file) == 0 && written;
#else
	return SDL_SaveFile(path, text, strlen(text));
#endif
}

struct config_text
{
	char *buffer;
	size_t length, capacity;
};

static void config_append(struct config_text *text, const char *string)
{
	size_t length = strlen(string);

	if (text->length + length + 1 > text->capacity)
	{
		size_t capacity = (text->capacity ? text->capacity : 4096) * 2 + length;
		char *buffer = realloc(text->buffer, capacity);

		if (!buffer)
			return;
		text->buffer = buffer;
		text->capacity = capacity;
	}
	memcpy(text->buffer + text->length, string, length + 1);
	text->length += length;
}

/* the first length characters of text, as a string of their own */
static char *config_copy(const char *text, size_t length)
{
	char *copy = malloc(length + 1);

	if (copy)
	{
		memcpy(copy, text, length);
		copy[length] = 0;
	}
	return copy;
}

/* the setting's value on this device, as the file holds a value: its
default, or the profile's value over it (before the profile is read, as in a
probe: the default) */
static const char *config_device_literal(const struct config_setting *setting)
{
	const char *literal = config_states[setting - config_settings].device_literal;

	return literal ? literal : setting->default_value;
}

/* one setting as the file holds it: its comment, and its key at the
device's value */
static void config_append_setting(struct config_text *text, const struct config_setting *setting)
{
	const char *dot = strchr(setting->name, '.');
	const char *line;
	char buffer[256];

	config_append(text, "\n");
	for (line = setting->comment; *line;)
	{
		size_t length = strcspn(line, "\n");

		snprintf(buffer, sizeof(buffer), "# %.*s\n", (int)length, line);
		config_append(text, buffer);
		line += length;
		if (*line)
			line++;
	}
#ifndef HALO_ILP32
	/* (iOS apps have no environment to set) */
	switch (setting->environment_style)
	{
	case _environment_value:
		snprintf(buffer, sizeof(buffer), "# (for one run: %s=<value>)\n", setting->environment);
		break;
	case _environment_set_is_true:
		snprintf(buffer, sizeof(buffer), "# (for one run: %s=1 makes it true)\n", setting->environment);
		break;
	case _environment_set_is_false:
		snprintf(buffer, sizeof(buffer), "# (for one run: %s=1 makes it false)\n", setting->environment);
		break;
	}
	config_append(text, buffer);
#endif
	snprintf(buffer, sizeof(buffer), "%s = %s\n", dot + 1, config_device_literal(setting));
	config_append(text, buffer);
}

/* the file with every setting of this build at its default */
static char *config_default_text(void)
{
	struct config_text text = { NULL, 0, 0 };
	char section[32] = "";
	size_t index;

#ifdef HALO_ILP32
	config_append(&text,
		"# Halo settings\n"
		"#\n"
		"# The game writes this file with the defaults when it is missing: delete\n"
		"# it to go back to them.\n");
#else
	config_append(&text,
		"# Halo settings\n"
		"#\n"
		"# The game writes this file with the defaults when it is missing: delete\n"
		"# it to go back to them. Each setting can also be set for one run with\n"
		"# the environment variable named with it, which wins over this file.\n");
#endif
	/* each table once, in the order its first setting has in the table above,
	with all of its settings: TOML rejects a table defined twice, and the
	table above doesn't keep a section's settings together (iOS's input
	settings come between display's) */
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		const char *dot = strchr(setting->name, '.');
		size_t length, other;
		char buffer[64];

		if (!(setting->platforms & CONFIG_PLATFORM) || !dot)
			continue;
		/* (the section's name and its dot) */
		length = (size_t)(dot - setting->name) + 1;
		/* (a section an earlier setting opened is written already) */
		for (other = 0; other < index; other++)
		{
			if ((config_settings[other].platforms & CONFIG_PLATFORM) &&
				!strncmp(config_settings[other].name, setting->name, length))
				break;
		}
		if (other < index)
			continue;
		snprintf(section, sizeof(section), "%.*s", (int)(length - 1), setting->name);
		snprintf(buffer, sizeof(buffer), "\n[%s]\n", section);
		config_append(&text, buffer);
		for (other = index; other < NUMBER_OF_CONFIG_SETTINGS; other++)
		{
			if ((config_settings[other].platforms & CONFIG_PLATFORM) &&
				!strncmp(config_settings[other].name, setting->name, length))
				config_append_setting(&text, &config_settings[other]);
		}
	}
	return text.buffer;
}

static int config_line_section(const char *line, const char *end, char *section, size_t size);

/* text with its repeated tables and keys folded away, or NULL if it has
none: a table's later headers are dropped and their lines join its first,
and a key set again in the same table keeps its first value. TOML rejects
both, and without this the whole file would be ignored for one slip (an
older version of this file's writer repeated [display] and [input] on
iOS). Each repeat is logged with its line. Only "[table]" headers and
"key = value" lines are recognized, which is all the settings use */
static char *config_fold_repeats(const char *text)
{
	enum { MAXIMUM_TABLES = 32, MAXIMUM_KEYS = 512 };
	struct config_fold_line
	{
		const char *start;
		size_t length;
		/* -1 before the first table */
		int table;
	};
	char tables[MAXIMUM_TABLES][64];
	char keys[MAXIMUM_KEYS][64];
	int key_tables[MAXIMUM_KEYS];
	int table_count = 0, key_count = 0, table = -1, repeats = 0, line_number = 1, order;
	struct config_fold_line *lines = NULL;
	size_t line_count = 0, capacity = 0, index;
	struct config_text out = { NULL, 0, 0 };
	const char *line;

	for (line = text; *line; line_number++)
	{
		const char *end = line + strcspn(line, "\n");
		const char *next = *end ? end + 1 : end;
		const char *start = line;
		char name[64];
		int keep = 1;

		while (start < end && (*start == ' ' || *start == '\t'))
			start++;
		if (start + 1 < end && start[0] == '[' && start[1] != '[' &&
			config_line_section(start, end, name, sizeof(name)))
		{
			int found;

			for (found = 0; found < table_count && strcmp(tables[found], name); found++)
				;
			if (found < table_count)
			{
				platform_log("config.toml line %d: [%s] is defined again; its settings join the first", line_number, name);
				repeats++;
				keep = 0;
			}
			else if (table_count < MAXIMUM_TABLES)
			{
				snprintf(tables[table_count], sizeof(tables[0]), "%s", name);
				table_count++;
			}
			table = found < table_count ? found : -1;
		}
		else if (table >= 0 && start < end && *start != '#')
		{
			size_t length = strcspn(start, " \t=\n");
			const char *equals = start + length;

			while (equals < end && (*equals == ' ' || *equals == '\t'))
				equals++;
			if (length && equals < end && *equals == '=' && length < sizeof(keys[0]))
			{
				int found;

				for (found = 0; found < key_count &&
					(key_tables[found] != table || strncmp(keys[found], start, length) || keys[found][length]); found++)
					;
				if (found < key_count)
				{
					platform_log("config.toml line %d: %s.%.*s is set again; the first value stays", line_number,
						tables[table], (int)length, start);
					repeats++;
					keep = 0;
				}
				else if (key_count < MAXIMUM_KEYS)
				{
					memcpy(keys[key_count], start, length);
					keys[key_count][length] = 0;
					key_tables[key_count] = table;
					key_count++;
				}
			}
		}
		if (keep)
		{
			if (line_count == capacity)
			{
				size_t new_capacity = capacity ? capacity * 2 : 256;
				struct config_fold_line *grown = realloc(lines, new_capacity * sizeof(*grown));

				if (!grown)
				{
					free(lines);
					return NULL;
				}
				lines = grown;
				capacity = new_capacity;
			}
			lines[line_count].start = line;
			lines[line_count].length = (size_t)(next - line);
			lines[line_count].table = table;
			line_count++;
		}
		line = next;
	}
	if (!repeats)
	{
		free(lines);
		return NULL;
	}
	/* the lines before the first table, then each table's lines in order */
	for (order = -1; order < table_count; order++)
	{
		for (index = 0; index < line_count; index++)
		{
			char *copy;

			if (lines[index].table != order)
				continue;
			copy = config_copy(lines[index].start, lines[index].length);
			if (!copy)
				continue;
			/* (the file's last line may lack its newline) */
			if (out.length && out.buffer[out.length - 1] != '\n')
				config_append(&out, "\n");
			config_append(&out, copy);
			free(copy);
		}
	}
	free(lines);
	return out.buffer;
}

static void config_origin_set(size_t index, enum config_origin origin, const char *literal);
static void config_origin_set_device(size_t index);

/* the settings of this build that text (the file, parsed as table) lacks,
added to it in their sections, keeping the rest as it is: a newer version's
settings appear in an older file. Returns the new text, or NULL if nothing
was missing */
static char *config_add_missing(const char *text, toml_datum_t table)
{
	char *result = NULL;
	size_t index;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		const char *dot = strchr(setting->name, '.');
		const char *current = result ? result : text;
		struct config_text block = { NULL, 0, 0 };
		struct config_text updated = { NULL, 0, 0 };
		char header[40];
		const char *line;
		const char *insert = NULL;

		if (!(setting->platforms & CONFIG_PLATFORM) || !dot || toml_seek(table, setting->name).type != TOML_UNKNOWN)
			continue;
		snprintf(header, sizeof(header), "[%.*s]", (int)(dot - setting->name), setting->name);
		/* the end of the section's last line that is not blank */
		for (line = current; *line; )
		{
			const char *start = line;
			size_t length = strcspn(line, "\n");

			while (*start == ' ' || *start == '\t')
				start++;
			if (insert && *start == '[')
				break;
			if (!insert && !strncmp(start, header, strlen(header)))
				insert = line + length;
			else if (insert && start < line + length && *start != '\r')
				insert = line + length;
			line += length;
			if (*line)
				line++;
		}
		if (insert)
		{
			if (*insert)
				insert++;
			config_append_setting(&block, setting);
		}
		else
		{
			/* no such section: a new one at the end */
			insert = current + strlen(current);
			config_append(&block, current[0] && insert[-1] != '\n' ? "\n\n" : "\n");
			config_append(&block, header);
			config_append(&block, "\n");
			config_append_setting(&block, setting);
		}
		if (!block.buffer)
			continue;
		{
			char *before = config_copy(current, (size_t)(insert - current));

			if (before)
				config_append(&updated, before);
			free(before);
		}
		if (insert > current && insert[-1] != '\n')
			config_append(&updated, "\n");
		config_append(&updated, block.buffer);
		config_append(&updated, insert);
		free(block.buffer);
		if (updated.buffer)
		{
			free(result);
			result = updated.buffer;
			platform_log("settings: added %s (new in this version) at its default", setting->name);
			config_origin_set_device(index);
		}
	}
	return result;
}

/* ---------- values */

static int config_text_is_false(const char *text)
{
	char lower[8];
	size_t index;

	for (index = 0; index + 1 < sizeof(lower) && text[index]; index++)
		lower[index] = (char)tolower((unsigned char)text[index]);
	lower[index] = 0;
	return !strcmp(lower, "0") || !strcmp(lower, "false") || !strcmp(lower, "no") || !strcmp(lower, "off");
}

/* whether a variable that only has to be set (_environment_set_is_true or
_environment_set_is_false) is: empty, "0", "false", "no" or "off" is not */
static int config_environment_set(const char *text)
{
	return text[0] && !config_text_is_false(text);
}

static void config_set_from_text(struct config_value *value, enum config_type type, const char *text)
{
	switch (type)
	{
	case _config_boolean:
		value->boolean = !config_text_is_false(text);
		break;
	case _config_integer:
		value->integer = strtol(text, NULL, 10);
		break;
	case _config_real:
		value->real = strtod(text, NULL);
		break;
	case _config_string:
		/* (the old string is kept, not freed: config_string's callers hold
		its pointer, and Settings writes few, seldom) */
		value->string = strdup(text);
		break;
	}
}

/* the value in the file, if it is there and of the setting's type (1; else 0) */
static int config_set_from_file(struct config_value *value, const struct config_setting *setting,
	toml_datum_t table)
{
	toml_datum_t datum = toml_seek(table, setting->name);
	int wrong_type = 0;

	if (datum.type == TOML_UNKNOWN)
		return 0;
	switch (setting->type)
	{
	case _config_boolean:
		if (datum.type == TOML_BOOLEAN)
			value->boolean = datum.u.boolean;
		else
			wrong_type = 1;
		break;
	case _config_integer:
		if (datum.type == TOML_INT64)
			value->integer = (long)datum.u.int64;
		else
			wrong_type = 1;
		break;
	case _config_real:
		if (datum.type == TOML_FP64)
			value->real = datum.u.fp64;
		else if (datum.type == TOML_INT64)
			value->real = (double)datum.u.int64;
		else
			wrong_type = 1;
		break;
	case _config_string:
		if (datum.type == TOML_STRING)
		{
			free(value->string);
			value->string = strdup(datum.u.s);
		}
		else
		{
			wrong_type = 1;
		}
		break;
	}
	if (wrong_type)
	{
		static const char *const expected[] = { "true or false", "a whole number", "a number", "a quoted string" };

		platform_log("config.toml line %d: %s should be %s; using %s", datum.lineno, setting->name,
			expected[setting->type], config_device_literal(setting));
	}
	return !wrong_type;
}

/* how many times a setting has been written (config_write): readers that
keep a setting watch this, to read it again */
static volatile unsigned long config_change_count;

/* config_setting_index's table: each setting's index plus one, by the hash of its name (open addressing, linear
probing; 0 is empty). Built on the first lookup; a name listed twice keeps its first index, as the scan it replaces
did. Every config_ read comes through here, many of them every frame */
#define CONFIG_SETTING_HASH_SIZE 2048
_Static_assert(NUMBER_OF_CONFIG_SETTINGS * 2 <= CONFIG_SETTING_HASH_SIZE, "config_setting_hash is too small");
static unsigned short config_setting_hash[CONFIG_SETTING_HASH_SIZE];
static int config_setting_hash_ready;
static pthread_mutex_t config_setting_hash_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned long config_name_hash(const char *name)
{
	unsigned long hash = 2166136261UL;

	while (*name)
		hash = ((hash ^ (unsigned char)*name++) * 16777619UL) & 0xffffffffUL;
	return hash;
}

static void config_setting_hash_build(void)
{
	size_t index;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		unsigned long slot = config_name_hash(config_settings[index].name) & (CONFIG_SETTING_HASH_SIZE - 1);

		while (config_setting_hash[slot] &&
			strcmp(config_settings[config_setting_hash[slot] - 1].name, config_settings[index].name))
			slot = (slot + 1) & (CONFIG_SETTING_HASH_SIZE - 1);
		if (!config_setting_hash[slot])
			config_setting_hash[slot] = (unsigned short)(index + 1);
	}
}

static long config_setting_index(const char *name)
{
	unsigned long slot;

	/* (the guest's C library has no pthread_once) */
	if (!__atomic_load_n(&config_setting_hash_ready, __ATOMIC_ACQUIRE))
	{
		pthread_mutex_lock(&config_setting_hash_lock);
		if (!config_setting_hash_ready)
		{
			config_setting_hash_build();
			__atomic_store_n(&config_setting_hash_ready, 1, __ATOMIC_RELEASE);
		}
		pthread_mutex_unlock(&config_setting_hash_lock);
	}
	for (slot = config_name_hash(name) & (CONFIG_SETTING_HASH_SIZE - 1); config_setting_hash[slot];
		slot = (slot + 1) & (CONFIG_SETTING_HASH_SIZE - 1))
	{
		if (!strcmp(config_settings[config_setting_hash[slot] - 1].name, name))
			return (long)config_setting_hash[slot] - 1;
	}
	return -1;
}

/* keys in the file that are no setting, likely misspelt */
static void config_report_unknown_keys(toml_datum_t table)
{
	int section_index;

	for (section_index = 0; section_index < table.u.tab.size; section_index++)
	{
		toml_datum_t section = table.u.tab.value[section_index];
		int key_index;

		if (section.type != TOML_TABLE)
		{
			platform_log("config.toml line %d: unknown setting %s", section.lineno, table.u.tab.key[section_index]);
			continue;
		}
		for (key_index = 0; key_index < section.u.tab.size; key_index++)
		{
			char name[128];

			snprintf(name, sizeof(name), "%s.%s", table.u.tab.key[section_index], section.u.tab.key[key_index]);
			if (config_setting_index(name) < 0)
				platform_log("config.toml line %d: unknown setting %s", section.u.tab.value[key_index].lineno, name);
		}
	}
}

/* the setting's built-in default, as a new file would have it */
static void config_set_builtin(size_t index)
{
	const char *default_value = config_settings[index].default_value;

	if (config_settings[index].type == _config_string)
	{
		/* written as a TOML basic string without escapes */
		size_t length = strlen(default_value);

		free(config_values[index].string);
		config_values[index].string = length >= 2 ? config_copy(default_value + 1, length - 2) : strdup("");
	}
	else
	{
		config_set_from_text(&config_values[index], config_settings[index].type, default_value);
	}
}

/* a value as config.toml holds one: true, 60, 1.5 (always with its point:
TOML reads 1 as an integer), "all" (quoted, its quotes and backslashes
escaped) */
static void config_literal(enum config_type type, const struct config_value *value, char *text, size_t size)
{
	switch (type)
	{
	case _config_boolean:
		snprintf(text, size, "%s", value->boolean ? "true" : "false");
		break;
	case _config_integer:
		snprintf(text, size, "%ld", value->integer);
		break;
	case _config_real:
		snprintf(text, size, "%.15g", value->real);
		if (!strpbrk(text, ".en") && strlen(text) + 3 < size)
			strcat(text, ".0");
		break;
	case _config_string:
	{
		const char *character = value->string ? value->string : "";
		size_t length = 0;

		if (size < 3)
		{
			text[0] = 0;
			break;
		}
		text[length++] = '"';
		for (; *character && length + 3 < size; character++)
		{
			if (*character == '"' || *character == '\\')
				text[length++] = '\\';
			text[length++] = *character;
		}
		text[length++] = '"';
		text[length] = 0;
		break;
	}
	}
}

/* a default as the table writes it ("0.6", "\"gl\"") in config_literal's
form, so that it compares with a value read from the file */
static void config_literal_of_default(enum config_type type, const char *default_value, char *text, size_t size)
{
	struct config_value value = { 0, 0, 0.0, NULL };
	size_t length = strlen(default_value);

	if (type == _config_string)
		value.string = length >= 2 ? config_copy(default_value + 1, length - 2) : strdup("");
	else
		config_set_from_text(&value, type, default_value);
	config_literal(type, &value, text, size);
	free(value.string);
}

/* ---------- the device's profile */

static int config_name_in(const char *name, const char *const *names, size_t count)
{
	size_t index;

	for (index = 0; index < count; index++)
	{
		if (!strcmp(names[index], name))
			return 1;
	}
	return 0;
}

static int config_is_profile_setting(size_t index)
{
	return config_name_in(config_settings[index].name, config_profile_keys,
		sizeof(config_profile_keys) / sizeof(config_profile_keys[0]));
}

/* a profile's value (true, 540, 0.6, metal) as the setting's type */
static void config_set_from_profile_text(struct config_value *value, enum config_type type, const char *text)
{
	if (type == _config_string)
	{
		free(value->string);
		value->string = strdup(text);
	}
	else
	{
		config_set_from_text(value, type, text);
	}
}

/* The profile the host chose (port/ios/host/host_profile.c), from
HALO_DEVICE_PROFILE_VALUES: a line for each value, lowest layer first,
"name<tab>value<tab>layer", and for a scene class's value a fourth field,
the class. Not one of the settings' own HALO_ variables, which win over the
file: these sit under it. A build with no host (the desktop's) has none. */
static void config_profile_load_text(const char *line, int point, int networked)
{

	while (line && *line)
	{
		const char *end = line + strcspn(line, "\n");
		char fields[5][96] = { "", "", "", "", "" };
		const char *field = line;
		int count = 0, scene;
		long index;

		while (count < 5 && field <= end)
		{
			size_t length = 0;

			while (field + length < end && field[length] != '\t')
				length++;
			snprintf(fields[count++], sizeof(fields[0]), "%.*s", (int)length, field);
			field += length + 1;
		}
		line = *end ? end + 1 : end;
		if (count < 3)
			continue;
		if (point && !port_profile_apply_allowed(fields[0], point, networked)) continue;
		/* Output mode selection belongs to the host, never to config.toml. */
		if (!strcmp(fields[0], "display.output_height"))
			continue;
		index = config_setting_index(fields[0]);
		if (index < 0 || !config_is_profile_setting((size_t)index) || !(config_settings[index].platforms & CONFIG_PLATFORM))
		{
			platform_log("settings: the device profile sets %s, which is not a setting a profile may set here; ignored",
				fields[0]);
			continue;
		}
		if (count == 5)
		{
			if (strcmp(fields[4], "external") || !config_name_in(fields[0], config_external_keys,
				sizeof(config_external_keys) / sizeof(config_external_keys[0])))
				continue;
			scene = 0;
			if (*fields[3])
			{
				for (scene = 1; scene < NUMBER_OF_CONFIG_SCENES && strcmp(config_scene_names[scene], fields[3]); scene++)
					;
				if (scene == NUMBER_OF_CONFIG_SCENES || !config_name_in(fields[0], config_scene_keys,
				sizeof(config_scene_keys) / sizeof(config_scene_keys[0])))
					continue;
			}
			free(config_states[index].external[scene].text);
			free(config_states[index].external[scene].source);
			config_states[index].external[scene].text = strdup(fields[1]);
			config_states[index].external[scene].source = strdup(fields[2]);
			config_set_from_profile_text(&config_states[index].external_value[scene], config_settings[index].type, fields[1]);
			continue;
		}
		if (count == 4 && !strcmp(fields[3], "variable"))
		{
			if (!config_name_in(fields[0], config_variable_keys, sizeof(config_variable_keys) / sizeof(config_variable_keys[0])))
			{
				platform_log("settings: the device profile sets %s for the variable workload, which this build cannot; ignored",
					fields[0]);
				continue;
			}
			free(config_states[index].variable.text);
			free(config_states[index].variable.source);
			config_states[index].variable.text = strdup(fields[1]);
			config_states[index].variable.source = strdup(fields[2]);
			config_set_from_profile_text(&config_states[index].variable_value, config_settings[index].type, fields[1]);
			continue;
		}
		if (count == 4)
		{
			for (scene = 1; scene < NUMBER_OF_CONFIG_SCENES && strcmp(config_scene_names[scene], fields[3]); scene++)
				;
			if (scene == NUMBER_OF_CONFIG_SCENES || !config_name_in(fields[0], config_scene_keys,
				sizeof(config_scene_keys) / sizeof(config_scene_keys[0])))
			{
				platform_log("settings: the device profile sets %s for scene class %s, which this build cannot; ignored",
					fields[0], fields[3]);
				continue;
			}
			/* (a later layer's takes the earlier one's place) */
			free(config_states[index].scene[scene].text);
			free(config_states[index].scene[scene].source);
			config_states[index].scene[scene].text = strdup(fields[1]);
			config_states[index].scene[scene].source = strdup(fields[2]);
			config_set_from_profile_text(&config_states[index].scene_value[scene], config_settings[index].type, fields[1]);
		}
		else if (config_states[index].profile_count < MAXIMUM_PROFILE_LAYERS)
		{
			struct config_profile_value *value = &config_states[index].profile[config_states[index].profile_count++];

			value->text = strdup(fields[1]);
			value->source = strdup(fields[2]);
		}
	}
}

/* the setting's device value: its default, with the profile's value over
it; what it has when nobody set it */
static void config_set_device(size_t index)
{
	const struct config_setting_state *state = &config_states[index];

	config_set_builtin(index);
	if (state->profile_count)
		config_set_from_profile_text(&config_values[index], config_settings[index].type,
			state->profile[state->profile_count - 1].text);
}

/* the device value kept, and as the file holds it: the table's own text
where no profile sets it, so a file without a profile is written as it
always was */
static void config_keep_device(size_t index)
{
	struct config_setting_state *state = &config_states[index];
	char literal[600];

	config_set_device(index);
	free(state->device.string);
	state->device = config_values[index];
	if (state->device.string)
		state->device.string = strdup(state->device.string);
	free(state->device_literal);
	if (state->profile_count)
	{
		config_literal(config_settings[index].type, &state->device, literal, sizeof(literal));
		state->device_literal = strdup(literal);
	}
	else
	{
		state->device_literal = strdup(config_settings[index].default_value);
	}
	state->source = state->profile_count ? _source_profile : _source_default;
}

/* ---------- config.origins: which of the file's values the game wrote as
defaults

config.toml holds every setting, so by itself it cannot say whether a value
is one the player chose or the default of the build that wrote it; a
default that changed, or a device profile, would never reach an existing
install. config.origins, beside it, has a line for each setting:

	display.render_height<tab>default<tab>0
	display.upscaled_textures<tab>chosen

"default" with the value the game wrote, or "chosen". Only the game writes
it (and tools/mac_run.py, to mark a runner's values chosen). Losing it is
harmless: config_origin_decide's rule for a setting with no line applies
again. */

static void config_origins_path(char *path, size_t size)
{
	char folder[1024];

	config_folder(folder, sizeof(folder));
	snprintf(path, size, "%sconfig.origins", folder);
}

static void config_origin_set(size_t index, enum config_origin origin, const char *literal)
{
	struct config_setting_state *state = &config_states[index];

	if (state->origin == origin && (origin != _origin_default ||
		(state->origin_literal && !strcmp(state->origin_literal, literal))))
		return;
	state->origin = origin;
	free(state->origin_literal);
	state->origin_literal = origin == _origin_default ? strdup(literal) : NULL;
	config_origins_changed = 1;
}

/* the setting's line in the file is the device value, written as a default */
static void config_origin_set_device(size_t index)
{
	char literal[600];

	config_literal(config_settings[index].type, &config_states[index].device, literal, sizeof(literal));
	config_origin_set(index, _origin_default, literal);
}

static void config_origins_read(void)
{
	char path[1024];
	size_t size = 0, index;
	char *text;
	const char *line;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		config_states[index].origin = _origin_none;
		free(config_states[index].origin_literal);
		config_states[index].origin_literal = NULL;
	}
	config_origins_changed = 0;
	config_origins_path(path, sizeof(path));
	text = config_read_file(path, &size);
	for (line = text ? text : ""; *line;)
	{
		const char *end = line + strcspn(line, "\r\n");
		const char *name_end = memchr(line, '\t', (size_t)(end - line));
		char name[128];
		long setting;

		if (name_end && (size_t)(name_end - line) < sizeof(name))
		{
			const char *kind = name_end + 1;

			snprintf(name, sizeof(name), "%.*s", (int)(name_end - line), line);
			setting = config_setting_index(name);
			if (setting >= 0 && end - kind == 6 && !strncmp(kind, "chosen", 6))
			{
				config_states[setting].origin = _origin_chosen;
			}
			else if (setting >= 0 && end - kind > 8 && !strncmp(kind, "default\t", 8))
			{
				config_states[setting].origin = _origin_default;
				free(config_states[setting].origin_literal);
				config_states[setting].origin_literal = config_copy(kind + 8, (size_t)(end - kind - 8));
			}
		}
		line = end + strspn(end, "\r\n");
	}
	free(text);
}

static void config_origins_write(void)
{
	struct config_text text = { NULL, 0, 0 };
	char path[1024], line[800];
	size_t index;

	if (!config_origins_changed)
		return;
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting_state *state = &config_states[index];

		if (state->origin == _origin_chosen)
			snprintf(line, sizeof(line), "%s\tchosen\n", config_settings[index].name);
		else if (state->origin == _origin_default && state->origin_literal)
			snprintf(line, sizeof(line), "%s\tdefault\t%s\n", config_settings[index].name, state->origin_literal);
		else
			continue;
		config_append(&text, line);
	}
	config_origins_path(path, sizeof(path));
	if (!text.buffer || !config_write_file(path, text.buffer))
		platform_log("settings: cannot write %s", path);
	else
		config_origins_changed = 0;
	free(text.buffer);
}

/* whether a value the file holds for a setting with no line in
config.origins is one a build wrote as a default: the built-in default now,
one main once shipped for a profile setting (port_config_former_defaults.h),
or this device's value */
static int config_literal_is_a_default(size_t index, const char *literal)
{
	const struct config_setting *setting = &config_settings[index];
	char other[600];
	size_t former;

	config_literal_of_default(setting->type, setting->default_value, other, sizeof(other));
	if (!strcmp(literal, other))
		return 1;
	config_literal(setting->type, &config_states[index].device, other, sizeof(other));
	if (!strcmp(literal, other))
		return 1;
	for (former = 0; former < sizeof(config_former_defaults) / sizeof(config_former_defaults[0]); former++)
	{
		if (strcmp(config_former_defaults[former].name, setting->name))
			continue;
		config_literal_of_default(setting->type, config_former_defaults[former].value, other, sizeof(other));
		if (!strcmp(literal, other))
			return 1;
	}
	return 0;
}

static char *config_text_set(const char *text, const char *name, const char *line_text);
static char *config_text_remove(const char *text, const char *name);

/* A config.toml from before display.effect_resolution had three values holds a
boolean there and display.mirror_resolution beside it: false is "xbox", true with
the mirror at "full" is "full", true otherwise "half". The file's text is
rewritten (the old text freed) with the one setting, and the setting's origin is
forgotten so config_origin_decide classes the migrated value afresh: the default
follows the device's profile, anything else is the player's. Returns 1 when the
text changed; the caller parses it again. */
static int config_migrate_effect_resolution(char **text, toml_datum_t table)
{
	toml_datum_t effect = toml_seek(table, "display.effect_resolution");
	toml_datum_t mirror = toml_seek(table, "display.mirror_resolution");
	const char *value;
	char line_text[64];
	char *changed;
	long index = config_setting_index("display.effect_resolution");

	if (effect.type != TOML_BOOLEAN && mirror.type == TOML_UNKNOWN)
		return 0;
	if (effect.type == TOML_BOOLEAN)
		value = !effect.u.boolean ? "xbox" : mirror.type == TOML_STRING && !strcmp(mirror.u.s, "full") ? "full" : "half";
	/* (a full mirror with no effect_resolution line: the old effect_resolution's default was true, so the two
	together were "full") */
	else if (effect.type == TOML_UNKNOWN && mirror.type == TOML_STRING && !strcmp(mirror.u.s, "full"))
		value = "full";
	else
		value = NULL;
	if (value)
	{
		snprintf(line_text, sizeof(line_text), "effect_resolution = \"%s\"\n", value);
		changed = config_text_set(*text, "display.effect_resolution", line_text);
		if (!changed)
			return 0;
		free(*text);
		*text = changed;
		platform_log("settings: display.effect_resolution is now \"%s\" (was %s, with display.mirror_resolution %s)",
			value, effect.type != TOML_BOOLEAN ? "unset" : effect.u.boolean ? "true" : "false",
			mirror.type == TOML_STRING ? mirror.u.s : "unset");
		if (index >= 0)
		{
			free(config_states[index].origin_literal);
			config_states[index].origin_literal = NULL;
			config_states[index].origin = _origin_none;
			config_origins_changed = 1;
		}
	}
	/* (display.mirror_resolution is no setting now: its line is dropped, a stray one with the new setting included) */
	if (mirror.type != TOML_UNKNOWN)
	{
		changed = config_text_remove(*text, "display.mirror_resolution");
		if (changed)
		{
			free(*text);
			*text = changed;
		}
	}
	return 1;
}

/* One setting the file holds (its value read into file_value, of the right
type), against config.origins:

	the line says   the file's value        then
	default T       T                       nobody touched it: the device value is in force, and if that
	                                        is no longer T the file's line is rewritten to it
	default T       not T                   somebody set it: chosen from now on
	chosen          any                     the file's value is in force
	(no line)       a default (see above)   as "default T, T"; logged, so a player who did choose that
	                                        value sees what happened and can set it again
	(no line)       anything else           chosen

Returns the file's new text when the line was rewritten (free the old), else
NULL. */
static char *config_origin_decide(size_t index, const struct config_value *file_value, const char *text)
{
	const struct config_setting *setting = &config_settings[index];
	struct config_setting_state *state = &config_states[index];
	char file_literal[600], device_literal[600], line_text[700];
	const char *dot = strchr(setting->name, '.');
	int reclassified = 0;

	config_literal(setting->type, file_value, file_literal, sizeof(file_literal));
	config_literal(setting->type, &state->device, device_literal, sizeof(device_literal));
	if (state->origin == _origin_none)
	{
		if (config_literal_is_a_default(index, file_literal))
		{
			config_origin_set(index, _origin_default, file_literal);
			reclassified = 1;
		}
		else
		{
			config_origin_set(index, _origin_chosen, NULL);
		}
	}
	else if (state->origin == _origin_default && strcmp(state->origin_literal, file_literal))
	{
		config_origin_set(index, _origin_chosen, NULL);
	}
	if (state->origin == _origin_chosen)
	{
		/* the file's value is in force */
		if (setting->type == _config_string)
		{
			free(config_values[index].string);
			config_values[index].string = strdup(file_value->string ? file_value->string : "");
		}
		else
		{
			config_values[index] = *file_value;
		}
		state->source = _source_file;
		return NULL;
	}
	/* a written default: the device value stays in force */
	if (!strcmp(file_literal, device_literal))
		return NULL;
	if (reclassified)
		platform_log("settings: %s = %s in config.toml is a default an earlier version wrote, not a choice: it becomes "
			"this device's %s. If you chose %s, set it again and it stays", setting->name, file_literal, device_literal,
			file_literal);
	else
		platform_log("settings: %s was the default %s in config.toml; it is now this device's %s", setting->name,
			file_literal, device_literal);
	config_origin_set(index, _origin_default, device_literal);
	snprintf(line_text, sizeof(line_text), "%s = %s\n", dot + 1, device_literal);
	return config_text_set(text, setting->name, line_text);
}

/* the setting's environment variable, if it is set, over the file */
static void config_set_from_environment(size_t index)
{
	const struct config_setting *setting = &config_settings[index];
	const char *environment = getenv(setting->environment);

	if (!environment || (setting->environment_style != _environment_value && !config_environment_set(environment)))
		return;
	switch (setting->environment_style)
	{
	case _environment_value:
		config_set_from_text(&config_values[index], setting->type, environment);
		break;
	case _environment_set_is_true:
		config_values[index].boolean = 1;
		break;
	case _environment_set_is_false:
		config_values[index].boolean = 0;
		break;
	}
	config_states[index].source = _source_environment;
}

/* the log's account of where the settings a profile may set got their
values: one line for each that anything but its default set (all of them
with debug.settings_dump), highest layer first. Other settings (network,
paths, controls) are never printed, so the lines are safe to pass on. */
static void config_dump(void)
{
	size_t index;
	int dump_all = 0;
	long dump_index = config_setting_index("debug.settings_dump");

	if (dump_index >= 0)
		dump_all = config_values[dump_index].boolean;
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		const struct config_setting_state *state = &config_states[index];
		struct config_text line = { NULL, 0, 0 };
		char literal[600], piece[800];
		int layer, scene;

		if (!config_is_profile_setting(index) || !(setting->platforms & CONFIG_PLATFORM))
			continue;
		if (!dump_all && state->source == _source_default && !state->profile_count)
		{
			/* (nothing set it; a scene class's or the variable context's value alone still has its line) */
			for (scene = 1; scene < NUMBER_OF_CONFIG_SCENES && !state->scene[scene].text; scene++)
				;
			if (scene == NUMBER_OF_CONFIG_SCENES && !state->variable.text)
				continue;
		}
		config_literal(setting->type, &config_values[index], literal, sizeof(literal));
		snprintf(piece, sizeof(piece), "settings: %s = %s", setting->name, literal);
		config_append(&line, piece);
		if (state->source == _source_environment)
		{
			snprintf(piece, sizeof(piece), " [environment %s]", setting->environment);
			config_append(&line, piece);
		}
		if (state->source == _source_file || (state->source == _source_environment && state->origin == _origin_chosen))
		{
			if (state->source == _source_environment)
				config_append(&line, " over a value");
			config_append(&line, " [config.toml, chosen]");
		}
		for (layer = state->profile_count - 1; layer >= 0; layer--)
		{
			struct config_value value = { 0, 0, 0.0, NULL };

			config_set_from_profile_text(&value, setting->type, state->profile[layer].text);
			config_literal(setting->type, &value, literal, sizeof(literal));
			free(value.string);
			if (layer == state->profile_count - 1 && state->source == _source_profile)
				snprintf(piece, sizeof(piece), " [%s]", state->profile[layer].source);
			else
				snprintf(piece, sizeof(piece), " over %s [%s]", literal, state->profile[layer].source);
			config_append(&line, piece);
		}
		config_literal_of_default(setting->type, setting->default_value, literal, sizeof(literal));
		if (state->source == _source_default)
			config_append(&line, " [default]");
		else
		{
			snprintf(piece, sizeof(piece), " over %s [default]", literal);
			config_append(&line, piece);
		}
		if (state->variable.text)
		{
			config_literal(setting->type, &state->variable_value, literal, sizeof(literal));
			snprintf(piece, sizeof(piece), "; variable %s [%s]%s", literal, state->variable.source,
				state->source == _source_file || state->source == _source_environment ? " (not used: the value is set)" : "");
			config_append(&line, piece);
		}
		for (scene = 1; scene < NUMBER_OF_CONFIG_SCENES; scene++)
		{
			if (!state->scene[scene].text)
				continue;
			config_literal(setting->type, &state->scene_value[scene], literal, sizeof(literal));
			snprintf(piece, sizeof(piece), "; %s %s [%s]%s", config_scene_names[scene], literal, state->scene[scene].source,
				state->source == _source_file || state->source == _source_environment ? " (not used: the value is set)" : "");
			config_append(&line, piece);
		}
		if (line.buffer)
			platform_log("%s", line.buffer);
		free(line.buffer);
	}
}

static void config_load(void)
{
	char path[1024];
	size_t size = 0;
	char *text;
	size_t index;
	int migrated;

	config_profile_load_text(getenv("HALO_DEVICE_PROFILE_VALUES"), 0, 0);
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
		config_keep_device(index);
	config_origins_read();

	config_path(path, sizeof(path));
	text = config_read_file(path, &size);
	if (text)
	{
		char *folded = config_fold_repeats(text);
		toml_result_t result;

		if (folded)
		{
			free(text);
			text = folded;
			size = strlen(text);
		}
		result = toml_parse(text, (int)size);
		migrated = result.ok && config_migrate_effect_resolution(&text, result.toptab);
		if (migrated)
		{
			toml_free(result);
			size = strlen(text);
			result = toml_parse(text, (int)size);
		}
		if (result.ok)
		{
			char *completed;
			int rewritten = migrated;

			for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
			{
				struct config_value file_value = { 0, 0, 0.0, NULL };
				char *changed;

				if (toml_seek(result.toptab, config_settings[index].name).type == TOML_UNKNOWN)
				{
					/* (not in the file: config_add_missing writes it, as a default) */
					if (config_states[index].origin != _origin_none)
					{
						config_states[index].origin = _origin_none;
						config_origins_changed = 1;
					}
					continue;
				}
				/* (a value of the wrong type is reported, and counts as nobody's: the device value) */
				if (!config_set_from_file(&file_value, &config_settings[index], result.toptab))
					continue;
				changed = config_origin_decide(index, &file_value, text);
				free(file_value.string);
				if (changed)
				{
					free(text);
					text = changed;
					rewritten = 1;
				}
			}
			config_report_unknown_keys(result.toptab);
			platform_log("settings: %s", path);
			completed = config_add_missing(text, result.toptab);
			/* (the folded file written back, so the player's edits and the
			settings screen's land in one place) */
			if ((completed || folded || rewritten) && !config_write_file(path, completed ? completed : text))
				platform_log("settings: cannot write %s", path);
			else if (folded)
				platform_log("settings: folded the repeats out of %s", path);
			free(completed);
		}
		else
		{
			platform_log("config.toml: %s; using the defaults", result.errmsg);
		}
		toml_free(result);
		free(text);
	}
	else
	{
		char *defaults = config_default_text();

		if (defaults && config_write_file(path, defaults))
		{
			platform_log("settings: wrote the defaults to %s", path);
			for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
			{
				if (config_settings[index].platforms & CONFIG_PLATFORM)
					config_origin_set_device(index);
			}
		}
		else
		{
			platform_log("settings: cannot write %s; using the defaults", path);
		}
		free(defaults);
	}
	config_origins_write();

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
		config_set_from_environment(index);
	config_dump();
}

static const struct config_value *config_value(const char *name, enum config_type type)
{
	static const struct config_value none = { 0, 0, 0.0, "" };
	long index;

	pthread_mutex_lock(&config_lock);
	if (!config_loaded)
	{
		config_load();
		config_loaded = 1;
	}
	pthread_mutex_unlock(&config_lock);
	index = config_setting_index(name);
	if (index < 0 || config_settings[index].type != type)
	{
		platform_log("settings: no %s setting %s", type == _config_string ? "string" : "such", name);
		return &none;
	}
	return &config_values[index];
}

/* ---------- writing a setting */

/* the line's key, if it is "key = ..." (after spaces), in key */
static int config_line_key(const char *line, const char *end, const char *key)
{
	size_t length = strlen(key);

	while (line < end && (*line == ' ' || *line == '\t'))
		line++;
	if ((size_t)(end - line) <= length || strncmp(line, key, length) != 0)
		return 0;
	line += length;
	while (line < end && (*line == ' ' || *line == '\t'))
		line++;
	return line < end && *line == '=';
}

/* the section the line opens, if it is "[section]" (after spaces) */
static int config_line_section(const char *line, const char *end, char *section, size_t size)
{
	const char *close;

	while (line < end && (*line == ' ' || *line == '\t'))
		line++;
	if (line >= end || *line != '[')
		return 0;
	close = memchr(line, ']', (size_t)(end - line));
	if (!close || (size_t)(close - line - 1) >= size)
		return 0;
	memcpy(section, line + 1, (size_t)(close - line - 1));
	section[close - line - 1] = 0;
	return 1;
}

/* text (config.toml's) with name's line as line_text ("key = value\n"): its
line there changed, or added at the end of its section (a new section at
the end when it has none), the rest kept as it is. NULL without memory */
static char *config_text_set(const char *text, const char *name, const char *line_text)
{
	const char *dot = strchr(name, '.');
	char section[64], key[64], current[64] = "";
	struct config_text out = { 0 };
	const char *line;
	int written = 0, in_section = 0;

	snprintf(section, sizeof(section), "%.*s", (int)(dot - name), name);
	snprintf(key, sizeof(key), "%s", dot + 1);
	for (line = text ? text : ""; *line;)
	{
		const char *end = line + strcspn(line, "\n");
		const char *next = *end ? end + 1 : end;

		if (config_line_section(line, end, current, sizeof(current)))
		{
			/* (leaving the section without the key: it goes at its end) */
			if (in_section && !written)
			{
				config_append(&out, line_text);
				written = 1;
			}
			in_section = !strcmp(current, section);
		}
		else if (in_section && !written && config_line_key(line, end, key))
		{
			config_append(&out, line_text);
			written = 1;
			line = next;
			continue;
		}
		{
			char *copy = config_copy(line, (size_t)(next - line));

			if (copy)
			{
				config_append(&out, copy);
				free(copy);
			}
		}
		line = next;
	}
	if (!written)
	{
		if (out.length && out.buffer[out.length - 1] != '\n')
			config_append(&out, "\n");
		if (!in_section)
		{
			char header[80];

			snprintf(header, sizeof(header), "\n[%s]\n", section);
			config_append(&out, header);
		}
		config_append(&out, line_text);
	}
	return out.buffer;
}

/* text (config.toml's) without name's line, the rest kept as it is. NULL without memory */
static char *config_text_remove(const char *text, const char *name)
{
	const char *dot = strchr(name, '.');
	char section[64], key[64], current[64] = "";
	struct config_text out = { 0 };
	const char *line;
	int in_section = 0;

	snprintf(section, sizeof(section), "%.*s", (int)(dot - name), name);
	snprintf(key, sizeof(key), "%s", dot + 1);
	for (line = text ? text : ""; *line;)
	{
		const char *end = line + strcspn(line, "\n");
		const char *next = *end ? end + 1 : end;

		if (config_line_section(line, end, current, sizeof(current)))
			in_section = !strcmp(current, section);
		else if (in_section && config_line_key(line, end, key))
		{
			line = next;
			continue;
		}
		{
			char *copy = config_copy(line, (size_t)(next - line));

			if (copy)
			{
				config_append(&out, copy);
				free(copy);
			}
		}
		line = next;
	}
	return out.buffer;
}

/* sets a setting, for now and in config.toml, from its value as text
("true", "60", "1.5", "all"): its line there is changed (or added), the rest
of the file kept as it is. The value is the player's choice from now on
(config.origins), unless it is the device's own value for the setting: then
it is a default again and follows the device's profile, so the menus'
Defaults puts a setting back in the profile's hands */
int config_write(const char *name, const char *value)
{
	const char *dot = strchr(name, '.');
	long index = config_setting_index(name);
	char literal[600], device_literal[600], line_text[700], path[1024];
	size_t size = 0;
	char *text, *out;
	int succeeded;

	if (index < 0 || !dot || (size_t)(dot - name) >= 64 || strlen(value) > 256)
		return 0;
	/* (the file read first, as the other settings are) */
	config_value(name, config_settings[index].type);
	pthread_mutex_lock(&config_lock);
	config_set_from_text(&config_values[index], config_settings[index].type, value);
	config_literal(config_settings[index].type, &config_values[index], literal, sizeof(literal));
	config_literal(config_settings[index].type, &config_states[index].device, device_literal, sizeof(device_literal));
	snprintf(line_text, sizeof(line_text), "%s = %s\n", dot + 1, literal);
	if (!strcmp(literal, device_literal))
	{
		config_origin_set((size_t)index, _origin_default, literal);
		config_states[index].source = config_states[index].profile_count ? _source_profile : _source_default;
	}
	else
	{
		config_origin_set((size_t)index, _origin_chosen, NULL);
		config_states[index].source = _source_file;
	}
	config_path(path, sizeof(path));
	text = config_read_file(path, &size);
	out = config_text_set(text, name, line_text);
	succeeded = out && config_write_file(path, out);
	config_origins_write();
	config_change_count++;
	pthread_mutex_unlock(&config_lock);
	free(out);
	free(text);
	return succeeded;
}

int config_write_boolean(const char *name, int value)
{
	long index = config_setting_index(name);

	return index >= 0 && config_settings[index].type == _config_boolean && config_write(name, value ? "true" : "false");
}

/* a setting's value as text ("true", "60", "1.5", "all"): 0 if there is no
such setting */
int config_explicit(const char *name)
{
	long index = config_setting_index(name);
	if (index < 0) return 0;
	(void)config_value(name, config_settings[index].type);
	return config_states[index].origin == _origin_chosen || config_states[index].source == _source_environment;
}

int config_text(const char *name, char *text, size_t size)
{
	long index = config_setting_index(name);
	const struct config_value *value;

	if (index < 0)
		return 0;
	value = config_value(name, config_settings[index].type);
	switch (config_settings[index].type)
	{
	case _config_boolean:
		snprintf(text, size, "%s", value->boolean ? "true" : "false");
		break;
	case _config_integer:
		snprintf(text, size, "%ld", value->integer);
		break;
	case _config_real:
		snprintf(text, size, "%.15g", value->real);
		break;
	case _config_string:
		snprintf(text, size, "%s", value->string ? value->string : "");
		break;
	}
	return 1;
}

void config_folder(char *path, size_t size)
{
	char file[1024];
	char *separator;

	config_path(file, sizeof(file));
	separator = strrchr(file, '/');
#ifndef HALO_ANDROID
	if (!separator || (strrchr(file, '\\') && strrchr(file, '\\') > separator))
		separator = strrchr(file, '\\');
#endif
	if (separator)
		separator[1] = 0;
	else
		file[0] = 0;
	snprintf(path, size, "%s", file);
}

/* ---------- public code */

char *config_file_read(const char *path, size_t *size)
{
	return config_read_file(path, size);
}

unsigned long config_changes(void)
{
	return config_change_count;
}

/* A profile replacement changes defaults, never player/environment choices.
   The latest complete snapshot is offered at each break, so deferred keys and
   removals naturally follow the newest generation without a second pending list. */
static void config_profile_signature(size_t index, char *text, size_t size)
{
	struct config_setting_state *state = &config_states[index];
	snprintf(text,size,"%s|%s",state->device_literal ? state->device_literal : "",state->variable.text ? state->variable.text : "");
	for (int i=0;i<NUMBER_OF_CONFIG_SCENES;i++)
	{
		size_t used = strlen(text);
		snprintf(text+used,size-used,"|%s|%s",state->scene[i].text ? state->scene[i].text : "",
			state->external[i].text ? state->external[i].text : "");
	}
}

int config_profile_apply_snapshot(const char *snapshot, const char *version, int point, int context)
{
	static const char *const points[] = {"", "level load", "cutscene start", "cutscene end", "local death", "local respawn"};
	struct profile_saved { char signature[1024]; struct config_value value; enum config_source source; };
	struct profile_saved *saved;
	char changes[2048] = "";
	int changed = 0;
	if (!snapshot || point < 1 || point > 5 || (context & CONFIG_PROFILE_FROZEN)) return 0;
	saved = calloc(NUMBER_OF_CONFIG_SETTINGS,sizeof(*saved));
	if (!saved) return 0;
	(void)config_integer("display.render_height");
	pthread_mutex_lock(&config_lock);
	for (size_t index=0;index<NUMBER_OF_CONFIG_SETTINGS;index++)
	{
		struct config_setting_state *state = &config_states[index];
		if (!config_is_profile_setting(index) || !port_profile_apply_allowed(config_settings[index].name,point,context & CONFIG_PROFILE_NETWORKED)) continue;
		config_profile_signature(index,saved[index].signature,sizeof(saved[index].signature));
		saved[index].value = config_values[index];
		saved[index].source = state->source;
		config_values[index].string = NULL;
		for (int layer=0;layer<state->profile_count;layer++)
		{
			free(state->profile[layer].text); free(state->profile[layer].source);
			memset(&state->profile[layer],0,sizeof(state->profile[layer]));
		}
		state->profile_count = 0;
		free(state->variable.text); free(state->variable.source); free(state->variable_value.string);
		memset(&state->variable,0,sizeof(state->variable));
		memset(&state->variable_value,0,sizeof(state->variable_value));
		for (int scene=0;scene<NUMBER_OF_CONFIG_SCENES;scene++)
		{
			free(state->scene[scene].text); free(state->scene[scene].source);
			free(state->scene_value[scene].string);
			free(state->external[scene].text); free(state->external[scene].source);
			free(state->external_value[scene].string);
			memset(&state->scene[scene],0,sizeof(state->scene[scene]));
			memset(&state->scene_value[scene],0,sizeof(state->scene_value[scene]));
			memset(&state->external[scene],0,sizeof(state->external[scene]));
			memset(&state->external_value[scene],0,sizeof(state->external_value[scene]));
		}
	}
	config_profile_load_text(snapshot,point,context & CONFIG_PROFILE_NETWORKED);
	for (size_t index=0;index<NUMBER_OF_CONFIG_SETTINGS;index++)
	{
		struct config_setting_state *state = &config_states[index];
		char after[1024], old_value[600], new_value[600];
		if (!config_is_profile_setting(index) || !port_profile_apply_allowed(config_settings[index].name,point,context & CONFIG_PROFILE_NETWORKED)) continue;
		config_keep_device(index);
		config_profile_signature(index,after,sizeof(after));
		if (saved[index].source == _source_file || saved[index].source == _source_environment)
		{
			free(config_values[index].string);
			config_values[index] = saved[index].value;
			state->source = saved[index].source;
		}
		else
		{
			config_literal(config_settings[index].type,&saved[index].value,old_value,sizeof(old_value));
			config_literal(config_settings[index].type,&config_values[index],new_value,sizeof(new_value));
			if (strcmp(saved[index].signature,after))
			{
				size_t used = strlen(changes);
				snprintf(changes+used,sizeof(changes)-used,"%s%s %s→%s",used ? ", " : "",config_settings[index].name,old_value,new_value);
			}
			free(saved[index].value.string);
		}
		if (strcmp(saved[index].signature,after)) changed++;
	}
	if (changed) config_change_count++;
	pthread_mutex_unlock(&config_lock);
	if (changed) platform_log("profile %.63s applied at %s: %s",version ? version : "unknown",points[point],*changes ? changes : "profile defaults updated (explicit overrides preserved)");
	free(saved);
	return changed;
}

#ifdef HALO_ILP32
int host_device_profile_pending(char *values, unsigned long capacity, char *version, unsigned long size);
void host_device_profile_applied(int point, int networked, int changed, int metalfx, int frame_rate);
#endif
void config_profile_safe_point(int point, int context)
{
#ifdef HALO_ILP32
	char version[64];
	static char snapshot[128 * 256];
	int changed;
	const char *rate;
	char *end;
	long cap;
	if ((context & CONFIG_PROFILE_FROZEN) || point < 1 || point > 5) return;
	if (!host_device_profile_pending(snapshot,sizeof(snapshot),version,sizeof(version))) return;
	changed = config_profile_apply_snapshot(snapshot,version,point,context);
	rate = config_string("display.frame_rate");
	cap = strtol(rate,&end,10);
	if (!strcmp(rate,"unlimited")) cap = -1;
	else if (!*rate || *end || cap < 1 || cap > 1000) cap = 0;
	host_device_profile_applied(point,context & CONFIG_PROFILE_NETWORKED,changed,
		!strcmp(config_string("display.upscaler"),"metalfx"),(int)cap);
#else
	(void)point; (void)context;
#endif
}

/* the setting's value when nobody has set it: its default with the device's
profile over it (what the menus' Defaults shows and writes) */
int config_default(const char *name, char *text, size_t size)
{
	long index = config_setting_index(name);
	const struct config_value *device;

	if (index < 0)
		return 0;
	/* (the profile read first) */
	config_value(name, config_settings[index].type);
	device = &config_states[index].device;
	switch (config_settings[index].type)
	{
	case _config_boolean:
		snprintf(text, size, "%s", device->boolean ? "true" : "false");
		break;
	case _config_integer:
		snprintf(text, size, "%ld", device->integer);
		break;
	case _config_real:
		snprintf(text, size, "%.15g", device->real);
		break;
	case _config_string:
		snprintf(text, size, "%s", device->string ? device->string : "");
		break;
	}
	return 1;
}

/* ---------- scene classes

A device profile may give a few settings another value for a scene class
(the main menu, cutscenes): [scene.menu.display] in its file. That value
stands in for the device's while the scene is that class, and only while
the setting's value comes from its default or the profile: one the player
or the environment set is the value in every scene. */

/* the workload context of the map in force (config_workload_set): 1 while it is variable */
static int config_workload_is_variable;

/* The game-thread sampler shares resolution without config_value's initialization lock. */
static const struct config_value *config_scene_value_cached(size_t index, int scene, int external)
{
	const struct config_setting_state *state = &config_states[index];
	if (state->source == _source_default || state->source == _source_profile)
	{
		if (external)
		{
			if (scene > 0 && scene < NUMBER_OF_CONFIG_SCENES && state->external[scene].text)
				return &state->external_value[scene];
			if (state->external[0].text)
				return &state->external_value[0];
		}
		if (scene > 0 && scene < NUMBER_OF_CONFIG_SCENES && state->scene[scene].text)
			return &state->scene_value[scene];
		if (config_workload_is_variable && state->variable.text)
			return &state->variable_value;
	}
	return &config_values[index];
}

static const struct config_value *config_scene_value(const char *name, enum config_type type, int scene)
{
	const struct config_value *value = config_value(name, type);
	long index = config_setting_index(name);
	int external = 0;
#ifdef HALO_ILP32
	external = host_external_active();
#endif
	if (index >= 0 && config_settings[index].type == type)
		return config_scene_value_cached((size_t)index, scene, external);
	return value;
}

double config_scene_real(const char *name, int scene)
{
	return config_scene_value(name, _config_real, scene)->real;
}

long config_scene_integer(const char *name, int scene)
{
	return config_scene_value(name, _config_integer, scene)->integer;
}

int config_scene_boolean(const char *name, int scene)
{
	return config_scene_value(name, _config_boolean, scene)->boolean;
}

int config_scene_string(const char *name, int scene, char *text, size_t size)
{
	const struct config_value *value = config_scene_value(name, _config_string, scene);
	long index = config_setting_index(name);

	snprintf(text, size, "%s", value->string ? value->string : "");
	/* Context strings are copied rather than returned through the base string API. */
	return index >= 0 && value != &config_values[index];
}

const char *config_scene_name(int scene)
{
	return scene >= 0 && scene < NUMBER_OF_CONFIG_SCENES ? config_scene_names[scene] : "";
}

/* ---------- the workload context

The map in force is predictable (single-player campaign at standard
settings: the profile's own values) or variable (multiplayer, co-op beyond a
small LAN game, more enemies: [variable.display] over them), decided as each
map loads (port/linux/game/scene_class.c). The scene classes lie over either. */

int config_workload_variable(void)
{
	return config_workload_is_variable;
}

int config_workload_set(int variable, const char *reasons)
{
	char changes[512] = "";
	size_t index, length = 0;

	(void)config_boolean("debug.settings_dump");   /* the first load */
	variable = variable != 0;
	if (variable == config_workload_is_variable)
		return 0;
	pthread_mutex_lock(&config_lock);
	config_workload_is_variable = variable;
	config_change_count++;
	pthread_mutex_unlock(&config_lock);
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting_state *state = &config_states[index];
		char literal[600];
		int written;

		if (!state->variable.text || (state->source != _source_default && state->source != _source_profile))
			continue;
		config_literal(config_settings[index].type, variable ? &state->variable_value : &config_values[index], literal,
			sizeof(literal));
		written = snprintf(changes + length, sizeof(changes) - length, "%s%s %s [%s]", length ? ", " : "",
			config_settings[index].name, literal, variable ? state->variable.source : "predictable");
		if (written < 0 || (size_t)written >= sizeof(changes) - length)
			break;
		length += (size_t)written;
	}
	platform_log("workload: %s (%s)%s%s", variable ? "variable" : "predictable", reasons && *reasons ? reasons : "",
		*changes ? ": " : "", changes);
	return 1;
}

void config_profile_settings_text(char *text, size_t size)
{
	size_t index, length = 0;

	(void)config_boolean("debug.settings_dump");   /* the first load */
	if (size)
		text[0] = 0;
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		char literal[600];
		int written;

		if (!config_is_profile_setting(index) || !(config_settings[index].platforms & CONFIG_PLATFORM))
			continue;
		config_literal(config_settings[index].type, config_scene_value(config_settings[index].name,
			config_settings[index].type, CONFIG_SCENE_GAMEPLAY), literal, sizeof(literal));
		written = snprintf(text + length, size - length, "%s = %s\n", config_settings[index].name, literal);
		if (written < 0 || (size_t)written >= size - length)
		{
			text[length] = 0;
			break;
		}
		length += (size_t)written;
	}
}

/* Capture the typed resolver rather than reinterpreting config.toml on the host. */
struct graphics_json { char *text; size_t size, length; int valid; };
static void graphics_append(struct graphics_json *json, const char *format, ...)
{
	if (!json->valid) return;
	va_list arguments; va_start(arguments,format);
	int count = vsnprintf(json->text+json->length,json->size-json->length,format,arguments);
	va_end(arguments);
	if (count < 0 || (size_t)count >= json->size-json->length) json->valid = 0;
	else json->length += (size_t)count;
}
static void graphics_string(struct graphics_json *json, const char *value)
{
	if (!value) value = "";
	if (strlen(value) > CONFIG_GRAPHICS_MAXIMUM_STRING_BYTES) { json->valid = 0; return; }
	graphics_append(json,"\"");
	for (const unsigned char *c = (const unsigned char *)value; *c; c++)
	{
		if (*c < 32) graphics_append(json,"\\u%04x",*c);
		else if (*c == '"' || *c == '\\') graphics_append(json,"\\%c",*c);
		else graphics_append(json,"%c",*c);
	}
	graphics_append(json,"\"");
}
/* printf's decimal separator is locale-sensitive. Normalize it without changing the process locale.
   Validate the resulting JSON number grammar; unsupported formatting fails the whole capture explicitly. */
static void graphics_real(struct graphics_json *json, double value)
{
	char literal[64];
	if (!isfinite(value)) { json->valid = 0; return; }
	int written = snprintf(literal,sizeof(literal),"%.17g",value);
	if (written < 0 || (size_t)written >= sizeof(literal)) { json->valid = 0; return; }
	const char *point = localeconv()->decimal_point;
	char *separator = point && *point ? strstr(literal,point) : NULL;
	if (separator && strcmp(point,".")) {
		size_t length = strlen(point);
		memmove(separator+1,separator+length,strlen(separator+length)+1); *separator = '.';
	}
	const char *cursor = literal;
	if (*cursor == '-') cursor++;
	if (*cursor == '0') cursor++;
	else { if (*cursor < '1' || *cursor > '9') { json->valid = 0; return; } while (*cursor >= '0' && *cursor <= '9') cursor++; }
	if (*cursor == '.') { cursor++; if (*cursor < '0' || *cursor > '9') { json->valid = 0; return; } while (*cursor >= '0' && *cursor <= '9') cursor++; }
	if (*cursor == 'e' || *cursor == 'E') { cursor++; if (*cursor == '+' || *cursor == '-') cursor++; if (*cursor < '0' || *cursor > '9') { json->valid = 0; return; } while (*cursor >= '0' && *cursor <= '9') cursor++; }
	if (*cursor) { json->valid = 0; return; }
	graphics_append(json,"%s%s",literal,strpbrk(literal,".eE") ? "" : ".0");
}

static int graphics_setting(const char *name)
{
	if (!strncmp(name,"display.",8)) return 1;
#define CAPTURE_NAME(key) if (!strcmp(name,key)) return 1;
	GRAPHICS_CAPTURE_DEBUG_KEYS(CAPTURE_NAME)
#undef CAPTURE_NAME
	return 0;
}
int config_graphics_settings_snapshot(int scene, struct game_perf_graphics_snapshot *snapshot)
{
	int external = 0;
	if (!snapshot) return 0;
	memset(snapshot, 0, sizeof(*snapshot));
	/* Initialization and updates belong to ordinary settings access, never this sampler. */
	if (!config_loaded || scene < 0 || scene >= NUMBER_OF_CONFIG_SCENES) return 0;
#ifdef HALO_ILP32
	external = host_external_active();
#endif
	snapshot->schema = 1;
	snapshot->scene = (uint32_t)scene;
	snapshot->configuration_revision = config_changes();
	snapshot->external_output = external != 0;
	for (size_t index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		if (!(setting->platforms & CONFIG_PLATFORM) || !graphics_setting(setting->name)) continue;
		if (snapshot->count == GAME_PERF_GRAPHICS_MAX_KEYS) goto invalid;
		struct game_perf_graphics_value *entry = &snapshot->entries[snapshot->count];
		size_t length = strnlen(setting->name, sizeof(entry->name));
		if (length == sizeof(entry->name)) goto invalid;
		memcpy(entry->name, setting->name, length + 1);
		const struct config_value *value = config_scene_value_cached(index, scene, external);
		switch (setting->type)
		{
		case _config_boolean:
			entry->type = GAME_PERF_GRAPHICS_BOOLEAN;
			entry->value.boolean = value->boolean != 0;
			break;
		case _config_integer:
			entry->type = GAME_PERF_GRAPHICS_INTEGER;
			entry->value.integer = value->integer;
			break;
		case _config_real:
			if (!isfinite(value->real)) goto invalid;
			entry->type = GAME_PERF_GRAPHICS_REAL;
			entry->value.real = value->real;
			break;
		case _config_string:
		{
			const char *text = value->string ? value->string : "";
			length = strnlen(text, sizeof(entry->value.string));
			if (length == sizeof(entry->value.string)) goto invalid;
			entry->type = GAME_PERF_GRAPHICS_STRING;
			memcpy(entry->value.string, text, length + 1);
			break;
		}
		default: goto invalid;
		}
		snapshot->count++;
	}
	snapshot->valid = 1;
	return 1;
invalid:
	snapshot->count = 0;
	return 0;
}

int config_graphics_settings_json(int scene, char *text, size_t size)
{
	struct graphics_json json = { text, size < CONFIG_GRAPHICS_MAXIMUM_BYTES ? size : CONFIG_GRAPHICS_MAXIMUM_BYTES, 0, 1 };
	unsigned count = 0;
	if (!text || !size) return 0;
	text[0] = 0;
	if (scene < 0 || scene >= NUMBER_OF_CONFIG_SCENES) return 0;
	(void)config_boolean("debug.settings_dump");
	int external = 0;
#ifdef HALO_ILP32
	external = host_external_active();
#endif
	graphics_append(&json,"{\"schema\":1,\"scene\":\"%s\",\"configuration_revision\":%lu,\"external_output\":%s,\"values\":{",
		config_scene_name(scene),config_changes(),external ? "true" : "false");
	for (size_t index = 0; json.valid && index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		if (!(setting->platforms & CONFIG_PLATFORM) || !graphics_setting(setting->name)) continue;
		if (++count > CONFIG_GRAPHICS_MAXIMUM_KEYS) { json.valid = 0; break; }
		const struct config_value *value = config_scene_value(setting->name,setting->type,scene);
		graphics_append(&json,"%s",count > 1 ? "," : "");
		graphics_string(&json,setting->name); graphics_append(&json,":");
		switch (setting->type)
		{
		case _config_boolean: graphics_append(&json,"%s",value->boolean ? "true" : "false"); break;
		case _config_integer: graphics_append(&json,"%ld",value->integer); break;
		case _config_real: graphics_real(&json,value->real); break;
		case _config_string: graphics_string(&json,value->string); break;
		}
	}
	graphics_append(&json,"}}");
	if (!json.valid) text[0] = 0;
	return json.valid;
}

int config_scene_describe(int scene, char *text, size_t size)
{
	size_t index, length = 0;

	(void)config_boolean("debug.settings_dump");   /* the first load */
	if (size)
		text[0] = 0;
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS && scene >= 0 && scene < NUMBER_OF_CONFIG_SCENES; index++)
	{
		const struct config_setting_state *state = &config_states[index];
		const struct config_value *value = config_scene_value(config_settings[index].name, config_settings[index].type, scene);
		const char *source = NULL;
		int external = 0;
		char literal[600];
		int written;

		if (state->source != _source_default && state->source != _source_profile)
			continue;
		if (scene > 0 && value == &state->scene_value[scene])
			source = state->scene[scene].source;
		else if (value == &state->external_value[scene])
			source = state->external[scene].source, external = 1;
		else if (value == &state->external_value[0])
			source = state->external[0].source, external = 1;
		else if (value == &state->variable_value)
			source = state->variable.source, external = 2;
		if (!source)
			continue;
		config_literal(config_settings[index].type, value, literal, sizeof(literal));
		written = snprintf(text + length, size - length, "%s%s %s [%s%s]", length ? ", " : "", config_settings[index].name,
			literal, source, external == 1 ? ", external" : external == 2 ? ", variable" : "");
		if (written < 0 || (size_t)written >= size - length)
			break;
		length += (size_t)written;
	}
	return length > 0;
}

int config_boolean(const char *name)
{
	return config_value(name, _config_boolean)->boolean;
}

/* one boolean read again from the file, as at start-up (the device's value
when the file lacks it or does not parse), the environment still winning: for a
setting another process changes while the game runs (the iOS host, from the
Settings app's switch) */
int config_reload_boolean(const char *name)
{
	long index = config_setting_index(name);
	char path[1024];
	size_t size = 0;
	char *text;
	int value;

	(void)config_boolean(name);   /* the first load, defaults and all */
	if (index < 0 || config_settings[index].type != _config_boolean)
		return config_boolean(name);
	config_path(path, sizeof(path));
	text = config_read_file(path, &size);
	pthread_mutex_lock(&config_lock);
	config_set_device((size_t)index);
	config_states[index].source = config_states[index].profile_count ? _source_profile : _source_default;
	if (text)
	{
		toml_result_t result = toml_parse(text, (int)size);

		if (result.ok)
		{
			struct config_value read = {0};
			if (config_set_from_file(&read,&config_settings[index],result.toptab))
			{
				char literal[600];
				config_literal(config_settings[index].type,&read,literal,sizeof(literal));
				if (config_states[index].origin != _origin_default || !config_states[index].origin_literal ||
					strcmp(literal,config_states[index].origin_literal))
				{
					config_values[index].boolean = read.boolean;
					config_states[index].source = _source_file;
				}
				free(read.string);
			}
		}
		toml_free(result);
	}
	config_set_from_environment((size_t)index);
	value = config_values[index].boolean;
	pthread_mutex_unlock(&config_lock);
	free(text);
	return value;
}

long config_integer(const char *name)
{
	return config_value(name, _config_integer)->integer;
}

double config_real(const char *name)
{
	return config_value(name, _config_real)->real;
}

const char *config_string(const char *name)
{
	const char *string = config_value(name, _config_string)->string;

	return string ? string : "";
}
