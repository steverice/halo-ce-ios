/* Native progress formatting shared by the UI and the asset-free probe. */
#ifndef HALO_HOST_BENCHMARK_PROGRESS_H
#define HALO_HOST_BENCHMARK_PROGRESS_H
#include <stdio.h>
#include <stddef.h>
#include <string.h>

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
/* "cost:<case>" names the settings-cost suite; any other suite shows as it is named */
static inline void host_benchmark_progress_kind(const struct host_benchmark_progress *p, char *text, size_t size) {
    if (!strncmp(p->mode, "cost:", 5)) snprintf(text, size, "Settings cost");
    else snprintf(text, size, "%s", p->mode[0] ? p->mode : "Benchmark");
}
/* the level's name: the title's first line (its second line is the case id) */
static inline void host_benchmark_progress_level(const struct host_benchmark_progress *p, char *text, size_t size) {
    size_t length = strcspn(p->title, "\n");
    if (length >= size) length = size - 1;
    memcpy(text, p->title, length);
    text[length] = 0;
}
/* with_case: the case id (the title's second line) follows the suite kind; with_percent: the measured window's
   percentage, for a screen without a bar */
static inline void host_benchmark_progress_text(const struct host_benchmark_progress *p, char *text, size_t size,
    int with_case, int with_percent) {
    const char *phase = p->presented ? "Presented check" : p->phase == HOST_BENCHMARK_MEASURING ? "Measuring" :
        p->phase == HOST_BENCHMARK_LEAD_IN ? "Lead-in" : "Loading";
    unsigned long seconds = p->elapsed > 0 ? (unsigned long)p->elapsed : 0;
    char measured[64] = "", kind[160], id[160] = "";
    if (with_percent && p->measured_fraction >= 0 && p->phase == HOST_BENCHMARK_MEASURING) {
        double fraction = p->measured_fraction > 1 ? 1 : p->measured_fraction;
        snprintf(measured, sizeof(measured), "\nMeasured window: %.0f%%", fraction * 100);
    }
    host_benchmark_progress_kind(p, kind, sizeof(kind));
    if (with_case) {
        const char *newline = strchr(p->title, '\n');
        if (newline && newline[1]) snprintf(id, sizeof(id), " · %s", newline + 1);
    }
    char part[64] = "Preparing cases";
    if (p->number > 0) snprintf(part, sizeof(part), "Case %d of %d", p->number, p->total);
    snprintf(text, size, "%s%s\n%s\n%s · Elapsed %lu:%02lu%s", kind, id, part, phase, seconds / 60, seconds % 60, measured);
}
/* the Mac's text-only screen */
static inline void host_benchmark_progress_detail(const struct host_benchmark_progress *p, char *text, size_t size) {
    host_benchmark_progress_text(p, text, size, 0, 1);
}
#endif
