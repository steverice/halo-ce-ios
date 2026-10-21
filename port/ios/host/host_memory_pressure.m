/* Memory pressure: the system's warnings ask the texture cache to drop what
   was not drawn lately. Only a flag is set here, on whatever thread the system
   uses; the guest's game thread reads it (host_memory_pressure_poll) and drops
   the textures at the start of its next frame (xgpu_texture_cache_begin_frame).
   Textures load again when drawn. */
#include <stdatomic.h>
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <notify.h>
#include <os/proc.h>
#include "host.h"
#if !HALO_NATIVE_MAC
#import <UIKit/UIKit.h>
#endif

static atomic_int pressure_pending;

int host_memory_pressure_poll(void) {
    return atomic_exchange(&pressure_pending,0);
}

/* -1 where the system does not say */
int host_memory_available_megabytes(void) {
#if HALO_NATIVE_MAC
    return -1;
#else
    return (int)(os_proc_available_memory()>>20);
#endif
}

/* `notifyutil -p visr.simulate-memory-pressure` asks for the same on the Mac
   and in the simulator, where the system raises no warning of its own (the
   simulator's Simulate Memory Warning and the `memory_pressure` tool do too) */
#define HOST_MEMORY_PRESSURE_NOTIFICATION "visr.simulate-memory-pressure"

static void host_memory_pressure_note(const char *source) {
    host_logf(HOST_LOG_INFO,"memory: pressure (%s), textures not drawn lately will be dropped",source);
    atomic_store(&pressure_pending,1);
}

void host_memory_pressure_start(void) {
    static dispatch_source_t source;
    static int token;

#if !HALO_NATIVE_MAC
    [[NSNotificationCenter defaultCenter] addObserverForName:UIApplicationDidReceiveMemoryWarningNotification
        object:nil queue:nil usingBlock:^(NSNotification *note) {
            (void)note;
            host_memory_pressure_note("memory warning");
        }];
#endif
    source=dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE,0,
        DISPATCH_MEMORYPRESSURE_WARN|DISPATCH_MEMORYPRESSURE_CRITICAL,
        dispatch_get_global_queue(QOS_CLASS_UTILITY,0));
    dispatch_source_set_event_handler(source,^{
        host_memory_pressure_note(dispatch_source_get_data(source)&DISPATCH_MEMORYPRESSURE_CRITICAL?"critical":"warning");
    });
    dispatch_resume(source);
    notify_register_dispatch(HOST_MEMORY_PRESSURE_NOTIFICATION,&token,dispatch_get_global_queue(QOS_CLASS_UTILITY,0),
        ^(int t){(void)t;host_memory_pressure_note("requested");});
}
