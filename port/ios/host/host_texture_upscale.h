/* The on-device upscaler's lifecycle (host_texture_upscale.m), driven by the original-texture worker's host signals,
   the import screen and the game's loading screen and pause. */
#pragma once
#include <stddef.h>
/* the device profile's display.upscaled_textures, read before the import screen can need it (main thread) */
void host_texture_upscale_resolve_profile(const char *root);
void host_texture_upscale_init(const char *root);
int host_texture_upscale_enabled(void);
void host_texture_upscale_scan(void);
void host_texture_upscale_map_will_load(const char *name);
void host_texture_upscale_game_started(int is_game);
void host_texture_upscale_paused(int paused);
/* The import phase: runs the whole game's work, calling progress with the work units done and in all (input
   texels) about ten times a second until it is done or progress returns 0 (Skip: the work goes on in the
   background). 0 when the generator is off on this device, so there is no phase. */
typedef int (*host_texture_upscale_progress)(void *context, unsigned long long done, unsigned long long total);
int host_texture_upscale_prepare_import(const char *root, host_texture_upscale_progress progress, void *context);
/* The loading screen's wait before a level (texture_upscale_schedule.h's enum texture_upscale_level_wait). */
int host_texture_upscale_level_begin(const char *name);
int host_texture_upscale_level_poll(unsigned long long *done, unsigned long long *total);
void host_texture_upscale_level_end(int skipped);
int host_texture_upscale_level_ready(void);
/* the Settings app's line: empty, "Not used on this device" or "N of M ready" */
void host_texture_upscale_status(char *text, size_t size);
/* for the import's continued processing (host_texture_upscale_continued.m): the whole game's work units ready and in
   all; whether a pump is queued or running; stop (1) or resume (0) the work while the app is in the background */
void host_texture_upscale_progress_units(unsigned long long *done, unsigned long long *total);
int host_texture_upscale_busy(void);
void host_texture_upscale_background_stop(int stop);
/* host_texture_upscale_continued.m: the import phase began (1) or ended (0); does nothing where the API is missing */
void host_texture_upscale_continued_phase(int began);
/* the Settings app's delete: through the worker when the upscaler runs (between textures, the manifest emptied with
   the files), else at once; 0 on success or when scheduled */
int host_texture_upscale_delete_cache(const char *directory);
/* the texture cache's folder for this data root (Library/Caches/texture-cache on iPhone, iPad and Vision Pro) */
void host_texture_cache_path(const char *root, char *text, size_t size);
void host_texture_cache_directory(char *text, unsigned long size);
