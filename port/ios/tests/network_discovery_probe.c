/* The production client's compatibility gate, with its platform boundaries
   replaced. Literal kinds come from the permitted v38 LAN description. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "halo_network_version.h"
#include "halo_network_content.h"
/* Exercise the new browser contract independently of the version bump. */
#undef HALO_PORT_NETWORK_VERSION
#define HALO_PORT_NETWORK_VERSION 38

typedef int boolean;
#define TRUE 1
#define FALSE 0
#define NONE -1
#define MAXIMUM_NETWORK_ADVERTISED_GAMES 2
#define csprintf sprintf
struct network_advertised_game { int unused; };
struct network_game_client { struct network_advertised_game available_games[2]; };
static struct { unsigned version, flags, kind; } network_game_client_advertised_versions[2];
static int told;
static char last_message[400];
static void platform_show_message(char const *title, char const *message)
{
	(void)title;
	told++;
	snprintf(last_message, sizeof(last_message), "%s", message);
}
static void network_event(char const *format, ...) { (void)format; }
/* PRODUCTION */
int main(void)
{
	struct network_game_client client = { 0 };
	struct network_advertised_game *game = client.available_games;
	unsigned const kinds[] = { 1, 2, 3, 128, 129, 130, 131, 4, 64, 255 };
	unsigned index;
	network_game_client_advertised_versions[0].version = HALO_PORT_NETWORK_VERSION;
	network_game_client_advertised_versions[0].flags = HALO_PORT_ADVERTISED_DISTRIBUTED_FLAG;
	assert(network_game_client_advertised_game_compatible(&client, game, FALSE));
	for (index = 0; index < sizeof(kinds) / sizeof(kinds[0]); index++)
	{
		network_game_client_advertised_versions[0].kind = kinds[index];
		assert(!network_game_client_advertised_game_compatible(&client, game, FALSE));
		assert(told == 0);
		assert(!network_game_client_advertised_game_compatible(&client, game, TRUE));
		assert(told == 1 && (strstr(last_message, "not supported") || strstr(last_message, "unknown game kind")));
		told = 0;
	}
	network_game_client_advertised_versions[0].kind = 0;
	network_game_client_advertised_versions[0].flags = 0;
	assert(!network_game_client_advertised_game_compatible(&client, game, TRUE));
	assert(told == 1 && strstr(last_message, "lockstep"));
	puts("PASS: LAN game kind refusal and visible reason");
	return 0;
}
