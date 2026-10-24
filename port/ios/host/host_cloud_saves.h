/* Cloud saves in the host (host_cloud_saves.m): the sync rules of port/runtime/cloud_saves.c run on
   a queue of their own against the player's private iCloud database (cloud_store_cloudkit.m).
   Compiled only with HALO_CLOUD_SAVES (tools/ios_build.py --cloud-saves); without it these are
   nothing, and the app is as it was. See docs/cloud-saves.md. */
#ifndef HOST_CLOUD_SAVES_H
#define HOST_CLOUD_SAVES_H

#if HALO_CLOUD_SAVES
/* The Settings app and guest menu share this device-local opt-in preference.
   Enabling waits for the next launch; disabling stops subsequent sync passes. */
int host_cloud_saves_enabled(void);
void host_cloud_saves_set_enabled(int enabled);
const char *host_cloud_saves_status(void);
/* Store operations also check the run gate after an in-flight operation finishes. */
int host_cloud_saves_sync_allowed(void);
/* As soon as the data folder is known, before the import screen: starts the launch pass (restores,
   downloads, uploads) when the build's signature carries the iCloud container, the player has not
   turned the setting off, and this is not a test run. */
void host_cloud_saves_start(const char *data_root, const char *save_root);
/* Just before the game starts, on the main thread: waits a bounded time for the launch pass while
   running the run loop, shows what the player should know, and from then on lets nothing change the
   local saves but the game. */
void host_cloud_saves_before_game(void);
/* The game removed a directory (host_syscall.c). Any thread; does nothing for other paths. */
void host_cloud_saves_directory_removed(const char *path);
/* The app is about to exit: one last upload pass, for at most this long. */
void host_cloud_saves_leaving(double seconds);

struct cloud_store;
/* cloud_store_cloudkit.m: the store on the container's private database; 0 when the container
   cannot be opened (no entitlement in this signature) */
int host_cloudkit_store(const char *container, struct cloud_store *store);
#else
static inline int host_cloud_saves_enabled(void) { return 0; }
static inline void host_cloud_saves_set_enabled(int enabled) { (void)enabled; }
static inline const char *host_cloud_saves_status(void) { return "Not available in this build"; }
static inline void host_cloud_saves_start(const char *data_root, const char *save_root) { (void)data_root; (void)save_root; }
static inline void host_cloud_saves_before_game(void) {}
static inline void host_cloud_saves_directory_removed(const char *path) { (void)path; }
static inline void host_cloud_saves_leaving(double seconds) { (void)seconds; }
#endif

#endif
