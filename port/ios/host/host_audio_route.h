/* Whether an audio route change needs the sound device closed and opened again (host_sdl_audio_route_changed).
   iOS moves a running AudioQueue to a new route by itself, so headphones, AirPods or a Bluetooth speaker coming or
   going leave it playing; a reopen there would only cost a frame hitch and a gap. It is reopened when the route
   change involves an output that carries a display or a car (HDMI, AirPlay, USB audio, DisplayPort, CarPlay: an
   external display connecting mid-game has left the queue stopped), when the session's rate or channel count
   changed, and on recovery (an interruption's end, a media services reset). A queue that stops on any other
   change is caught by the stall check (host_audio_stall.h). Reopening can trigger another notification; the
   output's identity, rate and channels are compared so it does not reopen forever. */
#pragma once
#include <math.h>
#include <stdio.h>
#include <string.h>
struct host_audio_route { char identity[1024]; double rate; int channels, external, initialized, recover; };
/* identity: the outputs' types, IDs and names; external: one of them is HDMI, AirPlay, USB audio, DisplayPort or
   CarPlay. Returns 1 when the device should be reopened. */
static inline int host_audio_route_update(struct host_audio_route *state,const char *identity,double rate,int channels,
    int external,int recover) {
    state->recover|=recover;
    if(!identity || !*identity || rate<=0)return 0;
    int format=fabs(rate-state->rate)>0.5 || channels!=state->channels;
    int moved=strcmp(identity,state->identity)!=0;
    int reopen=state->initialized && (state->recover || format || (moved && (external || state->external)));
    snprintf(state->identity,sizeof(state->identity),"%s",identity);
    state->rate=rate;state->channels=channels;state->external=external!=0;state->initialized=1;state->recover=0;
    return reopen;
}
