/* Immutable recordings for one benchmark session. This also accepts trusted bundled legacy routes;
   downloaded snapshots have already passed host_recording_pack_validate before reaching this helper. */
#import <Foundation/Foundation.h>

/* Verify exact file inventory/content and the current guest/profile/settings identity. An active update is
   deliberately absent from identity: an unfinished session retains its original source and update metadata. */
NSDictionary *host_benchmark_snapshot_load(NSString *folder, NSDictionary *identity);
/* Copy sources in order (later files override earlier ones) into folder/recordings. A completed private
   staging folder, including .snapshot.json, replaces the previous snapshot atomically. No source is changed. */
NSDictionary *host_benchmark_snapshot_create(NSString *folder, NSArray<NSString *> *sources,
    NSDictionary *identity, NSDictionary *updates, NSError **error);
/* SHA-256 of sorted JSON, also used for the resolved device profile's content identity. */
NSString *host_benchmark_snapshot_digest(id value);

/* Read a bounded, regular map capture without exporting a device path. Snapshot text is cumulative;
   delta emits only complete records appended between the two snapshots. Missing evidence stays explicit. */
NSDictionary *host_benchmark_shader_snapshot(NSString *folder, NSString *map);
NSDictionary *host_benchmark_shader_delta(NSDictionary *before, NSDictionary *after);

/* Assemble current dependency candidates and prior display-mode intervals for one portable report. */
NSDictionary *host_benchmark_shader_evidence(NSArray<NSDictionary *> *runs, NSArray<NSDictionary *> *previous, NSString *folder);
/* A deep mutable JSON copy only: replace descriptor bodies in the consent preview/search summary. */
void host_benchmark_shader_summarize(NSMutableDictionary *report);
