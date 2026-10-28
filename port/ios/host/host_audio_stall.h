/* When to reopen a sound device that has stopped asking for sound (host_external_poll): a stream that should be
   playing with no callback for 1.5 s (host_sdl_audio_stalled), never during a system interruption (a call, Siri,
   an alarm: the system has stopped the queue on purpose and resumes it when the interruption ends). The first
   reopen comes at once, the next after 3, 6 and 12 s, then every 30 s while the device stays stopped; the waits
   start over once callbacks come again or an interruption ends. Times in nanoseconds. */
#pragma once
#include <stdint.h>
struct host_audio_stall { uint64_t reopened; int reopens; };
static inline uint64_t host_audio_stall_wait(int reopens) {
    static const uint64_t waits[]={0,3,6,12};
    return (reopens<4?waits[reopens]:30)*1000000000ull;
}
/* interrupted: a system interruption is under way; stalled: host_sdl_audio_stalled's answer (not asked while
   interrupted). Returns 1 when the device should be reopened now. */
static inline int host_audio_stall_due(struct host_audio_stall *state,uint64_t now,int interrupted,int stalled) {
    if(interrupted || !stalled) {state->reopens=0;return 0;}
    if(state->reopens && now-state->reopened<host_audio_stall_wait(state->reopens))return 0;
    state->reopened=now;state->reopens++;
    return 1;
}
