/* Exercise the real SDL stream lock and the host's cross-thread PCM handoff.
   A stalled callback is a failure (alarm), not an indefinitely hanging test. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifndef HOST_SDL_SOURCE
#define HOST_SDL_SOURCE "../host/host_sdl.c"
#endif
#include HOST_SDL_SOURCE

static int panel_open;
int host_window_up(void) { return 0; }
int host_audio_output_up(void) { return panel_open; }

static unsigned produced;
static unsigned callbacks;

void host_logf(int priority, const char *format, ...)
{
    (void)priority;
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

void host_fatal(const char *format, ...)
{
    (void)format;
    abort();
}

int host_native_thread_create(void *(*function)(void *), void *arg, size_t size)
{
    (void)size;
    pthread_t thread;
    int error = pthread_create(&thread, NULL, function, arg);
    if (!error) pthread_detach(thread);
    return error;
}

uint32_t host_call_guest(uint32_t callback, uint32_t userdata, uint32_t stream,
                        uint32_t additional, uint32_t total)
{
    (void)callback; (void)userdata; (void)total;
    ++callbacks;
    while (additional)
    {
        float buffer[256];
        unsigned bytes = SDL_min(additional, sizeof(buffer));
        for (unsigned i = 0; i < bytes / sizeof(float); ++i)
            buffer[i] = (float)(produced++ % 251) / 251.0f;
        assert(host_sdl_put_audio_stream_data(stream, buffer, (int)bytes));
        /* The bridge must copy before the guest reuses its stack buffer. */
        memset(buffer, 0, sizeof(buffer));
        additional -= bytes;
    }
    assert(!host_sdl_put_audio_stream_data(stream, NULL, -1));
    return 0;
}

int main(void)
{
    SDL_Event press={.type=SDL_EVENT_KEY_DOWN};
    assert(!held_back(&press));
    panel_open=1;
    assert(held_back(&press));
    press.type=SDL_EVENT_KEY_UP;
    assert(!held_back(&press));
    panel_open=0;
    alarm(10);
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    assert(SDL_Init(SDL_INIT_AUDIO));
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, 48000};
    uint32_t handle = host_sdl_open_audio_stream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, 1, 0);
    assert(handle);
    SDL_AudioStream *stream = handle_get(handle, _handle_audio);
    /* Simplified streams own their device and cannot be unbound. Keeping
       the device paused lets this test pull samples deterministically. */
    assert(SDL_AudioStreamDevicePaused(stream));
    assert(SDL_SetAudioStreamFormat(stream, &spec, &spec));

    unsigned consumed = 0;
    for (int iteration = 0; iteration < 100; ++iteration)
    {
        /* The large request also exercises PCM buffer growth beyond 64 KB. */
        float output[32768];
        int bytes = iteration == 50 ? sizeof(output) : 4096;
        assert(SDL_GetAudioStreamData(stream, output, bytes) == bytes);
        for (int i = 0; i < bytes / (int)sizeof(float); ++i)
            assert(output[i] == (float)(consumed++ % 251) / 251.0f);
    }
    assert(callbacks == 100);
    assert(consumed == produced);
    /* Route replacement keeps the guest handle and PCM source format. */
    host_sdl_audio_route_changed();
    SDL_AudioStream *replacement=handle_get(handle,_handle_audio);
    assert(replacement && replacement!=stream);
    SDL_PauseAudioStreamDevice(replacement);
    SDL_AudioSpec source;
    assert(SDL_GetAudioStreamFormat(replacement,&source,NULL));
    assert(source.freq==spec.freq && source.format==spec.format && source.channels==spec.channels);
    assert(audio_bindings[0]->handle==handle);
    assert(SDL_AudioStreamDevicePaused(replacement));
    host_sdl_audio_route_changed();
    replacement=handle_get(handle,_handle_audio);
    assert(SDL_AudioStreamDevicePaused(replacement));
    assert(SDL_GetAudioStreamFormat(replacement,&source,NULL));
    assert(source.freq==spec.freq);
    SDL_ResumeAudioStreamDevice(replacement);
    host_sdl_audio_route_changed();
    assert(!SDL_AudioStreamDevicePaused(handle_get(handle,_handle_audio)));
    alarm(0);
    printf("PASS: %u SDL callbacks, %u exact PCM samples; no cross-thread stream deadlock\n", callbacks, consumed);
    return 0;
}
