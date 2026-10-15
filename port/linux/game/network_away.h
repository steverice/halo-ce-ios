/*
NETWORK_AWAY.H

A network game's machines whose app went to the background (an Apple
device's, suspended in seconds), shown to the players: network_away.c.
*/

#ifndef __NETWORK_AWAY_H
#define __NETWORK_AWAY_H

/* each away notice internet play has (p2p_take_away_notice) shown on the
local players' screens in a game, and a rejoin's steps at the main menu
(once it is loaded); once a frame, from the game loop */
void network_away_update(
	boolean main_menu_loaded);
/* the game this machine joined was lost (its host gone): joined again from
the main menu if this machine or its host was away (p2p_rejoin_invite) */
void network_away_game_lost(
	void);

#endif
