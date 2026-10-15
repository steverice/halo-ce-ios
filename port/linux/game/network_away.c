/*
NETWORK_AWAY.C

A network game's machines whose app went to the background. An Apple
device suspends an app in the background within seconds (its game held at
its next frame, port/ios/host/host_lifecycle.m), so internet play's peer
says it is away and how long it is waited for (p2p.c, network.background_grace):
the host waits for a joiner that long rather than dropping it at 15 seconds
(network_server_manager.c), and a connection to it waits as long
(network_connection.c). The players see it here: "NAME's game is in the
background", "NAME is back", or "NAME did not come back in time", on each
local player's screen (hud_print_message). A joiner sees its host's.
*/

/* ---------- headers */

#include "cseries.h"
#include "main/main.h"
#include "game/game.h"
#include "game/players.h"
#include "interface/hud_messaging.h"
#include "interface/player_ui.h"
#include "interface/ui_widget.h"
#include "networking/network_game_globals.h"
#include "networking/network_client_manager.h"
#include "network_away.h"
#include "../src/p2p.h" /* port: port/linux/src/p2p.c */

#include <string.h>

/* ---------- prototypes */

/* the platform layer's (port/linux/src) */
void platform_log(char const *format, ...);
/* menu_tags.c's: a screen's name where the PC version's menus are up */
char const *pc_menus_screen(char const *name);

/* ---------- constants */

enum
{
	/* how long a rejoin looks for the game before it gives up (the host is
	reached again first) */
	REJOIN_SECONDS = 60,
};

/* ---------- globals */

/* a joiner's game lost while it or its host was away: joined again from
the main menu, as the system link test's bot joins (network_test.c) */
static struct
{
	boolean pending, set_up, joined;
	char invite[256];
	float seconds;
} rejoin;

/* network_server_manager.c: the name of the host's machine at an internet
play peer's address (network byte order) */
boolean network_game_server_port_machine_name_at_address(
	unsigned long address,
	wchar_t *name,
	short size);

/* ---------- private code */

/* the rejoin's steps, a frame at a time at the main menu */
static void rejoin_update(
	boolean main_menu_loaded)
{
	static unsigned long last_time;
	unsigned long now = system_milliseconds();
	float seconds = last_time ? (now - last_time) / 1000.0f : 0.0f;

	last_time = now;
	if (!rejoin.pending || game_in_progress() || !main_menu_loaded)
		return;
	rejoin.seconds += seconds;
	if (!rejoin.set_up)
	{
		rejoin.set_up = TRUE;
		dispose_global_network_game_client();
		dispose_global_network_game_server();
		if (!create_global_network_game_client() || !p2p_join_invite(rejoin.invite))
		{
			platform_log("network away: the game cannot be joined again");
			rejoin.pending = FALSE;
			return;
		}
		game_connection_set(_game_connection_network_client);
		player_ui_local_player_joined_multiplayer_game(0);
		platform_log("network away: joining the game again (its host's invite)");
	}
	else if (!rejoin.joined && network_game_client_join_first_available_game())
	{
		rejoin.joined = TRUE;
		rejoin.pending = FALSE;
		ui_widgets_close_all();
		ui_widget_load_by_name_or_tag(
			pc_menus_screen("ui\\shell\\main_menu\\multiplayer_type_select\\connected\\pregame\\connected_pregame_screen"),
			NONE, NULL, NONE, NONE, NONE, NONE);
		platform_log("network away: joined the game again");
	}
	else if (rejoin.seconds > REJOIN_SECONDS)
	{
		platform_log("network away: the game was not found again in %d s; not joining it", REJOIN_SECONDS);
		rejoin.pending = FALSE;
	}
}

/* ---------- public code */

void network_away_game_lost(
	void)
{
	char invite[sizeof(rejoin.invite)];

	if (!p2p_rejoin_invite(invite, sizeof(invite)))
		return;
	memset(&rejoin, 0, sizeof(rejoin));
	strncpy(rejoin.invite, invite, sizeof(rejoin.invite) - 1);
	rejoin.pending = TRUE;
	platform_log("network away: the game was lost while this machine or its host was away; it is joined again "
		"from the main menu");
}

void network_away_update(
	boolean main_menu_loaded)
{
	struct p2p_away_notice notice;

	rejoin_update(main_menu_loaded);

	while (p2p_take_away_notice(&notice))
	{
		wchar_t name[32];
		wchar_t text[128];
		short local_index;

		if (!game_in_progress())
		{
			continue;
		}
		if (notice.is_host)
		{
			ustrncpy(name, L"The host", NUMBEROF(name) - 1);
			name[NUMBEROF(name) - 1] = 0;
		}
		else if (!global_network_game_server_get() ||
			!network_game_server_port_machine_name_at_address(notice.address, name, NUMBEROF(name)))
		{
			ustrncpy(name, L"A player", NUMBEROF(name) - 1);
			name[NUMBEROF(name) - 1] = 0;
		}
		if (notice.seconds > 0)
		{
			usnprintf(text, NUMBEROF(text) - 1, L"%s's game is in the background: waiting up to %d seconds",
				name, notice.seconds);
		}
		else if (notice.seconds == 0)
		{
			usnprintf(text, NUMBEROF(text) - 1, L"%s is back", name);
		}
		else
		{
			usnprintf(text, NUMBEROF(text) - 1, L"%s did not come back in time", name);
		}
		text[NUMBEROF(text) - 1] = 0;
		for (local_index = 0; local_index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; local_index++)
		{
			if (local_player_get_player_index(local_index) != NONE)
			{
				hud_print_message(local_index, text);
			}
		}
	}
}
