/* Shared by the encoder and its independent boundary regression probe. */
#ifndef ASTC_QUALITY_H
#define ASTC_QUALITY_H
#include "astc_encode.h"
#include <cmath>
#include <stddef.h>
struct astc_quality_metrics
{
	double squared_error = 0;
	size_t rgb_channels = 0;
	int64_t crossing_deltas[257] = {};
	bool alpha_valid = true;
	void add(const uint8_t *original, const uint8_t *decoded, size_t texels, bool binary)
	{
		for (size_t i = 0; i < texels; ++i)
		{
			unsigned alpha = original[i * 4 + 3], actual = decoded[i * 4 + 3];
			if (alpha) for (unsigned c = 0; c < 3; ++c)
			{
				int difference = int(original[i * 4 + c]) - decoded[i * 4 + c];
				squared_error += difference * difference;
				++rgb_channels;
			}
			if (binary) alpha_valid &= std::abs(int(alpha) - int(actual)) <= 8;
			else if (alpha != actual)
			{
				unsigned low = alpha < actual ? alpha : actual, high = alpha > actual ? alpha : actual;
				++crossing_deltas[low + 1]; --crossing_deltas[high + 1];
			}
		}
	}
	bool valid(size_t texels, const astc_alpha_policy *policy) const
	{
		if (!alpha_valid || (rgb_channels && squared_error / rgb_channels > 65025.0 / std::pow(10.0, 3.8))) return false;
		/* Unresolved or blended usage retains the original conservative gate. */
		int64_t crossings = 0;
		for (unsigned r = 1; r < 256; ++r)
		{
			crossings += crossing_deltas[r];
			if ((!policy || !policy->known || (policy->boundaries[r / 8] & (1u << (r % 8)))) &&
				uint64_t(crossings) * 1000 > texels) return false;
		}
		return true;
	}
};

#endif
