/* Host-owned presentation storage. Lifecycle calls come from the benchmark
   worker; callbacks carry only the public scalar token. */
#ifndef HALO_HOST_BENCHMARK_PRESENTATION_H
#define HALO_HOST_BENCHMARK_PRESENTATION_H
#import <Foundation/Foundation.h>
#include "host_benchmark.h"
#include "benchmark_plan.h"
void host_benchmark_presentation_begin(enum benchmark_scene_kind kind, size_t capacity);
void host_benchmark_presentation_window(void);
/* Never waits on the main thread. Closes admission before waiting; returns an
   immutable detached report after a monotonic bounded drain. */
NSDictionary *host_benchmark_presentation_finish(double timeout);
/* Close synchronously on the guest/UI thread, then drain on a worker. The
   caller retries next-run planning until take_report returns a snapshot. */
void host_benchmark_presentation_end_async(void);
NSDictionary *host_benchmark_presentation_take_report(void);
NSDictionary *host_benchmark_presentation_diagnostics(void);
/* Applies only to newly written records. Existing resumed files keep their
   original semantics; recovered interrupted runs have no captured evidence. */
/* pacing: enum host_benchmark_pacing (host_benchmark_offscreen.h) */
NSDictionary *host_benchmark_measurement_record(NSDictionary *record, NSDictionary *presentation, int pacing);
/* Human summary of the serialized legacy submission measurements. */
NSString *host_benchmark_submission_summary(NSDictionary *record);
#endif
