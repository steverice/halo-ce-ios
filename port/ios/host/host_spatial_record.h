/* The pose stream's recorder and replayer (debug.controller_record,
debug.controller_replay), for host_spatial.m. Plain C, so it runs on every
host (the Mac runner, the simulator, the headset).

A recording is NAME.poses in the working directory (the app's data folder: on
a headset, its Documents folder): the line "halo pose recording 1\n", then one
record per sample, in the host's byte order: uint32 frame (the game samples
taken so far), uint32 kind (HALO_SPATIAL_SAMPLE_GAME or _DRAW), and the
struct halo_spatial_sample as it crosses to the guest (halo_spatial.h's
pinned layout). */
#ifndef HOST_SPATIAL_RECORD_H
#define HOST_SPATIAL_RECORD_H

#include "halo_spatial.h"

/* debug.controller_record: open NAME.poses for writing ("" for none). Logs
once; a file that can't be opened is logged and nothing records */
void host_spatial_record_start(const char *name);
/* append a sample of this kind; flushes once a second. A game sample counts
as a new frame */
void host_spatial_record_write(int kind, const struct halo_spatial_sample *sample);

/* debug.controller_replay: load NAME.poses ("" for none). Returns 1 with the
file's samples in memory (logs "spatial: replaying NAME (n samples)"), 0 with
a warning for a missing or malformed file */
int host_spatial_replay_start(const char *name);
/* the next recorded sample of this kind in its own order, holding the last at
the end; synthetic is set to 2. Returns 0 if the file has none of this kind
(the sample is left untouched) */
int host_spatial_replay_next(int kind, struct halo_spatial_sample *sample);

#endif
