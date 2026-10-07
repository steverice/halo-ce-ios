/*
HOST_SDL.C

SDL3 on behalf of the guest (guest/runtime/guest_sdl.c). SDL objects are
64-bit pointers, which the guest cannot hold; it gets small handles into the
table here instead.

The guest calls these on its own threads, whose stacks remain inside the
native guest arena (host_thread.c). SDL's audio thread is the exception: it has no guest stack,
so the audio callback is handed to a thread that has one.
*/

#include <TargetConditionals.h>
#if TARGET_OS_VISION
#include "host_theater.h"
#include "host_stereo.h"
#endif
#include "host.h"
#include "host_config.h"

#include <SDL3/SDL.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include "ios_host.h"
#include "host_join_link.h"

#define HANDLE_COUNT 256

enum handle_type
{
	_handle_free,
	_handle_window,
	_handle_context,
	_handle_gamepad,
	_handle_audio,
};

struct handle
{
	int type;
	void *object;
};

static struct handle handles[HANDLE_COUNT];
static pthread_mutex_t handle_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t handle_new(int type, void *object)
{
	uint32_t index;

	if (!object)
		return 0;
	pthread_mutex_lock(&handle_lock);
	/* an object that already has a handle keeps it */
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == type && handles[index].object == object)
		{
			pthread_mutex_unlock(&handle_lock);
			return index;
		}
	}
	for (index = 1; index < HANDLE_COUNT; index++)
	{
		if (handles[index].type == _handle_free)
		{
			handles[index].type = type;
			handles[index].object = object;
			pthread_mutex_unlock(&handle_lock);
			return index;
		}
	}
	pthread_mutex_unlock(&handle_lock);
	host_logf(HOST_LOG_ERROR, "out of SDL handles");
	return 0;
}

static void *handle_get(uint32_t handle, int type)
{
	void *object = NULL;

	if (handle == 0 || handle >= HANDLE_COUNT)
		return NULL;
	pthread_mutex_lock(&handle_lock);
	if (handles[handle].type == type)
		object = handles[handle].object;
	pthread_mutex_unlock(&handle_lock);
	return object;
}

/* ---------- general */

int host_sdl_init(uint32_t flags)
{
	return SDL_Init((SDL_InitFlags)flags);
}

int host_sdl_set_hint(const char *name, const char *value)
{
	return SDL_SetHint(name, value);
}

void host_sdl_get_error(char *buffer, uint32_t size)
{
	SDL_strlcpy(buffer, SDL_GetError(), size);
}

void host_sdl_scancode_name(int32_t scancode, char *buffer, uint32_t size)
{
	SDL_strlcpy(buffer, SDL_GetScancodeName((SDL_Scancode)scancode), size);
}

int32_t host_sdl_scancode_from_name(const char *name)
{
	return (int32_t)SDL_GetScancodeFromName(name);
}

int64_t host_sdl_ticks(void)
{
	return (int64_t)SDL_GetTicks();
}

int64_t host_sdl_thread_id(void)
{
	return (int64_t)SDL_GetCurrentThreadID();
}

/* ---------- video */

/* the Metal window's view (display.renderer = "metal"), whose layer the Metal
backend draws into (gpu_metal.m) */
static SDL_MetalView metal_view;

uint32_t host_sdl_create_window(const char *title, int width, int height, int64_t flags)
{
	SDL_Window *window = SDL_CreateWindow(title, width, height, (SDL_WindowFlags)flags);

	/* a GL window gets its view, and then its touch controls, from
	host_sdl_gl_create_context. A Metal window has no context, so both
	happen here, in the same order: SDL_Metal_CreateView replaces the
	window's root view, which would drop controls attached before it */
	if (window && (flags & SDL_WINDOW_METAL))
	{
		metal_view = SDL_Metal_CreateView(window);
		if (!metal_view)
			host_logf(HOST_LOG_ERROR, "SDL_Metal_CreateView failed: %s", SDL_GetError());
		host_ios_touch_attach(window);
	}
	return handle_new(_handle_window, window);
}

void *host_sdl_metal_layer(void)
{
	return metal_view ? SDL_Metal_GetLayer(metal_view) : NULL;
}

static int pinned_width, pinned_height;

void host_sdl_pin_window_pixels(int width, int height)
{
	pinned_width = width;
	pinned_height = height;
}

void host_sdl_window_size_in_pixels(uint32_t window, int *width, int *height)
{
	SDL_Window *object = handle_get(window, _handle_window);

	*width = 0;
	*height = 0;
#if TARGET_OS_VISION
	/* theater mode: the game renders for the screen in the room, not for its
	hidden window (host_theater.m); in head-tracked stereo, for the eyes'
	views (host_stereo.m) */
	if (object && host_theater_active())
	{
		if (!host_stereo_picture_size(width, height))
			host_theater_picture_size(width, height);
		return;
	}
#endif
	if (object && pinned_width)
	{
		*width = pinned_width;
		*height = pinned_height;
		return;
	}
	if (object)
		SDL_GetWindowSizeInPixels(object, width, height);
}

int host_sdl_set_relative_mouse(uint32_t window, int enabled)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_SetWindowRelativeMouseMode(object, enabled != 0) : 0;
}

int host_sdl_gl_set_attribute(int attribute, int value)
{
	/* EAGL exposes ES 3.0; capabilities are queried from the created context. */
	if (attribute == SDL_GL_CONTEXT_MINOR_VERSION) value = 0;
	return SDL_GL_SetAttribute((SDL_GLAttr)attribute, value);
}

uint32_t host_sdl_gl_create_context(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);
	SDL_GLContext context = object ? SDL_GL_CreateContext(object) : NULL;
	if (context) host_ios_touch_attach(object);
	return handle_new(_handle_context, context);
}

int host_sdl_gl_make_current(uint32_t window, uint32_t context)
{
	return SDL_GL_MakeCurrent(handle_get(window, _handle_window), handle_get(context, _handle_context));
}

int host_sdl_gl_set_swap_interval(int interval)
{
	return SDL_GL_SetSwapInterval(interval);
}

int host_sdl_gl_swap_window(uint32_t window)
{
	SDL_Window *object = handle_get(window, _handle_window);

	return object ? SDL_GL_SwapWindow(object) : 0;
}

/* ---------- events */

/* host_main.m: a second window scene's start, undone (see there); and its
test */
void host_extra_scene_poll(void);
void host_extra_scene_test(void);

/* (the system's keyboard, below) */
static void text_input_update(void);

int host_sdl_poll_event(void *event)
{
	SDL_Event host_event;
	static int test_extra_scene = -1;
	static unsigned long polls;

	host_extra_scene_poll();
#if TARGET_OS_VISION
	if (test_extra_scene < 0)
	{
		char value[16];

		host_config_string("debug.test_extra_scene", "false", value, sizeof(value));
		test_extra_scene = !strcmp(value, "true");
	}
	/* a few seconds in, once the game's window is up */
	if (test_extra_scene && ++polls == 600)
		host_extra_scene_test();
	/* and later, that the host still reads config.toml */
	if (test_extra_scene && polls == 1800)
		host_logf(HOST_LOG_INFO, "debug.test_extra_scene: config.toml's display.theater_distance reads %.2f",
			host_config_real("display.theater_distance", -1.0));
	/* debug.test_theater_reopen: the immersive space closed, then opened again
	from the window's button */
	{
		static int test_theater_reopen = -1;
		static unsigned long theater_polls;

		if (test_theater_reopen < 0)
		{
			char value[16];

			host_config_string("debug.test_theater_reopen", "false", value, sizeof(value));
			test_theater_reopen = !strcmp(value, "true");
		}
		if (test_theater_reopen)
		{
			theater_polls++;
			if (theater_polls == 900)
				host_theater_test_close();
			if (theater_polls == 3600)
				host_theater_test_reopen();
		}
	}
#else
	(void)test_extra_scene;
	(void)polls;
#endif
	text_input_update();
	/* (a malformed join link's message, between frames: host_join_link.c) */
	host_join_link_poll();
	if (!SDL_PollEvent(&host_event))
		return 0;
	/* the layouts agree except for the pointers of text, drop and user
	events, which the guest does not read */
	memcpy(event, &host_event, sizeof(host_event));
	return 1;
}

/* ---------- gamepads */

int host_sdl_get_gamepads(uint32_t *ids, int capacity)
{
	return host_ios_gamepads(ids, capacity);
}

uint32_t host_sdl_open_gamepad(uint32_t id)
{
	SDL_Gamepad *gamepad = SDL_OpenGamepad((SDL_JoystickID)id);

	if (gamepad)
		host_logf(HOST_LOG_INFO, "gamepad %u: %s (type %d, %04x:%04x)", (unsigned)id, SDL_GetGamepadName(gamepad),
			(int)SDL_GetGamepadType(gamepad), SDL_GetGamepadVendor(gamepad), SDL_GetGamepadProduct(gamepad));
	return handle_new(_handle_gamepad, gamepad);
}

uint32_t host_sdl_gamepad_from_id(uint32_t id)
{
	return handle_new(_handle_gamepad, SDL_GetGamepadFromID((SDL_JoystickID)id));
}

int host_sdl_gamepad_axis(uint32_t gamepad, int axis)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? host_ios_gamepad_axis(object, axis) : 0;
}

int host_sdl_gamepad_button(uint32_t gamepad, int button)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? host_ios_gamepad_button(object, button) : 0;
}

int host_sdl_gamepad_type(uint32_t gamepad)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? host_ios_gamepad_type(object) : SDL_GAMEPAD_TYPE_UNKNOWN;
}

int host_sdl_rumble_gamepad(uint32_t gamepad, uint32_t low, uint32_t high, uint32_t milliseconds)
{
	SDL_Gamepad *object = handle_get(gamepad, _handle_gamepad);

	return object ? SDL_RumbleGamepad(object, (Uint16)low, (Uint16)high, milliseconds) : 0;
}

/* ---------- audio */

/* SDL holds its stream lock while calling audio_callback. The guest worker
must stage its PCM here: calling SDL_PutAudioStreamData from that worker would
wait for the lock held by the callback that is waiting for the worker. Only
the original SDL callback thread submits the staged samples to the stream. */
struct audio_binding
{
	uint32_t handle;
	uint32_t callback;
	uint32_t userdata;
	pthread_mutex_t lock;
	pthread_cond_t requested;
	pthread_cond_t done;
	int pending;
	int additional;
	int total;
	unsigned char *samples;
	int length;
	int capacity;
	SDL_AudioFormat format;
	int logged_output;
};

static __thread struct audio_binding *audio_worker_binding;

static void *audio_thread(void *context)
{
	struct audio_binding *binding = context;

	audio_worker_binding = binding;
	pthread_mutex_lock(&binding->lock);
	for (;;)
	{
		int additional, total;

		while (!binding->pending)
			pthread_cond_wait(&binding->requested, &binding->lock);
		additional = binding->additional;
		total = binding->total;
		pthread_mutex_unlock(&binding->lock);
		host_call_guest(binding->callback, binding->userdata, binding->handle, (uint32_t)additional, (uint32_t)total);
		pthread_mutex_lock(&binding->lock);
		binding->pending = 0;
		pthread_cond_signal(&binding->done);
	}
	return NULL;
}

static void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
	struct audio_binding *binding = userdata;

	pthread_mutex_lock(&binding->lock);
	binding->length = 0;
	binding->additional = additional;
	binding->total = total;
	binding->pending = 1;
	pthread_cond_signal(&binding->requested);
	while (binding->pending)
		pthread_cond_wait(&binding->done, &binding->lock);
	if (binding->length && !SDL_PutAudioStreamData(stream, binding->samples, binding->length))
		host_logf(HOST_LOG_ERROR, "cannot submit audio: %s", SDL_GetError());
	if (!binding->logged_output && binding->format == SDL_AUDIO_F32)
	{
		const float *samples = (const float *)binding->samples;
		float peak = 0.0f;
		for (int i = 0; i < binding->length / (int)sizeof(float); ++i)
			peak = SDL_max(peak, SDL_fabsf(samples[i]));
		if (peak > 0.0001f)
		{
			host_logf(HOST_LOG_INFO, "audio output active: %d PCM bytes, peak %.4f", binding->length, peak);
			binding->logged_output = 1;
		}
	}
	pthread_mutex_unlock(&binding->lock);
}

/* the open audio streams, which host_sdl_audio_pause stops while the app is
in the background (host_lifecycle.m) */
static SDL_AudioStream *audio_streams[4];

void host_sdl_audio_pause(int paused)
{
	size_t index;

	for (index = 0; index < SDL_arraysize(audio_streams); index++)
		if (audio_streams[index])
		{
			if (paused)
				SDL_PauseAudioStreamDevice(audio_streams[index]);
			else
				SDL_ResumeAudioStreamDevice(audio_streams[index]);
		}
}

uint32_t host_sdl_open_audio_stream(uint32_t device, const void *spec, uint32_t callback, uint32_t userdata)
{
	struct audio_binding *binding = SDL_calloc(1, sizeof(*binding));
	SDL_AudioStream *stream;

	if (!binding)
		return 0;
	binding->callback = callback;
	binding->userdata = userdata;
	binding->format = ((const SDL_AudioSpec *)spec)->format;
	pthread_mutex_init(&binding->lock, NULL);
	pthread_cond_init(&binding->requested, NULL);
	pthread_cond_init(&binding->done, NULL);
	stream = SDL_OpenAudioDeviceStream((SDL_AudioDeviceID)device, spec,
		callback ? audio_callback : NULL, binding);
	if (!stream)
	{
		pthread_cond_destroy(&binding->done);
		pthread_cond_destroy(&binding->requested);
		pthread_mutex_destroy(&binding->lock);
		SDL_free(binding);
		return 0;
	}
	/* the host's audio device, one of the host inputs that reaches game state (the guest mixes
	on its callback's cadence): compared between runners by tools/mac_run.py compare-inputs */
	{
		SDL_AudioSpec device_spec;
		int device_frames = 0;
		if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &device_spec, &device_frames))
			host_logf(HOST_LOG_INFO, "audio device: %d Hz, %d channels, format 0x%x, %d sample frames",
				device_spec.freq, device_spec.channels, (unsigned)device_spec.format, device_frames);
	}
	/* the device starts paused, so no callback can run before this */
	binding->handle = handle_new(_handle_audio, stream);
	{
		size_t index;

		for (index = 0; index < SDL_arraysize(audio_streams); index++)
			if (!audio_streams[index])
			{
				audio_streams[index] = stream;
				break;
			}
	}
	if (callback && host_native_thread_create(audio_thread, binding, 256 * 1024) != 0)
		host_fatal("cannot start the audio thread");
	return binding->handle;
}

int host_sdl_put_audio_stream_data(uint32_t stream, const void *data, int length)
{
	struct audio_binding *binding = audio_worker_binding;

	if (binding && binding->handle == stream)
	{
		/* The callback is asleep until this worker signals completion. Copy
		   now because the guest's mix buffer is on its stack and is reused. */
		if (length < 0 || (!data && length) || length > INT_MAX - binding->length)
			return SDL_SetError("invalid guest audio buffer");
		int needed = binding->length + length;
		if (needed > binding->capacity)
		{
			int capacity = SDL_max(needed, 65536);
			void *samples = SDL_realloc(binding->samples, (size_t)capacity);
			if (!samples)
				return SDL_OutOfMemory();
			binding->samples = samples;
			binding->capacity = capacity;
		}
		if (length)
			SDL_memcpy(binding->samples + binding->length, data, (size_t)length);
		binding->length = needed;
		return 1;
	}
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	return object ? SDL_PutAudioStreamData(object, data, length) : 0;
}

int host_sdl_resume_audio_stream_device(uint32_t stream)
{
	SDL_AudioStream *object = handle_get(stream, _handle_audio);

	return object ? SDL_ResumeAudioStreamDevice(object) : 0;
}

/* ---------- the clipboard (internet play's invite links) */

int host_sdl_set_clipboard_text(const char *text)
{
	return SDL_SetClipboardText(text) ? 1 : 0;
}

void host_sdl_get_clipboard_text(char *buffer, uint32_t size)
{
	char *text = SDL_GetClipboardText();

	SDL_strlcpy(buffer, text ? text : "", size);
	SDL_free(text);
}

/* ---------- the system's keyboard (a menu's text field: a join code, a
password, a game's name)

On a phone, a tablet or a headset the keyboard is the system's, on screen,
up while the field is edited. SDL turns what is typed on it into key presses
(SDL_uikitviewcontroller.m's SDL_SendKeyboardUnicodeKey, shift held for a
capital), which the guest reads as a hardware keyboard's; its Return is a
Return key press. On an Apple TV SDL's hidden text field brings no keyboard
up, so the field is asked for with tvOS's own dialog (host_tv.m), whose text
the guest takes whole (host_sdl_text_result). Either is begun in
host_sdl_poll_event, on the main thread, which UIKit needs. */

static int text_input_wanted, text_input_on, text_input_masked;
static char text_input_title[64], text_input_text[512];

void host_sdl_text_input(int on, const char *title, const char *text, int masked)
{
	if (on)
	{
		SDL_strlcpy(text_input_title, title ? title : "", sizeof(text_input_title));
		SDL_strlcpy(text_input_text, text ? text : "", sizeof(text_input_text));
		text_input_masked = masked != 0;
	}
	text_input_wanted = on != 0;
}

int host_sdl_text_result(char *buffer, uint32_t size)
{
#if TARGET_OS_TV
	return host_tv_text_result(buffer, size);
#else
	(void)buffer;
	(void)size;
	return 0;
#endif
}

static void text_input_update(void)
{
	SDL_Window *window;

#if TARGET_OS_TV
	host_tv_text_prompt_tick();
#endif
	if (text_input_wanted == text_input_on || !(window = host_sdl_window()))
		return;
#if TARGET_OS_TV
	if (host_tv_text_prompt(text_input_wanted, text_input_title, text_input_text, text_input_masked))
		text_input_on = text_input_wanted;
#else
	text_input_on = text_input_wanted;
	if (text_input_on)
	{
		SDL_StartTextInput(window);
		host_logf(HOST_LOG_INFO, "keyboard: up for a text field");
	}
	else
	{
		SDL_StopTextInput(window);
	}
#endif
}

/* the guest's window (host_sdl_create_window makes one), for the GPU backend's
present in the host (gpu_gl.c, host_gpu.c) */
SDL_Window *host_sdl_window(void)
{
	unsigned index;

	for (index = 1; index < HANDLE_COUNT; index++)
		if (handles[index].type == _handle_window)
			return handles[index].object;
	return NULL;
}

/* UIKit renders to a view framebuffer, rather than framebuffer zero. */
uint32_t host_ios_default_framebuffer(void)
{
	SDL_Window *window = host_sdl_window();

	return window ? (uint32_t)SDL_GetNumberProperty(SDL_GetWindowProperties(window),
		SDL_PROP_WINDOW_UIKIT_OPENGL_FRAMEBUFFER_NUMBER, 0) : 0;
}

/* ---------- messages for the player (sdl_platform.c, guest_sdl.c) */

/* Android's short toast; iOS has none, so the message isn't shown */
int host_sdl_show_toast(const char *message, int duration, int gravity, int x, int y)
{
	(void)message;
	(void)duration;
	(void)gravity;
	(void)x;
	(void)y;
	return 0;
}

int host_sdl_show_simple_message_box(unsigned int flags, const char *title, const char *message)
{
	return SDL_ShowSimpleMessageBox((SDL_MessageBoxFlags)flags, title, message, host_sdl_window());
}
