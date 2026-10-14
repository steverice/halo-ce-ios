/* Join codes' text (p2p_code.h): made, read and shown. No network and no
keys here, so that tools/ios_test.py checks it as it is. */

#include "p2p_code.h"

#include <string.h>

void p2p_code_make(const unsigned char *random, char *code)
{
	int index;

	for (index = 0; index < P2P_CODE_LENGTH; index++)
	{
		unsigned int value = ((unsigned int)random[2 * index] << 8) | random[2 * index + 1];

		code[index] = P2P_CODE_ALPHABET[value % P2P_CODE_ALPHABET_SIZE];
	}
	code[P2P_CODE_LENGTH] = 0;
}

int p2p_code_normalize(const char *text, char *code)
{
	int length = 0;

	for (; text && *text; text++)
	{
		char character = *text;

		if (character == ' ' || character == '-' || character == '\t' || character == '\r' || character == '\n')
			continue;
		if (character >= 'a' && character <= 'z')
			character = (char)(character - 'a' + 'A');
		if (length == P2P_CODE_LENGTH || !strchr(P2P_CODE_ALPHABET, character))
			return 0;
		code[length++] = character;
	}
	code[length] = 0;
	return length == P2P_CODE_LENGTH;
}

void p2p_code_display(const char *code, char *text)
{
	int half = P2P_CODE_LENGTH / 2;

	memcpy(text, code, (size_t)half);
	text[half] = '-';
	memcpy(text + half + 1, code + half, (size_t)(P2P_CODE_LENGTH - half));
	text[P2P_CODE_LENGTH + 1] = 0;
}
