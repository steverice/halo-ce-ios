#pragma once
#ifdef __OBJC__
#import <Foundation/Foundation.h>
NSString *host_profile_update_folder(NSString *bundled);
NSString *host_recording_update_folder(NSString *bundled);
NSData *host_shader_update_list(const char *name);
NSDictionary *host_data_updates_metadata(void);
/* These calls only read or publish immutable memory snapshots; disk and feeds run in the background. */
void host_data_updates_menu_start(void);
NSDictionary *host_data_updates_menu_state(void);
void host_data_updates_menu_finish(void);
#endif
void host_data_updates_healthy(void);
/* 1 after a menu or world frame finishes drawing, 0 before any loading frame. */
void host_data_updates_frame_ready(int ready);
