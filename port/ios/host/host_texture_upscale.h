/* Debug generator lifecycle, driven by the original-texture worker's host signals. */
#pragma once
void host_texture_upscale_init(const char *root);
void host_texture_upscale_scan(void);
void host_texture_upscale_map_will_load(const char *name);
void host_texture_upscale_game_started(int is_game);
