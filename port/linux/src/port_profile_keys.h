/*
PORT_PROFILE_KEYS.H

The settings a device profile (profiles/, read by port/ios/host/host_profile.c)
may set, each with its type and the values it accepts. This is the whole
list: a profile that names any other setting fails tools/profile_check.py,
and the loader ignores the line and says so in the log. Controls, sound,
network, paths, comfort and layout choices and debug switches are the
player's on every device and never belong here.

An X-macro list, included with the four macros defined:

	PROFILE_BOOLEAN(name)
	PROFILE_INTEGER(name, minimum, maximum)
	PROFILE_REAL(name, minimum, maximum)
	PROFILE_STRING(name, choices)
	PROFILE_SCENE(name)

PROFILE_SCENE names a setting above that a profile may also set for one
scene class ([scene.menu.display], [scene.cutscene.display]): the ones the
game can change between frames while it runs. display.frame_rate as a
scene class's value is the most frames a second while the scene is that
class, on every device: each frame stays up the whole number of refreshes
that gives it ("30" at 60 Hz: two). The setting itself is still the Mac
app's cap.

choices is the accepted values separated by '|'; "#" among them accepts a
whole number from 30 to 1000 (a frame rate: a profile cannot hold the game
below 30). A number outside its range or a string that is
no choice is refused, not clamped: a profile is reviewed text, and a value
the game would change silently is a mistake in it.

tools/profile_check.py reads this file as text: keep one entry a line.
*/

PROFILE_STRING("display.renderer", "gl|metal")
PROFILE_INTEGER("display.render_height", 0, 4320)
PROFILE_REAL("display.render_scale", 0.25, 1.0)
PROFILE_STRING("display.upscaler", "bilinear|metalfx")
PROFILE_INTEGER("display.anisotropic_filtering", 1, 16)
PROFILE_INTEGER("display.shadow_map_size", 128, 1024)
PROFILE_BOOLEAN("display.effect_resolution")
PROFILE_STRING("display.mirror_resolution", "half|full")
PROFILE_STRING("display.model_lod", "max|auto")
PROFILE_REAL("display.lod_scale", 0.5, 4.0)
PROFILE_BOOLEAN("display.compressed_textures")
PROFILE_BOOLEAN("display.high_res_hud")
PROFILE_BOOLEAN("display.high_res_text")
PROFILE_BOOLEAN("display.upscaled_textures")
PROFILE_BOOLEAN("display.foveation")
PROFILE_REAL("display.render_quality", 0.1, 1.0)
PROFILE_INTEGER("display.frame_repeat", 0, 3)
PROFILE_REAL("display.hud_resolution", 0.5, 1.0)
PROFILE_STRING("display.frame_pacing", "off|tick|refresh")
PROFILE_STRING("display.frame_rate", "display|unlimited|#")
PROFILE_STRING("display.async_shaders", "auto|on|off")
PROFILE_BOOLEAN("display.interpolation")

PROFILE_SCENE("display.render_scale")
PROFILE_SCENE("display.frame_rate")
