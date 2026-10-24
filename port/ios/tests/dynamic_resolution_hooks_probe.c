/* Suspension drains bounded shadow rows before the report segment changes. */
#include "host_perf_hooks.h"
#include "host_perf_reports.h"
#include "dynamic_resolution.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned rows;
static game_perf_frame last;
void host_config_string(const char *name, const char *fallback, char *out, size_t size) {
    (void)name; snprintf(out,size,"%s",fallback);
}
void host_perf_reports_frame(const game_perf_frame *frame) { last=*frame;assert(frame->budget_us==16667);rows++; }
void host_perf_reports_event(const game_perf_event *event) { (void)event; }
void host_perf_metal_join(game_perf_frame *frame) {
    frame->drs.available=1|8;
    if (frame->frame_id>1) frame->drs.available |= 2;
    frame->drs.scene_us=1000;frame->drs.presentation_us=16667;
}
static void present(unsigned index) {
    host_perf_hooks_phase_at(7,0,1000000+index*16667);
    host_perf_hooks_present_begin_at(1000100+index*16667);
    host_perf_hooks_dynamic_config(1,1920,1081,540,0,1);
    host_perf_hooks_budget(16667);
    host_perf_hooks_cpu(1000);
    host_perf_hooks_present_end_at(1000200+index*16667);
}
int main(void) {
    assert(!setenv("HALO_DYNAMIC_RESOLUTION_SHADOW","true",1));
    host_perf_hooks_dynamic_ignore(DYNAMIC_RESOLUTION_LOAD);
    for (unsigned i=0;i<3;i++) present(i);
    assert(rows==0);
    host_perf_hooks_dynamic_pause();
    assert(rows==3 && last.frame_id==3);
    assert(last.drs.ignore & DYNAMIC_RESOLUTION_LOAD);
    assert(last.drs.available & 4);
    host_perf_hooks_dynamic_resume();
    present(3);
    assert(rows==3);
    host_perf_hooks_dynamic_flush();
    assert(rows==4 && last.frame_id==4);
    assert(last.drs.ignore & DYNAMIC_RESOLUTION_RESUME);
    assert(last.drs.actual_lines==1081 && last.drs.chosen_lines==1081 && last.drs.floor_lines==541);
    return 0;
}
