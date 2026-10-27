#include "host_external_mode.h"
#include <assert.h>
#include <stdio.h>
#include <math.h>
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
    struct host_external_mode oversized[] = {{3840,2160,1,0}, {4096,2160,1,0}};
    assert(host_external_mode_choose(oversized,2,0,1080,0,0)==0);
    oversized[0].refused=1;
    assert(host_external_mode_choose(oversized,2,0,1080,0,0)==1);
    oversized[1].refused=1;
    assert(host_external_mode_choose(oversized,2,0,1080,0,0)==-1);
    int width=0,height=0;
    assert(host_external_mode_drawable(3840,2160,1080,&width,&height));
    assert(width==1920 && height==1080);
    assert(host_external_mode_drawable(4096,2160,1080,&width,&height));
    assert(width==1920 && height==1012);
    assert(host_external_mode_drawable(1280,720,1080,&width,&height));
    assert(width==1280 && height==720);
    assert(host_external_mode_drawable(3840,2160,0,&width,&height));
    assert(width==3840 && height==2160);
    assert(!host_external_mode_drawable(0,2160,1080,&width,&height));
    /* native policy: the host passes cap 0, so a 2560x1600 preferred mode stays even under a 1080-line profile */
    struct host_external_mode native[] = {{1680,1050,1,0},{1920,1080,1,0},{2560,1600,1,0}};
    assert(host_external_mode_choose(native,3,2,0,0,0)==2);
    assert(host_external_mode_drawable(2560,1600,1080,&width,&height) && width==1728 && height==1080);
    native[2].refused=1;
    assert(host_external_mode_choose(native,3,2,0,0,0)==0); /* next best keeps the 16:10 shape */
    puts("PASS: preferred, caps, forced modes, refusals, and PAL fallback");
    /* the drawable fills the screen, scaled up: 1728x1080 on a 2560x1600 display is the same 16:10 shape */
    double rect[4];
    assert(host_external_fit(2560,1600,1728,1080,rect));
    assert(rect[0]==0 && rect[1]==0 && rect[2]==2560 && rect[3]==1600);
    /* a 16:9 picture on a 16:10 screen: bars above and below only */
    assert(host_external_fit(2560,1600,1920,1080,rect));
    assert(rect[0]==0 && rect[2]==2560 && fabs(rect[3]-1440)<1e-6 && fabs(rect[1]-80)<1e-6);
    /* a 16:10 picture on a 16:9 screen: bars at the sides only */
    assert(host_external_fit(1920,1080,1728,1080,rect));
    assert(rect[1]==0 && rect[3]==1080 && fabs(rect[2]-1728)<1e-6 && fabs(rect[0]-96)<1e-6);
    /* points, not pixels: a 1280x800 point screen at 2x */
    assert(host_external_fit(1280,800,1728,1080,rect));
    assert(rect[0]==0 && rect[1]==0 && rect[2]==1280 && rect[3]==800);
    /* a drawable larger than the screen is scaled down to it, never cropped */
    assert(host_external_fit(1280,720,3840,2160,rect));
    assert(fabs(rect[2]-1280)<1e-6 && fabs(rect[3]-720)<1e-6);
    assert(!host_external_fit(0,720,1920,1080,rect) && !host_external_fit(1280,720,0,1080,rect));
    puts("PASS: the drawable is fitted to the screen, scaled up, bars only for a different shape");
}
