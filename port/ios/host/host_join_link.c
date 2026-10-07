/* Join links the app is opened with (CFBundleURLTypes' halo scheme). UIKit
gives the URL to SDL's scene delegate, at launch (its connection options) or
while the app runs (scene:openURLContexts:), and SDL posts it as a drop event;
an event watch takes it here. A valid link is written to join_link.txt in the
data folder, which internet play's thread looks for every second
(poll_invite_file, p2p.c, as upstream's Android activity writes it) and joins
as Direct Link's PASTE LINK does (p2p_invite_received). So a link is taken
whatever the game is doing: in a menu, loading or mid-campaign, internet play
reaches the invite's host in the background, and the host's game is then in
Join Game > Direct Link's list, as on the desktop. With internet play off, the
game takes the file itself and asks to turn internet play on (sdl_platform.c's
platform_link_consent); on a cold launch with it on, the file waits until
internet play starts. Internet play tells how the join
goes in the game's own log (stderr), which the app keeps only in stderr.log:
from a link on, it goes there whether or not the file was made before launch. */
#include "host_join_link.h"
#include "host_config.h"
#include "ios_host.h"
#include <SDL3/SDL.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char link_path[1100], temporary_path[1100], game_log_path[1100];

/* the game's log (its stderr: internet play's lines among it) kept in
stderr.log from here, unless it already is (the file was there at launch,
which host_main.m looks for: made by hand, a tool, or an earlier link) */
static void keep_game_log(void)
{
	static int kept;
	struct stat status;
	int existed = stat(game_log_path, &status) == 0;
	int descriptor;

	if (kept)
		return;
	kept = 1;
	if (existed)
		return;
	descriptor = open(game_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (descriptor < 0 || dup2(descriptor, 2) < 0)
	{
		host_logf(HOST_LOG_WARN, "join link: cannot keep the game's log in stderr.log");
		if (descriptor >= 0)
			close(descriptor);
		return;
	}
	close(descriptor);
	host_logf(HOST_LOG_INFO, "join link: the game's log (internet play's steps) goes to stderr.log from here");
}

/* a halo: link that is not a join link, waiting to be told to the player
(host_join_link_poll). The main thread's alone: SDL calls the event watch
where the drop event is posted, inside UIKit's callbacks on the main
thread, and the guest polls events on the main thread too */
static int refused_waiting;

static bool SDLCALL join_link_event(void *userdata, SDL_Event *event)
{
	(void)userdata;
	if (event->type == SDL_EVENT_DROP_FILE)
		host_join_link_open(event->drop.data);
	return true;
}

void host_join_link_install(const char *data_root)
{
	snprintf(link_path, sizeof(link_path), "%s/join_link.txt", data_root);
	snprintf(temporary_path, sizeof(temporary_path), "%s/join_link.new", data_root);
	snprintf(game_log_path, sizeof(game_log_path), "%s/stderr.log", data_root);
	/* a link from a run that ended before internet play took it is not one
	the player just opened */
	if (remove(link_path) == 0)
		host_logf(HOST_LOG_INFO, "join link: removed one a previous run did not take");
	/* (and half a write, should a run have ended in the middle of one) */
	remove(temporary_path);
	/* (and a link the game was asking about when a run ended: sdl_platform.c's
	platform_link_consent) */
	{
		char asked_path[1100];

		snprintf(asked_path, sizeof(asked_path), "%s/join_link.asked", data_root);
		remove(asked_path);
	}
	/* SDL drops a URL that arrives before its event queue is up, and the XISO
	import screen spins the run loop before SDL_Init */
	if (!SDL_InitSubSystem(SDL_INIT_EVENTS) || !SDL_AddEventWatch(join_link_event, NULL))
		host_logf(HOST_LOG_ERROR, "join link: cannot watch SDL's events (%s); links will not open games",
			SDL_GetError());
}

void host_join_link_open(const char *url)
{
	char invite[HOST_JOIN_LINK_SIZE];
	FILE *file;
	int parsed = host_join_link_parse(url, invite);
	int written;

	if (!parsed)
		return;
	if (parsed < 0)
	{
		host_logf(HOST_LOG_WARN, "join link: ignored a halo: link that is not a join link (%.80s)", url);
		refused_waiting = 1;
		return;
	}
	/* written whole, then renamed into place: the game takes the file by
	renaming it, and must never read half a link */
	file = fopen(temporary_path, "wb");
	written = file && fputs(invite, file) >= 0;
	if (file && fclose(file) != 0)
		written = 0;
	if (!written || rename(temporary_path, link_path) != 0)
	{
		remove(temporary_path);
		host_logf(HOST_LOG_ERROR, "join link: cannot write %s for the game", link_path);
		return;
	}
	host_logf(HOST_LOG_INFO, "join link: opened with %s, for the game to take", invite);
	keep_game_log();
}

/* shown between the guest's frames, never from the event watch: the watch
runs inside SDL_PumpEvents or a UIKit callback, and a message box spins the
run loop until it is answered. (The guest polls with its input_lock held,
platform_pump_events: nothing the box runs takes that lock, and the p2p
thread never does, so the box cannot deadlock; the game's input simply
waits for the answer.) */
void host_join_link_poll(void)
{
	char null_renderer[16];

	if (!refused_waiting)
		return;
	refused_waiting = 0;
	/* (a run nobody watches, as platform_show_message skips its box) */
	host_config_string("debug.null_renderer", "false", null_renderer, sizeof(null_renderer));
	if (!strcmp(null_renderer, "true"))
	{
		host_logf(HOST_LOG_INFO, "join link: no message for it (debug.null_renderer)");
		return;
	}
	host_logf(HOST_LOG_INFO, "join link: told the player a halo: link is not a join link");
	host_sdl_show_simple_message_box(SDL_MESSAGEBOX_INFORMATION, "Halo",
		"That link isn't a Halo invite, or it's from an older version of the game.");
}
