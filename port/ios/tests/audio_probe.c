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

void host_crash(const char *format, ...)
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
    /* Route replacement keeps the guest handle and PCM source format. (The old stream is freed before the new one
       is made, so the new one may have its address: the device it is bound to tells them apart.) */
    SDL_AudioDeviceID old_device=SDL_GetAudioStreamDevice(stream);
    host_sdl_audio_route_changed();
    SDL_AudioStream *replacement=handle_get(handle,_handle_audio);
    assert(replacement && SDL_GetAudioStreamDevice(replacement)!=old_device);
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
    /* A reopen must reach the physical device (on iOS a new AudioQueue on the new route): a stream opened
       while the old one was still bound joined the old device. The sample-frames hint applies only when a
       physical device opens, so the new size shows the device itself was closed and opened. */
    {
        SDL_AudioSpec device_spec;
        int before=0,after=0;
        assert(SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(handle_get(handle,_handle_audio)),&device_spec,&before));
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES,before==2048?"1536":"2048");
        host_sdl_audio_route_changed();
        assert(SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(handle_get(handle,_handle_audio)),&device_spec,&after));
        assert(after!=before && (after==2048 || after==1536));
        assert(!SDL_AudioStreamDevicePaused(handle_get(handle,_handle_audio)));
        SDL_ResetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
    }
    /* A stream that should be playing with no callback for 1.5 s is stalled; a paused one never is, and a resume
       starts its clock again. (Checked at times ahead of the clock: the dummy device's callbacks keep coming.) */
    {
        Uint64 now=SDL_GetTicksNS();
        assert(!host_sdl_audio_stalled(now));
        assert(host_sdl_audio_stalled(now+10*SDL_NS_PER_SECOND));
        SDL_PauseAudioStreamDevice(handle_get(handle,_handle_audio));
        assert(!host_sdl_audio_stalled(now+10*SDL_NS_PER_SECOND));
        atomic_store(&audio_bindings[0]->last_callback_ns,now-10*SDL_NS_PER_SECOND);
        assert(host_sdl_resume_audio_stream_device(handle));
        assert(!host_sdl_audio_stalled(SDL_GetTicksNS()));
    }
    alarm(0);
    printf("PASS: %u SDL callbacks, %u exact PCM samples; no cross-thread stream deadlock\n", callbacks, consumed);
    return 0;
}
