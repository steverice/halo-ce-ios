#include "host_touch_replay.h"
#include <assert.h>
#include <stdio.h>

static struct host_touch_replay_gate gate;
static int held_button, held_axis, resets;
static void reset(void) { held_button = held_axis = 0; resets++; }
static void press(void) {
    if (!host_touch_replay_blocked(&gate)) { held_button = 1; held_axis = 123; }
}
int main(void) {
    press();
    assert(held_button && held_axis);
    assert(host_touch_replay_set(&gate, 1, reset));
    assert(!held_button && !held_axis && resets == 1);
    press();
    assert(!held_button && !held_axis);
    assert(!host_touch_replay_set(&gate, 1, reset));
    assert(resets == 1);
    assert(host_touch_replay_set(&gate, 0, reset));
    assert(!held_button && !held_axis);
    press();
    assert(held_button && held_axis && resets == 2);
    assert(host_touch_replay_set(&gate, 1, reset));
    assert(!held_button && !held_axis && resets == 3);
    puts("touch replay gate: passed");
}
