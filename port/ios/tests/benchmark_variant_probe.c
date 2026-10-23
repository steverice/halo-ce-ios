/* host_benchmark_variant.h: what each pass of a display-variant suite wants, how its report is named, which pass is
   the last, and what comes back afterward, walked through the three A10X variants on an Apple TV whose profile caps
   the output at 720 and whose upscaler is bilinear. */
#include "host_benchmark_variant.h"
#include <assert.h>
#include <string.h>

int main(void)
{
	const struct host_benchmark_variant variants[3] = {
		{ "out-720", { "AppleTV6,2" }, 1, 720, 720, HOST_BENCHMARK_UPSCALER_UNSET },
		{ "out-1080-bilinear", { "AppleTV6,2" }, 1, 720, 1080, HOST_BENCHMARK_UPSCALER_BILINEAR },
		{ "out-1080-metalfx", { "AppleTV6,2" }, 1, 720, 1080, HOST_BENCHMARK_UPSCALER_METALFX },
	};
	const int launch_cap = 720, launch_upscaler = HOST_BENCHMARK_UPSCALER_BILINEAR;
	const int caps[3] = { 720, 1080, 1080 }, upscalers[3] = { 0, 0, 1 };
	const char *const names[3] = { "variant-00.json", "variant-01.json", "variant-02.json" };
	int cap = launch_cap, upscaler = launch_upscaler, changes = 0;
	char name[32];

	for (int pass = 0; pass < 3; pass++)
	{
		int wanted = host_benchmark_variant_cap(&variants[pass], launch_cap);
		assert(wanted == caps[pass] && host_benchmark_variant_upscaler(&variants[pass], launch_upscaler) == upscalers[pass]);
		/* (the first pass is already at the profile's 720: no drawable change) */
		if (wanted != cap) { cap = wanted; changes++; }
		upscaler = host_benchmark_variant_upscaler(&variants[pass], launch_upscaler);
		host_benchmark_variant_report_name(pass, name, sizeof(name));
		assert(!strcmp(name, names[pass]));
		assert(host_benchmark_variant_last_pass(pass, 3, 0) == (pass == 2));
	}
	assert(changes == 1 && cap == 1080 && upscaler == HOST_BENCHMARK_UPSCALER_METALFX);
	/* restored afterward: the launch's cap and upscaler differ from the last pass's, so both are asked for again */
	assert(launch_cap != cap && launch_upscaler != upscaler);
	/* a suite asked to finish ends in the pass it is in */
	assert(host_benchmark_variant_last_pass(0, 3, 1));
	/* a variant that names no cap or upscaler keeps the launch's */
	const struct host_benchmark_variant render_only = { "render-540", { "iPad7,2" }, 1, 540, -1, -1 };
	assert(host_benchmark_variant_cap(&render_only, 0) == 0 && host_benchmark_variant_upscaler(&render_only, 1) == 1);
	/* resumed in the second pass; a stored index out of range starts again from the first */
	assert(host_benchmark_variant_resumed_index(1, 3) == 1);
	assert(host_benchmark_variant_resumed_index(3, 3) == 0 && host_benchmark_variant_resumed_index(-1, 3) == 0);
	/* carrying on after the app was ended: inside a pass (its journal unfinished), or between passes when every
	earlier pass's report is kept; not with an earlier report missing, nor outside the suite's passes */
	assert(host_benchmark_variant_resumes(1, 3, 1, 0));
	assert(host_benchmark_variant_resumes(0, 3, 1, 0));
	assert(host_benchmark_variant_resumes(2, 3, 0, 2));
	assert(!host_benchmark_variant_resumes(2, 3, 0, 1));
	assert(!host_benchmark_variant_resumes(0, 3, 0, 0));
	assert(!host_benchmark_variant_resumes(3, 3, 1, 3) && !host_benchmark_variant_resumes(-1, 3, 1, 0));
	puts("PASS: display variant passes want their own cap and upscaler, are named in order, and end where they should");
	return 0;
}
