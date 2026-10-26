/* iOS host services for the ILP32 guest in a native address arena. */
#ifndef HALO_IOS_HOST_H
#define HALO_IOS_HOST_H
#include "host.h"
#include <stdint.h>
extern uintptr_t host_arena;
extern uint64_t host_arena_size;   /* 4 GiB, or the shorter reservation used when that is refused */
extern int host_arena_try_full;    /* tests set 0 to take the shorter reservation */
extern int host_arena_force_unaligned; /* tests set 1 to place it off every multiple of 4 GiB */
/* A guest pointer is an offset into the arena, which need not start at a multiple of 4 GiB. */
static inline void *host_pointer(uint64_t p) { return p ? (void *)(host_arena + (uint32_t)p) : NULL; }
static inline uint32_t guest_pointer(const void *p) { return p ? (uint32_t)((uintptr_t)p - host_arena) : 0; }
uint32_t host_ios_default_framebuffer(void);
struct SDL_Window;
struct SDL_Gamepad;
/* the guest's window (host_sdl.c), for the GPU backend's present (gpu_gl.c) */
struct SDL_Window *host_sdl_window(void);
/* a message box over the game (host_sdl.c; the guest's SDL_ShowSimpleMessageBox
comes here too) */
int host_sdl_show_simple_message_box(unsigned int flags, const char *title, const char *message);
/* a message with one or two buttons over the game, theater mode too
(host_share.m): the index pressed, or -1 with no window scene or off the main
thread (the caller then shows SDL's box); waits */
int host_alert(const char *title, const char *message, const char *first, const char *second);
/* a share sheet is up over the game (host_share.m): the game's input waits */
int host_window_up(void);
#if defined(__OBJC__) && !HALO_NATIVE_MAC
@class UIWindow;
/* a window of its own at the alert level on the game's scene, shown and key */
UIWindow *host_present_window(void);
/* nonzero for a window of host_present_window's (theater mode leaves those be) */
int host_is_present_window(UIWindow *window);
#endif
/* the CAMetalLayer of a Metal window's view (host_sdl.c), or NULL */
void *host_sdl_metal_layer(void);
void host_ios_touch_initialize(void);
void host_ios_prepare_assets(const char *documents);
/* tvOS: receive the player's XISO over the local network into <documents>/maps. */
void host_tv_import(const char *documents);
void host_ios_touch_attach(struct SDL_Window *window);
void host_ios_touch_reset(void);
void host_touch_replay_active(int active);
void host_ios_touch_pause(void);
/* debug.frame_counter (host_frame_counter.m): a label in the window's corner
showing wall time and rendered frames, plus existing frame/CPU/GPU timings;
these are not simulation ticks or replay time */
/* the app in the background (host_lifecycle.m): the game's audio pauses and
the game holds at its next Present until the app is back, since iOS ends an
app that submits GPU work in the background and a closed visionOS window only
backgrounds the app */
void host_lifecycle_install(void);
void host_lifecycle_hold(void);
/* gpu_metal.m (display.frame_deadline): the frame's work has ended; nothing on a Vision Pro */
void gpu_metal_frame_deadline_end(void);
void host_sdl_audio_pause(int paused);
/* the runner's pinned display (HALO_HOST_DISPLAY, host_main.m): the drawable size the guest is told */
void host_sdl_pin_window_pixels(int width, int height);
/* a tvOS profile's built-in output cap (display.output_height, host_output_cap.h): pinned as above, and the
   size gpu_metal.m gives the layer's drawable; host_sdl_output_cap is 0 when there is none */
void host_sdl_cap_output(int width, int height);
void host_sdl_uncap_output(void);
int host_sdl_output_cap(int *width, int *height);
/* the built-in output's own size, before any cap, and the cap last asked for (0: none), whether or not it lowered
   the drawable: a cap at or above the panel's height is asked for and changes nothing */
void host_sdl_output_native(int width, int height, int requested_cap);
void host_sdl_native_output(int *width, int *height);
int host_sdl_output_cap_requested(void);
/* gpu_metal.m: a built-in output cap (0: none) or upscaler (0 bilinear, 1 MetalFX) to change to as the game runs, the
   cap between frames (host_external_commit), a cap below zero forgetting one still waiting; 1 once nothing asked for
   is still waiting. The benchmark's display
   variants use them (host_benchmark.m). */
void gpu_metal_request_output_cap(int cap);
void gpu_metal_output_cap_commit(void);
int gpu_metal_output_cap_settled(void);
/* Game-thread query after GPU initialization, including validation wrappers. */
int host_gpu_metal_active(void);
void gpu_metal_set_upscaler(int metalfx);
int gpu_metal_upscaler(void);
/* display.frame_rate (host_gpu_dispatch.c), any cap or "unlimited" in the Mac
app, a cap under the display's rate on an iPhone, an iPad or an Apple TV:
whether frames come as fast as they are drawn; the cap, 0 for none; and the
wait that holds the cap, called once a frame before it is shown, which
returns the seconds it slept */
int host_frame_rate_unlimited(void);
double host_frame_rate_cap(void);
/* Effective game setting at a safe point: 0 display, -1 unlimited, 1..1000 cap. */
void host_frame_rate_profile(int rate);
/* the most frames a second for the frames from now on: a scene class's rate from the device profile
   (gpu_frame_context, gpu_metal.m) when one is in force, else host_frame_rate_cap's; 0 for none. What the
   frame pacer spaces frames for and judges them against (host_frame_pacer.c) */
double host_frame_rate_target(void);
/* whether Low Power Mode's frame rate is in force for the frames from now on (low_power.h, host_gpu_dispatch.c:
   host_frame_rate_cap and host_frame_rate_target then hold its halved rate) */
int host_frame_rate_low_power(void);
void host_frame_rate_scene(double rate);
double host_frame_rate_limit(void);
/* a benchmark case's display.frame_rate for the runs from now on (frames a second, as the configuration's would be
   taken on this device), or 0 to go back to the configuration's */
void host_frame_rate_override(double rate);
void host_frame_counter_start(int fixed_timestep, int metal);
void host_frame_counter_show(unsigned long frame);
void host_frame_counter_stop(void);
void host_frame_counter_sample(double interval, double cpu, double gpu);
int host_ios_gamepads(uint32_t *out, int capacity);
void host_spatial_rumble_hold(int hold);
int host_spatial_sense_menus_held(void);
int host_spatial_owns_sense_controllers(void);
int host_sdl_controller_aim_owns(uint32_t id);
int host_ios_gamepad_type(struct SDL_Gamepad *pad);
int host_ios_gamepad_axis(struct SDL_Gamepad *pad, int axis);
int host_ios_gamepad_button(struct SDL_Gamepad *pad, int button);
int host_linux_errno(int native_errno);
/* posix_net.c: the system link interface's address and netmask, network byte order; 0 if none */
int host_ranked_interface(uint32_t *address, uint32_t *netmask);
void host_install_signal_handlers(void);
uint32_t host_guest_invoke(uintptr_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uintptr_t arena);
uint32_t host_guest_on_stack(uintptr_t function, uint32_t argument, uintptr_t arena, void *stack_top);
/* host_tv.m: a menu's text field asked for with tvOS's dialog (on: shown,
with its title, text and whether it is a password's; off: dismissed; 0 if
it cannot be shown yet, to be asked again), and its text once it is done: 1
and the text, 2 if canceled, 0 while neither (each result is given once) */
int host_tv_text_prompt(int on, const char *title, const char *text, int masked);
int host_tv_text_result(char *buffer, uint32_t size);
void host_tv_text_prompt_tick(void);
#endif
