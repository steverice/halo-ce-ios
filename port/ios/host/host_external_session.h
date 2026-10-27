/* Scene ownership is independent of audio routing. A late disconnect from a
   replaced scene must not tear down the new receiver's output. */
#pragma once
#include <stdint.h>
struct host_external_session { uintptr_t owner; int negotiating, pending; };
static inline void host_external_session_connect(struct host_external_session *s,uintptr_t owner) {
    /* Recover locally while the replacement scene negotiates, even if the
       old disconnect has not reached a screen commit yet. */
    s->owner=owner;s->negotiating=1;s->pending=1;
}
static inline int host_external_session_disconnect(struct host_external_session *s,uintptr_t owner) {
    if(!owner || s->owner!=owner)return 0;
    s->owner=0;s->negotiating=0;s->pending=1;return 1;
}
static inline void host_external_session_accept(struct host_external_session *s) {
    s->negotiating=0;s->pending=1;
}
static inline void host_external_session_complete(struct host_external_session *s) { s->pending=0; }
/* The scene accessory the system offers a connected display through is evaluated when it is registered and when the
   view controller appears. A display plugged in while the app is already running (the accessory registered at launch)
   can be left mirrored without the registration ever becoming available. When a display has been seen (a second
   screen, or a video audio route) for a while with no scene and no availability, the registration is made again,
   at most twice per connection. */
struct host_external_refresh { double seen_at; int attempts; };
static inline int host_external_refresh_due(struct host_external_refresh *r,int display_seen,int available,
    int connected,double now) {
    if(!display_seen || available || connected) {r->seen_at=0;r->attempts=0;return 0;}
    if(r->seen_at==0) {r->seen_at=now;return 0;}
    if(now-r->seen_at<1.5 || r->attempts>=2)return 0;
    r->attempts++;r->seen_at=now;return 1;
}
