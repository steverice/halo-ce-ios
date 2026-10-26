/* Low Power Mode as the system reports it: NSProcessInfo's lowPowerModeEnabled, read at install and again at every
   NSProcessInfoPowerStateDidChangeNotification, which can come on any thread. The frame rate it leads to is
   low_power.h's, latched between frames by host_gpu_dispatch.c. An Apple TV reports it off; a Mac reports it from
   macOS 12. */
#import <Foundation/Foundation.h>
#include "host_low_power.h"
#include <stdatomic.h>
#include <TargetConditionals.h>

#if HALO_NATIVE_MAC
#include "host_macos.h"
#elif !HALO_LOW_POWER_TESTING
#include "host_external.h"
#endif

static _Atomic int enabled;
static _Atomic unsigned long notices;

static int process_reader(void)
{
    if (@available(iOS 9.0, tvOS 9.0, macOS 12.0, *))
        return NSProcessInfo.processInfo.lowPowerModeEnabled ? 1 : 0;
    return 0;
}

static int (*reader)(void) = process_reader;

#if HALO_LOW_POWER_TESTING
void host_low_power_test_reader(int (*test_reader)(void))
{
    reader = test_reader ? test_reader : process_reader;
}
#endif

void host_low_power_install(void)
{
    static dispatch_once_t once;

    dispatch_once(&once, ^{
        atomic_store(&enabled, reader());
        [NSNotificationCenter.defaultCenter addObserverForName:NSProcessInfoPowerStateDidChangeNotification object:nil
            queue:nil usingBlock:^(NSNotification *note) {
                (void)note;
                atomic_store(&enabled, reader());
                atomic_fetch_add(&notices, 1);
            }];
    });
}

int host_low_power_enabled(void)
{
    return atomic_load(&enabled);
}

unsigned long host_low_power_notices(void)
{
    return atomic_load(&notices);
}

double host_low_power_display_rate(void)
{
#if HALO_LOW_POWER_TESTING
    return 0.0;
#elif HALO_NATIVE_MAC
    return host_macos_refresh_rate();
#else
    struct host_external_snapshot output;

    host_external_snapshot(&output);
    return output.refresh_hertz > 0 ? (double)output.refresh_hertz : 0.0;
#endif
}

int host_low_power_display_limited(void)
{
#if TARGET_OS_IOS && !TARGET_OS_MACCATALYST && !HALO_NATIVE_MAC
    return 1;
#else
    return 0;
#endif
}
