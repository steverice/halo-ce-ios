/* A join code: a short stand-in for an internet game's invite link, for a
player who can type eight characters but not paste a link (an Apple TV and
its remote). The code is P2P_CODE_LENGTH characters of P2P_CODE_ALPHABET,
which leaves out 0, O, 1, I and L, and is read in either case, with spaces
and dashes ignored. How a code finds its game: p2p.c ("join codes"). */

#ifndef P2P_CODE_H
#define P2P_CODE_H

#define P2P_CODE_ALPHABET "23456789ABCDEFGHJKMNPQRSTUVWXYZ"
#define P2P_CODE_ALPHABET_SIZE 31
#define P2P_CODE_LENGTH 8
/* the random bytes a code is made from: two a character, so that the
alphabet's characters are all but equally likely */
#define P2P_CODE_RANDOM_SIZE (2 * P2P_CODE_LENGTH)

/* the code of random bytes: P2P_CODE_LENGTH characters and a terminator */
void p2p_code_make(const unsigned char *random, char *code);
/* text typed for a code: 1 and the code (as p2p_code_make writes it) if it
is one, its spaces and dashes left out and its letters in either case; else
0 */
int p2p_code_normalize(const char *text, char *code);
/* a code as it is shown: its halves with a dash between them ("ABCD-EFGH"),
and a terminator (P2P_CODE_LENGTH + 2 characters) */
void p2p_code_display(const char *code, char *text);

#endif
