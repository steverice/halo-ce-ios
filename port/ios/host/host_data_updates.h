#pragma once
#ifdef __OBJC__
#import <Foundation/Foundation.h>
NSString *host_profile_update_folder(NSString *bundled);
NSString *host_recording_update_folder(NSString *bundled);
NSDictionary *host_data_updates_metadata(void);
#endif
void host_data_updates_healthy(void);
