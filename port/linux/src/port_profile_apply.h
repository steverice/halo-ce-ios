/* Apache-2.0. Every admitted profile setting has an explicit apply policy.
   Hot fields do not rebuild shaders or persistent resources. Level fields
   may allocate/reload resources; session fields require a new GPU/session. */
#pragma once
#include <string.h>
enum port_profile_apply_class { PORT_PROFILE_UNKNOWN, PORT_PROFILE_HOT, PORT_PROFILE_LEVEL, PORT_PROFILE_SESSION, PORT_PROFILE_LOCAL_INPUT };
static inline int port_profile_apply_class(const char *name)
{
	static const struct { const char *name; int policy; } settings[] = {
		{"debug.dynamic_resolution_shadow", PORT_PROFILE_SESSION}, /* read once when the first frame is observed */
		{"display.dynamic_resolution", PORT_PROFILE_SESSION}, /* read once with the shadow switch */
		{"display.dynamic_resolution_floor", PORT_PROFILE_SESSION},
		{"display.renderer", PORT_PROFILE_SESSION}, /* GPU backend/window selected before guest launch */
		{"display.render_height", PORT_PROFILE_HOT},
		{"display.output_height", PORT_PROFILE_SESSION},
		{"display.render_scale", PORT_PROFILE_HOT},
		{"display.upscaler", PORT_PROFILE_LEVEL},
		{"display.anisotropic_filtering", PORT_PROFILE_LEVEL},
		{"display.dynamic_light_limit", PORT_PROFILE_HOT},
		{"display.specular_light_limit", PORT_PROFILE_HOT},
		{"display.shadow_map_size", PORT_PROFILE_LEVEL}, /* atlas target recreation */
		{"display.shadow_minimum_size", PORT_PROFILE_HOT},
		{"display.effect_resolution", PORT_PROFILE_LEVEL},
		{"display.model_lod", PORT_PROFILE_HOT},
		{"display.lod_scale", PORT_PROFILE_HOT},
		{"display.compressed_textures", PORT_PROFILE_SESSION}, /* GPU initialization capability */
		{"display.high_res_hud", PORT_PROFILE_LEVEL},
		{"display.high_res_text", PORT_PROFILE_LEVEL},
		{"display.upscaled_textures", PORT_PROFILE_LEVEL},
		{"display.anti_aliasing", PORT_PROFILE_LEVEL},
		{"display.foveation", PORT_PROFILE_SESSION}, /* compositor layer creation */
		{"display.render_quality", PORT_PROFILE_SESSION},
		{"display.frame_repeat", PORT_PROFILE_SESSION},
		{"display.hud_resolution", PORT_PROFILE_LEVEL},
		{"display.frame_pacing", PORT_PROFILE_SESSION}, /* session clock setup */
		{"display.frame_rate", PORT_PROFILE_HOT},
		{"display.async_shaders", PORT_PROFILE_SESSION},
		{"display.interpolation", PORT_PROFILE_LEVEL}, /* interpolation touches tick presentation */
		{"input.aim_loss_grace", PORT_PROFILE_LOCAL_INPUT},
		{"display.texture_idle_seconds", PORT_PROFILE_HOT},
		{"display.clutter_distance", PORT_PROFILE_HOT},
		{"network.player_capacity", PORT_PROFILE_HOT},
	};
	for (unsigned i=0; i<sizeof(settings)/sizeof(settings[0]); i++)
		if (!strcmp(name,settings[i].name)) return settings[i].policy;
	return PORT_PROFILE_UNKNOWN;
}
static inline int port_profile_apply_allowed(const char *name, int point, int networked)
{
	if (point < 1 || point > 5) return 0;
	int policy = port_profile_apply_class(name);
	return policy == PORT_PROFILE_HOT || (point == 1 &&
		(policy == PORT_PROFILE_LEVEL || (policy == PORT_PROFILE_LOCAL_INPUT && !networked)));
}
