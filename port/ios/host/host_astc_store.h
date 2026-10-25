/* Apache-2.0. Arrival hooks only schedule work; normal level loads never wait. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void host_astc_store_init(const char *data_root);
void host_astc_store_scan(void);
void host_astc_map_arrived(const char *path);
void host_astc_map_will_load(const char *name);
/* A game begins (a level) or the menus open; the worker pauses during play. */
void host_astc_game_started(void);
void astc_store_map_arrived(const char *path);
void astc_store_map_will_load(const char *name);
/* Return zero to leave preparation; committed records resume on the next scan. */
typedef int (*host_astc_progress)(void *context, uint64_t done, uint64_t total);
void host_astc_store_prepare_import(const char *root, host_astc_progress progress, void *context);
#ifdef __cplusplus
}
#endif
