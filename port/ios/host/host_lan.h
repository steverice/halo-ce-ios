/* The machines Bonjour found (host_bonjour.m), the TXT record it advertises
and matches, and the holds that keep discovery up: pure, so the probe runs
it (port/ios/tests/host_lan_probe.c). The caller locks. */

#pragma once
#include <stdint.h>

#define HOST_LAN_MAXIMUM_PEERS 64
#define HOST_LAN_MAXIMUM_ADDRESSES 4
#define HOST_LAN_NAME_SIZE 64
#define HOST_LAN_SEARCH_HOLD_SECONDS 10.0
struct host_lan_peer
{
	int used;
	char name[HOST_LAN_NAME_SIZE];
	uint32_t interface_index;
	uint32_t addresses[HOST_LAN_MAXIMUM_ADDRESSES];
	int address_count;
	unsigned long long added;
	void *resolve, *address_lookup;
};
struct host_lan_table { struct host_lan_peer peers[HOST_LAN_MAXIMUM_PEERS]; unsigned long long serial; char self[HOST_LAN_NAME_SIZE]; };
void host_lan_table_reset(struct host_lan_table *table, const char *self);
int host_lan_peer_add(struct host_lan_table *table, const char *name, uint32_t interface_index, struct host_lan_peer *evicted);
int host_lan_peer_find(const struct host_lan_table *table, const char *name, uint32_t interface_index);
int host_lan_peer_remove(struct host_lan_table *table, const char *name, uint32_t interface_index, struct host_lan_peer *removed);
void host_lan_peer_address(struct host_lan_table *table, int slot, uint32_t address);
int host_lan_addresses(const struct host_lan_table *table, uint32_t *addresses, int maximum_count);
int host_lan_txt_build(unsigned char *txt, int size, unsigned version, uint32_t fork);
int host_lan_txt_matches(const unsigned char *txt, int length, unsigned version, uint32_t fork);
struct host_lan_hold { int hosting; double search_until; };
void host_lan_hold_request(struct host_lan_hold *hold, int hosting, int searching, double now);
int host_lan_hold_wanted(const struct host_lan_hold *hold, double now);
