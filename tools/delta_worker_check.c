/* The thread that asks Delta's list (port/linux/src/p2p_delta.c) against a stand-in server, for tools/test_delta.py:
   browsing, announcing, an invite that changes, a game that ends, and quitting. The rest of internet play is
   stood in for here. Usage: delta_worker_check (the server's address in HALO_DELTA_URL) */
#include "p2p_internal.h"
#include "delta_list.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

pthread_mutex_t p2p_lock = PTHREAD_MUTEX_INITIALIZER;

static int announce_setting;
/* the game hosted, as p2p_lobby_delta_wanted gives it (under p2p_lock) */
static int wanted;
static struct delta_announce hosted;

void platform_log(const char *format, ...)
{
	va_list arguments;

	printf("LOG ");
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	printf("\n");
	fflush(stdout);
}

void Sleep(unsigned long milliseconds) { usleep((useconds_t)milliseconds * 1000); }

unsigned long p2p_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned long)now.tv_sec * 1000 + (unsigned long)now.tv_nsec / 1000000;
}

int config_boolean(const char *name)
{
	if (!strcmp(name, "network.delta_announce"))
		return announce_setting;
	return 1;
}

const char *config_string(const char *name)
{
	return !strcmp(name, "network.delta_url") ? getenv("HALO_DELTA_URL") : "";
}

int p2p_lobby_listed(void) { return wanted; }

int p2p_lobby_delta_wanted(struct delta_announce *announce)
{
	if (!wanted)
		return 0;
	*announce = hosted;
	return 1;
}

void p2p_lobby_delta_games(const struct delta_game *games, int count)
{
	printf("GAMES %d", count);
	if (count)
		printf(" %s %s %d", games[0].name, games[0].map, games[0].version);
	printf("\n");
	fflush(stdout);
}

static void host(const char *invite, int players)
{
	pthread_mutex_lock(&p2p_lock);
	wanted = 1;
	memset(&hosted, 0, sizeof(hosted));
	strcpy(hosted.invite, invite);
	strcpy(hosted.name, "Checked game");
	strcpy(hosted.map, "bloodgulch");
	hosted.engine = 2;
	hosted.players = players;
	hosted.maximum_players = 16;
	hosted.open = 1;
	hosted.version = 29;
	pthread_mutex_unlock(&p2p_lock);
}

static void phase(const char *name)
{
	printf("PHASE %s\n", name);
	fflush(stdout);
	usleep(2000 * 1000);
}

int main(void)
{
	pthread_mutex_lock(&p2p_lock);
	p2p_delta_browse(1);
	p2p_delta_update();
	pthread_mutex_unlock(&p2p_lock);
	phase("browsing");
	announce_setting = 1;
	host("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 3);
	phase("announced");
	host("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 3);
	phase("another invite");
	pthread_mutex_lock(&p2p_lock);
	wanted = 0;
	pthread_mutex_unlock(&p2p_lock);
	phase("ended");
	host("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc", 5);
	phase("hosting again");
	p2p_delta_quit();
	printf("PHASE quit\n");
	return 0;
}
