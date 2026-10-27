#pragma once
#ifdef __OBJC__
#import <Foundation/Foundation.h>
NSString *host_profile_update_folder(NSString *bundled);
/* Resolves verified generations off presentation; the visitor must not call back into launch setup. */
void host_profile_updates_watch(NSString *dropIn, void (^visitor)(NSString *folder, NSDictionary *identity));
NSString *host_recording_update_folder(NSString *bundled);
NSData *host_shader_update_list(const char *name);
NSDictionary *host_data_updates_metadata(void);
/* These calls only read or publish immutable memory snapshots; disk and feeds run in the background. */
void host_data_updates_menu_start(void);
/* profileFinished waits for the fresh check even when cached data already makes Benchmark ready. */
NSDictionary *host_data_updates_menu_state(void);
/* The newest released version for this platform when it is newer than the running build, else nil: read from the
   cached latest-release record, never from the network. A hook for a future update note. */
NSString *host_data_updates_latest_newer(void);
void host_data_updates_menu_finish(void);
#endif
void host_data_updates_healthy(void);
/* 1 after a menu or world frame finishes drawing, 0 before any loading frame. */
void host_data_updates_frame_ready(int ready);
