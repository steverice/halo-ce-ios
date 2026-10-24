/*
GUEST_HOST.H

The host services the guest imports (the guest's view; the host defines
them in port/ios/host). Every name here must also be listed in
port/ios/host_imports.list, which generates the import stubs.

Parameter types follow the rules in halo_guest_abi.h: 32-bit values are
int or unsigned int, 64-bit values long long, and pointers are passed as
they are. The GPU backend's (host_gpu_*) are gpu.h's own: fixed-width
types, and structs both sides lay out alike (tools/ios_test.py pins the
layouts).
*/

#ifndef __GUEST_HOST_H
#define __GUEST_HOST_H

#include "gpu.h"
#include "../../shader_observations.h"
#include "halo_stereo.h"
#include "halo_spatial.h"

/* Whether the current output is an external display (context values are never saved). */
int host_external_active(void);
void host_external_commit(int allow_pause);

/* Only gameplay touch controls are suppressed; native dialogs remain interactive. */
void host_touch_replay_active(int active);

/* ---------- process */

/* performs a Linux system call on the guest's behalf, converting the
structures whose layout differs; returns the raw result (-errno on failure) */
long long host_syscall(long long number, long long a, long long b, long long c,
	long long d, long long e, long long f);

/* Stable runtime log priorities; translated by the native host. */
void host_log(int priority, const char *text);
void host_abort(const char *reason) __attribute__((noreturn));
void host_exit(int code) __attribute__((noreturn));
/* 1 after main_game_render finishes, 0 before main_pregame_render draws.
   The host waits for the following presentation before accepting trial data. */
void host_data_updates_frame_ready(int ready);
/* the benchmark's CPU split (host_benchmark.h): main_loop's marks around its
ticks (0 and 1, with how many ran) and its render (2 and 3), and the D3D8
device's translation time within the render (4, in nanoseconds) */
void host_frame_cpu_phase(int phase, int ticks);

/* 1 once for each memory warning since the last call, else 0 (the texture
   cache drops what was not drawn lately), and the memory left to the app in
   MB (-1: not known) */
int host_memory_pressure_poll(void);
int host_memory_available_megabytes(void);

int host_external_cycle(int direction);
int host_external_force(const char *mode);
void host_external_mode_label(char *text, unsigned long size);

/* ---------- the device benchmark (port/ios/host/host_benchmark.m; the guest's
side is port/linux/src/benchmark_suite.c) */

/* starts a suite ("quick" or "full"), or carries on the one the journal says
was cut off, on a device whose display has drawable_lines lines and whose
settings in force are the rest; 0 if the benchmark cannot run now (the log
says why) */
int host_benchmark_start(const char *mode, int drawable_lines, int base_lines, int anisotropy, int shadow_size,
	int model_lod_max);
/* the next run as text ("scene=a10-crew map=a10 difficulty=hard recording=a10-bench
lead=0 seconds=0 lines=540 anisotropy=4 shadow=256 lod=1"), after the pause
between runs; 0 once the suite is over and its report written */
/* 3 waits for an output switch; 4 asks the guest to restart with its committed output sizing. */
int host_benchmark_next(char *text, unsigned int size);
void host_benchmark_phase(int phase);
/* the measured window opens (1) or closes (0); the render size it drew at */
void host_benchmark_window(int open, int width, int height, int opened_tick);
/* Bounded resolved graphics JSON, published only during an open measured window. */
void host_benchmark_graphics(const char *json);
/* the run is over: a status by name (benchmark_plan.h) and a note for the report */
void host_benchmark_end(const char *status, const char *note);
/* A fatal guest assertion seals the active run and writes a failed partial report immediately. */
void host_benchmark_fault(const char *note);
/* debug.benchmark_offscreen: the setting, and whether debug.benchmark_suite
names a suite; 1 when the host honors it (host_benchmark.h) */
int host_benchmark_offscreen_request(int setting, int suite_named);
/* a presented run inside an offscreen suite, and the game after it: the frames shown in real time (on) or
not; 1 when the guest's clock follows (host_benchmark.h) */
int host_benchmark_presented(int on);
/* the host's errno on this thread, after a call to a host function */
int host_errno(void);

/* ---------- threads

The guest's thread pointer (its struct pthread) is kept by the host for
each thread. */

unsigned int host_get_tp(void);
void host_set_tp(unsigned int thread);
/* starts a host thread with a stack in guest memory that calls the image's
__guest_thread_start(thread); returns 0 or an errno value */
int host_thread_create(unsigned int thread, unsigned int stack_size);

/* ---------- memory write tracking (port/linux/src/memory_watch.c) */

void host_memory_watch_initialize(void);
void host_memory_watch_protect(unsigned int address, unsigned int size);
unsigned int host_memory_watch_generation(unsigned int address, unsigned int size);
unsigned int host_memory_watch_serial(void);
void host_memory_watch_prepare_write(unsigned int address, unsigned int size);
void host_memory_watch_forget(unsigned int address, unsigned int size);

/* ---------- SDL (guest/runtime/guest_sdl.c)

Window, context, gamepad and audio stream objects are small integer
handles on this side. */

int host_sdl_init(unsigned int flags);
int host_sdl_set_hint(const char *name, const char *value);
void host_sdl_get_error(char *buffer, unsigned int size);
long long host_sdl_ticks(void);
long long host_sdl_thread_id(void);
unsigned int host_sdl_create_window(const char *title, int width, int height, long long flags);
void host_sdl_window_size_in_pixels(unsigned int window, int *width, int *height);
/* Current native Mac display maximum; 0 keeps other hosts' menu behavior. */
int host_sdl_display_max_fps(void);
int host_sdl_set_relative_mouse(unsigned int window, int enabled);
int host_sdl_gl_set_attribute(int attribute, int value);
unsigned int host_sdl_gl_create_context(unsigned int window);
int host_sdl_gl_make_current(unsigned int window, unsigned int context);
int host_sdl_gl_set_swap_interval(int interval);
int host_sdl_gl_swap_window(unsigned int window);
int host_sdl_poll_event(void *event);
int host_sdl_set_clipboard_text(const char *text);
void host_sdl_get_clipboard_text(char *buffer, unsigned int size);
/* the system's keyboard up for a menu's text field (its title, its text,
and whether it is a password's), or put away (xinput_sdl.c's
platform_text_field). On a phone, a tablet or a headset its keys come as key
events; an Apple TV asks with a dialog of its own (host_tv.m), whose text
host_sdl_text_result gives once it is done: 1 and the text, 2 if it was
canceled, 0 while there is none */
void host_sdl_text_input(int on, const char *title, const char *text, int masked);
int host_sdl_text_result(char *buffer, unsigned int size);
/* the keyboard's keys by name (the controls' bindings, xinput_sdl.c) */
void host_sdl_scancode_name(int scancode, char *buffer, unsigned int size);
int host_sdl_scancode_from_name(const char *name);
int host_sdl_show_toast(const char *message, int duration, int gravity, int x, int y);
int host_sdl_show_simple_message_box(unsigned int flags, const char *title, const char *message);
/* a message with one or two buttons (first, then second, or NULL): the
index of the one pressed, or -1 */
int host_sdl_show_message_box(unsigned int flags, const char *title, const char *message, const char *first,
	const char *second);
int host_sdl_get_gamepads(unsigned int *ids, int capacity);
unsigned int host_sdl_open_gamepad(unsigned int id);
unsigned int host_sdl_gamepad_from_id(unsigned int id);
int host_sdl_gamepad_axis(unsigned int gamepad, int axis);
int host_sdl_gamepad_button(unsigned int gamepad, int button);
int host_sdl_gamepad_type(unsigned int gamepad);
int host_sdl_rumble_gamepad(unsigned int gamepad, unsigned int low, unsigned int high, unsigned int milliseconds);
/* callback: void (*)(void *userdata, unsigned int stream, int additional, int total),
called on the audio thread */
unsigned int host_sdl_open_audio_stream(unsigned int device, const void *spec, unsigned int callback, unsigned int userdata);
int host_sdl_put_audio_stream_data(unsigned int stream, const void *data, int length);
int host_sdl_resume_audio_stream_device(unsigned int stream);

/* ---------- the GPU backend (gpu.h), which runs in the host from step 4: each
gpu_* function is the host_gpu_* import of the same signature (guest_gpu.c;
tools/ios_bridges.py maps the import back to gpu_*) */

gpu_texture host_gpu_texture_create(const struct gpu_texture_description *description);
void host_gpu_texture_upload(gpu_texture texture, uint32_t face, uint32_t level, const void *data, uint32_t size);
void host_gpu_texture_copy_level(gpu_texture source, gpu_texture destination, uint32_t level);
void host_gpu_texture_generate_mipmaps(gpu_texture texture, uint32_t base_level);
void host_gpu_texture_destroy(gpu_texture texture);
uint32_t host_gpu_texture_read(gpu_texture texture, void *pixels, uint32_t size);
gpu_buffer host_gpu_buffer_create(uint32_t size);
void host_gpu_buffer_write(gpu_buffer buffer, uint32_t offset, uint32_t size, const void *data, uint32_t flags);
void host_gpu_stream_reserve(uint32_t vertex_bytes, uint32_t index_bytes);
uint32_t host_gpu_stream(uint32_t kind, const void *data, uint32_t size, gpu_buffer *buffer);
gpu_shader host_gpu_shader_create(uint32_t stage, const char *source);
void host_gpu_clear(const struct gpu_clear *clear, const struct gpu_rect *rectangles, uint32_t count);
uint32_t host_gpu_draw(const struct gpu_draw *draw, const struct gpu_constant_store *constants, const struct gpu_uniforms *uniforms);
void host_gpu_visibility_begin(void);
void host_gpu_visibility_end(uint32_t slot);
uint32_t host_gpu_visibility_result(uint32_t slot, uint32_t *samples);
void host_gpu_flush(void);
void host_gpu_upscale(gpu_texture source, gpu_texture destination, const struct gpu_rect *area, uint32_t anti_alias);
uint32_t host_gpu_anti_alias_prepare(uint32_t method, const char *smaa_shader, const void *area_texels, const void *search_texels);
void host_gpu_anti_alias(uint32_t method, gpu_texture target, const struct gpu_rect *rect);
uint32_t host_gpu_vertical_blank_wait(void);
uint32_t host_gpu_companion_size(uint32_t *width, uint32_t *height);
void host_gpu_companion_set(gpu_texture target);
void host_gpu_frame_start(void);
uint32_t host_gpu_present(gpu_texture back_buffer);
uint32_t host_gpu_present_stereo(const struct gpu_stereo_present *present);
uint32_t host_gpu_call_count_take(void);
uint32_t host_gpu_warm_list_read(const char *name, char *text, uint32_t size);
void host_gpu_warm_begin(void);
void host_gpu_pipeline_warm(const struct gpu_pipeline_description *description);
void host_gpu_warm_end(void);
uint32_t host_gpu_pipeline_built_take(struct gpu_pipeline_description *description);
void host_gpu_frame_context(uint32_t scene_class, uint32_t frame_rate, const char *level);
uint32_t host_gpu_compiles_outstanding(void);
void host_gpu_compile_progress(uint32_t action, struct gpu_compile_progress *progress);
void host_gpu_initialize(uint32_t flags, struct gpu_capabilities *capabilities);

/* ---------- stereo (port/linux/game/stereo.c; the host's port/ios/host/host_stereo.m)

struct halo_stereo_frame crosses as it is (tools/ios_test.py pins its layout). */

/* HEAD mode, at the game's frame begin: opens the Compositor's next frame and
fills the eyes and the head's turn from it; eye_count stays 0 without one */
void host_stereo_frame(struct halo_stereo_frame *frame);

/* Fixed-width spatial samples and feedback cross the host/guest boundary unchanged. */
void host_spatial_sample(int kind, struct halo_spatial_sample *sample);
void host_spatial_feedback(const struct halo_spatial_feedback *feedback);

/* ---------- Bink movies (guest_bink.c; the host's port/ios/host/host_bink.c)

One movie at a time, decoded in the host (port/runtime/bink_decoder.h, whose
functions these are). A host built without the decoder opens nothing. */

/* 32-bit members only: the same layout on both sides */
struct host_bink_information
{
	unsigned int width, height;
	unsigned int frame_count;
	unsigned int frame_rate, frame_rate_divisor;
	unsigned int audio_rate, audio_channels;
};

/* path is a host path; 1 if the movie opened */
int host_bink_open(const char *path, struct host_bink_information *information);
void host_bink_close(void);
int host_bink_decode_frame(void);
/* the last frame decoded as X8R8G8B8, into guest memory */
void host_bink_copy_frame(void *pixels, int pitch, int height);
int host_bink_audio_read(short *samples, int capacity, unsigned int until);
void host_bink_audio_skip(unsigned int position);

#endif

uint32_t host_shader_observation_activate(const char *map_name, int suppressed, char *map_key, uint32_t map_key_size);
void host_shader_observation_append(uint32_t token, const struct shader_observation *descriptor);
int host_shader_observation_recording(void);
