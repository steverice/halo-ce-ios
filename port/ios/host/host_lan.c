/* host_lan.h's table, TXT record and holds. */
#include "host_lan.h"
#include "../../linux/include/halo_network_version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void host_lan_table_reset(struct host_lan_table *table, const char *self)
{
	memset(table, 0, sizeof(*table));
	snprintf(table->self, sizeof(table->self), "%s", self);
}

int host_lan_peer_find(const struct host_lan_table *table, const char *name, uint32_t interface_index)
{
	int slot;

	for (slot = 0; slot < HOST_LAN_MAXIMUM_PEERS; slot++)
		if (table->peers[slot].used && table->peers[slot].interface_index == interface_index &&
			!strcmp(table->peers[slot].name, name))
			return slot;
	return -1;
}

int host_lan_peer_add(struct host_lan_table *table, const char *name, uint32_t interface_index,
	struct host_lan_peer *evicted)
{
	int slot, oldest = 0;

	memset(evicted, 0, sizeof(*evicted));
	if (!strcmp(name, table->self))
		return -1;
	slot = host_lan_peer_find(table, name, interface_index);
	if (slot >= 0)
		return slot;
	for (slot = 0; slot < HOST_LAN_MAXIMUM_PEERS && table->peers[slot].used; slot++)
		if (table->peers[slot].added < table->peers[oldest].added)
			oldest = slot;
	if (slot == HOST_LAN_MAXIMUM_PEERS)
	{
		slot = oldest;
		*evicted = table->peers[slot];
	}
	memset(&table->peers[slot], 0, sizeof(table->peers[slot]));
	table->peers[slot].used = 1;
	snprintf(table->peers[slot].name, sizeof(table->peers[slot].name), "%s", name);
	table->peers[slot].interface_index = interface_index;
	table->peers[slot].added = ++table->serial;
	return slot;
}

int host_lan_peer_remove(struct host_lan_table *table, const char *name, uint32_t interface_index,
	struct host_lan_peer *removed)
{
	int slot = host_lan_peer_find(table, name, interface_index);

	memset(removed, 0, sizeof(*removed));
	if (slot < 0)
		return 0;
	*removed = table->peers[slot];
	memset(&table->peers[slot], 0, sizeof(table->peers[slot]));
	return 1;
}

void host_lan_peer_address(struct host_lan_table *table, int slot, uint32_t address)
{
	struct host_lan_peer *peer;
	int index;

	if (slot < 0 || slot >= HOST_LAN_MAXIMUM_PEERS || !table->peers[slot].used)
		return;
	peer = &table->peers[slot];
	for (index = 0; index < peer->address_count; index++)
		if (peer->addresses[index] == address)
			return;
	if (peer->address_count < HOST_LAN_MAXIMUM_ADDRESSES)
		peer->addresses[peer->address_count++] = address;
}

int host_lan_addresses(const struct host_lan_table *table, uint32_t *addresses, int maximum_count)
{
	int count = 0, slot, index, seen;

	for (slot = 0; slot < HOST_LAN_MAXIMUM_PEERS; slot++)
	{
		const struct host_lan_peer *peer = &table->peers[slot];

		for (index = 0; peer->used && index < peer->address_count && count < maximum_count; index++)
		{
			for (seen = 0; seen < count && addresses[seen] != peer->addresses[index]; seen++)
				;
			if (seen == count)
				addresses[count++] = peer->addresses[index];
		}
	}
	return count;
}

int host_lan_txt_build(unsigned char *txt, int size, unsigned version, uint32_t fork)
{
	char entry[32];
	int length = 0, written;

	written = snprintf(entry, sizeof(entry), "v=%u", version);
	if (written + 1 > size)
		return 0;
	txt[length++] = (unsigned char)written;
	memcpy(txt + length, entry, (size_t)written);
	length += written;
	if (fork)
	{
		written = snprintf(entry, sizeof(entry), "f=%08x", (unsigned)fork);
		if (length + written + 1 > size)
			return length;
		txt[length++] = (unsigned char)written;
		memcpy(txt + length, entry, (size_t)written);
		length += written;
	}
	return length;
}

int host_lan_txt_matches(const unsigned char *txt, int length, unsigned version, uint32_t fork)
{
	unsigned long found_version = 0, found_fork = 0;
	int has_version = 0, position = 0;

	while (position < length)
	{
		int entry = txt[position++];
		char value[32];

		if (entry > length - position)
			return 0;
		if (entry > 2 && entry - 2 < (int)sizeof(value) && txt[position + 1] == '=')
		{
			memcpy(value, txt + position + 2, (size_t)(entry - 2));
			value[entry - 2] = 0;
			if (txt[position] == 'v')
			{
				found_version = strtoul(value, NULL, 10);
				has_version = 1;
			}
			else if (txt[position] == 'f')
				found_fork = strtoul(value, NULL, 16);
		}
		position += entry;
	}
	return has_version && found_version <= 0xFFFF && halo_network_version_joinable(version, (unsigned)found_version) && found_fork == fork;
}

void host_lan_hold_request(struct host_lan_hold *hold, int hosting, int searching, double now)
{
	hold->hosting = hosting != 0;
	if (searching)
		hold->search_until = now + HOST_LAN_SEARCH_HOLD_SECONDS;
}

int host_lan_hold_wanted(const struct host_lan_hold *hold, double now)
{
	return hold->hosting || now < hold->search_until;
}
