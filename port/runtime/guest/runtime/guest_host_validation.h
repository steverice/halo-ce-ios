/*
GUEST_HOST_VALIDATION.H

Render validation's host imports (port/linux/src/gpu_validate.h), which exist
only in a build configured with it: tools/ios_bridges.py reads this header as
well as guest_host.h when it is given --render-validation.
*/

#ifndef __HALO_GUEST_HOST_VALIDATION_H
#define __HALO_GUEST_HOST_VALIDATION_H

#include <stdint.h>
#include "gpu_validate.h"

void host_gpu_validation_identity(const struct gpu_validation_identity *identity);
void host_gpu_validation_note(uint32_t reason, uint32_t detail, uint32_t value);

#endif
