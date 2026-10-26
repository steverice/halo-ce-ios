/* The network versions this build plays with (halo_network_version.h): which hosts a
client joins, what each version turns on, and the datagram stamp window. */
#include "../../linux/include/halo_network_version.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
	unsigned version;

	assert(HALO_PORT_NETWORK_VERSION == 38 && HALO_PORT_NETWORK_OLDEST_JOINABLE == 29);
	/* a build of 32 joins hosts of 29, 30, 31 and 32, and no others */
	for (version = 0; version < 40; version++)
	{
		int expected = version >= 29 && version <= 32;

		assert(halo_network_version_joinable(32, version) == expected);
	}
	/* a build of 38 joins hosts of 29 to 32, 34 to 36, 38 and 39, and no others
	   (no host of 33 or 37 was released to join) */
	for (version = 0; version < 0x10000; version++)
		assert(halo_network_version_joinable(38, version) ==
			((version >= 29 && version <= 32) || (version >= 34 && version <= 36) || version == 38 || version == 39));
	/* a build of 29 (a VISR before the range) joins only its own */
	assert(halo_network_version_joinable(29, 29) && !halo_network_version_joinable(29, 30));
	/* message 90 comes from hosts of 32 */
	assert(!halo_network_version_has_aim_policy(29) && !halo_network_version_has_aim_policy(31));
	assert(halo_network_version_has_aim_policy(32) && halo_network_version_has_aim_policy(33));
	/* what each host version sends and takes */
	for (version = 29; version <= 39; version++)
	{
		unsigned long selector;

		assert(halo_network_version_has_ally_switch(version) == (version >= 33));
		assert(halo_network_version_has_static_shields(version) == (version >= 34));
		/* 33 and 34 keep the 13,120-byte record with the 28-byte options block */
		assert(halo_network_version_has_mode_options(version) == (version >= 35));
		/* 35 sends no message 91; 36 and 37 send it without selector 6 */
		assert(halo_network_version_has_shield_policy(version) == (version >= 36));
		for (selector = 0; selector <= 8; selector++)
			assert(halo_network_version_shield_selector_valid(version, selector) ==
				(version >= 36 && (selector <= 5 || (selector == 6 && version >= 38))));
		assert(!halo_network_version_shield_selector_valid(version, 0xFFFFFFFFul));
		assert(halo_network_version_has_game_kind(version) == (version >= 37));
		assert(halo_network_version_has_dedicated(version) == (version >= 39));
	}
	/* a stamp within 450 ticks of the latest either way is taken */
	assert(halo_network_stamp_in_window(1000, 1000) && halo_network_stamp_in_window(1000, 1450));
	assert(halo_network_stamp_in_window(1000, 550));
	assert(!halo_network_stamp_in_window(1000, 1451) && !halo_network_stamp_in_window(1000, 549));
	assert(!halo_network_stamp_in_window(0, 100000));
	/* a datagram out of the window is refused until the latest is forgotten (the clock was set: it jumped
	   600 ticks, as after a long stall), or its sender's stream moves the latest; a stream stamp is always taken */
	assert(!halo_network_stamp_accepts(1000, 1600, 0));
	assert(halo_network_stamp_accepts(-1, 1600, 0));
	assert(halo_network_stamp_accepts(1000, 1600, 1));
	assert(halo_network_stamp_accepts(1000, 1400, 0));
	puts("PASS: version range, per-version capabilities, stamp window");
	return 0;
}
