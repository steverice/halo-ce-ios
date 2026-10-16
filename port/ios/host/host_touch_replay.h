/* Shared replay gate for UIKit event delivery and the virtual gamepad reader. */
#ifndef HALO_HOST_TOUCH_REPLAY_H
#define HALO_HOST_TOUCH_REPLAY_H
#include <stdatomic.h>

struct host_touch_replay_gate { atomic_int active; };

static inline int host_touch_replay_blocked(struct host_touch_replay_gate *gate) {
    return atomic_load(&gate->active);
}

/* Publish the gate before clearing held input, so a queued press cannot reapply it. */
static inline int host_touch_replay_set(struct host_touch_replay_gate *gate, int active, void (*reset)(void)) {
    active = active != 0;
    if (atomic_exchange(&gate->active, active) == active) return 0;
    reset();
    return 1;
}
#endif
