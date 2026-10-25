#ifndef ASTC_ENCODE_H
#define ASTC_ENCODE_H
#include "astc_rewrite.h"
/* Bounded tile buffers, excluding the output candidate and reusable codec context. */
#define ASTC_ENCODER_CONTEXT_RESERVE (2u * 1024u * 1024u)
#define ASTC_ENCODER_TILE_SCRATCH (128u * 128u * 9u)
#define ASTC_ENCODER_VERSION "astcenc-baff485b-fast-4x4-alpha-v2"
#ifdef __cplusplus
extern "C" {
#endif
/* Coverage boundaries use alpha >= r: D3DCMP_GREATER reference 127 is r=128.
   Unknown and blended uses keep the all-boundaries gate. An empty known mask
   is only valid when every material use is known not to test or blend alpha. */
struct astc_alpha_policy
{
	uint8_t boundaries[32];
	bool known;
};
enum astc_encode_result { ASTC_ENCODE_OK, ASTC_ENCODE_EXCLUDED, ASTC_ENCODE_ERROR };
/* Discardable output, with the same publication rules as the worker API below. */
enum astc_encode_result astc_encode_level_policy(enum astc_dxt_format format, const void *source,
	unsigned width, unsigned height, void *output, const struct astc_alpha_policy *policy);
/* Encode one DXT level as ASTC 4x4 LDR; failed quality checks leave output untouched.
   Source includes padded edge blocks; output requires ceil(w/4)*ceil(h/4)*16 bytes.
   Safe to call from background threads; each thread owns a single-thread encoder. */
enum astc_encode_result astc_encode_level_result(enum astc_dxt_format format, const void *source,
	unsigned width, unsigned height, void *output);
/* Worker output may be a mapped temporary file. It can contain partial bytes on
   exclusion/error and must only be published after ASTC_ENCODE_OK. No level-sized
   candidate is allocated; auxiliary copies fit TILE_SCRATCH + CONTEXT_RESERVE. */
enum astc_encode_result astc_encode_level_discardable(enum astc_dxt_format format, const void *source,
	unsigned width, unsigned height, void *output);
bool astc_encode_level(enum astc_dxt_format format, const void *source,
	unsigned width, unsigned height, void *output);
#ifdef __cplusplus
}
#endif
#endif
