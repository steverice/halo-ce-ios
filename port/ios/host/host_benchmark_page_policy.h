#ifndef HALO_HOST_BENCHMARK_PAGE_POLICY_H
#define HALO_HOST_BENCHMARK_PAGE_POLICY_H

#include <stdlib.h>
#include <TargetConditionals.h>

/* The guest's DEBUG symbol does not classify the host app's distribution. */
static inline int host_benchmark_page_minutes(const char *setting) {
#ifdef HALO_TESTER_BUILD
	if (!setting) return 30;
	int minutes = atoi(setting);
	return minutes > 0 ? minutes : 0;
#elif TARGET_OS_OSX && !TARGET_OS_MACCATALYST
	/* The native Mac host's page is a local result alert, never a web page; it appears only on request. */
	if (!setting) return 0;
	int minutes = atoi(setting);
	return minutes > 0 ? minutes : 0;
#else
	(void)setting;
	return 0;
#endif
}

#endif
