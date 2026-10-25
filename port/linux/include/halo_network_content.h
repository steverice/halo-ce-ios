/* Content fields established by the version 38 black-box format description.
   These helpers do not implement the new game modes or shield presentations. */
#ifndef HALO_NETWORK_CONTENT_H
#define HALO_NETWORK_CONTENT_H

#include "halo_port_limits.h"

#define HALO_NETWORK_LEGACY_OPTIONS_SIZE 28
#define HALO_NETWORK_MODE_OPTIONS_SIZE 16
#define HALO_NETWORK_OPTIONS_SIZE 44
#define HALO_NETWORK_LEGACY_SETTINGS_SIZE (HALO_PORT_NETWORK_GAME_SIZE - HALO_NETWORK_MODE_OPTIONS_SIZE)

static inline char const *halo_network_kind_label(unsigned kind)
{
	if (kind & 0x7C || kind > 0xFF) return "UNSUPPORTED";
	if (kind & 0x80) return "HARDCORE";
	switch (kind)
	{
	case 0: return "";
	case 1: return "SWAT";
	case 2: return "INFECTION";
	case 3: return "GUN GAME";
	default: return "UNSUPPORTED";
	}
}

static inline char const *halo_network_kind_reason(unsigned kind)
{
	if (kind & 0x7C || kind > 0xFF) return "This game advertises an unknown game kind.";
	if (kind & 0x80) return "Hardcore rules are not supported by VISR.";
	switch (kind)
	{
	case 0: return 0;
	case 1: return "SWAT rules are not supported by VISR.";
	case 2: return "Infection is not supported by VISR.";
	case 3: return "Gun Game is not supported by VISR.";
	default: return "This game advertises an unknown game kind.";
	}
}

static inline void halo_network_options_defaults(unsigned char *extension)
{
	static unsigned char const defaults[HALO_NETWORK_MODE_OPTIONS_SIZE] =
		{0, 0, 1, 150, 200, 5, 10, 1, 0, 1, 0, 0, 0, 0, 0, 0};
	unsigned i;
	for (i = 0; i < HALO_NETWORK_MODE_OPTIONS_SIZE; i++) extension[i] = defaults[i];
}

static inline char const *halo_network_options_reason(unsigned char const *extension)
{
	unsigned i;
	char const *reason = halo_network_kind_reason(extension[0]);
	if (reason) return reason;
	if (extension[1]) return "Hardcore rules are not supported by VISR.";
	/* Inactive Infection and Gun Game parameters are preserved, not enabled.
	   The six trailing bytes have no established meaning; refuse nonzero data. */
	for (i = 10; i < HALO_NETWORK_MODE_OPTIONS_SIZE; i++)
		if (extension[i]) return "This game uses options that VISR does not support.";
	return 0;
}

/* The 29–34 record has a 28-byte options block and the same four-byte
   local tail. Move that tail before filling the new extension. */
static inline void halo_network_settings_expand_legacy(unsigned char *settings)
{
	unsigned i;
	for (i = 0; i < 4; i++)
		settings[HALO_PORT_NETWORK_GAME_LOCAL_DATA_OFFSET + i] =
			settings[HALO_PORT_NETWORK_GAME_LOCAL_DATA_OFFSET - HALO_NETWORK_MODE_OPTIONS_SIZE + i];
	halo_network_options_defaults(settings + HALO_PORT_NETWORK_GAME_VARIANT_OPTIONS_OFFSET + HALO_NETWORK_LEGACY_OPTIONS_SIZE);
}

#endif
