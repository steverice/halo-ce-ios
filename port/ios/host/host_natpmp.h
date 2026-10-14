/* NAT-PMP (RFC 6886) and PCP (RFC 6887) for internet play on Apple devices:
the router at the default route's gateway, asked over unicast UDP on its
port 5351, maps a UDP port of its internet address to the tunnel's socket
here, as UPnP does on the desktop (posix_upnp.c), which an Apple device
cannot use: a UPnP router is found by multicast, which needs an entitlement
Apple grants on request. NAT-PMP is asked first; a router that does not
answer it, or answers that it speaks only PCP, is asked with PCP's MAP.
posix_net.c's posix_upnp_forward_udp and posix_upnp_stop_forwarding_udp
call these; p2p.c renews the mapping and removes it.

The packets are made and read by the functions below, which
port/ios/tests/natpmp_probe.c checks without a network. Ports are in host
byte order here; addresses are IPv4 in network byte order. */

#ifndef HOST_NATPMP_H
#define HOST_NATPMP_H

#include <stdint.h>

enum
{
	NATPMP_PORT = 5351,
	/* a mapping's lifetime asked for, in seconds (p2p.c renews it after
	half an hour) */
	NATPMP_LIFETIME = 3600,
	/* NAT-PMP's result codes (RFC 6886 3.5) */
	NATPMP_SUCCESS = 0,
	NATPMP_UNSUPPORTED_VERSION = 1,
	NATPMP_NOT_AUTHORIZED = 2,
	NATPMP_NETWORK_FAILURE = 3,
	NATPMP_OUT_OF_RESOURCES = 4,
	NATPMP_UNSUPPORTED_OPCODE = 5,
	/* PCP's (RFC 6887 7.4), of those that matter here */
	PCP_SUCCESS = 0,
	PCP_UNSUPP_VERSION = 1,
	PCP_NOT_AUTHORIZED = 2,
	PCP_MALFORMED_REQUEST = 3,
	PCP_UNSUPP_OPCODE = 4,
	PCP_UNSUPP_OPTION = 5,
	PCP_MALFORMED_OPTION = 6,
	PCP_NETWORK_FAILURE = 7,
	PCP_NO_RESOURCES = 8,
	PCP_UNSUPP_PROTOCOL = 9,
	PCP_USER_EX_QUOTA = 10,
	PCP_CANNOT_PROVIDE_EXTERNAL = 11,
	PCP_ADDRESS_MISMATCH = 12,
	PCP_EXCESSIVE_REMOTE_PEERS = 13,
	/* the packets' sizes */
	NATPMP_ADDRESS_REQUEST_SIZE = 2,
	NATPMP_ADDRESS_RESPONSE_SIZE = 12,
	NATPMP_MAP_REQUEST_SIZE = 12,
	NATPMP_MAP_RESPONSE_SIZE = 16,
	PCP_MAP_SIZE = 60,
	PCP_NONCE_SIZE = 12,
};

/* NAT-PMP: the request for the router's internet address, and a UDP
mapping's (a lifetime of 0 removes it); returns the size written */
int natpmp_address_request(unsigned char *packet);
int natpmp_map_request(unsigned char *packet, uint16_t internal_port, uint16_t external_port, uint32_t lifetime);
/* NAT-PMP's answers read: 1 and the result code (and, on success, what it
gives) if the packet is the answer to that request, else 0 */
int natpmp_address_response(const unsigned char *packet, int size, int *result, uint32_t *external_address);
int natpmp_map_response(const unsigned char *packet, int size, uint16_t internal_port, int *result,
	uint16_t *external_port, uint32_t *lifetime);

/* PCP: a MAP request for a UDP port of this machine's client address
(the address this machine reaches the router from), with a nonce of
PCP_NONCE_SIZE bytes that its answer must carry, the external port and
address suggested (0: any), and the lifetime (0 removes it) */
int pcp_map_request(unsigned char *packet, uint32_t client_address, const unsigned char *nonce,
	uint16_t internal_port, uint16_t external_port, uint32_t external_address, uint32_t lifetime);
/* PCP's answer to that MAP read: 1 and its result code (on success, the
external port, address and lifetime given) if the packet answers it, else
0; *version is the router's version when it answers with
PCP_UNSUPP_VERSION (a NAT-PMP router answering PCP, or the reverse) */
int pcp_map_response(const unsigned char *packet, int size, const unsigned char *nonce, uint16_t internal_port,
	int *result, uint16_t *external_port, uint32_t *external_address, uint32_t *lifetime);

/* a result code in words, for the log */
const char *natpmp_result_text(int result);
const char *pcp_result_text(int result);

/* the network side (host_natpmp.c, not in the probe) */

/* the router maps a UDP port of its internet address (external_port
first, then any) to port of this machine for NATPMP_LIFETIME seconds;
blocks up to a few seconds. 1 on success, with the router's internet
address, the port and the lifetime it gave; else 0 and why in error. Either
way *method is how it was asked last: 1 NAT-PMP, 2 PCP, 3 both, unanswered */
int natpmp_map_udp(uint16_t port, uint16_t external_port, uint32_t *external_address, uint16_t *mapped_port,
	uint32_t *lifetime, int *method, char *error, int error_size);
/* the mapping natpmp_map_udp made for port removed (lifetime 0) */
void natpmp_unmap_udp(uint16_t port, uint16_t external_port);

#endif
