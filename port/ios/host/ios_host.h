/* iOS host services for the ILP32 guest in an aligned native address arena. */
#ifndef HALO_IOS_HOST_H
#define HALO_IOS_HOST_H
#include "host.h"
#include <stdint.h>
extern uintptr_t host_arena;
static inline void *host_pointer(uint64_t p) { return p ? (void *)(host_arena | (uint32_t)p) : NULL; }
static inline uint32_t guest_pointer(const void *p) { return (uint32_t)(uintptr_t)p; }
uint32_t host_ios_default_framebuffer(void);
struct SDL_Window;
struct SDL_Gamepad;
/* the guest's window (host_sdl.c), for the GPU backend's present (gpu_gl.c) */
struct SDL_Window *host_sdl_window(void);
/* the CAMetalLayer of a Metal window's view (host_sdl.c), or NULL */
void *host_sdl_metal_layer(void);
void host_ios_touch_initialize(void);
void host_ios_prepare_assets(const char *documents);
/* tvOS: receive the player's XISO over the local network into <documents>/maps. */
void host_tv_import(const char *documents);
void host_ios_touch_attach(struct SDL_Window *window);
void host_ios_touch_reset(void);
/* debug.frame_counter (host_frame_counter.m): a label in the window's corner
showing the renderer, the frame number and the game time, 1/30 s a frame with
fixed_timestep, else the time since the first frame */
/* the app in the background (host_lifecycle.m): the game's audio pauses and
the game holds at its next Present until the app is back, since iOS ends an
app that submits GPU work in the background and a closed visionOS window only
backgrounds the app */
void host_lifecycle_install(void);
void host_lifecycle_hold(void);
void host_sdl_audio_pause(int paused);
/* the runner's pinned display (HALO_HOST_DISPLAY, host_main.m): the drawable size the guest is told */
void host_sdl_pin_window_pixels(int width, int height);
/* display.frame_rate (host_gpu_dispatch.c), which only the Mac app honors:
whether frames come as fast as they are drawn; the cap, 0 for none; and the
wait that holds the cap, called once a frame before it is shown, which
returns the seconds it slept */
int host_frame_rate_unlimited(void);
double host_frame_rate_cap(void);
double host_frame_rate_limit(void);
void host_frame_counter_start(int fixed_timestep, int metal);
void host_frame_counter_show(unsigned long frame);
int host_ios_gamepads(uint32_t *out, int capacity);
int host_ios_gamepad_type(struct SDL_Gamepad *pad);
int host_ios_gamepad_axis(struct SDL_Gamepad *pad, int axis);
int host_ios_gamepad_button(struct SDL_Gamepad *pad, int button);
int host_linux_errno(int native_errno);
void host_install_signal_handlers(void);
uint32_t host_guest_invoke(uintptr_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uintptr_t arena);
uint32_t host_guest_on_stack(uintptr_t function, uint32_t argument, uintptr_t arena, void *stack_top);
#endif
