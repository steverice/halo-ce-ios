/* NAT-PMP and PCP asked of the router (host_natpmp.h): its address from
the routing table, a socket connected to its port 5351, and each request
sent again at 250, 500 and 1000 ms (RFC 6886 3.1's schedule, cut short:
p2p.c asks on a thread of its own and asks again later). */

#include "host_natpmp.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#if __has_include(<net/route.h>)
#include <net/route.h>
#else
/* (the iOS and tvOS SDKs leave out net/route.h; Darwin's layout, as
macOS's header has it) */
struct rt_metrics
{
	uint32_t rmx_locks, rmx_mtu, rmx_hopcount;
	int32_t rmx_expire;
	uint32_t rmx_recvpipe, rmx_sendpipe, rmx_ssthresh, rmx_rtt, rmx_rttvar, rmx_pksent, rmx_state, rmx_filler[3];
};
struct rt_msghdr
{
	unsigned short rtm_msglen;
	unsigned char rtm_version, rtm_type;
	unsigned short rtm_index;
	int rtm_flags, rtm_addrs;
	pid_t rtm_pid;
	int rtm_seq, rtm_errno, rtm_use;
	uint32_t rtm_inits;
	struct rt_metrics rtm_rmx;
};
#define RTF_GATEWAY 0x2
#define RTA_DST 0x1
#define RTA_GATEWAY 0x2
#define NET_RT_FLAGS 2
#endif

/* the default route's gateway (network byte order), or 0 */
static uint32_t default_gateway(void)
{
	int mib[6] = { CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_GATEWAY };
	size_t size = 0;
	char *table, *at;
	uint32_t gateway = 0;

	if (sysctl(mib, 6, NULL, &size, NULL, 0) < 0 || !size || !(table = malloc(size)))
		return 0;
	if (sysctl(mib, 6, table, &size, NULL, 0) < 0)
	{
		free(table);
		return 0;
	}
	for (at = table; at + sizeof(struct rt_msghdr) <= table + size && !gateway;)
	{
		struct rt_msghdr *message = (struct rt_msghdr *)at;
		struct sockaddr *addresses[2] = { NULL, NULL };
		char *address = at + sizeof(*message);
		int bit;

		if (!message->rtm_msglen)
			break;
		/* the addresses after the header, each rounded up to 4 bytes */
		for (bit = 0; bit < 8 && address < at + message->rtm_msglen; bit++)
		{
			struct sockaddr *one = (struct sockaddr *)address;
			int length;

			if (!(message->rtm_addrs & (1 << bit)))
				continue;
			if (bit < 2)
				addresses[bit] = one;
			length = one->sa_len ? one->sa_len : (int)sizeof(uint32_t);
			address += (length + 3) & ~3;
		}
		if (addresses[0] && addresses[1] && addresses[0]->sa_family == AF_INET &&
			addresses[1]->sa_family == AF_INET && (message->rtm_flags & RTF_GATEWAY) &&
			((struct sockaddr_in *)addresses[0])->sin_addr.s_addr == 0)
		{
			gateway = ((struct sockaddr_in *)addresses[1])->sin_addr.s_addr;
		}
		at += message->rtm_msglen;
	}
	free(table);
	return gateway;
}

/* a UDP socket connected to the router's port 5351, and the address this
machine reaches it from (network byte order) */
static int router_socket(uint32_t *client_address)
{
	struct sockaddr_in router = { 0 }, local = { 0 };
	socklen_t length = sizeof(local);
	uint32_t gateway = default_gateway();
	int fd;

	if (!gateway || (fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
		return -1;
	router.sin_len = sizeof(router);
	router.sin_family = AF_INET;
	router.sin_port = htons(NATPMP_PORT);
	router.sin_addr.s_addr = gateway;
	if (connect(fd, (struct sockaddr *)&router, sizeof(router)) < 0 ||
		getsockname(fd, (struct sockaddr *)&local, &length) < 0)
	{
		close(fd);
		return -1;
	}
	*client_address = local.sin_addr.s_addr;
	return fd;
}

/* the request sent, again at each wait (ms) that passes with no answer the
reader takes; 1 once it takes one */
typedef int (*answer_reader)(const unsigned char *packet, int size, void *context);

static int ask(int fd, const unsigned char *request, int request_size, int tries, answer_reader reader, void *context)
{
	int wait = 250;
	int try;

	for (try = 0; try < tries; try++, wait *= 2)
	{
		struct timeval start, now;

		if (send(fd, request, (size_t)request_size, 0) < 0)
			return 0;
		gettimeofday(&start, NULL);
		for (;;)
		{
			struct pollfd poll_fd = { fd, POLLIN, 0 };
			unsigned char answer[1100];
			long elapsed;
			ssize_t size;

			gettimeofday(&now, NULL);
			elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
			if (elapsed >= wait || poll(&poll_fd, 1, (int)(wait - elapsed)) <= 0)
				break;
			size = recv(fd, answer, sizeof(answer), 0);
			/* (a gateway that runs neither refuses the datagram: an ICMP port
			unreachable, which the connected socket reports) */
			if (size < 0)
				return 0;
			if (reader(answer, (int)size, context))
				return 1;
		}
	}
	return 0;
}

struct natpmp_answer
{
	uint16_t internal_port;
	int result;
	uint32_t address;
	uint16_t external_port;
	uint32_t lifetime;
	const unsigned char *nonce;
};

static int read_natpmp_address(const unsigned char *packet, int size, void *context)
{
	struct natpmp_answer *answer = context;

	return natpmp_address_response(packet, size, &answer->result, &answer->address);
}

static int read_natpmp_map(const unsigned char *packet, int size, void *context)
{
	struct natpmp_answer *answer = context;

	return natpmp_map_response(packet, size, answer->internal_port, &answer->result, &answer->external_port,
		&answer->lifetime);
}

static int read_pcp_map(const unsigned char *packet, int size, void *context)
{
	struct natpmp_answer *answer = context;

	return pcp_map_response(packet, size, answer->nonce, answer->internal_port, &answer->result,
		&answer->external_port, &answer->address, &answer->lifetime);
}

/* PCP's nonce for each internal port's mapping: kept for the run, as a
renewal and a removal must carry the one that made it (RFC 6887 11.2) */
static unsigned char pcp_nonce[PCP_NONCE_SIZE];
static int has_pcp_nonce;

static int pcp_map(int fd, uint32_t client_address, uint16_t port, uint16_t external_port, uint32_t lifetime,
	struct natpmp_answer *answer, int tries)
{
	unsigned char request[PCP_MAP_SIZE];

	if (!has_pcp_nonce)
	{
		arc4random_buf(pcp_nonce, sizeof(pcp_nonce));
		has_pcp_nonce = 1;
	}
	memset(answer, 0, sizeof(*answer));
	answer->internal_port = port;
	answer->nonce = pcp_nonce;
	pcp_map_request(request, client_address, pcp_nonce, port, external_port, 0, lifetime);
	return ask(fd, request, sizeof(request), tries, read_pcp_map, answer);
}

/* which the router answered last: 1 NAT-PMP, 2 PCP (a removal asks it) */
static int router_speaks;

int natpmp_map_udp(uint16_t port, uint16_t external_port, uint32_t *external_address, uint16_t *mapped_port,
	uint32_t *lifetime, int *method, char *error, int error_size)
{
	unsigned char request[NATPMP_MAP_REQUEST_SIZE];
	struct natpmp_answer answer = { 0 };
	uint32_t client_address;
	int fd = router_socket(&client_address);

	*method = 1;
	if (fd < 0)
	{
		snprintf(error, (size_t)error_size, "no router in the routing table");
		return 0;
	}
	/* NAT-PMP: the router's address, then the mapping */
	natpmp_address_request(request);
	if (ask(fd, request, NATPMP_ADDRESS_REQUEST_SIZE, 3, read_natpmp_address, &answer) &&
		answer.result == NATPMP_SUCCESS)
	{
		uint32_t address = answer.address;

		memset(&answer, 0, sizeof(answer));
		answer.internal_port = port;
		natpmp_map_request(request, port, external_port, NATPMP_LIFETIME);
		if (!ask(fd, request, NATPMP_MAP_REQUEST_SIZE, 3, read_natpmp_map, &answer))
		{
			close(fd);
			snprintf(error, (size_t)error_size, "the router did not answer the request for a mapping");
			return 0;
		}
		close(fd);
		if (answer.result != NATPMP_SUCCESS)
		{
			snprintf(error, (size_t)error_size, "the router refused the mapping (result %d: %s)",
				answer.result, natpmp_result_text(answer.result));
			return 0;
		}
		router_speaks = 1;
		*external_address = address;
		*mapped_port = answer.external_port;
		*lifetime = answer.lifetime;
		return 1;
	}
	/* a router that answered NAT-PMP otherwise than "unsupported version"
	(it speaks PCP) said no */
	if (answer.result != NATPMP_SUCCESS && answer.result != NATPMP_UNSUPPORTED_VERSION)
	{
		close(fd);
		snprintf(error, (size_t)error_size, "the router refused (result %d: %s)", answer.result,
			natpmp_result_text(answer.result));
		return 0;
	}
	/* PCP */
	*method = 2;
	if (!pcp_map(fd, client_address, port, external_port, NATPMP_LIFETIME, &answer, 2))
	{
		close(fd);
		*method = 3;
		snprintf(error, (size_t)error_size, "the router answers neither");
		return 0;
	}
	close(fd);
	if (answer.result != PCP_SUCCESS)
	{
		snprintf(error, (size_t)error_size, "the router refused the mapping (result %d: %s)", answer.result,
			pcp_result_text(answer.result));
		return 0;
	}
	router_speaks = 2;
	*external_address = answer.address;
	*mapped_port = answer.external_port;
	*lifetime = answer.lifetime;
	return 1;
}

void natpmp_unmap_udp(uint16_t port, uint16_t external_port)
{
	struct natpmp_answer answer;
	uint32_t client_address;
	int fd = router_socket(&client_address);

	(void)external_port;
	if (fd < 0)
		return;
	if (router_speaks == 1)
	{
		unsigned char request[NATPMP_MAP_REQUEST_SIZE];

		memset(&answer, 0, sizeof(answer));
		answer.internal_port = port;
		/* (a lifetime of 0 and an external port of 0 removes the internal
		port's mapping: RFC 6886 3.4) */
		natpmp_map_request(request, port, 0, 0);
		ask(fd, request, NATPMP_MAP_REQUEST_SIZE, 2, read_natpmp_map, &answer);
	}
	else if (router_speaks == 2)
	{
		pcp_map(fd, client_address, port, 0, 0, &answer, 2);
	}
	close(fd);
}
