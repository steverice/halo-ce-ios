#include <assert.h>
#include "../host/host_benchmark_page_policy.h"

int main(void) {
#ifdef HALO_TESTER_BUILD
	assert(host_benchmark_page_minutes(NULL) == 30);
	assert(host_benchmark_page_minutes("5") == 5);
	assert(host_benchmark_page_minutes("0") == 0);
#else
	assert(host_benchmark_page_minutes(NULL) == 0);
	assert(host_benchmark_page_minutes("5") == 0);
#endif
	return 0;
}
