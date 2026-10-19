#pragma once
#include <SDL3/SDL.h>
/* The shared guest event pump consumes QUIT, including a Cocoa window close. */
static inline void host_macos_normalize_event(SDL_Event *event) {
    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        event->type = SDL_EVENT_QUIT;
}
