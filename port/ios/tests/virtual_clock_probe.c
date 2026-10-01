#include "halo_virtual_clock.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    unsigned long frame;

    /* thirty frames are one second on every clock */
    assert(halo_virtual_clock_counter(30) - halo_virtual_clock_counter(0) == HALO_VIRTUAL_CLOCK_RATE);
    assert(halo_virtual_clock_milliseconds(30) - halo_virtual_clock_milliseconds(0) == 1000UL);
    assert(halo_virtual_clock_seconds(30) - halo_virtual_clock_seconds(0) == 1ULL);
    for (frame = 0; frame < 3000; frame++) {
        unsigned long step = halo_virtual_clock_milliseconds(frame + 1) - halo_virtual_clock_milliseconds(frame);

        /* the performance counter moves exactly one count, 1/30 s, per frame */
        assert(halo_virtual_clock_counter(frame + 1) - halo_virtual_clock_counter(frame) == 1ULL);
        /* milliseconds round, but the remainders don't accumulate */
        assert(step == 33UL || step == 34UL);
    }
    assert(halo_virtual_clock_milliseconds(3000) - halo_virtual_clock_milliseconds(0) == 100000UL);
    /* like a real clock, it doesn't start at zero */
    assert(halo_virtual_clock_milliseconds(0) != 0);
    assert(halo_virtual_clock_counter(0) != 0);
    assert(halo_virtual_clock_seconds(0) > 946684800ULL); /* after 2000-01-01 */
    /* ten hours of frames: the header's arithmetic is 64-bit by construction,
       and this checks the values (unsigned long is 64-bit here, 32 on the guest) */
    assert(halo_virtual_clock_milliseconds(1080000UL) - halo_virtual_clock_milliseconds(0) == 36000000UL);
    assert(halo_virtual_clock_seconds(1080000UL) - halo_virtual_clock_seconds(0) == 36000ULL);
    /* debug.exit_after in frames: 0 or less is never, and any positive time quits */
    assert(halo_virtual_clock_exit_frame(40.0) == 1200UL);
    assert(halo_virtual_clock_exit_frame(0.0) == 0UL);
    assert(halo_virtual_clock_exit_frame(-5.0) == 0UL);
    assert(halo_virtual_clock_exit_frame(0.01) == 1UL);
    puts("PASS: virtual clock rate, remainders, starting values, long runs and exit frames");
    return 0;
}
