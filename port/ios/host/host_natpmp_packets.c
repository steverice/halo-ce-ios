/* NAT-PMP's and PCP's packets made and read (host_natpmp.h). No network
here: port/ios/tests/natpmp_probe.c checks these as they are. */

#include "host_natpmp.h"

#include <string.h>

static void put16(unsigned char *bytes, uint16_t value)
{
	bytes[0] = (unsigned char)(value >> 8);
	bytes[1] = (unsigned char)value;
}

static void put32(unsigned char *bytes, uint32_t value)
{
	bytes[0] = (unsigned char)(value >> 24);
	bytes[1] = (unsigned char)(value >> 16);
	bytes[2] = (unsigned char)(value >> 8);
	bytes[3] = (unsigned char)value;
}

static uint16_t get16(const unsigned char *bytes)
{
	return (uint16_t)(bytes[0] << 8 | bytes[1]);
}

static uint32_t get32(const unsigned char *bytes)
{
	return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}

/* an IPv4 address (network byte order) as PCP's IPv4-mapped IPv6 one */
static void put_mapped(unsigned char *bytes, uint32_t address)
{
	memset(bytes, 0, 10);
	bytes[10] = 0xFF;
	bytes[11] = 0xFF;
	memcpy(bytes + 12, &address, 4);
}

int natpmp_address_request(unsigned char *packet)
{
	packet[0] = 0;
	packet[1] = 0;
	return NATPMP_ADDRESS_REQUEST_SIZE;
}

int natpmp_map_request(unsigned char *packet, uint16_t internal_port, uint16_t external_port, uint32_t lifetime)
{
	packet[0] = 0;
	/* (1: UDP) */
	packet[1] = 1;
	packet[2] = 0;
	packet[3] = 0;
	put16(packet + 4, internal_port);
	put16(packet + 6, external_port);
	put32(packet + 8, lifetime);
	return NATPMP_MAP_REQUEST_SIZE;
}

int natpmp_address_response(const unsigned char *packet, int size, int *result, uint32_t *external_address)
{
	/* (an error answer may be only the header: version, opcode, result,
	epoch) */
	if (size < 4 || packet[0] != 0 || packet[1] != 128)
		return 0;
	*result = get16(packet + 2);
	if (*result == NATPMP_SUCCESS)
	{
		if (size < NATPMP_ADDRESS_RESPONSE_SIZE)
			return 0;
		memcpy(external_address, packet + 8, 4);
	}
	return 1;
}

int natpmp_map_response(const unsigned char *packet, int size, uint16_t internal_port, int *result,
	uint16_t *external_port, uint32_t *lifetime)
{
	if (size < 4 || packet[0] != 0 || packet[1] != 128 + 1)
		return 0;
	*result = get16(packet + 2);
	if (*result != NATPMP_SUCCESS)
		return 1;
	if (size < NATPMP_MAP_RESPONSE_SIZE || get16(packet + 8) != internal_port)
		return 0;
	*external_port = get16(packet + 10);
	*lifetime = get32(packet + 12);
	return 1;
}

int pcp_map_request(unsigned char *packet, uint32_t client_address, const unsigned char *nonce,
	uint16_t internal_port, uint16_t external_port, uint32_t external_address, uint32_t lifetime)
{
	memset(packet, 0, PCP_MAP_SIZE);
	packet[0] = 2;
	/* (a request, MAP) */
	packet[1] = 1;
	put32(packet + 4, lifetime);
	put_mapped(packet + 8, client_address);
	memcpy(packet + 24, nonce, PCP_NONCE_SIZE);
	/* (17: UDP) */
	packet[36] = 17;
	put16(packet + 40, internal_port);
	put16(packet + 42, external_port);
	put_mapped(packet + 44, external_address);
	return PCP_MAP_SIZE;
}

int pcp_map_response(const unsigned char *packet, int size, const unsigned char *nonce, uint16_t internal_port,
	int *result, uint16_t *external_port, uint32_t *external_address, uint32_t *lifetime)
{
	static const unsigned char mapped_prefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };

	/* a NAT-PMP router's answer to a PCP request: NAT-PMP's header, its
	result "unsupported version" (RFC 6887 9) */
	if (size >= 4 && packet[0] == 0 && packet[1] == 128 + 1)
	{
		*result = PCP_UNSUPP_VERSION;
		return 1;
	}
	if (size < 24 || packet[0] != 2 || packet[1] != (0x80 | 1))
		return 0;
	*result = packet[3];
	/* (an error answer may be the header alone; one that carries the MAP
	must be this request's) */
	if (*result != PCP_SUCCESS)
		return size < PCP_MAP_SIZE || !memcmp(packet + 24, nonce, PCP_NONCE_SIZE);
	if (size < PCP_MAP_SIZE || memcmp(packet + 24, nonce, PCP_NONCE_SIZE) || packet[36] != 17 ||
		get16(packet + 40) != internal_port || memcmp(packet + 44, mapped_prefix, sizeof(mapped_prefix)))
	{
		return 0;
	}
	*lifetime = get32(packet + 4);
	*external_port = get16(packet + 42);
	memcpy(external_address, packet + 56, 4);
	return 1;
}

const char *natpmp_result_text(int result)
{
	switch (result)
	{
	case NATPMP_SUCCESS: return "success";
	case NATPMP_UNSUPPORTED_VERSION: return "unsupported version";
	case NATPMP_NOT_AUTHORIZED: return "not authorized or refused";
	case NATPMP_NETWORK_FAILURE: return "network failure";
	case NATPMP_OUT_OF_RESOURCES: return "out of resources";
	case NATPMP_UNSUPPORTED_OPCODE: return "unsupported opcode";
	default: return "unknown result";
	}
}

const char *pcp_result_text(int result)
{
	static const char *const texts[] = {
		"success", "unsupported version", "not authorized", "malformed request", "unsupported opcode",
		"unsupported option", "malformed option", "network failure", "no resources", "unsupported protocol",
		"user over quota", "cannot provide that external port", "address mismatch", "excessive remote peers",
	};

	return result >= 0 && result < (int)(sizeof(texts) / sizeof(*texts)) ? texts[result] : "unknown result";
}
