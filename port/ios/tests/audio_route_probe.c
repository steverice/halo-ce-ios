#include "host_audio_route.h"
#include "host_audio_stall.h"
#include <assert.h>
#include <stdio.h>

#define S 1000000000ull

int main(void) {
    struct host_audio_route state={0};
    /* the first reading only records */
    assert(!host_audio_route_update(&state,"speaker",48000,2,0,0));
    /* to and from an output that carries a display: reopen */
    assert(host_audio_route_update(&state,"hdmi:monitor",48000,2,1,0));
    /* Stream reopening may itself send a route notification. */
    assert(!host_audio_route_update(&state,"hdmi:monitor",48000,2,1,0));
    assert(host_audio_route_update(&state,"speaker",48000,2,0,0));
    /* headphones or Bluetooth at the same format: iOS moves the running queue */
    assert(!host_audio_route_update(&state,"headphones",48000,2,0,0));
    assert(!host_audio_route_update(&state,"bluetooth:airpods",48000,2,0,0));
    /* a rate or channel change always reopens */
    assert(host_audio_route_update(&state,"bluetooth:airpods",44100,2,0,0));
    assert(host_audio_route_update(&state,"bluetooth:airpods",44100,1,0,0));
    assert(host_audio_route_update(&state,"speaker",44100,1,0,1));
    assert(!host_audio_route_update(&state,"",0,0,0,0));
    assert(!host_audio_route_update(&state,"speaker",44100,1,0,0));
    /* a recovery seen while the route could not be read carries over to the next reading */
    assert(!host_audio_route_update(&state,"",0,0,0,1));
    assert(host_audio_route_update(&state,"speaker",44100,1,0,0));
    assert(!host_audio_route_update(&state,"speaker",44100,1,0,0));
    puts("PASS: external-output route switches, format changes, interruption recovery, headphones and Bluetooth left running");

    struct host_audio_stall stall={0};
    /* a system interruption never reopens, however long the device has been silent */
    for(uint64_t t=0;t<120*S;t+=S/4)assert(!host_audio_stall_due(&stall,t,1,1));
    /* reopens at once, then after 3, 6 and 12 s, then every 30 s */
    uint64_t t=200*S,last=0,expected[]={0,3,6,12,30,30,30};
    int seen=0;
    for(uint64_t now=t;now<t+200*S && seen<7;now+=S/4)
        if(host_audio_stall_due(&stall,now,0,1)) {
            if(seen)assert(now-last==expected[seen]*S);
            last=now;seen++;
        }
    assert(seen==7);
    /* callbacks back: the waits start over */
    assert(!host_audio_stall_due(&stall,last+S,0,0));
    assert(host_audio_stall_due(&stall,last+2*S,0,1));
    assert(!host_audio_stall_due(&stall,last+3*S,0,1));
    /* an interruption in the middle starts them over too */
    assert(!host_audio_stall_due(&stall,last+4*S,1,1));
    assert(host_audio_stall_due(&stall,last+5*S,0,1));
    puts("PASS: stall reopens wait 0, 3, 6, 12, then 30 s; none during an interruption; waits restart after recovery");

    int interrupted=0,recover=0;
    /* a call begins: held, no reopen; it ends: released, reopen */
    host_audio_interruption(1,0,&interrupted,&recover);
    assert(interrupted && !recover);
    host_audio_interruption(0,0,&interrupted,&recover);
    assert(!interrupted && recover);
    /* AirPods or a cable dropped: no hold, reopen on the speaker */
    interrupted=recover=0;
    host_audio_interruption(1,1,&interrupted,&recover);
    assert(!interrupted && recover);
    /* a disconnect during a call keeps the call's hold and does not reopen under it */
    interrupted=1;recover=0;
    host_audio_interruption(1,1,&interrupted,&recover);
    assert(interrupted && !recover);
    puts("PASS: interruptions hold the stall check until their end; route disconnects reopen without holding it");
}
