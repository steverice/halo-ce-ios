/* Join links (halo://join/<64 hex digits>, internet play's invites, p2p.c) the
   app is opened with: host_join_link.c hands them to the game. Header-only
   parsing so tests/join_link_probe.c can check it on the Mac. */
#pragma once
#include <stddef.h>
#include <stdio.h>

/* the hex digits of an invite: the host's key hash and the token (p2p.c's
   P2P_KEY_HASH_SIZE + P2P_TOKEN_SIZE, 16 bytes each) */
#define HOST_JOIN_LINK_DIGITS 64
#define HOST_JOIN_LINK_PREFIX "halo://join/"
/* the invite, written back as the game makes it: the prefix, the digits, a NUL */
#define HOST_JOIN_LINK_SIZE (sizeof(HOST_JOIN_LINK_PREFIX) - 1 + HOST_JOIN_LINK_DIGITS + 1)

/* ASCII letters only: no other byte folds onto the prefix */
static inline char host_join_link_lower(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c | 0x20) : c;
}

/* url, as the system opened the app with it: 1 with invite (HOST_JOIN_LINK_SIZE)
   filled with the link the game takes (prefix in lowercase, digits in
   lowercase); 0 when url is not a halo: link at all (another URL, a file),
   which is none of this code's business; -1 when it is a halo: link but not a
   join link: another path, the wrong number of digits, a query, anything
   after the digits but one slash. Nothing from a link is used but its digits. */
static inline int host_join_link_parse(const char *url, char *invite)
{
	static const char prefix[] = HOST_JOIN_LINK_PREFIX;
	size_t index;

	if (!url)
		return 0;
	/* the scheme, in any case (RFC 3986) */
	for (index = 0; index < 5; index++)
	{
		if (host_join_link_lower(url[index]) != prefix[index])
			return 0;
	}
	/* "//join/": the authority too is case-insensitive */
	for (; prefix[index]; index++)
	{
		if (host_join_link_lower(url[index]) != prefix[index])
			return -1;
	}
	url += index;
	for (index = 0; index < HOST_JOIN_LINK_DIGITS; index++)
	{
		char digit = host_join_link_lower(url[index]);

		if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f')))
			return -1;
	}
	if (url[index] == '/')
		index++;
	if (url[index])
		return -1;
	snprintf(invite, HOST_JOIN_LINK_SIZE, "%s", prefix);
	for (index = 0; index < HOST_JOIN_LINK_DIGITS; index++)
	{
		invite[sizeof(prefix) - 1 + index] = host_join_link_lower(url[index]);
	}
	invite[sizeof(prefix) - 1 + HOST_JOIN_LINK_DIGITS] = 0;
	return 1;
}

/* from main, with the data folder, before anything spins the run loop: clears
   a link a previous run left untaken, and starts listening for links (SDL's
   events subsystem comes up early for it) */
void host_join_link_install(const char *data_root);
/* a URL the app was opened with (SDL's drop event, or a scene of the host's own) */
void host_join_link_open(const char *url);
/* from the guest's event poll (host_sdl_poll_event): tells the player about a
halo: link that was not a join link, once, if one came since the last poll */
void host_join_link_poll(void);
