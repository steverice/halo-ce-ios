/* External output lifecycle. UIKit and the game call these on the main thread.
   A pending output becomes current only at the guest's screen commit. */
#pragma once
#include <stddef.h>
struct host_external_snapshot {
    int external, width, height, refresh_hertz;
    unsigned generation;
    char name[128], reason[32];
    struct { int width, height; } modes[64];
    int mode_count;
};
void host_external_install(void);
/* The launch menu is up: show the splash on an external display until the game attaches. */
void host_external_menu_splash(void);
void host_external_attach(void *sdl_window, int metal);
void host_external_poll(void);
void host_external_commit(int allow_pause);
int host_external_active(void);
int host_external_size(int *width, int *height);
void *host_external_phone_layer(void);
void *host_external_screen(void);
/* the UIWindowScene the renderer presents in (its display's clock drives frame pacing); NULL if unknown */
void *host_external_scene(void);
double host_external_companion_dim(void);
void host_external_snapshot(struct host_external_snapshot *out);
int host_external_cycle(int direction);
int host_external_force(const char *mode);
int host_external_ready(void);
void host_external_mode_label(char *text, size_t size);
/* gpu_metal.m: drains the old output before exchanging layers. */
int gpu_metal_output(void *layer, void *screen, int width, int height);
/* host_sdl.c: close and reopen the output streams (and so the device) after an audio route change. */
void host_sdl_audio_route_changed(void);
/* host_sdl.c: a stream that should be playing has had no callback for over 1.5 s (now: SDL_GetTicksNS). */
int host_sdl_audio_stalled(unsigned long long now);
/* host_sdl.c: every stream's stall clock starts now. */
void host_sdl_audio_touch(void);
