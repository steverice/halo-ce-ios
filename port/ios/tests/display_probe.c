#include "halo_display.h"
#include <assert.h>
#include <stdio.h>

static void check(long dw,long dh,long logical,long aspect,long height,long limit,long w,long h) {
    struct halo_display_size size=halo_display_render_size(dw,dh,logical,aspect,height,limit);
    assert(size.width==w && size.height==h);
}
/* display.native_hud: the size the HUD's back buffer gets (no render height)
   must be the picture's place in the drawable, as gpu_present letterboxes
   it (gpu_metal.m, gpu_gl.c), so that Present copies it pixel for pixel. */
static void check_native_presents_whole(long dw,long dh,long logical,long aspect) {
    struct halo_display_size size=halo_display_render_size(dw,dh,logical,aspect,0,16384);
    struct halo_display_size shown=halo_display_present_size(dw,dh,size.width,size.height);
    long width=shown.width, height=shown.height;
    if(width!=size.width || height!=size.height)
        printf("%ldx%ld (logical %ld, aspect %ld): %ldx%ld presented at %ldx%ld\n",dw,dh,logical,aspect,size.width,size.height,width,height);
    assert(width==size.width && height==size.height);
}
int main(void) {
    long dw,dh;
    for(dw=640;dw<=7680;dw+=97) for(dh=480;dh<=4320;dh+=61){
        long logical=(480*dw+dh/2)/dh;
        logical=(logical<640?640:logical>1600?1600:logical)&~1L;
        check_native_presents_whole(dw,dh,logical,0);
        check_native_presents_whole(dw,dh,640,640);
    }
    check_native_presents_whole(1920,1080,854,0);
    check_native_presents_whole(2732,2048,640,0);
    check_native_presents_whole(2622,1206,1042,0);
    /* Native must use the full physical drawable, including when the Xbox
       layout has rounded the display's ratio to an even column count. */
    check(2752,2064,640,0,0,16384,2752,2064);
    check(2868,1320,1042,0,0,16384,2868,1320);
    check(2266,1488,730,0,0,16384,2266,1488);
    /* Performance presets preserve shape; original 4:3 must letterbox. */
    check(2752,2064,640,0,1080,16384,1440,1080);
    check(2868,1320,1042,0,720,16384,1564,720);
    check(2868,1320,640,640,0,16384,1760,1320);
    check(2752,2064,854,854,480,16384,854,480);
    /* Never supersample accidentally or exceed the GPU's texture limit. */
    check(1280,720,854,0,1080,16384,1280,720);
    check(7680,4320,854,0,0,4096,4096,2304);
    check(2752,2064,640,0,-1,16384,2752,2064);
    check(2752,2064,640,0,1,16384,320,240);
    check(0,0,640,0,0,16384,640,480);
    puts("PASS: native pixels, aspect preservation, resolution presets, GPU size limits, and a native picture presented whole");
    return 0;
}
