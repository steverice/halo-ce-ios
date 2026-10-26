/*
PORT_PROFILE_KEYS.H

The settings a device profile (profiles/, read by port/ios/host/host_profile.c)
may set, each with its type and the values it accepts. This is the whole
list: a profile that names any other setting fails tools/profile_check.py,
and the loader ignores the line and says so in the log. Controls, sound,
network, paths, comfort and layout choices and debug switches are the
player's on every device and never belong here; measurement-only
debug.dynamic_resolution_shadow may also be set by a validation profile.
Other exceptions are
input.aim_loss_grace, a hardware property (how long a device's hand tracking
may drop out before the aim leaves the hand) and not a choice, and
display.texture_idle_seconds (how long the texture cache keeps an undrawn
texture, which a device's memory decides), and display.clutter_distance (how
far bodies, loose items and garbage are drawn, which a device's speed decides),
and network.player_capacity (the most players the device holds at 30 frames a
second, which the host's default and the server browser's marks follow; it
refuses no one).

An X-macro list, included with all seven macros defined:

	PROFILE_BOOLEAN(name)
	PROFILE_INTEGER(name, minimum, maximum)
	PROFILE_REAL(name, minimum, maximum)
	PROFILE_STRING(name, choices)
	PROFILE_SCENE(name)
	PROFILE_VARIABLE(name)
	PROFILE_EXTERNAL(name)

PROFILE_EXTERNAL names a setting that may vary while driving an external output.
Display output height is profile-only: valid in the external context, and in a
tvOS profile's own [display], where it caps the built-in output's drawable
(port/ios/host/host_output_cap.h). No scene class may set it.

PROFILE_SCENE names a setting above that a profile may also set for one
scene class ([scene.menu.display], [scene.cutscene.display]): the ones the
game can change between frames while it runs. display.frame_rate as a
scene class's value is the most frames a second while the scene is that
class, on every device: each frame stays up the whole number of refreshes
that gives it ("30" at 60 Hz: two). The setting itself caps the Mac app
at any rate, and an iPhone, an iPad or an Apple TV under the display's
highest rate ("60" on a 120 Hz screen); a Vision Pro ignores it.

PROFILE_VARIABLE names a setting a profile may also set for the variable
workload context ([variable.display]): multiplayer maps, campaign co-op
(networked or split screen) and campaign with more enemies than 1x, decided
as each map loads (port/linux/game/scene_class.c) and kept for the whole map.
Single-player campaign at 1x is the predictable context: the profile's own
values. The scene classes' values lie over either. Because the context only
changes at a map load, it may hold settings that reallocate targets (shadow
maps, anti-aliasing), which scene classes may not. display.dynamic_resolution
and its floor follow it through the host (host_dynamic_resolution_scene);
display.render_height is dynamic resolution's ceiling in both contexts.

choices is the accepted values separated by '|'; "#" among them accepts a
whole number from 30 to 1000 (a frame rate: a profile cannot hold the game
below 30). A number outside its range or a string that is
no choice is refused, not clamped: a profile is reviewed text, and a value
the game would change silently is a mistake in it.

tools/profile_check.py reads this file as text: keep one entry a line.
*/

PROFILE_STRING("display.renderer", "gl|metal")
PROFILE_INTEGER("display.render_height", 0, 4320)
PROFILE_INTEGER("display.output_height", 0, 4320)
PROFILE_REAL("display.render_scale", 0.25, 1.0)
PROFILE_STRING("display.upscaler", "bilinear|metalfx")
PROFILE_INTEGER("display.anisotropic_filtering", 1, 16)
PROFILE_INTEGER("display.dynamic_light_limit", 0, 128)
PROFILE_INTEGER("display.specular_light_limit", 0, 128)
PROFILE_INTEGER("display.shadow_map_size", 128, 1024)
PROFILE_STRING("display.effect_resolution", "xbox|half|full")
PROFILE_STRING("display.model_lod", "max|auto")
PROFILE_REAL("display.lod_scale", 0.5, 4.0)
PROFILE_BOOLEAN("display.compressed_textures")
PROFILE_BOOLEAN("display.high_res_hud")
PROFILE_BOOLEAN("display.high_res_text")
PROFILE_BOOLEAN("display.upscaled_textures")
PROFILE_STRING("display.anti_aliasing", "off|fxaa|smaa")
PROFILE_BOOLEAN("display.foveation")
PROFILE_REAL("display.render_quality", 0.1, 1.0)
PROFILE_INTEGER("display.frame_repeat", 0, 3)
PROFILE_REAL("display.hud_resolution", 0.5, 1.0)
PROFILE_STRING("display.frame_pacing", "off|tick|refresh")
PROFILE_STRING("display.frame_rate", "display|unlimited|#")
PROFILE_STRING("display.async_shaders", "auto|on|off")
PROFILE_BOOLEAN("display.interpolation")
PROFILE_BOOLEAN("display.dynamic_resolution")
PROFILE_INTEGER("display.dynamic_resolution_floor", 240, 2160)
PROFILE_REAL("input.aim_loss_grace", 0.0, 5.0)
PROFILE_REAL("display.texture_idle_seconds", 10.0, 600.0)
PROFILE_REAL("display.clutter_distance", 0.0, 200.0)
PROFILE_INTEGER("network.player_capacity", 2, 128)

PROFILE_SCENE("display.render_scale")
PROFILE_SCENE("display.render_height")
PROFILE_SCENE("display.frame_rate")

PROFILE_VARIABLE("display.frame_rate")
PROFILE_VARIABLE("display.render_height")
PROFILE_VARIABLE("display.render_scale")
PROFILE_VARIABLE("display.anti_aliasing")
PROFILE_VARIABLE("display.shadow_map_size")
PROFILE_VARIABLE("display.dynamic_resolution")
PROFILE_VARIABLE("display.dynamic_resolution_floor")

PROFILE_EXTERNAL("display.output_height")
PROFILE_EXTERNAL("display.render_height")
PROFILE_EXTERNAL("display.render_scale")
PROFILE_EXTERNAL("display.anti_aliasing")

PROFILE_BOOLEAN("debug.dynamic_resolution_shadow")
