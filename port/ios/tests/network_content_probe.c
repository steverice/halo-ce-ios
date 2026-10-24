/* Fixtures transcribed from the permitted black-box format description. */
#include "../../linux/include/halo_network_content.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	unsigned char defaults[16] = {0, 0, 1, 150, 200, 5, 10, 1, 0, 1, 0, 0, 0, 0, 0, 0};
	unsigned char extension[16];
	unsigned char settings[HALO_PORT_NETWORK_GAME_SIZE];
	unsigned i;

	assert(HALO_PORT_NETWORK_GAME_VARIANT_OPTIONS_OFFSET == 13088);
	assert(HALO_PORT_NETWORK_GAME_SIZE == 13136);
	halo_network_options_defaults(extension);
	assert(!memcmp(extension, defaults, sizeof(defaults)));
	assert(!halo_network_options_reason(extension));
	/* Edited inactive Infection options remain ordinary Slayer. */
	extension[2] = 2; extension[3] = 175; extension[4] = 250;
	extension[5] = 10; extension[6] = 15; extension[7] = 0;
	assert(!halo_network_options_reason(extension));
	for (i = 1; i < 256; i++)
	{
		extension[0] = (unsigned char)i;
		assert(halo_network_options_reason(extension));
		assert(halo_network_kind_reason(i));
	}
	extension[0] = 0;
	extension[1] = 1;
	assert(strstr(halo_network_options_reason(extension), "Hardcore"));
	extension[1] = 0;
	extension[15] = 1;
	assert(halo_network_options_reason(extension));
	assert(!halo_network_kind_reason(0));
	assert(!strcmp(halo_network_kind_label(1), "SWAT"));
	assert(!strcmp(halo_network_kind_label(2), "INFECTION"));
	assert(!strcmp(halo_network_kind_label(3), "GUN GAME"));
	assert(!strcmp(halo_network_kind_label(128), "HARDCORE"));
	assert(halo_network_kind_reason(131));

	memset(settings, 0xA5, sizeof(settings));
	/* Preserve the entire meaningful legacy prefix and the four-byte local tail. */
	settings[13116] = 0x11; settings[13117] = 0x22;
	settings[13118] = 0x33; settings[13119] = 0x44;
	halo_network_settings_expand_legacy(settings);
	for (i = 0; i < 13116; i++) assert(settings[i] == 0xA5);
	assert(!memcmp(settings + 13116, defaults, 16));
	assert(settings[13132] == 0x11 && settings[13133] == 0x22);
	assert(settings[13134] == 0x33 && settings[13135] == 0x44);
	puts("PASS: version 38 options, content refusal, legacy settings conversion");
	return 0;
}
