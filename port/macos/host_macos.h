#pragma once
/* Native host services; SDL owns the Cocoa window and Metal view. */
const char *host_macos_data_root(void);
void host_macos_install(void);
void host_macos_window_ready(void *sdl_window);
double host_macos_refresh_rate(void);
#ifdef __OBJC__
@class CADisplayLink;
/* Create on the main thread; caller schedules and invalidates on its pacing thread. */
CADisplayLink *host_macos_display_link(id target, SEL selector);
#endif
