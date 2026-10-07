/* LAN discovery on iOS, iPadOS, visionOS and the Mac runner (spec section 2). */
#import <Foundation/Foundation.h>
#include "ios_host.h"
#include "host_config.h"
#include "posix.h"
#include <TargetConditionals.h>
#include <string.h>
#include "host_lan.h"
#include "halo_lan_targets.h"
#include <arpa/inet.h>
#include <dns_sd.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

/* port/ios/CMakeLists.txt: the chosen entitlements file has
com.apple.developer.networking.multicast */
#ifndef HALO_MULTICAST_ENTITLED
#define HALO_MULTICAST_ENTITLED 0
#endif

/* network.lan_broadcast, read once: whether real broadcasts are sent ("auto":
on a Mac, both the iPad app and the Mac Catalyst runner, and on an iOS or
visionOS device only with the multicast entitlement) */
static int broadcast_allowed(void)
{
	static int allowed = -1;

	if (allowed < 0)
	{
		char setting[16];

		host_config_string("network.lan_broadcast", "auto", setting, sizeof(setting));
		if (!strcmp(setting, "on"))
			allowed = 1;
		else if (!strcmp(setting, "off"))
			allowed = 0;
		else
			allowed = (NSProcessInfo.processInfo.isiOSAppOnMac || NSProcessInfo.processInfo.isMacCatalystApp) ? 1 :
				HALO_MULTICAST_ENTITLED;
		host_logf(HOST_LOG_INFO, "LAN: broadcasts %s (network.lan_broadcast = \"%s\")",
			allowed ? "on" : "off", setting);
	}
	return allowed;
}

/* the table is written on the Bonjour queue and read by posix_lan_peers on
the game's thread; the holds are written by posix_lan_discovery on the game's
thread and read on the queue: both only under the lock (host_lan.h: the
caller locks) */
static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
static struct host_lan_table table;
static _Atomic int refused;

#if !TARGET_OS_TV
#define SERVICE_TYPE "_halo-ce._udp"
/* the server port, a label only: registering binds nothing */
#define SERVER_PORT 5150

/* every DNS-SD reference lives on this serial queue (never the main queue,
which the guest pumps once a frame), and is deallocated there, never under
the lock */
static dispatch_queue_t queue;
/* kept: under ARC a timer nothing holds is released and stops firing */
static dispatch_source_t timer;
static struct host_lan_hold hold;
static unsigned short wanted_version;
static uint32_t wanted_fork;
static DNSServiceRef registration, browse;
static int running, retry_seconds;
static char self_name[HOST_LAN_NAME_SIZE];

static double monotonic_seconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* a peer the table handed back (removed or evicted, a copy): its lookups
end here, on the queue, after the lock is dropped */
static void release_peer(struct host_lan_peer *peer)
{
	if (peer->resolve)
		DNSServiceRefDeallocate((DNSServiceRef)peer->resolve);
	if (peer->address_lookup)
		DNSServiceRefDeallocate((DNSServiceRef)peer->address_lookup);
	peer->resolve = peer->address_lookup = NULL;
}

static void stop(void)
{
	void *lookups[HOST_LAN_MAXIMUM_PEERS * 2];
	int slot, count = 0, index;

	if (registration)
		DNSServiceRefDeallocate(registration);
	if (browse)
		DNSServiceRefDeallocate(browse);
	registration = browse = NULL;
	os_unfair_lock_lock(&lock);
	for (slot = 0; slot < HOST_LAN_MAXIMUM_PEERS; slot++)
	{
		if (!table.peers[slot].used)
			continue;
		if (table.peers[slot].resolve)
			lookups[count++] = table.peers[slot].resolve;
		if (table.peers[slot].address_lookup)
			lookups[count++] = table.peers[slot].address_lookup;
	}
	host_lan_table_reset(&table, self_name);
	os_unfair_lock_unlock(&lock);
	for (index = 0; index < count; index++)
		DNSServiceRefDeallocate((DNSServiceRef)lookups[index]);
	if (running)
		host_logf(HOST_LOG_INFO, "bonjour: stopped");
	running = 0;
}

/* local network access refused, or not yet answered: DNS-SD reports the
same error while the alert is still up (TN3179), so the menus say access is
off only after a second denial in a row, a retry cycle later; try again in 2
seconds */
static int denials;

static void refuse(void)
{
	if (++denials >= 2 && !atomic_exchange(&refused, 1))
		host_logf(HOST_LOG_INFO, "bonjour: local network access is refused");
	stop();
	retry_seconds = 2;
}

static void allowed(void)
{
	denials = 0;
	atomic_store(&refused, 0);
}

static void DNSSD_API address_reply(DNSServiceRef ref, DNSServiceFlags flags, uint32_t interface_index,
	DNSServiceErrorType error, const char *hostname, const struct sockaddr *address, uint32_t ttl, void *context)
{
	int slot = (int)(intptr_t)context;
	char name[HOST_LAN_NAME_SIZE], text[INET_ADDRSTRLEN];
	uint32_t ip;

	(void)ref;
	(void)interface_index;
	(void)hostname;
	(void)ttl;
	if (error != kDNSServiceErr_NoError || !(flags & kDNSServiceFlagsAdd) || !address || address->sa_family != AF_INET)
		return;
	ip = ((const struct sockaddr_in *)address)->sin_addr.s_addr;
	/* (a peer on this same machine, as in the Mac runner's tests, also
	resolves to loopback, which xnet.c would take for this machine itself) */
	if ((ntohl(ip) >> 24) == 127)
		return;
	os_unfair_lock_lock(&lock);
	host_lan_peer_address(&table, slot, ip);
	snprintf(name, sizeof(name), "%s", table.peers[slot].name);
	os_unfair_lock_unlock(&lock);
	host_logf(HOST_LOG_INFO, "bonjour: %s at %s", name, inet_ntop(AF_INET, &ip, text, sizeof(text)));
}

/* (only the queue writes the table, so the queue reads it without the lock;
the game's thread reads it only under the lock) */
static void DNSSD_API resolve_reply(DNSServiceRef ref, DNSServiceFlags flags, uint32_t interface_index,
	DNSServiceErrorType error, const char *fullname, const char *host_target, uint16_t port, uint16_t txt_length,
	const unsigned char *txt, void *context)
{
	int slot = (int)(intptr_t)context;
	struct host_lan_peer *peer = &table.peers[slot];
	struct host_lan_peer gone;
	char name[HOST_LAN_NAME_SIZE];
	DNSServiceRef lookup = NULL;
	unsigned short version;
	uint32_t fork;

	(void)ref;
	(void)flags;
	(void)fullname;
	(void)port;
	if (error != kDNSServiceErr_NoError || !peer->used)
		return;
	os_unfair_lock_lock(&lock);
	version = wanted_version;
	fork = wanted_fork;
	os_unfair_lock_unlock(&lock);
	if (!host_lan_txt_matches(txt, txt_length, version, fork))
	{
		snprintf(name, sizeof(name), "%s", peer->name);
		host_logf(HOST_LOG_INFO, "bonjour: skipping %s (another version or fork)", name);
		os_unfair_lock_lock(&lock);
		host_lan_peer_remove(&table, name, peer->interface_index, &gone);
		os_unfair_lock_unlock(&lock);
		release_peer(&gone);
		return;
	}
	if (peer->address_lookup)
		return;
	if (DNSServiceGetAddrInfo(&lookup, 0, interface_index, kDNSServiceProtocol_IPv4, host_target, address_reply,
		context) != kDNSServiceErr_NoError)
		return;
	if (DNSServiceSetDispatchQueue(lookup, queue) != kDNSServiceErr_NoError)
	{
		DNSServiceRefDeallocate(lookup);
		return;
	}
	os_unfair_lock_lock(&lock);
	peer->address_lookup = lookup;
	os_unfair_lock_unlock(&lock);
}

static void DNSSD_API browse_reply(DNSServiceRef ref, DNSServiceFlags flags, uint32_t interface_index,
	DNSServiceErrorType error, const char *name, const char *type, const char *domain, void *context)
{
	struct host_lan_peer gone;
	DNSServiceRef resolve = NULL;
	int slot, found;

	(void)ref;
	(void)context;
	if (error == kDNSServiceErr_PolicyDenied)
	{
		refuse();
		return;
	}
	if (error != kDNSServiceErr_NoError)
	{
		host_logf(HOST_LOG_INFO, "bonjour: browsing failed (%d)", (int)error);
		return;
	}
	allowed();
	if (!(flags & kDNSServiceFlagsAdd))
	{
		os_unfair_lock_lock(&lock);
		found = host_lan_peer_remove(&table, name, interface_index, &gone);
		os_unfair_lock_unlock(&lock);
		if (found)
		{
			release_peer(&gone);
			host_logf(HOST_LOG_INFO, "bonjour: %s left", name);
		}
		return;
	}
	os_unfair_lock_lock(&lock);
	slot = host_lan_peer_add(&table, name, interface_index, &gone);
	os_unfair_lock_unlock(&lock);
	if (gone.used)
	{
		release_peer(&gone);
		host_logf(HOST_LOG_INFO, "bonjour: %s dropped (the table is full)", gone.name);
	}
	if (slot < 0 || table.peers[slot].resolve)
		return;
	host_logf(HOST_LOG_INFO, "bonjour: found %s", name);
	if (DNSServiceResolve(&resolve, 0, interface_index, name, type, domain, resolve_reply, (void *)(intptr_t)slot) !=
		kDNSServiceErr_NoError)
		return;
	if (DNSServiceSetDispatchQueue(resolve, queue) != kDNSServiceErr_NoError)
	{
		DNSServiceRefDeallocate(resolve);
		return;
	}
	os_unfair_lock_lock(&lock);
	table.peers[slot].resolve = resolve;
	os_unfair_lock_unlock(&lock);
}

static void DNSSD_API register_reply(DNSServiceRef ref, DNSServiceFlags flags, DNSServiceErrorType error,
	const char *name, const char *type, const char *domain, void *context)
{
	(void)ref;
	(void)flags;
	(void)type;
	(void)domain;
	(void)context;
	if (error == kDNSServiceErr_PolicyDenied)
	{
		refuse();
		return;
	}
	if (error == kDNSServiceErr_NoError)
	{
		allowed();
		host_logf(HOST_LOG_INFO, "bonjour: registered %s", name);
	}
	else
		host_logf(HOST_LOG_INFO, "bonjour: registering failed (%d)", (int)error);
}

static void start(void)
{
	unsigned char txt[64];
	unsigned short version;
	uint32_t fork;
	int txt_length;
	DNSServiceErrorType error;

	os_unfair_lock_lock(&lock);
	version = wanted_version;
	fork = wanted_fork;
	os_unfair_lock_unlock(&lock);
	txt_length = host_lan_txt_build(txt, sizeof(txt), version, fork);
	error = DNSServiceRegister(&registration, 0, kDNSServiceInterfaceIndexAny, self_name, SERVICE_TYPE, NULL, NULL,
		htons(SERVER_PORT), (uint16_t)txt_length, txt, register_reply, NULL);
	if (error == kDNSServiceErr_NoError)
		error = DNSServiceSetDispatchQueue(registration, queue);
	if (error == kDNSServiceErr_NoError)
		error = DNSServiceBrowse(&browse, 0, kDNSServiceInterfaceIndexAny, SERVICE_TYPE, NULL, browse_reply, NULL);
	if (error == kDNSServiceErr_NoError)
		error = DNSServiceSetDispatchQueue(browse, queue);
	if (error == kDNSServiceErr_PolicyDenied)
	{
		refuse();
		return;
	}
	if (error != kDNSServiceErr_NoError)
	{
		host_logf(HOST_LOG_INFO, "bonjour: cannot start (%d)", (int)error);
		stop();
		retry_seconds = 2;
		return;
	}
	running = 1;
	host_logf(HOST_LOG_INFO, "bonjour: discovering as %s", self_name);
}

/* once a second on the queue, and when the game's wish changes */
static void tick(void)
{
	int wanted;

	os_unfair_lock_lock(&lock);
	wanted = host_lan_hold_wanted(&hold, monotonic_seconds());
	os_unfair_lock_unlock(&lock);
	if (!wanted)
	{
		if (running)
			stop();
		retry_seconds = 0;
		return;
	}
	if (running || (retry_seconds > 0 && --retry_seconds > 0))
		return;
	start();
}

static void set_up(void)
{
	uint64_t identifier = ((uint64_t)arc4random() << 16 | (arc4random() & 0xFFFF)) & 0xFFFFFFFFFFFFull;

	queue = dispatch_queue_create("halo.bonjour", DISPATCH_QUEUE_SERIAL);
	snprintf(self_name, sizeof(self_name), "Halo %012llx", (unsigned long long)identifier);
	os_unfair_lock_lock(&lock);
	host_lan_table_reset(&table, self_name);
	os_unfair_lock_unlock(&lock);
	timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
	dispatch_source_set_event_handler(timer, ^{ tick(); });
	dispatch_resume(timer);
}
#endif

int posix_lan_status(void)
{
	return (broadcast_allowed() ? _posix_lan_broadcast : 0) | (atomic_load(&refused) ? _posix_lan_refused : 0);
}

/* the real broadcast's address (spec section 2): on a Mac, both the iPad app
and the Mac Catalyst runner, 255.255.255.255 as before; on an entitled iOS or
visionOS device, the subnet-directed broadcast of the system link interface */
posix_ulong posix_lan_broadcast_address(void)
{
	uint32_t address, netmask;

	if (NSProcessInfo.processInfo.isiOSAppOnMac || NSProcessInfo.processInfo.isMacCatalystApp ||
		!host_ranked_interface(&address, &netmask))
		return 0xFFFFFFFFu;
	return (posix_ulong)halo_lan_subnet_broadcast(address, netmask);
}

void posix_lan_discovery(int hosting, int searching, unsigned short version, posix_ulong fork)
{
#if TARGET_OS_TV
	/* tvOS: no Bonjour (spec: tvOS gets iOS's behavior with discovery off) */
	(void)hosting;
	(void)searching;
	(void)version;
	(void)fork;
#else
	static dispatch_once_t once;
	double now = monotonic_seconds();
	int wake;

	dispatch_once(&once, ^{ set_up(); });
	os_unfair_lock_lock(&lock);
	wake = !host_lan_hold_wanted(&hold, now) || hold.hosting != (hosting != 0);
	host_lan_hold_request(&hold, hosting, searching, now);
	wanted_version = version;
	wanted_fork = (uint32_t)fork;
	os_unfair_lock_unlock(&lock);
	if (wake)
		dispatch_async(queue, ^{ tick(); });
#endif
}

int posix_lan_peers(posix_ulong *addresses, int maximum_count)
{
	uint32_t found[HOST_LAN_MAXIMUM_PEERS * HOST_LAN_MAXIMUM_ADDRESSES];
	int count, index;

	if (maximum_count > (int)(sizeof(found) / sizeof(found[0])))
		maximum_count = (int)(sizeof(found) / sizeof(found[0]));
	os_unfair_lock_lock(&lock);
	count = host_lan_addresses(&table, found, maximum_count);
	os_unfair_lock_unlock(&lock);
	for (index = 0; index < count; index++)
		addresses[index] = found[index];
	return count;
}
