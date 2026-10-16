#include "host_external_mode.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    struct host_external_mode modes[] = {
        {640,480,1,0}, {720,576,1,0}, {1280,720,1,0},
        {1920,1080,1,0}, {2560,1600,1,0}, {3840,2160,1,0}
    };
    assert(host_external_mode_choose(modes,6,4,0,0,0)==4);
    assert(host_external_mode_choose(modes,6,4,1080,0,0)==3);
    assert(host_external_mode_choose(modes,6,4,720,0,0)==2);
    assert(host_external_mode_choose(modes,6,4,1080,3840,2160)==5);
    modes[3].refused=1;
    assert(host_external_mode_choose(modes,6,4,1080,0,0)==2);
    modes[2].refused=1;
    assert(host_external_mode_choose(modes,6,4,1080,0,0)==0);
    assert(host_external_mode_choose(modes+1,1,0,1080,0,0)==0);
    assert(host_external_mode_choose(modes,6,4,0,999,999)==-1);
    modes[5].refused=1;
    assert(host_external_mode_choose(modes,6,4,0,3840,2160)==-1);
    puts("PASS: preferred, caps, forced modes, refusals, and PAL fallback");
}
