#include "host_lan.h"
#include "../../linux/include/halo_port_limits.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	static struct host_lan_table table;
	struct host_lan_peer gone;
	struct host_lan_hold hold = { 0, 0.0 };
	uint32_t out[512];
	unsigned char txt[64];
	char name[HOST_LAN_NAME_SIZE];
	int slot, other, length, index;

	host_lan_table_reset(&table, "Halo 0000000000aa");
	/* this machine's own instance is never a peer */
	assert(host_lan_peer_add(&table, "Halo 0000000000aa", 4, &gone) == -1 && !gone.used);
	/* a peer, its addresses each once and at most four */
	slot = host_lan_peer_add(&table, "Halo 0000000000bb", 4, &gone);
	assert(slot >= 0 && !gone.used);
	assert(host_lan_peer_add(&table, "Halo 0000000000bb", 4, &gone) == slot);
	host_lan_peer_address(&table, slot, 0x0201A8C0u);
	host_lan_peer_address(&table, slot, 0x0201A8C0u);
	for (index = 0; index < 6; index++)
		host_lan_peer_address(&table, slot, 0x0A000000u + (uint32_t)index);
	assert(table.peers[slot].address_count == HOST_LAN_MAXIMUM_ADDRESSES);
	/* the same instance on a second interface (the hotspot bridge) is its own
	entry: removing one keeps the other's addresses */
	other = host_lan_peer_add(&table, "Halo 0000000000bb", 9, &gone);
	assert(other >= 0 && other != slot);
	host_lan_peer_address(&table, other, 0x0102A8C0u);
	assert(host_lan_peer_remove(&table, "Halo 0000000000bb", 4, &gone) == 1 && gone.address_count == 4);
	assert(host_lan_peer_find(&table, "Halo 0000000000bb", 4) < 0);
	assert(host_lan_addresses(&table, out, 512) == 1 && out[0] == 0x0102A8C0u);
	assert(host_lan_peer_remove(&table, "Halo 0000000000bb", 4, &gone) == 0);
	/* addresses are reported once across peers, and never more than asked */
	slot = host_lan_peer_add(&table, "Halo 0000000000cc", 4, &gone);
	host_lan_peer_address(&table, slot, 0x0102A8C0u);
	host_lan_peer_address(&table, slot, 0x0302A8C0u);
	assert(host_lan_addresses(&table, out, 512) == 2);
	assert(host_lan_addresses(&table, out, 1) == 1);
	/* a full table: the 65th peer evicts the oldest, which is handed back */
	host_lan_table_reset(&table, "Halo 0000000000aa");
	for (index = 0; index < HOST_LAN_MAXIMUM_PEERS; index++)
	{
		snprintf(name, sizeof(name), "Halo %012d", index);
		assert(host_lan_peer_add(&table, name, 4, &gone) >= 0 && !gone.used);
	}
	assert(host_lan_peer_add(&table, "Halo 0000000000ff", 4, &gone) >= 0);
	assert(gone.used && !strcmp(gone.name, "Halo 000000000000"));
	assert(host_lan_peer_find(&table, "Halo 000000000000", 4) < 0);
	assert(host_lan_peer_find(&table, "Halo 0000000000ff", 4) >= 0);

	/* the TXT record: v=<network version>, and f= only for a fork */
	length = host_lan_txt_build(txt, sizeof(txt), HALO_PORT_NETWORK_VERSION, 0);
	{
		char expected[16];
		int expected_length = snprintf(expected, sizeof(expected), "v=%u", (unsigned)HALO_PORT_NETWORK_VERSION);

		assert(length == expected_length + 1 && txt[0] == expected_length && !memcmp(txt + 1, expected, expected_length));
	}
	assert(host_lan_txt_matches(txt, length, HALO_PORT_NETWORK_VERSION, 0));
	assert(!host_lan_txt_matches(txt, length, HALO_PORT_NETWORK_VERSION + 1, 0));
	assert(!host_lan_txt_matches(txt, length, HALO_PORT_NETWORK_VERSION, 0x48564953u));
	length = host_lan_txt_build(txt, sizeof(txt), 0x8001, 0x48564953u);
	assert(host_lan_txt_matches(txt, length, 0x8001, 0x48564953u));
	assert(!host_lan_txt_matches(txt, length, 0x8001, 0));
	/* a record without v=, or cut short, matches nothing */
	assert(!host_lan_txt_matches((const unsigned char *)"\x03" "f=1", 4, HALO_PORT_NETWORK_VERSION, 0));
	assert(!host_lan_txt_matches((const unsigned char *)"\x09" "v=1", 4, HALO_PORT_NETWORK_VERSION, 0));

	/* holds: hosting holds without a time limit; a search holds 10 s */
	assert(!host_lan_hold_wanted(&hold, 0.0));
	host_lan_hold_request(&hold, 0, 1, 100.0);
	assert(host_lan_hold_wanted(&hold, 109.9) && !host_lan_hold_wanted(&hold, 110.0));
	host_lan_hold_request(&hold, 1, 0, 200.0);
	assert(host_lan_hold_wanted(&hold, 100000.0));
	host_lan_hold_request(&hold, 0, 0, 300.0);
	assert(!host_lan_hold_wanted(&hold, 300.0));
	puts("PASS: peers (self, addresses, interfaces, removal, eviction), TXT records, holds");
	return 0;
}
