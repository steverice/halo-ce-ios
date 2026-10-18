#ifndef HOST_RECORDING_PACK_H
#define HOST_RECORDING_PACK_H

/* Benchmark case count is bounded by the generic planner, not by built-in scene definitions. */
#define HOST_RECORDING_PACK_MAX_ROUTES 16
#define HOST_RECORDING_PACK_MAX_FILES 160
#define HOST_RECORDING_PACK_MAX_FILE_BYTES (4 * 1024 * 1024)
#define HOST_RECORDING_PACK_MAX_TOTAL_BYTES (12 * 1024 * 1024)

/* 1: a complete, portable snapshot; 0: rejected. Bundled legacy recordings are not validated here.
   The caller keeps folder private and unchanged throughout validation and staging. */
int host_recording_pack_validate(const char *folder, void (*log)(const char *));

#endif
