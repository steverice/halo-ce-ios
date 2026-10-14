/* Join codes' text (port/linux/src/p2p_code.c): made only of the alphabet,
read back in either case with spaces and dashes, shown with a dash, and
anything else refused. Run by tools/ios_test.py. */
#include "p2p_code.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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
	puts("PASS: join codes: alphabet, either case, spaces and dashes, and refusals");
	return 0;
}
