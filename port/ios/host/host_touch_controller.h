/* Which gamepad input puts the touch overlay away again (host_touch.m): a
   hardware controller's button press, or one of its sticks or triggers pushed
   past half way. The touch controls' own virtual gamepad never counts, nor does
   a stick resting off center. Kept free of SDL so the probe builds alone. */
#ifndef HALO_HOST_TOUCH_CONTROLLER_H
#define HALO_HOST_TOUCH_CONTROLLER_H
#include <stdint.h>

enum host_touch_controller_input {
    HOST_TOUCH_CONTROLLER_OTHER,
    HOST_TOUCH_CONTROLLER_BUTTON_DOWN,
    HOST_TOUCH_CONTROLLER_AXIS,
};

#define HOST_TOUCH_CONTROLLER_AXIS_THRESHOLD 16384

static inline int host_touch_controller_hides(enum host_touch_controller_input input,
    uint32_t device, uint32_t touch_device, int value) {
    if (device == touch_device) return 0;
    switch (input) {
    case HOST_TOUCH_CONTROLLER_BUTTON_DOWN: return 1;
    case HOST_TOUCH_CONTROLLER_AXIS:
        return value >= HOST_TOUCH_CONTROLLER_AXIS_THRESHOLD || value <= -HOST_TOUCH_CONTROLLER_AXIS_THRESHOLD;
    default: return 0;
    }
}
#endif
