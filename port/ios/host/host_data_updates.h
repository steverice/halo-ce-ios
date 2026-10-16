#pragma once
#ifdef __OBJC__
#import <Foundation/Foundation.h>
NSString *host_profile_update_folder(NSString *bundled);
NSString *host_recording_update_folder(NSString *bundled);
NSDictionary *host_data_updates_metadata(void);
#endif
void host_data_updates_healthy(void);
/* 1 after a menu or world frame finishes drawing, 0 before any loading frame. */
void host_data_updates_frame_ready(int ready);
