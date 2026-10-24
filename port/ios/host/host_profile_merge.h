/* Apache-2.0. Merge a safe point's effective host profile without retaining shadowed layers. */
#pragma once
#include "host_profile.h"
#include "../../linux/src/port_profile_apply.h"

static inline void host_profile_merge_at_break(const struct host_profile *installed,
	const struct host_profile *offered, int point, int networked, struct host_profile *result)
{
	*result = *offered;
	result->count = 0;
	const struct host_profile *layers[] = {installed, offered};
	for (int layer = 0; layer < 2; layer++) {
		const struct host_profile *profile = layers[layer];
		for (int i = 0; i < profile->count; i++) {
			const struct host_profile_value *value = &profile->values[i];
			if (port_profile_apply_allowed(value->name, point, networked) != layer) continue;
			int shadowed = 0;
			for (int j = i + 1; j < profile->count; j++) {
				const struct host_profile_value *later = &profile->values[j];
				if (value->external == later->external && !strcmp(value->name, later->name) &&
					!strcmp(value->scene, later->scene)) { shadowed = 1; break; }
			}
			/* The admitted vocabulary has fewer than 128 distinct name/scene/output tuples. */
			if (!shadowed && result->count < HOST_PROFILE_MAXIMUM_VALUES)
				result->values[result->count++] = *value;
		}
	}
}
