#include <assert.h>
#include "../host/host_benchmark_page_policy.h"

int main(void) {
#ifdef HALO_TESTER_BUILD
	assert(host_benchmark_page_minutes(NULL) == 30);
	assert(host_benchmark_page_minutes("5") == 5);
	assert(host_benchmark_page_minutes("0") == 0);
#elif TARGET_OS_OSX && !TARGET_OS_MACCATALYST
	/* Native Mac: the local result alert shows only when a launch asks for it. */
	assert(host_benchmark_page_minutes(NULL) == 0);
	assert(host_benchmark_page_minutes("0") == 0);
	assert(host_benchmark_page_minutes("1") == 1);
#else
	assert(host_benchmark_page_minutes(NULL) == 0);
	assert(host_benchmark_page_minutes("5") == 0);
#endif
	return 0;
}
