/* Ordinary-play contributions. Every entry point is inert in ineligible builds. */
#pragma once
#include <stdint.h>
struct shader_observation;
void host_shader_upload_start(const char *guest_sha256);
void host_shader_upload_suspend(void);
void host_shader_upload_prepare(void);
void host_shader_upload_settings(void);
uint32_t host_shader_observation_activate(const char *map_name, int suppressed, char *map_key, uint32_t map_key_size);
void host_shader_observation_append(uint32_t token, const struct shader_observation *descriptor);
int host_shader_observation_recording(void);
#ifdef __OBJC__
#import <Foundation/Foundation.h>
BOOL host_shader_upload_validate(NSDictionary *payload);
NSData *host_shader_upload_gzip(NSData *payload);
NSTimeInterval host_shader_upload_retry(NSUInteger attempt, NSTimeInterval cloud_delay, double jitter);
#endif
