#include "host_touch_controller.h"
#include <assert.h>
#include <stdio.h>

enum { TOUCH = 7, PAD = 9 };

int main(void) {
    /* a hardware controller's press or a firm push puts the overlay away */
    assert(host_touch_controller_hides(HOST_TOUCH_CONTROLLER_BUTTON_DOWN, PAD, TOUCH, 0));
    assert(host_touch_controller_hides(HOST_TOUCH_CONTROLLER_AXIS, PAD, TOUCH, 32767));
    assert(host_touch_controller_hides(HOST_TOUCH_CONTROLLER_AXIS, PAD, TOUCH, -32768));
    assert(host_touch_controller_hides(HOST_TOUCH_CONTROLLER_AXIS, PAD, TOUCH, HOST_TOUCH_CONTROLLER_AXIS_THRESHOLD));
    /* a drifting stick, a trigger at rest and other events do not */
    assert(!host_touch_controller_hides(HOST_TOUCH_CONTROLLER_AXIS, PAD, TOUCH, 4000));
    assert(!host_touch_controller_hides(HOST_TOUCH_CONTROLLER_AXIS, PAD, TOUCH, -4000));
    assert(!host_touch_controller_hides(HOST_TOUCH_CONTROLLER_AXIS, PAD, TOUCH, 0));
    assert(!host_touch_controller_hides(HOST_TOUCH_CONTROLLER_OTHER, PAD, TOUCH, 32767));
    /* the touch controls' own gamepad never hides itself */
    assert(!host_touch_controller_hides(HOST_TOUCH_CONTROLLER_BUTTON_DOWN, TOUCH, TOUCH, 0));
    assert(!host_touch_controller_hides(HOST_TOUCH_CONTROLLER_AXIS, TOUCH, TOUCH, 32767));
    puts("touch overlay controller input: passed");
}
