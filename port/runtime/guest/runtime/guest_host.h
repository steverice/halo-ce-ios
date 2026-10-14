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
#include "halo_stereo.h"

/* ---------- process */

/* performs a Linux system call on the guest's behalf, converting the
structures whose layout differs; returns the raw result (-errno on failure) */
long long host_syscall(long long number, long long a, long long b, long long c,
	long long d, long long e, long long f);

/* Stable runtime log priorities; translated by the native host. */
void host_log(int priority, const char *text);
void host_abort(const char *reason) __attribute__((noreturn));
void host_exit(int code) __attribute__((noreturn));
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
uint32_t host_gpu_vertical_blank_wait(void);
uint32_t host_gpu_present(gpu_texture back_buffer);
uint32_t host_gpu_present_stereo(const struct gpu_stereo_present *present);
uint32_t host_gpu_call_count_take(void);
uint32_t host_gpu_warm_list_read(const char *name, char *text, uint32_t size);
void host_gpu_warm_begin(void);
void host_gpu_pipeline_warm(const struct gpu_pipeline_description *description);
void host_gpu_warm_end(void);
uint32_t host_gpu_pipeline_built_take(struct gpu_pipeline_description *description);
void host_gpu_frame_context(uint32_t scene_class, uint32_t frame_rate, const char *level);
void host_gpu_initialize(uint32_t flags, struct gpu_capabilities *capabilities);

/* ---------- stereo (port/linux/game/stereo.c; the host's port/ios/host/host_stereo.m)

struct halo_stereo_frame crosses as it is (tools/ios_test.py pins its layout). */

/* HEAD mode, at the game's frame begin: opens the Compositor's next frame and
fills the eyes and the head's turn from it; eye_count stays 0 without one */
void host_stereo_frame(struct halo_stereo_frame *frame);

#endif
