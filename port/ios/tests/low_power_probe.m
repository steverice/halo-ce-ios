/* Low Power Mode's frame rate (low_power.h) and its notices (host_low_power.m), natively (tools/ios_test.py): the
   power-state notification is posted here as the system posts it, with the state read from a made-up switch, and
   the rate each kind of frame aims for is checked across the transitions. host_gpu_dispatch.c's own functions are
   driven through the same transitions by data_pack_launch_probe.m. */
#import <Foundation/Foundation.h>
#include "host_low_power.h"
#include "low_power.h"
#include <math.h>
#include <stdio.h>

static int failures, checks;
#define CHECK(condition) do { checks++; if (!(condition)) { failures++; \
    fprintf(stderr, "low_power_probe: line %d: %s\n", __LINE__, #condition); } } while (0)

enum { GAMEPLAY, MENU };

static int system_state;
static int read_state(void) { return system_state; }

static void post(int on)
{
    system_state = on;
    [NSNotificationCenter.defaultCenter postNotificationName:NSProcessInfoPowerStateDidChangeNotification
        object:NSProcessInfo.processInfo];
}

static void caps(void)
{
    /* a ProMotion iPhone: "display" (0) and 120 go to 60; a profile's 60 to 30 */
    CHECK(low_power_cap(0.0, 120.0, 1, GAMEPLAY) == 60.0);
    CHECK(low_power_cap(120.0, 120.0, 1, GAMEPLAY) == 60.0);
    CHECK(low_power_cap(60.0, 120.0, 1, GAMEPLAY) == 30.0);
    /* (whether or not the screen still says 120 in Low Power Mode) */
    CHECK(low_power_cap(0.0, 60.0, 1, GAMEPLAY) == 30.0);
    /* a 60 Hz device: 30; a profile's or a cutscene's 30 stays 30, the game's tick rate */
    CHECK(low_power_cap(0.0, 60.0, 0, GAMEPLAY) == 30.0);
    CHECK(low_power_cap(30.0, 60.0, 0, GAMEPLAY) == 30.0);
    CHECK(low_power_target(30.0, low_power_cap(30.0, 120.0, 1, GAMEPLAY)) == 30.0);
    CHECK(low_power_target(40.0, low_power_cap(40.0, 60.0, 0, GAMEPLAY)) == 30.0);
    /* menus keep the display's pace: 60 on a ProMotion display, which Low Power Mode holds there; none at 60 Hz */
    CHECK(low_power_cap(0.0, 120.0, 1, MENU) == 60.0);
    CHECK(low_power_cap(0.0, 60.0, 1, MENU) == 0.0);
    CHECK(low_power_cap(0.0, 120.0, 0, MENU) == 0.0);
    CHECK(low_power_target(30.0, low_power_cap(30.0, 120.0, 1, MENU)) == 30.0);
    /* a Mac's 120 Hz display ("display" or "unlimited", 0 here): 60; a cap over the display: the display's half */
    CHECK(low_power_cap(0.0, 120.0, 0, GAMEPLAY) == 60.0);
    CHECK(low_power_cap(240.0, 120.0, 0, GAMEPLAY) == 60.0);
    CHECK(low_power_cap(0.0, 144.0, 0, GAMEPLAY) == 72.0);
    /* an unknown display: 60's half; never over a ProMotion display's 60 */
    CHECK(low_power_cap(0.0, 0.0, 0, GAMEPLAY) == 30.0);
    CHECK(low_power_cap(0.0, 0.0, 1, GAMEPLAY) == 30.0);
    /* the composition: the lower of the two, 0 for the display's */
    CHECK(low_power_target(0.0, 0.0) == 0.0);
    CHECK(low_power_target(0.0, 60.0) == 60.0);
    CHECK(low_power_target(24.0, 30.0) == 24.0);
    CHECK(low_power_override_setting("true") == LOW_POWER_FOLLOW);
    CHECK(low_power_override_setting("") == LOW_POWER_FOLLOW);
    CHECK(low_power_override_setting("false") == LOW_POWER_IGNORE);
    CHECK(low_power_override_setting("always") == LOW_POWER_ALWAYS);
}

/* the notices as frames see them: nothing until the latch between frames, then one transition each way */
static void transitions(void)
{
    struct low_power state = { LOW_POWER_FOLLOW, 0, 0.0 };
    unsigned long seen;

    host_low_power_test_reader(read_state);
    system_state = 1;
    host_low_power_install();
    /* (read at install: on from the first frame) */
    CHECK(host_low_power_enabled() == 1);
    post(0);
    CHECK(host_low_power_enabled() == 0);
    CHECK(low_power_latch(&state, host_low_power_enabled(), 0.0, 120.0, 1, GAMEPLAY) == LOW_POWER_UNCHANGED);
    CHECK(state.cap == 0.0);

    seen = host_low_power_notices();
    post(1);
    CHECK(host_low_power_notices() == seen + 1 && host_low_power_enabled() == 1);
    /* a notice from another thread, as the system may send it */
    {
        dispatch_semaphore_t done = dispatch_semaphore_create(0);

        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            post(1);
            dispatch_semaphore_signal(done);
        });
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        CHECK(host_low_power_notices() == seen + 2 && host_low_power_enabled() == 1);
    }
    CHECK(state.cap == 0.0);
    CHECK(low_power_latch(&state, host_low_power_enabled(), 0.0, 120.0, 1, GAMEPLAY) == LOW_POWER_TURNED_ON);
    CHECK(state.on && state.cap == 60.0 && low_power_target(0.0, state.cap) == 60.0);
    CHECK(low_power_latch(&state, host_low_power_enabled(), 0.0, 120.0, 1, GAMEPLAY) == LOW_POWER_UNCHANGED);
    /* into the menu and out again: the display's 60, then gameplay's 60 */
    CHECK(low_power_latch(&state, host_low_power_enabled(), 0.0, 120.0, 1, MENU) == LOW_POWER_UNCHANGED);
    CHECK(state.cap == 60.0);
    /* a cutscene's 30 */
    CHECK(low_power_latch(&state, host_low_power_enabled(), 30.0, 120.0, 1, GAMEPLAY) == LOW_POWER_CAP_CHANGED);
    CHECK(low_power_target(30.0, state.cap) == 30.0);
    CHECK(low_power_latch(&state, host_low_power_enabled(), 0.0, 120.0, 1, GAMEPLAY) == LOW_POWER_CAP_CHANGED);
    CHECK(state.cap == 60.0);

    post(0);
    CHECK(low_power_latch(&state, host_low_power_enabled(), 0.0, 120.0, 1, GAMEPLAY) == LOW_POWER_TURNED_OFF);
    CHECK(!state.on && state.cap == 0.0 && low_power_target(0.0, state.cap) == 0.0);
    CHECK(low_power_latch(&state, host_low_power_enabled(), 0.0, 120.0, 1, GAMEPLAY) == LOW_POWER_UNCHANGED);

    /* debug.low_power_frame_rate */
    state.override = LOW_POWER_ALWAYS;
    CHECK(low_power_latch(&state, 0, 60.0, 60.0, 0, GAMEPLAY) == LOW_POWER_TURNED_ON && state.cap == 30.0);
    state.override = LOW_POWER_IGNORE;
    CHECK(low_power_latch(&state, 1, 60.0, 60.0, 0, GAMEPLAY) == LOW_POWER_TURNED_OFF && state.cap == 0.0);
    host_low_power_test_reader(NULL);
}

int main(void)
{
    @autoreleasepool {
        caps();
        transitions();
    }
    if (failures)
        fprintf(stderr, "low_power_probe: %d of %d checks failed\n", failures, checks);
    else
        printf("low_power_probe: %d checks passed\n", checks);
    return failures != 0;
}
