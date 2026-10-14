/* internet play's away notice (p2p_away.h): the packet an app in the background sends its peers, the seconds it
   asks them to wait (clamped to what two bytes hold), a packet too short to read, and when a wait is up, across
   the milliseconds clock's wrap */
#include "p2p_away.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

int main(void) {
    unsigned char packet[P2P_AWAY_PACKET_SIZE];

    assert(P2P_AWAY_PACKET_SIZE == 3);
    /* the default, 60 seconds: 0x003c, network byte order */
    p2p_away_packet_make(packet, 9, 1, 60);
    assert(packet[0] == 9 && packet[1] == 0 && packet[2] == 60);
    assert(p2p_away_packet_seconds(packet, sizeof(packet)) == 60);
    /* more than 255 uses both bytes */
    p2p_away_packet_make(packet, 9, 1, 300);
    assert(packet[1] == 1 && packet[2] == 44);
    assert(p2p_away_packet_seconds(packet, sizeof(packet)) == 300);
    /* the least and the most */
    assert(p2p_away_clamp_grace(0) == 1 && p2p_away_clamp_grace(-5) == 1);
    assert(p2p_away_clamp_grace(65535) == 65535 && p2p_away_clamp_grace(70000) == 65535);
    p2p_away_packet_make(packet, 9, 1, 0);
    assert(p2p_away_packet_seconds(packet, sizeof(packet)) == 1);
    p2p_away_packet_make(packet, 9, 1, 1000000);
    assert(p2p_away_packet_seconds(packet, sizeof(packet)) == 65535);
    /* back: 0 whatever the grace */
    p2p_away_packet_make(packet, 9, 0, 60);
    assert(packet[1] == 0 && packet[2] == 0);
    assert(p2p_away_packet_seconds(packet, sizeof(packet)) == 0);
    /* too short: never read as "back" */
    assert(p2p_away_packet_seconds(packet, 2) == -1 && p2p_away_packet_seconds(packet, 0) == -1);
    /* the wait: not over before its end, over at it and after, and across the wrap of the clock */
    assert(!p2p_away_wait_over(1000, 1001) && p2p_away_wait_over(1001, 1001) && p2p_away_wait_over(5000, 1001));
    unsigned long near_wrap = ULONG_MAX - 500;
    unsigned long until = near_wrap + 60000;
    assert(!p2p_away_wait_over(near_wrap, until) && !p2p_away_wait_over(until - 1, until));
    assert(p2p_away_wait_over(until, until) && p2p_away_wait_over(until + 1000, until));
    printf("away probe ok\n");
    return 0;
}
