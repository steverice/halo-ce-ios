/* Run the real advertisement receiver with a documented kind-byte fixture.
   Platform lifetime, name-copy, and transport identity boundaries are local. */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "halo_network_version.h"
typedef unsigned char byte;
typedef unsigned short word;
typedef int boolean;
#define wchar_t uint16_t
#define TRUE 1
#define FALSE 0
#define MAXIMUM_NETWORK_ADVERTISED_GAMES 2
#define MAXIMUM_NETWORK_MACHINE_COUNT 128
#define TRANSPORT_NONCE_LENGTH 8
#define NETWORK_GAME_NAME_LENGTH 16
#define _game_advertisement_open_bit 1
#define _game_advertisement_has_teams_bit 2
#define _game_advertisement_oddball_variant_bit 3
#define game_engine_oddball 3
#define _network_game_platform_xbox 0
#define _network_game_platform_pc 1
#define _error_silent 0
#define TEST_FLAG(flags, bit) ((flags) & (1u << (bit)))
#define csmemset memset
#define csmemcpy memcpy
#define NUMBEROF(a) (sizeof(a) / sizeof((a)[0]))
#define match_assert(file, line, condition) assert(condition)
struct network_game_map { int32_t version; char name[128]; };
/* DECLARATIONS */
static int network_game_client_advertised_game_is_valid(struct network_advertised_game *game) { return game->valid; }
static int transport_nonce_is_equal(byte const *a, byte const *b) { return !memcmp(a, b, 8); }
static unsigned long system_milliseconds(void) { return 1000; }
static void ustrncpy(void *a, void const *b, size_t count) { memcpy(a, b, count * 2); }
static void network_event(char const *format, ...) { (void)format; }
static void error(int level, char const *message) { (void)level; (void)message; }
/* PRODUCTION */
int main(void)
{
	struct network_advertised_game games[2] = { 0 };
	struct message_server_game_advertise advertisement = { 0 };
	unsigned const kinds[] = { 0, 1, 2, 3, 128, 131, 4, 255 };
	unsigned i;

	/* 280-byte wire datagram has a three-byte prefix; kind is at96. */
	assert((byte *)&advertisement.__unknown5A[3] - (byte *)&advertisement == 93);
	advertisement.nonce[0] = 1;
	advertisement.game_name[0] = 'T';
	advertisement.flags = 2;
	advertisement.engine_type = 2;
	advertisement.maximum_player_count = 128;
	advertisement.__unknown5A[0] = 38;
	advertisement.__unknown5A[2] = 1;
	for (i = 0; i < NUMBEROF(kinds); i++)
	{
		advertisement.__unknown5A[3] = (byte)kinds[i];
		assert(add_advertised_game(games, &advertisement));
		assert(network_game_client_advertised_versions[0].version == 38);
		assert(network_game_client_advertised_versions[0].flags == 1);
		assert(network_game_client_advertised_versions[0].kind == kinds[i]);
		assert(games[0].engine_type == 2);
	}
	/* Before37 the same reserved byte is not a kind. Refreshing the same
	   slot must clear a previously unsupported kind. */
	advertisement.__unknown5A[0] = 32;
	advertisement.__unknown5A[3] = 131;
	assert(add_advertised_game(games, &advertisement));
	assert(network_game_client_advertised_versions[0].kind == 0);
	puts("PASS: LAN kind retention, independent engine, legacy fallback");
	return 0;
}
