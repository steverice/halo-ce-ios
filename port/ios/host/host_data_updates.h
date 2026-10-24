#pragma once
#ifdef __OBJC__
#import <Foundation/Foundation.h>
NSString *host_profile_update_folder(NSString *bundled);
/* Resolves verified generations off presentation; the visitor must not call back into launch setup. */
void host_profile_updates_watch(NSString *dropIn, void (^visitor)(NSString *folder, NSDictionary *identity));
NSString *host_recording_update_folder(NSString *bundled);
NSData *host_shader_update_list(const char *name);
NSDictionary *host_data_updates_metadata(void);
#endif
void host_data_updates_healthy(void);
/* 1 after a menu or world frame finishes drawing, 0 before any loading frame. */
void host_data_updates_frame_ready(int ready);
