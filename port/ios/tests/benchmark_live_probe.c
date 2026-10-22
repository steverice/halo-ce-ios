#include "host_benchmark_live.h"
#include <assert.h>
#include <stdio.h>
static void near(double actual,double expected) { assert(fabs(actual-expected)<1e-7); }
int main(void) {
    struct benchmark_live live; struct benchmark_live_snapshot view;
    benchmark_live_reset(&live); benchmark_live_read(&live,10,&view); assert(!view.count && !view.cpu_available && !view.gpu_available);
    benchmark_live_add(&live,10,0.02,0.004,0); benchmark_live_read(&live,10,&view);
    assert(view.count==1 && view.cpu_available && !view.gpu_available); near(view.fps,50); near(view.cpu_ms,4);
    benchmark_live_add(&live,10.02,0.02,0,0.008); benchmark_live_add(&live,10.04,0.02,NAN,0);
    benchmark_live_read(&live,10.04,&view); assert(view.cpu_available && view.gpu_available); near(view.cpu_ms,2); near(view.gpu_ms,8.0/3.0);
    benchmark_live_read(&live,11.55,&view); assert(!view.count && !view.cpu_available && !view.gpu_available);
    benchmark_live_add(&live,12,0.01,0.001,0); benchmark_live_read(&live,12,&view); assert(view.count==1 && !view.gpu_available); near(view.fps,100);
    benchmark_live_add(&live,11,0.01,0.001,0.002); benchmark_live_read(&live,12,&view); assert(view.count==1); /* Old monotonic data cannot revive an old scene. */
    benchmark_live_add(&live,12.01,NAN,0,0); benchmark_live_add(&live,12.01,0,0,0); benchmark_live_add(&live,INFINITY,0.01,0,0);
    benchmark_live_read(&live,12.01,&view); assert(view.count==1);
    /* The graph spans 0 to 40 ms: 33.3 ms (30 fps) is inside it with room, 120 and 60 fps are well above the bottom. */
    assert(BENCHMARK_LIVE_GRAPH_MAX_MS>1000.0/30 && BENCHMARK_LIVE_GRAPH_MAX_MS<=50);
    benchmark_live_reset(&live);
    for(unsigned i=0;i<300;i++)benchmark_live_add(&live,20+i*0.01,i==299?0.2:0.01,i<240?0.001:0.003,i<240?0.002:0.006);
    benchmark_live_read(&live,22.99,&view); assert(view.count==240 && view.clipped==1); near(view.interval_ms[0],10); near(view.interval_ms[239],200); near(view.peak_ms,200);
    near(view.fps,1000.0/((59*10.0+200.0)/60)); near(view.cpu_ms,3); near(view.gpu_ms,6);
    benchmark_live_reset(&live); benchmark_live_add(&live,30,0.01,-1,-1); benchmark_live_read(&live,30,&view); assert(view.count==1 && !view.cpu_available && !view.gpu_available);
    /* Zero is valid CPU work; no completed GPU time is unknown without evidence. */
    benchmark_live_reset(&live); benchmark_live_add(&live,31,0.01,0,0); benchmark_live_read(&live,31,&view); assert(view.cpu_available && !view.gpu_available); near(view.cpu_ms,0);
    for(unsigned i=0;i<2000;i++)benchmark_live_add(&live,32+i*0.001,0.001,0.0005,0.0007);
    benchmark_live_read(&live,34,&view); assert(view.count==240); near(view.cpu_ms,0.5); near(view.gpu_ms,0.7);
    benchmark_live_reset(&live); benchmark_live_read(&live,34,&view); assert(!view.count);
    benchmark_live_add(&live,40,0.01,0.001,0.004);
    for(unsigned i=1;i<=3;i++)benchmark_live_add(&live,40+i*0.01,0.01,0.001,0);
    benchmark_live_read(&live,40.03,&view); near(view.gpu_ms,1); /* [4,0,0,0] is 1 ms/render, not 4. */
    for(unsigned i=4;i<200;i++)benchmark_live_add(&live,40+i*0.01,0.01,-1,0);
    benchmark_live_read(&live,41.99,&view); assert(!view.gpu_available && !view.cpu_available && view.count==200);
    puts("live benchmark: PASS (rolling means, raw spikes, bounded history, availability, stale and reset)");
}
