/* MASTER_LIST.H — authenticated official master responses. */
#ifndef MASTER_LIST_H
#define MASTER_LIST_H

enum
{
	MASTER_MAXIMUM_GAMES = 512,
	MASTER_MAXIMUM_ANSWER = 1048576,
	MASTER_MAXIMUM_LISTING = 268,
};

struct master_game
{
	char id[17], region[32];
	unsigned long long updated;
	unsigned char listing[MASTER_MAXIMUM_LISTING];
	int listing_size;
};

/* Returns the accepted count, or -1 with a reason in error. now is Unix seconds.
The signature is the complete Signature header, including "ed25519=". */
int master_parse_list(const char *body, int size, const char *signature,
	const char *signature_key, long long now, struct master_game *games,
	int maximum, char *error, int error_size);

#endif
