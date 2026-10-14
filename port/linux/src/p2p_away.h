/*
P2P_AWAY.H

The away notice of internet play (p2p.c): the packet a machine whose app
went to the background sends its peers, how long it asks them to wait, and
when a peer's wait is up. Header-only so that port/ios/tests/away_probe.c
checks it without the tunnel.
*/

#ifndef __P2P_AWAY_H
#define __P2P_AWAY_H

/* the packet is 3 bytes: the packet type, then the seconds the peers wait
(network byte order); 0 seconds is "back" */
#define P2P_AWAY_PACKET_SIZE 3
#define P2P_AWAY_MAXIMUM_SECONDS 65535

/* the seconds asked for, as the packet carries them: at least 1 and at most
what two bytes hold */
static inline long p2p_away_clamp_grace(long grace)
{
	if (grace < 1)
		return 1;
	if (grace > P2P_AWAY_MAXIMUM_SECONDS)
		return P2P_AWAY_MAXIMUM_SECONDS;
	return grace;
}

/* the packet: type, and the seconds when away (the clamped grace), else 0 */
static inline void p2p_away_packet_make(unsigned char *inner, unsigned char type, int away, long grace)
{
	long seconds = away ? p2p_away_clamp_grace(grace) : 0;

	inner[0] = type;
	inner[1] = (unsigned char)(seconds >> 8);
	inner[2] = (unsigned char)seconds;
}

/* the seconds in a packet (past its type byte), or -1 for one too short */
static inline int p2p_away_packet_seconds(const unsigned char *inner, int size)
{
	if (size < P2P_AWAY_PACKET_SIZE)
		return -1;
	return inner[1] << 8 | inner[2];
}

/* whether a wait that ends at until (milliseconds on the same clock, which
may wrap) is up at now */
static inline int p2p_away_wait_over(unsigned long now, unsigned long until)
{
	return (long)(now - until) >= 0;
}

#endif
