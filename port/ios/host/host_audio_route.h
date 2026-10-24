/* Reopening SDL can trigger another notification; compare the actual output
   identity and rate rather than reopening forever on every notification. */
#pragma once
#include <math.h>
#include <stdio.h>
#include <string.h>
struct host_audio_route { char identity[1024]; double rate; int initialized, recover; };
static inline int host_audio_route_update(struct host_audio_route *state,const char *identity,double rate,int recover) {
    state->recover|=recover;
    if(!identity || !*identity || rate<=0)return 0;
    int changed=state->initialized && (state->recover || strcmp(identity,state->identity) || fabs(rate-state->rate)>0.5);
    snprintf(state->identity,sizeof(state->identity),"%s",identity);
    state->rate=rate;state->initialized=1;state->recover=0;
    return changed;
}
