/* Apache-2.0. Loading/worker-only map fingerprints shared by report collectors. */
#pragma once
#import <Foundation/Foundation.h>
/* Hashing is shared by texture preparation and enabled report collectors. */
void host_map_identity_init(void);
/* Returns an immutable stat-valid cache entry, or nil while hashing asynchronously.
 * These functions inspect files and must never run on a frame or render hook. */
NSDictionary *host_map_identity_lookup(const char *map_name);
NSDictionary *host_map_identity_resolve(const char *map_name);
void host_map_identity_prepare_ui(void);
/* Loading/test barrier for already scheduled work. */
void host_map_identity_wait(void);
