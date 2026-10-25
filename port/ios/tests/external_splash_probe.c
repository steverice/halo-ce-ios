#include "host_external_splash.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    struct host_external_splash s={0};
    /* connected before the menu: the menu asks for the splash */
    assert(!host_external_splash_connect(&s,1) && s.parked==1);
    assert(host_external_splash_menu(&s,1)==1);
    host_external_splash_shown(&s,1);
    assert(host_external_splash_menu(&s,1)==0);
    /* disconnect at the menu; reconnect shows it again */
    assert(host_external_splash_disconnect(&s,1) && !s.parked && !s.shown);
    assert(host_external_splash_connect(&s,2) && s.shown==2);
    /* a late disconnect of the replaced scene leaves the new one alone */
    assert(!host_external_splash_disconnect(&s,1) && s.parked==2);
    /* Play: the game takes the parked scene once */
    assert(host_external_splash_attach(&s)==2);
    assert(!s.parked && !s.shown && !s.enabled);
    assert(host_external_splash_attach(&s)==0);
    /* OpenGL ES keeps system mirroring */
    struct host_external_splash gl={0};
    assert(!host_external_splash_connect(&gl,3));
    assert(host_external_splash_menu(&gl,0)==0 && !gl.shown);
    assert(!host_external_splash_connect(&gl,4));
    puts("PASS: menu-time external splash handoff, disconnect, reconnect, and GL mirroring");
}
