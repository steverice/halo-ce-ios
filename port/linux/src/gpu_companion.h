/* Post-scene packets survive until Present; all caller-owned state is copied. */
#ifndef HALO_GPU_COMPANION_H
#define HALO_GPU_COMPANION_H
#include "gpu.h"
#include "halo_display.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The layout width is rounded to even columns; the native picture keeps
   the output's exact aspect. Fit that picture before mapping game units. */
static inline void companion_picture_scale(long logical_width, long logical_height,
    long native_width, long native_height, long phone_width, long phone_height, float scale[2])
{
    scale[0]=scale[1]=0;
    if(logical_width<=0 || logical_height<=0 || native_width<=0 || native_height<=0 ||
        phone_width<=0 || phone_height<=0)return;
    struct halo_display_size fit=halo_display_present_size(phone_width,phone_height,native_width,native_height);
    scale[0]=(float)fit.width/(float)logical_width;
    scale[1]=(float)fit.height/(float)logical_height;
}

struct companion_record
{
	struct gpu_draw draw;
	struct gpu_constant_store constants;
	struct gpu_uniforms uniforms;
	struct gpu_clear clear;
	float bounds[4]; /* game coordinates, before target-pixel rounding */
	int is_clear;
};
struct companion_records
{
	struct companion_record *items;
	size_t count, capacity;
	int failed;
};
static inline int companion_eligible(int enabled, int scene, int movie, uint32_t target, uint32_t back_buffer)
{
	return enabled && !scene && !movie && target && target == back_buffer;
}
static inline struct gpu_rect companion_rect(const float bounds[4], float sx, float sy)
{
	struct gpu_rect r;
	r.x = (int32_t)floorf(bounds[0] * sx + 0.5f);
	r.y = (int32_t)floorf(bounds[1] * sy + 0.5f);
	r.width = (int32_t)floorf(bounds[2] * sx + 0.5f) - r.x;
	r.height = (int32_t)floorf(bounds[3] * sy + 0.5f) - r.y;
	return r;
}
static inline struct companion_record *companion_record_add(struct companion_records *records, const float bounds[4])
{
	struct companion_record *item;
	if (records->failed)
		return NULL;
	if (records->count == records->capacity)
	{
		size_t capacity = records->capacity ? records->capacity * 2 : 64;
		void *items = capacity <= 4096 ? realloc(records->items, capacity * sizeof(*item)) : NULL;
		if (!items)
		{
			records->failed = 1;
			return NULL;
		}
		records->items = items;
		records->capacity = capacity;
	}
	item = &records->items[records->count++];
	memset(item, 0, sizeof(*item));
	memcpy(item->bounds, bounds, sizeof(item->bounds));
	return item;
}
static inline struct companion_record *companion_record_draw(struct companion_records *records,
	const struct gpu_draw *draw, const struct gpu_constant_store *constants,
	const struct gpu_uniforms *uniforms, const float bounds[4])
{
	struct companion_record *item = companion_record_add(records, bounds);
	if (item)
	{
		item->draw = *draw;
		item->constants = *constants;
		item->uniforms = *uniforms;
	}
	return item;
}
static inline void companion_records_reset(struct companion_records *records)
{
	records->count = 0;
	records->failed = 0;
}
#endif
