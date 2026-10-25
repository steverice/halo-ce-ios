/* When original textures are converted to ASTC 4x4 (debug.texture_astc). */
#ifndef ASTC_POLICY_H
#define ASTC_POLICY_H
#include <string.h>

/* Release policy for "auto", the default: convert only on GPUs that lack BC
 * (MTLDevice.supportsBCTextureCompression). Keep 0 until the conversion has
 * passed device validation; set it to 1 (or build with -DVISR_ASTC_AUTOMATIC=1)
 * to turn automatic conversion on. "force" and "off" ignore it. */
#ifndef VISR_ASTC_AUTOMATIC
#define VISR_ASTC_AUTOMATIC 0
#endif

/* "force" emulates a GPU without BC for validation; "off" disables conversion.
 * Any other value, including "auto" and the older "on", follows the GPU. */
static inline int astc_policy_converts(const char *mode, int bc_supported)
{
	if (mode && !strcmp(mode, "force"))
		return 1;
	if (mode && !strcmp(mode, "off"))
		return 0;
	return VISR_ASTC_AUTOMATIC && !bc_supported;
}
#endif
