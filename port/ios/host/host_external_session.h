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
