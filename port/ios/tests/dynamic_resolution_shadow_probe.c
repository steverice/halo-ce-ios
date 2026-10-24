/* Frame attribution matters: a virtual decision never changes the measured level. */
#include "dynamic_resolution_shadow.h"
#include <assert.h>
int main(void) {
    struct dynamic_resolution_shadow shadow = {0};
    game_perf_frame frame = {.frame_id=1,.cpu_us=1000,.budget_us=16667};
    frame.drs=(game_perf_dynamic_resolution){.actual_lines=1080,.ceiling_lines=1080,.floor_lines=540,
        .scene_us=19000,.presentation_us=33334,.presented_us=1000000,.available=1|2|8,.late=1};
    dynamic_resolution_shadow_frame(&shadow,&frame,1920);
    assert(frame.drs.available & 4);
    assert(frame.drs.action==DYNAMIC_RESOLUTION_DROP && frame.drs.chosen_lines<1080);
    assert(frame.drs.actual_lines==1080 && shadow.plan.history[0].level==0);
    frame.frame_id++;
    frame.drs.late=0;
    dynamic_resolution_shadow_frame(&shadow,&frame,1920);
    assert(shadow.plan.history[1].level==0); /* still measured at the physical ceiling after a virtual drop */
    assert(frame.drs.actual_lines==1080);
    frame.frame_id++;
    frame.drs.ignore=DYNAMIC_RESOLUTION_COMPILE;
    dynamic_resolution_shadow_frame(&shadow,&frame,1920);
    assert(frame.drs.ignore & DYNAMIC_RESOLUTION_COMPILE);
    assert(shadow.plan.ignore_frames==3);
    frame.frame_id++;
    frame.drs.available=8;frame.drs.ignore=DYNAMIC_RESOLUTION_SCENE;
    dynamic_resolution_shadow_frame(&shadow,&frame,1920);
    assert(!(frame.drs.available & 4)); /* no invented presentation interval */
    frame.frame_id++;
    frame.drs.available=1|2|8;frame.drs.ignore=DYNAMIC_RESOLUTION_LOAD;
    dynamic_resolution_shadow_frame(&shadow,&frame,1920);
    assert(shadow.plan.ignore_seconds>0.9);
    assert(shadow.plan.ignore_frames==29); /* scene event survives missing callback */
    frame.drs.ceiling_lines=720;frame.drs.actual_lines=720;frame.drs.floor_lines=540;
    frame.drs.ignore=0;frame.drs.available=8;frame.frame_id++;
    dynamic_resolution_shadow_frame(&shadow,&frame,1280);
    assert(frame.drs.chosen_lines==720); /* a new ceiling resets virtual history */
    /* A visible loading screen must never enter the fit, even after two seconds. */
    struct dynamic_resolution_shadow loading_shadow = {0};
    frame=(game_perf_frame){.cpu_us=1000,.budget_us=16667,.flags=GAME_PERF_LOADING};
    frame.drs=(game_perf_dynamic_resolution){.actual_lines=1080,.ceiling_lines=1080,.floor_lines=540,
        .scene_us=19000,.presentation_us=16667,.available=1|2|8,.late=1};
    for (unsigned i=0;i<120;i++) {
        frame.frame_id++;
        dynamic_resolution_shadow_frame(&loading_shadow,&frame,1920);
        assert(frame.drs.chosen_lines==1080 && loading_shadow.plan.count==0);
        frame.drs.ignore=0;
    }
    frame.flags=0;frame.drs.ignore=DYNAMIC_RESOLUTION_LOAD;
    for (unsigned i=0;i<30;i++) {
        frame.frame_id++;
        dynamic_resolution_shadow_frame(&loading_shadow,&frame,1920);
        assert(frame.drs.chosen_lines==1080 && loading_shadow.plan.count==0);
        frame.drs.ignore=0;
    }
    return 0;
}
