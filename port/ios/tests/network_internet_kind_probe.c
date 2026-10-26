/* Asset-free tests of variable-length internet listings and retained rows. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "p2p.h"
#include "halo_network_content.h"
#include "halo_network_version.h"
#define P2P_KEY_SIZE 32
#define P2P_KEY_HASH_SIZE 16
#define P2P_TOKEN_SIZE 16
#define P2P_SEALED_TOKEN_SIZE 56
#define P2P_SIGNATURE_SIZE 64
#define LISTING_FORMAT 1
#define STAMP_SIZE 8
#define MINIMUM_LISTING_SIZE 140
#define MAXIMUM_LISTING_SIZE 268
#define MAXIMUM_GAMES 2
#define MAXIMUM_TOMBSTONES 2
#define _listing_open 1
#define _listing_in_progress 2
#define _listing_has_teams 4
#define _listing_closed 8
#define _listing_password 16
/* DECLARATIONS */
static struct { struct game games[MAXIMUM_GAMES]; struct tombstone tombstones[MAXIMUM_TOMBSTONES]; int next_tombstone; } lobby;
static struct tombstone *find_tombstone(const unsigned char *hash) { return NULL; }
static struct game *find_game(const unsigned char *hash) { return NULL; }
static unsigned long p2p_now(void) { return 1; }
static void p2p_hex(const unsigned char *bytes, int count, char *text) { memset(text, 'a', count * 2); text[count * 2] = 0; }
static void p2p_identifier_from_hash(const unsigned char *hash, unsigned char *identifier) { memset(identifier, 0, 6); }
static void sanitize(char *out, int maximum, const char *in, int length) { memcpy(out, in, length); out[length] = 0; }
/* PRODUCTION */
static int record(unsigned char *bytes, int locked, int length, unsigned kind)
{
	int offset = locked ? 105 : 65;
	memset(bytes, 0, MAXIMUM_LISTING_SIZE + 1);
	memcpy(bytes, "HL\x01\x00\x26", 5);
	bytes[5] = _listing_open | (locked ? _listing_password : 0);
	bytes[9] = 1;
	bytes[offset - 3] = 2; bytes[offset - 2] = 16; bytes[offset - 1] = 2;
	for (int i = 0; i < 3; i++)
	{
		bytes[offset++] = length;
		memset(bytes + offset, 'A' + i, length);
		offset += length;
	}
	bytes[offset] = kind;
	/* Nonzero remaining stamp bytes must not become kind. */
	memset(bytes + offset + 1, 0x83, 7);
	return offset + 72;
}
int main(void)
{
	unsigned char bytes[MAXIMUM_LISTING_SIZE + 1];
	struct listing parsed;
	struct queued queued = {0};
	unsigned kinds[] = {0, 1, 2, 3, 0x80, 0x83, 4, 0xff};
	for (int locked = 0; locked <= 1; locked++)
	for (int length = 0; length <= 7; length += 7)
	for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++)
	{
		int size = record(bytes, locked, length, kinds[k]);
		assert(listing_read(bytes, size, &parsed));
		memset(&lobby, 0, sizeof(lobby));
		memcpy(queued.payload, bytes, size); queued.size = size;
		listing_take(&queued, &parsed);
		assert(lobby.games[0].used);
		assert(lobby.games[0].listing.kind == kinds[k]);
		assert(lobby.games[0].listing.joinable == (kinds[k] == 0));
		assert(lobby.games[0].listing.locked == locked);
		for (int truncated = 0; truncated < size; truncated++)
			assert(!listing_read(bytes, truncated, &parsed));
		assert(!listing_read(bytes, size + 1, &parsed));
	}
	/* Corrupt each length independently, including a length spanning the stamp. */
	for (int field = 0; field < 3; field++)
	{
		int size = record(bytes, 0, 7, 0);
		bytes[65 + 8 * field] = 255;
		assert(!listing_read(bytes, size, &parsed));
	}
	/* Captured master listing headers: internet/master-v1-servers.json.
	   Version/flags are 00 27 23 and 00 27 21; counts are reported players,
	   not machines, so an empty dedicated game stays at zero players. */
	unsigned const flags[] = {0x23, 0x21, 0x01, 0xa3};
	for (unsigned version = 38; version <= 39; version++)
	for (unsigned f = 0; f < sizeof(flags) / sizeof(flags[0]); f++)
	{
		int size = record(bytes, 0, 0, 0);
		bytes[4] = version; bytes[5] = flags[f]; bytes[62] = 0;
		assert(listing_read(bytes, size, &parsed));
		memset(&lobby, 0, sizeof(lobby));
		memcpy(queued.payload, bytes, size); queued.size = size;
		listing_take(&queued, &parsed);
		assert(lobby.games[0].listing.dedicated == (version == 39 && f < 2));
		assert(lobby.games[0].listing.player_count == 0);
		assert(lobby.games[0].listing.network_version == version);
	}
	puts("PASS: internet kinds, locked records, variable strings, truncation");
	return 0;
}
