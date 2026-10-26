/* Low Power Mode as the system reports it (host_low_power.m), for low_power.h's frame rate in host_gpu_dispatch.c. */
#ifndef HALO_HOST_LOW_POWER_H
#define HALO_HOST_LOW_POWER_H

/* once, before the first frame: reads the state and watches NSProcessInfoPowerStateDidChangeNotification */
void host_low_power_install(void);
/* whether Low Power Mode was on at the last notice (any thread) */
int host_low_power_enabled(void);
/* the notices seen since host_low_power_install */
unsigned long host_low_power_notices(void);
/* the output's highest refresh rate (an external display's, else the screen's; the Mac app's display), 0 unknown */
double host_low_power_display_rate(void);
/* 1 where Low Power Mode turns a display's faster rates off: an iPhone or an iPad */
int host_low_power_display_limited(void);

#if HALO_LOW_POWER_TESTING
/* low_power_probe.m: read the state from this instead of NSProcessInfo */
void host_low_power_test_reader(int (*reader)(void));
#endif

#endif
