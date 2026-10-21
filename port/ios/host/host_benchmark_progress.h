/* Native progress formatting shared by the UI and the asset-free probe. */
#ifndef HALO_HOST_BENCHMARK_PROGRESS_H
#define HALO_HOST_BENCHMARK_PROGRESS_H
#include <stdio.h>
#include <stddef.h>

enum host_benchmark_phase { HOST_BENCHMARK_LOADING, HOST_BENCHMARK_LEAD_IN, HOST_BENCHMARK_MEASURING };
struct host_benchmark_progress {
    char title[192], mode[128];
    int number, total;
    double fraction, elapsed, measured_fraction;
    int offscreen, reality_check, presented, phase;
};
static inline int host_benchmark_progress_visible(const struct host_benchmark_progress *p) {
    return p->offscreen && !p->presented;
}
static inline void host_benchmark_progress_detail(const struct host_benchmark_progress *p, char *text, size_t size) {
    const char *phase = p->presented ? "Presented check" : p->phase == HOST_BENCHMARK_MEASURING ? "Measuring" :
        p->phase == HOST_BENCHMARK_LEAD_IN ? "Lead-in" : "Loading";
    unsigned long seconds = p->elapsed > 0 ? (unsigned long)p->elapsed : 0;
    char measured[64] = "";
    if (p->measured_fraction >= 0 && p->phase == HOST_BENCHMARK_MEASURING) {
        double fraction = p->measured_fraction > 1 ? 1 : p->measured_fraction;
        snprintf(measured, sizeof(measured), "\nMeasured window: %.0f%%", fraction * 100);
    }
    char part[64] = "Preparing cases";
    if (p->number > 0) snprintf(part, sizeof(part), "Case %d of %d", p->number, p->total);
    snprintf(text, size, "%s — offscreen benchmark\n%s\n%s · Elapsed %lu:%02lu%s",
        p->mode, part, phase, seconds / 60, seconds % 60, measured);
}
#endif
