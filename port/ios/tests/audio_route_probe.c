#include "host_audio_route.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    struct host_audio_route state={0};
    assert(!host_audio_route_update(&state,"speaker",48000,0));
    assert(host_audio_route_update(&state,"airplay:receiver",48000,0));
    /* Stream reopening may itself send a route notification. */
    assert(!host_audio_route_update(&state,"airplay:receiver",48000,0));
    assert(host_audio_route_update(&state,"airplay:receiver",44100,0));
    assert(host_audio_route_update(&state,"speaker",44100,0));
    assert(host_audio_route_update(&state,"speaker",44100,1));
    assert(!host_audio_route_update(&state,"",0,0));
    assert(!host_audio_route_update(&state,"speaker",44100,0));
    assert(!host_audio_route_update(&state,"",0,1));
    assert(host_audio_route_update(&state,"speaker",44100,0));
    assert(!host_audio_route_update(&state,"speaker",44100,0));
    puts("PASS: same-rate route switches, rate changes, interruption recovery, notification deduplication");
}
