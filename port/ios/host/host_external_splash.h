/* The launch menu shows a splash on a connected external display instead of a
   mirror, until the game attaches and takes the scene. Scene identity is
   opaque; UIKit and the game call these on the main thread. */
#pragma once
#include <stdint.h>
struct host_external_splash {
    uintptr_t parked;   /* connected scene waiting for the game */
    uintptr_t shown;    /* scene currently showing the splash */
    int enabled;        /* menu is up (or was) and the renderer is Metal */
};
/* The launch menu appeared. Returns the parked scene that needs a splash now, or 0. */
static inline uintptr_t host_external_splash_menu(struct host_external_splash *s,int metal) {
    s->enabled=metal?1:0;
    return s->enabled && s->parked && s->shown!=s->parked ? s->parked : 0;
}
/* A scene connected before the game attached. Returns 1 when it should show the splash. */
static inline int host_external_splash_connect(struct host_external_splash *s,uintptr_t scene) {
    s->parked=scene;
    if(s->enabled && scene) { s->shown=scene;return 1; }
    s->shown=0;return 0;
}
static inline void host_external_splash_shown(struct host_external_splash *s,uintptr_t scene) { s->shown=scene; }
/* A scene went away. Returns 1 when it was the parked one; the phone menu is untouched either way. */
static inline int host_external_splash_disconnect(struct host_external_splash *s,uintptr_t scene) {
    if(!scene || s->parked!=scene)return 0;
    s->parked=0;s->shown=0;return 1;
}
/* The game attached. Returns the parked scene to hand over (its splash window is retired first), or 0. */
static inline uintptr_t host_external_splash_attach(struct host_external_splash *s) {
    uintptr_t scene=s->parked;
    s->parked=0;s->shown=0;s->enabled=0;
    return scene;
}
