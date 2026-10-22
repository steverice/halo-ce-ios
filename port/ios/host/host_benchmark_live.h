/* Live submission telemetry only; it never contributes to benchmark reports.
   The caller serializes access. All ingestion uses bounded storage and O(1) work. */
#ifndef HALO_HOST_BENCHMARK_LIVE_H
#define HALO_HOST_BENCHMARK_LIVE_H
#include <math.h>
#include <stddef.h>
#include <string.h>
#define BENCHMARK_LIVE_HISTORY 240
#define BENCHMARK_LIVE_AVERAGE 60
#define BENCHMARK_LIVE_STALE_SECONDS 1.5
/* the graph spans 0 to 40 ms: 33.3 (30 fps) sits near the top, and a 120 or 60 fps device's line stays clear of the bottom;
   a longer render is clipped to the top edge and marked */
#define BENCHMARK_LIVE_GRAPH_MAX_MS 40.0
struct benchmark_live {
    double interval[BENCHMARK_LIVE_HISTORY], cpu[BENCHMARK_LIVE_HISTORY], gpu[BENCHMARK_LIVE_HISTORY];
    double interval_sum, cpu_sum, gpu_sum, last_sample, last_cpu, last_gpu_positive;
    unsigned next, count, cpu_count, gpu_count;
    int gpu_seen;
};
struct benchmark_live_snapshot {
    double interval_ms[BENCHMARK_LIVE_HISTORY];
    double fps, cpu_ms, gpu_ms, peak_ms;
    unsigned count, clipped;
    int cpu_available, gpu_available;
};
static inline void benchmark_live_reset(struct benchmark_live *live) { memset(live,0,sizeof(*live)); }
static inline void benchmark_live_add(struct benchmark_live *live, double now, double interval, double cpu, double gpu) {
    if(!isfinite(now) || !isfinite(interval) || interval<=0 || (live->count && now<live->last_sample))return;
    if(live->count && now-live->last_sample>BENCHMARK_LIVE_STALE_SECONDS)benchmark_live_reset(live);
    if(live->count>=BENCHMARK_LIVE_AVERAGE) {
        unsigned old=(live->next+BENCHMARK_LIVE_HISTORY-BENCHMARK_LIVE_AVERAGE)%BENCHMARK_LIVE_HISTORY;
        live->interval_sum-=live->interval[old];
        if(live->cpu[old]>=0) { live->cpu_sum-=live->cpu[old]; live->cpu_count--; }
        if(live->gpu[old]>=0) { live->gpu_sum-=live->gpu[old]; live->gpu_count--; }
    }
    double cpu_ms=isfinite(cpu) && cpu>=0 ? cpu*1000.0 : -1;
    double gpu_ms=isfinite(gpu) && gpu>=0 ? gpu*1000.0 : -1;
    live->interval[live->next]=interval*1000.0;
    live->cpu[live->next]=cpu_ms; live->gpu[live->next]=gpu_ms;
    live->interval_sum+=interval*1000.0;
    if(cpu_ms>=0) { live->cpu_sum+=cpu_ms; live->cpu_count++; live->last_cpu=now; }
    /* A zero increment alone cannot establish GPU timestamp availability. Once
       positive completed work is observed, zero increments still count in the
       per-submission mean; otherwise async completion would inflate the mean. */
    if(gpu_ms>=0) { live->gpu_sum+=gpu_ms; live->gpu_count++; }
    if(gpu_ms>0) { live->last_gpu_positive=now; live->gpu_seen=1; }
    live->last_sample=now; live->next=(live->next+1)%BENCHMARK_LIVE_HISTORY;
    if(live->count<BENCHMARK_LIVE_HISTORY)live->count++;
}
static inline void benchmark_live_read(const struct benchmark_live *live, double now, struct benchmark_live_snapshot *out) {
    memset(out,0,sizeof(*out));
    if(!live->count || !isfinite(now) || now<live->last_sample || now-live->last_sample>BENCHMARK_LIVE_STALE_SECONDS)return;
    unsigned mean_count=live->count<BENCHMARK_LIVE_AVERAGE ? live->count : BENCHMARK_LIVE_AVERAGE;
    out->fps=live->interval_sum>0 ? mean_count*1000.0/live->interval_sum : 0;
    out->cpu_available=live->cpu_count && now-live->last_cpu<=BENCHMARK_LIVE_STALE_SECONDS;
    out->gpu_available=live->gpu_count && live->gpu_seen && now-live->last_gpu_positive<=BENCHMARK_LIVE_STALE_SECONDS;
    if(out->cpu_available)out->cpu_ms=live->cpu_sum/live->cpu_count;
    if(out->gpu_available)out->gpu_ms=live->gpu_sum/live->gpu_count;
    out->count=live->count;
    unsigned first=(live->next+BENCHMARK_LIVE_HISTORY-live->count)%BENCHMARK_LIVE_HISTORY;
    for(unsigned i=0;i<live->count;i++) {
        double value=live->interval[(first+i)%BENCHMARK_LIVE_HISTORY];
        out->interval_ms[i]=value;
        if(value>out->peak_ms)out->peak_ms=value;
        if(value>BENCHMARK_LIVE_GRAPH_MAX_MS)out->clipped++;
    }
}
#endif
