/* Encoder outputs only; direct rewrites are never persisted. */
#ifndef ASTC_STORE_H
#define ASTC_STORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C"
{
#endif
/* Bump when the encoder build, preset, alpha policy, or record layout changes. */
#define ASTC_STORE_VERSION "astcenc-baff485b-fast-4x4-alpha-v1"
struct astc_store_key
{
	uint8_t bytes[16];
};
struct astc_store_span
{
	const void *bytes;
	size_t size;
};
struct astc_store_record
{
	const uint8_t *bytes;
	size_t size;
	void *pin;
};
/* Spans are face-major, then mip-major, without guest face padding. */
void astc_store_key_compute(struct astc_store_key *key, uint32_t format, uint32_t width,
							uint32_t height, uint32_t levels, uint32_t faces,
							const struct astc_store_span *spans, size_t count);
/* Only lookup, release, key_compute, and generation belong on the render thread.
 * The other calls may perform I/O and must run on the worker.
 * root is the game data root. Fingerprints are 64 lowercase SHA-256 hex digits.
 * Stop the worker before shutdown. Existing lookup views remain valid. */
bool astc_store_init(const char *root);
void astc_store_shutdown(void);
bool astc_store_select_map(const char *fingerprint);
bool astc_store_select_shared_map(const char *fingerprint);
bool astc_store_lookup(const struct astc_store_key *key, struct astc_store_record *record);
bool astc_store_lookup_map(const char *fingerprint, const struct astc_store_key *key,
						   struct astc_store_record *record);
void astc_store_release(struct astc_store_record *record);
bool astc_store_write_encoder(const char *fingerprint, const struct astc_store_key *key,
							  const void *bytes, size_t size);
bool astc_store_write_exclusion(const char *fingerprint, const struct astc_store_key *key);
bool astc_store_is_excluded(const char *fingerprint, const struct astc_store_key *key);
bool astc_store_map_complete(const char *fingerprint);
bool astc_store_mark_complete(const char *fingerprint);
void astc_store_cleanup(const char *const *fingerprints, size_t count);
/* Independent of the upscale plan; includes TEXTURE_CACHE_FLOOR_SLACK. */
bool astc_store_has_space(uint64_t projected_bytes);
uint64_t astc_store_generation(void);
#ifdef __cplusplus
}
#endif
#endif
