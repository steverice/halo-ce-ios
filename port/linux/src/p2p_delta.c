/*
P2P_DELTA.C

Internet play's use of Delta's public game list (delta_list.h; the service is
run by others, network.delta_url):

- Browsing (network.delta_browse): while the server browser is open, the
  list's games are asked for (GET /v1/games, nothing else sent) and handed to
  p2p_lobby.c, which shows them with the signalling brokers' games.
- Announcing (network.delta_announce, off by default): the public game this
  machine hosts for the internet, without a password, is listed with
  POST /v1/announce, again every minute (the list drops a game it has not
  heard of in 75 seconds), and withdrawn with POST /v1/withdraw when it ends,
  goes private or changes invite, and when the game quits. The list's own
  probe of the game is not described; a refusal waits and tries again
  (delta_announce_step).

The requests block for seconds, so a thread of its own makes them, started
the first time either is wanted. It takes p2p_lock for a moment to hand over
games and to read the game hosted, and never holds its own lock while it
does; the p2p thread, which holds p2p_lock when it calls in here, may take
the thread's lock under it.
*/

#include "platform.h"
#include "posix.h"
#include "port_config.h"
#include "p2p_internal.h"
#include "delta_list.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
	/* milliseconds */
	POLL_INTERVAL = 250,
	/* the list is asked for its games this often (it allows 60 calls a
	minute), at least this long after the last, and after a failure this
	long */
	BROWSE_INTERVAL = 15000,
	BROWSE_MINIMUM = 5000,
	BROWSE_RETRY = 60000,
	REQUEST_TIMEOUT = 15000,
	/* an announcement or a withdrawal waits this long for each step, so that quitting, which waits for one under
	way, usually sees it end */
	ANNOUNCE_TIMEOUT = 4000,
	QUIT_TIMEOUT = 3000,
	QUIT_WAIT = 6000,
	URL_SIZE = 256,
	ERROR_SIZE = 160,
	FORM_SIZE = 640,
};

static struct
{
	pthread_mutex_t lock;
	int started;
	int browse, refresh, quitting;
	/* a request is under way (its answer not yet taken), so quitting waits */
	int busy;

	/* the invite being announced by a request under way (under lock), which quitting withdraws even if the
	request outlasts its wait */
	char pending[DELTA_INVITE_DIGITS + 1];

	/* the thread's alone, but for quitting once it is idle */
	struct delta_announce_state announce;
	/* when the games were asked for last, and when they are next (browse_asked: there was a last) */
	unsigned long browse_time, browse_next;
	int browse_asked;
	/* what was logged last, so that a list that stays unreachable says so once */
	int browse_failed, announce_failed;
} delta = { PTHREAD_MUTEX_INITIALIZER };

/* the thread's alone: an answer, the games read from it */
static char answer[DELTA_MAXIMUM_ANSWER + 1];
static struct delta_game games[DELTA_MAXIMUM_GAMES];

static int elapsed(unsigned long since, unsigned long time)
{
	return (unsigned int)(p2p_now() - since) >= (unsigned int)time;
}

/* the list's address and a path on it into url; 0 if there is none (the
setting is empty) */
static int list_url(char *url, int size, const char *path)
{
	const char *base = config_string("network.delta_url");
	int length;

	if (!base || !*base)
		return 0;
	length = (int)strlen(base);
	while (length && base[length - 1] == '/')
		length--;
	if (!length || length + (int)strlen(path) >= size)
		return 0;
	memcpy(url, base, (size_t)length);
	strcpy(url + length, path);
	return 1;
}

static int browsing_allowed(void)
{
	return config_boolean("network.delta_browse") && config_boolean("network.online") &&
		config_boolean("network.public_lobby");
}

static void browse_once(void)
{
	char url[URL_SIZE], error[ERROR_SIZE];
	int status, count = -1;

	delta.browse_time = p2p_now();
	delta.browse_next = delta.browse_time + BROWSE_INTERVAL;
	delta.browse_asked = 1;
	if (!list_url(url, sizeof(url), "/v1/games"))
		return;
	status = posix_https_request(url, NULL, 0, answer, sizeof(answer), REQUEST_TIMEOUT, error, sizeof(error));
	if (status == 200)
		count = delta_parse_games(answer, (int)strlen(answer), games, DELTA_MAXIMUM_GAMES);
	if (count < 0)
	{
		/* (the games it gave last stay, until the browser forgets them) */
		delta.browse_next = delta.browse_time + BROWSE_RETRY;
		if (!delta.browse_failed)
		{
			platform_log("Internet play: Delta's game list could not be read (%s)",
				status == 200 ? "an answer that is not the list" : status ? "the server answered an error" :
				error[0] ? error : "no answer");
		}
		delta.browse_failed = 1;
		return;
	}
	if (delta.browse_failed)
		platform_log("Internet play: Delta's game list is read again");
	delta.browse_failed = 0;
	pthread_mutex_lock(&p2p_lock);
	p2p_lobby_delta_games(games, count);
	pthread_mutex_unlock(&p2p_lock);
}

/* a request to the list: the status, and whether the answer began "ok" */
static int announce_request(const char *path, const char *form, int form_size, int timeout, int *ok)
{
	char url[URL_SIZE], error[ERROR_SIZE], reply[256];
	int status, seconds;

	*ok = 0;
	if (!list_url(url, sizeof(url), path))
		return 0;
	status = posix_https_request(url, form, form_size, reply, sizeof(reply), timeout, error, sizeof(error));
	if (status == 200)
		*ok = delta_announce_answer(reply, &seconds);
	/* (what the list answered, for the log: the form without its invite) */
	{
		const char *fields = form ? strchr(form, '&') : NULL;

		platform_log("Internet play: Delta %s%s%s -> %d %.60s", path, fields ? " ?" : "", fields ? fields + 1 : "",
			status, status == 200 ? reply : "");
	}
	if (!*ok && !delta.announce_failed)
	{
		platform_log("Internet play: Delta's list did not take the game (%s)",
			status ? "the server answered an error" : error[0] ? error : "no answer");
	}
	delta.announce_failed = !*ok;
	return status;
}

/* the invite's listing taken out of the list; nothing is sent while internet play is off (the list drops the
game in 75 seconds) */
static void withdraw(const char *invite, int timeout)
{
	char form[FORM_SIZE];
	int length = delta_withdraw_form(form, sizeof(form), invite);
	int ok;

	if (length && config_boolean("network.online"))
		announce_request("/v1/withdraw", form, length, timeout, &ok);
	delta.announce_failed = 0;
	platform_log("Internet play: the game is no longer listed in Delta's list");
}

static void announce_pass(void)
{
	struct delta_announce wanted;
	int want = 0;

	if (config_boolean("network.delta_announce") && config_boolean("network.online"))
	{
		pthread_mutex_lock(&p2p_lock);
		want = p2p_lobby_delta_wanted(&wanted);
		pthread_mutex_unlock(&p2p_lock);
	}
	switch (delta_announce_step(&delta.announce, want ? &wanted : NULL, p2p_now()))
	{
	case DELTA_STEP_WITHDRAW:
		withdraw(delta.announce.invite, ANNOUNCE_TIMEOUT);
		delta_withdraw_done(&delta.announce);
		break;
	case DELTA_STEP_ANNOUNCE:
	{
		char form[FORM_SIZE];
		int length = delta_announce_form(form, sizeof(form), &wanted);
		int ok = 0, status;
		int was_listed = delta.announce.listed;

		if (!length)
			break;
		pthread_mutex_lock(&delta.lock);
		strcpy(delta.pending, wanted.invite);
		pthread_mutex_unlock(&delta.lock);
		status = announce_request("/v1/announce", form, length, ANNOUNCE_TIMEOUT, &ok);
		delta_announce_done(&delta.announce, &wanted, status, ok, p2p_now());
		pthread_mutex_lock(&delta.lock);
		delta.pending[0] = 0;
		pthread_mutex_unlock(&delta.lock);
		if (ok && !was_listed)
			platform_log("Internet play: the game is listed in Delta's list (network.delta_announce)");
		break;
	}
	}
}

static void *delta_thread(void *unused)
{
	(void)unused;
	for (;;)
	{
		int browse, refresh;

		Sleep(POLL_INTERVAL);
		pthread_mutex_lock(&delta.lock);
		if (delta.quitting)
		{
			pthread_mutex_unlock(&delta.lock);
			return NULL;
		}
		browse = delta.browse;
		refresh = delta.refresh;
		delta.refresh = 0;
		delta.busy = 1;
		pthread_mutex_unlock(&delta.lock);

		if (browse && browsing_allowed())
		{
			if (!delta.browse_asked || (long)(p2p_now() - delta.browse_next) >= 0 ||
				(refresh && elapsed(delta.browse_time, BROWSE_MINIMUM)))
			{
				browse_once();
			}
		}
		else
		{
			delta.browse_asked = 0;
		}
		announce_pass();

		pthread_mutex_lock(&delta.lock);
		delta.busy = 0;
		pthread_mutex_unlock(&delta.lock);
	}
}

/* ---------- p2p.c's and p2p_lobby.c's side (under p2p_lock) */

void p2p_delta_browse(int on)
{
	pthread_mutex_lock(&delta.lock);
	delta.browse = on ? 1 : 0;
	delta.refresh = 0;
	pthread_mutex_unlock(&delta.lock);
}

void p2p_delta_refresh(void)
{
	pthread_mutex_lock(&delta.lock);
	delta.refresh = 1;
	pthread_mutex_unlock(&delta.lock);
}

void p2p_delta_update(void)
{
	pthread_t thread;
	int wanted;

	if (delta.started)
		return;
	pthread_mutex_lock(&delta.lock);
	wanted = delta.browse || (config_boolean("network.delta_announce") && p2p_lobby_listed());
	if (wanted && !delta.quitting && pthread_create(&thread, NULL, delta_thread, NULL) == 0)
	{
		pthread_detach(thread);
		delta.started = 1;
	}
	pthread_mutex_unlock(&delta.lock);
}

void p2p_delta_quit(void)
{
	unsigned long start = p2p_now();
	char invite[DELTA_INVITE_DIGITS + 1];

	pthread_mutex_lock(&delta.lock);
	delta.quitting = 1;
	/* (a request under way is let end: its answer would list the game again after the withdrawal) */
	while (delta.busy && !elapsed(start, QUIT_WAIT))
	{
		pthread_mutex_unlock(&delta.lock);
		Sleep(20);
		pthread_mutex_lock(&delta.lock);
	}
	/* (an announcement still under way after the wait is withdrawn as well, though it may land after) */
	invite[0] = 0;
	if (delta.pending[0])
		strcpy(invite, delta.pending);
	else if (!delta.busy && delta.announce.listed)
		strcpy(invite, delta.announce.invite);
	pthread_mutex_unlock(&delta.lock);
	if (invite[0])
		withdraw(invite, QUIT_TIMEOUT);
}
