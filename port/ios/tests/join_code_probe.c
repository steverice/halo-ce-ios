/* Join codes (port/linux/src/p2p_code.c): their text made only of the
alphabet, read back in either case with spaces and dashes, shown with a
dash, and anything else refused; and a host's record found and opened from
the code typed every way a player may type it, through p2p_code_lookup, which
p2p_join_code (the ENTER CODE screen's, and a code handed over as a link)
turns the text into a topic and key with. Run by tools/ios_test.py. */
#include "p2p_code.h"
#include "p2p_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (p2p_crypto.c's nonces) */
void posix_random_bytes(void *buffer, unsigned long size)
{
	unsigned char *bytes = buffer;
	unsigned long index;

	for (index = 0; index < size; index++)
		bytes[index] = (unsigned char)rand();
}

/* (p2p.c's, which p2p_crypto.c's WebRTC credentials use) */
void p2p_hex(const unsigned char *bytes, int size, char *text)
{
	static const char digits[] = "0123456789abcdef";
	int index;

	for (index = 0; index < size; index++)
	{
		text[index * 2] = digits[bytes[index] >> 4];
		text[index * 2 + 1] = digits[bytes[index] & 15];
	}
	text[size * 2] = 0;
}

/* the host's side for code: its topic and record of the invite; then each
typing of it as a joiner's lookup: the same topic, and the record opens to
the invite */
static void code_typed(const char *code, const char *const *typings, int count)
{
	unsigned char invite[P2P_KEY_HASH_SIZE + P2P_TOKEN_SIZE], opened[sizeof(invite)];
	unsigned char record[P2P_CODE_RECORD_SIZE], key[P2P_PASSWORD_KEY_SIZE];
	char host_topic[P2P_CODE_TOPIC_SIZE], topic[P2P_CODE_TOPIC_SIZE];
	int size, index;

	for (index = 0; index < (int)sizeof(invite); index++)
		invite[index] = (unsigned char)(index * 37 + code[0]);
	p2p_code_topic(code, host_topic);
	size = p2p_code_record_make(code, invite, record);
	assert(size > 3 && size <= P2P_CODE_RECORD_SIZE);
	assert(!strncmp(host_topic, P2P_CODE_TOPIC_PREFIX, strlen(P2P_CODE_TOPIC_PREFIX)));
	for (index = 0; index < count; index++)
	{
		memset(topic, 0, sizeof(topic));
		memset(opened, 0, sizeof(opened));
		if (!p2p_code_lookup(typings[index], topic, key))
		{
			printf("FAIL: \"%s\" is not read as the code %s\n", typings[index], code);
			exit(1);
		}
		if (strcmp(topic, host_topic) || !p2p_code_record_open(key, record, size, opened) ||
			memcmp(opened, invite, sizeof(invite)))
		{
			printf("FAIL: \"%s\" does not find the record of %s\n", typings[index], code);
			exit(1);
		}
	}
	/* (another code's key opens nothing, nor does an altered record) */
	assert(p2p_code_lookup("2345-6789", topic, key) && strcmp(topic, host_topic));
	assert(!p2p_code_record_open(key, record, size, opened));
	assert(p2p_code_lookup(typings[0], topic, key));
	record[size - 1] ^= 1;
	assert(!p2p_code_record_open(key, record, size, opened));
	record[size - 1] ^= 1;
	assert(!p2p_code_record_open(key, record, 3, opened));
	assert(!p2p_code_lookup("halo://join/00112233", topic, key));
	assert(!p2p_code_lookup("ABCD-EFG", topic, key));
}

int main(void)
{
	unsigned char random[P2P_CODE_RANDOM_SIZE];
	char code[P2P_CODE_LENGTH + 1], again[P2P_CODE_LENGTH + 1], shown[P2P_CODE_LENGTH + 2];
	int counts[P2P_CODE_ALPHABET_SIZE] = { 0 };
	int round, index;

	assert(strlen(P2P_CODE_ALPHABET) == P2P_CODE_ALPHABET_SIZE);
	assert(!strpbrk(P2P_CODE_ALPHABET, "0O1IL"));
	for (round = 0; round < 20000; round++)
	{
		for (index = 0; index < P2P_CODE_RANDOM_SIZE; index++)
			random[index] = (unsigned char)((round * 131 + index * 977 + (round >> 3) * 7) & 0xFF);
		p2p_code_make(random, code);
		assert(strlen(code) == P2P_CODE_LENGTH);
		for (index = 0; index < P2P_CODE_LENGTH; index++)
		{
			const char *at = strchr(P2P_CODE_ALPHABET, code[index]);

			assert(at);
			counts[at - P2P_CODE_ALPHABET]++;
		}
		p2p_code_display(code, shown);
		assert(strlen(shown) == P2P_CODE_LENGTH + 1 && shown[P2P_CODE_LENGTH / 2] == '-');
		assert(p2p_code_normalize(shown, again) && !strcmp(again, code));
	}
	for (index = 0; index < P2P_CODE_ALPHABET_SIZE; index++)
		assert(counts[index] > 0);
	/* typed in lower case, with spaces and a dash */
	assert(p2p_code_normalize(" abcd-efgh ", again) && !strcmp(again, "ABCDEFGH"));
	assert(p2p_code_normalize("2345 6789", again) && !strcmp(again, "23456789"));
	/* refused: the left-out characters, too short, too long, a link */
	assert(!p2p_code_normalize("ABCD-EFG0", again));
	assert(!p2p_code_normalize("ABCDEFGO", again));
	assert(!p2p_code_normalize("ABCDEFG1", again));
	assert(!p2p_code_normalize("abcdefgi", again));
	assert(!p2p_code_normalize("abcdefgl", again));
	assert(!p2p_code_normalize("ABCDEFG", again));
	assert(!p2p_code_normalize("ABCDEFGHJ", again));
	assert(!p2p_code_normalize("", again));
	assert(!p2p_code_normalize("halo://join/00112233", again));
	{
		/* as the host makes and shows it, and as a player types it */
		static const char *const typings[] = {
			"A8J5G7FV", "A8J5-G7FV", "a8j5g7fv", "a8j5-g7fv", "A8J5 G7FV", " a8J5 - G7fv ", "A8J5G7FV\n",
		};

		code_typed("A8J5G7FV", typings, (int)(sizeof(typings) / sizeof(*typings)));
	}
	puts("PASS: join codes: alphabet, either case, spaces and dashes, refusals, and the record found from the "
		"code typed with or without its dash, in either case");
	return 0;
}
