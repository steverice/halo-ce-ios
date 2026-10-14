/* NAT-PMP's and PCP's packets (port/ios/host/host_natpmp_packets.c): the
requests byte for byte as RFC 6886 and RFC 6887 lay them out, and answers
read, refused when they answer something else, and their result codes.
Run by tools/ios_test.py. */
#include "host_natpmp.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	unsigned char packet[PCP_MAP_SIZE];
	unsigned char nonce[PCP_NONCE_SIZE] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
	uint32_t address = 0, lifetime = 0;
	uint16_t port = 0;
	int result = -1;

	/* NAT-PMP's requests */
	assert(natpmp_address_request(packet) == 2 && packet[0] == 0 && packet[1] == 0);
	{
		static const unsigned char map[] = { 0, 1, 0, 0, 0x30, 0x39, 0x30, 0x3A, 0, 0, 0x0E, 0x10 };

		assert(natpmp_map_request(packet, 12345, 12346, 3600) == (int)sizeof(map));
		assert(!memcmp(packet, map, sizeof(map)));
	}
	/* NAT-PMP's answers: the router's address; a mapping; refusals */
	{
		static const unsigned char answer[] = { 0, 128, 0, 0, 0, 0, 0, 9, 203, 0, 113, 7 };
		static const unsigned char refused[] = { 0, 128, 0, 3, 0, 0, 0, 9 };

		assert(natpmp_address_response(answer, sizeof(answer), &result, &address) && result == 0);
		assert(address == inet_addr("203.0.113.7"));
		assert(natpmp_address_response(refused, sizeof(refused), &result, &address) && result == 3);
		assert(!natpmp_address_response(answer, 11, &result, &address));
		/* (another opcode's answer, and a PCP packet, are not this one's) */
		assert(!natpmp_address_response((const unsigned char *)"\0\201\0\0\0\0\0\0\0\0\0\0", 12, &result, &address));
		assert(!natpmp_address_response((const unsigned char *)"\2\200\0\0", 4, &result, &address));
	}
	{
		static const unsigned char answer[] = { 0, 129, 0, 0, 0, 0, 0, 9, 0x30, 0x39, 0xD4, 0x31, 0, 0, 0x0E, 0x10 };
		static const unsigned char refused[] = { 0, 129, 0, 2, 0, 0, 0, 9 };
		static const unsigned char version[] = { 0, 129, 0, 1, 0, 0, 0, 0 };

		assert(natpmp_map_response(answer, sizeof(answer), 12345, &result, &port, &lifetime) && result == 0);
		assert(port == 54321 && lifetime == 3600);
		/* (another internal port's) */
		assert(!natpmp_map_response(answer, sizeof(answer), 12346, &result, &port, &lifetime));
		assert(natpmp_map_response(refused, sizeof(refused), 12345, &result, &port, &lifetime) &&
			result == NATPMP_NOT_AUTHORIZED);
		assert(natpmp_map_response(version, sizeof(version), 12345, &result, &port, &lifetime) &&
			result == NATPMP_UNSUPPORTED_VERSION);
		assert(!strcmp(natpmp_result_text(NATPMP_NOT_AUTHORIZED), "not authorized or refused"));
		assert(!strcmp(natpmp_result_text(99), "unknown result"));
	}
	/* PCP's MAP request */
	{
		unsigned char expected[PCP_MAP_SIZE] = { 2, 1, 0, 0, 0, 0, 0x0E, 0x10 };

		expected[18] = expected[19] = 0xFF;
		expected[20] = 192; expected[21] = 168; expected[22] = 1; expected[23] = 20;
		memcpy(expected + 24, nonce, sizeof(nonce));
		expected[36] = 17;
		expected[40] = 0x30; expected[41] = 0x39; expected[42] = 0x30; expected[43] = 0x39;
		expected[54] = expected[55] = 0xFF;
		assert(pcp_map_request(packet, inet_addr("192.168.1.20"), nonce, 12345, 12345, 0, 3600) == PCP_MAP_SIZE);
		assert(!memcmp(packet, expected, PCP_MAP_SIZE));
	}
	/* PCP's answers: the mapping; a refusal (header alone, or with this
	MAP); another request's; a NAT-PMP router's "unsupported version" */
	{
		unsigned char answer[PCP_MAP_SIZE] = { 2, 0x81, 0, 0, 0, 0, 0x0E, 0x10, 0, 0, 0, 7 };
		static const unsigned char short_refusal[24] = { 2, 0x81, 0, 8 };
		static const unsigned char natpmp_router[] = { 0, 129, 0, 1, 0, 0, 0, 0 };

		memcpy(answer + 24, nonce, sizeof(nonce));
		answer[36] = 17;
		answer[40] = 0x30; answer[41] = 0x39; answer[42] = 0xD4; answer[43] = 0x31;
		answer[54] = answer[55] = 0xFF;
		answer[56] = 203; answer[57] = 0; answer[58] = 113; answer[59] = 7;
		assert(pcp_map_response(answer, sizeof(answer), nonce, 12345, &result, &port, &address, &lifetime));
		assert(result == 0 && port == 54321 && address == inet_addr("203.0.113.7") && lifetime == 3600);
		assert(!pcp_map_response(answer, sizeof(answer), nonce, 12346, &result, &port, &address, &lifetime));
		answer[24] ^= 1;
		assert(!pcp_map_response(answer, sizeof(answer), nonce, 12345, &result, &port, &address, &lifetime));
		answer[3] = PCP_CANNOT_PROVIDE_EXTERNAL;
		assert(!pcp_map_response(answer, sizeof(answer), nonce, 12345, &result, &port, &address, &lifetime));
		answer[24] ^= 1;
		assert(pcp_map_response(answer, sizeof(answer), nonce, 12345, &result, &port, &address, &lifetime) &&
			result == PCP_CANNOT_PROVIDE_EXTERNAL);
		assert(pcp_map_response(short_refusal, sizeof(short_refusal), nonce, 12345, &result, &port, &address,
			&lifetime) && result == PCP_NO_RESOURCES);
		assert(pcp_map_response(natpmp_router, sizeof(natpmp_router), nonce, 12345, &result, &port, &address,
			&lifetime) && result == PCP_UNSUPP_VERSION);
		assert(!pcp_map_response(answer, 23, nonce, 12345, &result, &port, &address, &lifetime));
		assert(!strcmp(pcp_result_text(PCP_NO_RESOURCES), "no resources"));
		assert(!strcmp(pcp_result_text(-1), "unknown result"));
	}
	puts("PASS: NAT-PMP and PCP: requests as laid out, answers read and refused, result codes");
	return 0;
}
