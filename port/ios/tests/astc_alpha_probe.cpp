#include "astc_quality.h"
#include <cassert>
#include <cstring>
int main()
{
	unsigned char original[4000] = {}, decoded[4000] = {};
	for (unsigned i = 0; i < 1000; ++i) original[i * 4 + 3] = decoded[i * 4 + 3] = 100;
	decoded[3] = decoded[7] = 101;
	astc_quality_metrics metrics;
	metrics.add(original, decoded, 1000, false);
	assert(!metrics.valid(1000, nullptr));
	astc_alpha_policy policy = {};
	policy.known = true;
	policy.boundaries[128 / 8] |= 1u << (128 % 8);
	assert(metrics.valid(1000, &policy));
	policy.boundaries[101 / 8] |= 1u << (101 % 8);
	assert(!metrics.valid(1000, &policy));
	policy.known = false;
	memset(policy.boundaries, 0, sizeof(policy.boundaries));
	assert(!metrics.valid(1000, &policy));
	policy.known = true;
	assert(metrics.valid(1000, &policy));
	decoded[7] = 100;
	astc_quality_metrics exact_bound;
	exact_bound.add(original, decoded, 1000, false);
	assert(exact_bound.valid(1000, nullptr)); // Exactly 0.1% is permitted.
	memset(decoded, 255, sizeof(decoded));
	astc_quality_metrics color;
	color.add(original, decoded, 1000, false);
	assert(!color.valid(1000, &policy));
	memset(original, 0, sizeof(original)); memset(decoded, 0, sizeof(decoded));
	decoded[3] = 9;
	astc_quality_metrics binary;
	binary.add(original, decoded, 1000, true);
	assert(!binary.valid(1000, &policy));
	original[3] = 0; decoded[3] = 1;
	astc_quality_metrics zero;
	zero.add(original, decoded, 1000, false);
	policy.boundaries[0] = 2; // alpha > 0 uses boundary 1.
	assert(zero.valid(1000, &policy));
	decoded[7] = 1;
	zero = {};
	zero.add(original, decoded, 1000, false);
	assert(!zero.valid(1000, &policy));
}
