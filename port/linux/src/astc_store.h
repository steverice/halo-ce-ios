/* Phase 2 store boundary. Direct rewrites are deliberately never persisted. */
#ifndef ASTC_STORE_H
#define ASTC_STORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct astc_store_key
{
	/* XXH3-128 over every source face and mip, format, dimensions, level
	 * count and encoder version. The encoder worker will own this hash. */
	uint8_t bytes[16];
};

struct astc_store_record
{
	/* Packed ASTC 4x4 blocks, face-major then mip-major, without face padding.
	 * A successful lookup pins this view until astc_store_release. */
	const uint8_t *bytes;
	size_t size;
};

static inline bool astc_store_lookup(const struct astc_store_key *key, struct astc_store_record *record)
{
	(void)key;
	record->bytes = NULL;
	record->size = 0;
	return false;
}

static inline void astc_store_release(struct astc_store_record *record)
{
	record->bytes = NULL;
	record->size = 0;
}

#endif
