#ifndef HOST_RECORDING_PACK_H
#define HOST_RECORDING_PACK_H

#include "../../runtime/replay_environment.h"

/* Benchmark case count is bounded by the generic planner, not by built-in scene definitions. */
#define HOST_RECORDING_PACK_MAX_ROUTES 16
#define HOST_RECORDING_PACK_MAX_FILES 160
#define HOST_RECORDING_PACK_MAX_FILE_BYTES (4 * 1024 * 1024)
/* No separate aggregate library cap: the existing file count and individual file
   bounds determine the largest valid pack. Use 64-bit arithmetic for encoding. */
#define HOST_RECORDING_PACK_MAX_TOTAL_BYTES (1ULL * HOST_RECORDING_PACK_MAX_FILES * HOST_RECORDING_PACK_MAX_FILE_BYTES)
/* A validated route has three large streams and two metadata files (read through
   4096-byte buffers), plus at most one catalog. Bound unauthenticated downloads
   by that grammar instead of assuming every allowed filename is a large stream. */
#define HOST_RECORDING_PACK_MAX_ENVELOPE_CONTENT_BYTES (HOST_RECORDING_PACK_MAX_ROUTES * (3ULL * HOST_RECORDING_PACK_MAX_FILE_BYTES + 2ULL * 4095ULL) + HOST_RECORDING_PACK_MAX_FILE_BYTES)
/* Base64 wraps each stream and then JSON. Per-entry allowance includes base64
   padding, names and punctuation; the remaining allowance covers manifests. */
#define HOST_RECORDING_PACK_MAX_ENVELOPE_BYTES (((((HOST_RECORDING_PACK_MAX_ENVELOPE_CONTENT_BYTES + 2ULL) / 3ULL) * 4ULL + HOST_RECORDING_PACK_MAX_FILES * 128ULL + 65536ULL + 2ULL) / 3ULL) * 4ULL + 65536ULL)

/* 1: a complete, portable snapshot; 0: rejected. Bundled legacy recordings are not validated here.
   The caller keeps folder private and unchanged throughout validation and staging. */
int host_recording_pack_validate(const char *folder, void (*log)(const char *));

#endif
