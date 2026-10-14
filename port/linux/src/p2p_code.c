/* Join codes (p2p_code.h): their text made, read and shown, and their
records at the brokers made and opened (p2p.c, "join codes", has the
design). No network here, so that tools/ios_test.py checks it as it is. */

#include "p2p_code.h"
#include "p2p_internal.h"

#include <stdio.h>
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

/* ---------- records */

static void code_hex(const unsigned char *bytes, int size, char *text)
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

void p2p_code_topic(const char *code, char *topic)
{
	unsigned char digest[P2P_SHA256_SIZE];
	char hashed[64];
	char hex[2 * 16 + 1];

	snprintf(hashed, sizeof(hashed), "hceu-join-code-topic-1:%s", code);
	p2p_sha256(hashed, (int)strlen(hashed), digest);
	code_hex(digest, 16, hex);
	snprintf(topic, P2P_CODE_TOPIC_SIZE, "%s%s", P2P_CODE_TOPIC_PREFIX, hex);
}

static void code_key(const char *code, unsigned char *key)
{
	static const char label[] = "hceu-join-code-key-1";
	unsigned char salt[P2P_SHA256_SIZE];

	p2p_sha256(label, (int)sizeof(label) - 1, salt);
	p2p_password_key(code, salt, key);
}

int p2p_code_record_make(const char *code, const unsigned char *invite, unsigned char *record)
{
	unsigned char key[P2P_PASSWORD_KEY_SIZE];
	int size;

	code_key(code, key);
	record[0] = 'H';
	record[1] = 'C';
	record[2] = 1;
	size = 3 + p2p_seal(key, invite, P2P_KEY_HASH_SIZE + P2P_TOKEN_SIZE, record + 3);
	memset(key, 0, sizeof(key));
	return size;
}

int p2p_code_lookup(const char *text, char *topic, unsigned char *key)
{
	char code[P2P_CODE_LENGTH + 1];

	/* (the code as the host made it, whatever was typed: the topic and key
	are of that alone) */
	if (!p2p_code_normalize(text, code))
		return 0;
	p2p_code_topic(code, topic);
	code_key(code, key);
	return 1;
}

int p2p_code_record_open(const unsigned char *key, const unsigned char *record, int size, unsigned char *invite)
{
	unsigned char bytes[P2P_KEY_HASH_SIZE + P2P_TOKEN_SIZE + P2P_SEAL_OVERHEAD];

	if (size < 3 || size - 3 > (int)sizeof(bytes) || record[0] != 'H' || record[1] != 'C' || record[2] != 1)
		return 0;
	if (p2p_open(key, record + 3, size - 3, bytes) != P2P_KEY_HASH_SIZE + P2P_TOKEN_SIZE)
		return 0;
	memcpy(invite, bytes, P2P_KEY_HASH_SIZE + P2P_TOKEN_SIZE);
	return 1;
}
