/*
P2P_MASTER.C

The official master's authenticated browser snapshot. Only the master is contacted:
listed hosts are reached by their existing invites, never probed while browsing.
The worker owns its cache and request clock; p2p_lock is held only for handoff.
*/
#include "platform.h"
#include "posix.h"
#include "port_config.h"
#include "p2p_internal.h"
#include "master_list.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* milliseconds: an open browser asks again this often, so its rows outlive the
lobby's five-minute expiry and their counts stay current (one request a minute) */
enum { MASTER_MINIMUM = 5000, MASTER_PERIOD = 60000, MASTER_TIMEOUT = 10000 };

static struct
{
	pthread_mutex_t lock;
	int started, quitting, browse, refresh;
	unsigned generation;
	/* Request/cache fields belong to the worker alone. These survive closing
	the browser, so reopening cannot bypass throttles or Retry-After. */
	int asked, cached_size;
	unsigned long asked_time, retry_time;
	unsigned long long retry_wait;
	char etag[128], signature[128], signature_key[32];
} master = { PTHREAD_MUTEX_INITIALIZER };

static char master_answer[MASTER_MAXIMUM_ANSWER + 1];
static char master_cached[MASTER_MAXIMUM_ANSWER + 1];
static struct master_game master_entries[MASTER_MAXIMUM_GAMES];
static struct p2p_listing master_shown[MASTER_MAXIMUM_GAMES];

static int master_allowed(void)
{
	return config_boolean("debug.master_list") && config_boolean("network.online") &&
		config_boolean("network.public_lobby");
}

/* Retry-After may be a delay or an HTTP date. A malformed value gets a
conservative minute; long delays remain intact across monotonic clock wraps. */
static unsigned long long master_retry_delay(const char *text)
{
	unsigned long long seconds = 0;
	const char *p = text;
	int day, year, hour, minute, second, month;
	char weekday[4] = {0}, month_name[4] = {0}, zone[4] = {0}, extra;
	static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

	if (*p)
	{
		for (; *p >= '0' && *p <= '9'; p++)
		{
			if (seconds > (ULLONG_MAX / 1000 - 9) / 10)
				return ULLONG_MAX;
			seconds = seconds * 10 + (unsigned)(*p - '0');
		}
		if (!*p)
			return seconds < 5 ? MASTER_MINIMUM : seconds * 1000;
	}
	if (sscanf(text, "%3[A-Za-z], %2d %3s %4d %2d:%2d:%2d %3s %c", weekday, &day, month_name, &year,
		&hour, &minute, &second, zone, &extra) == 8 && strlen(weekday) == 3 && !strcmp(zone, "GMT") &&
		(strstr("Sun Mon Tue Wed Thu Fri Sat", weekday) != NULL))
	{
		/* RFC 9110's IMF-fixdate starts with a comma after the weekday. */
		for (month = 0; month < 12 && strcmp(month_name, months[month]); month++)
			;
		if (month < 12 && year >= 1970 && year <= 9999 && day >= 1 && day <= 31 &&
			hour >= 0 && hour < 24 && minute >= 0 && minute < 60 && second >= 0 && second < 60)
		{
			long long days = 0, until;
			int y, m;
			static const int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
			for (y = 1970; y < year; y++)
				days += 365 + (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
			for (m = 0; m < month; m++)
				days += lengths[m] + (m == 1 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
			until = (days + day - 1) * 86400 + hour * 3600 + minute * 60 + second - (long long)time(NULL);
			if (until < 5) return MASTER_MINIMUM;
			return (unsigned long long)until * 1000;
		}
	}
	return 60000;
}

/* One pass is separate from sleeping so the scheduling/cache contract can
be exercised with a deterministic clock and transport in the worker probe. */
static void master_pass(void)
{
	struct posix_https_headers headers;
	char error[192] = {0};
	unsigned generation;
	unsigned long now = p2p_now();
	int status, count, shown = 0, index;

	if (master.retry_wait)
	{
		unsigned int passed = (unsigned int)(now - master.retry_time);
		master.retry_wait = passed >= master.retry_wait ? 0 : master.retry_wait - passed;
		master.retry_time = now;
	}

	pthread_mutex_lock(&master.lock);
	if (master.browse && master.asked && (unsigned int)(now - master.asked_time) >= MASTER_PERIOD)
		master.refresh = 1;
	if (master.quitting || !master.browse || !master.refresh || !master_allowed() ||
		(master.asked && (unsigned int)(now - master.asked_time) < MASTER_MINIMUM) ||
		master.retry_wait)
	{
		pthread_mutex_unlock(&master.lock);
		return;
	}
	master.refresh = 0;
	generation = master.generation;
	pthread_mutex_unlock(&master.lock);

	master.asked = 1;
	master.asked_time = now;
	master.retry_wait = 0;
	status = posix_https_get_signed("https://master.opencommunityedition.com/v1/servers", master.etag,
		master_answer, sizeof(master_answer), MASTER_TIMEOUT, &headers, error, sizeof(error));
	if (status == 429)
	{
		master.retry_time = p2p_now();
		master.retry_wait = master_retry_delay(headers.retry_after);
		/* A refresh refused by the master remains pending until it permits it. */
		pthread_mutex_lock(&master.lock);
		if (master.browse && master.generation == generation)
			master.refresh = 1;
		pthread_mutex_unlock(&master.lock);
		platform_log("Internet play: OpenCE master rate limited; waiting %llu seconds", master.retry_wait / 1000);
		return;
	}
	if (status == 200)
	{
		count = master_parse_list(master_answer, headers.body_size, headers.signature, headers.signature_key,
			(long long)time(NULL), master_entries, MASTER_MAXIMUM_GAMES, error, sizeof(error));
		if (count >= 0)
		{
			memcpy(master_cached, master_answer, (size_t)headers.body_size);
			master.cached_size = headers.body_size;
			snprintf(master.etag, sizeof(master.etag), "%s", headers.etag);
			snprintf(master.signature, sizeof(master.signature), "%s", headers.signature);
			snprintf(master.signature_key, sizeof(master.signature_key), "%s", headers.signature_key);
		}
	}
	else if (status == 304 && master.cached_size && master.etag[0])
	{
		/* A 304 cannot extend the signed timestamp's five-minute lifetime. */
		count = master_parse_list(master_cached, master.cached_size, master.signature, master.signature_key,
			(long long)time(NULL), master_entries, MASTER_MAXIMUM_GAMES, error, sizeof(error));
		if (count < 0)
		{
			master.etag[0] = 0;
			master.cached_size = 0;
			pthread_mutex_lock(&p2p_lock);
			pthread_mutex_lock(&master.lock);
			if (master.browse && master.generation == generation)
				p2p_lobby_master_games(NULL, 0);
			pthread_mutex_unlock(&master.lock);
			pthread_mutex_unlock(&p2p_lock);
		}
	}
	else
	{
		count = -1;
		if (!error[0]) snprintf(error, sizeof(error), "HTTP status %d", status);
	}
	if (count < 0)
	{
		platform_log("Internet play: OpenCE master list rejected (%s)", error);
		return;
	}
	for (index = 0; index < count; index++)
	{
		if (p2p_lobby_master_read(master_entries[index].listing, master_entries[index].listing_size,
			&master_shown[shown]))
		{
			snprintf(master_shown[shown].region, sizeof(master_shown[shown].region), "%s", master_entries[index].region);
			shown++;
		}
	}
	pthread_mutex_lock(&p2p_lock);
	pthread_mutex_lock(&master.lock);
	if (!master.quitting && master.browse && master.generation == generation && master_allowed())
	{
		p2p_lobby_master_games(master_shown, shown);
		platform_log("Internet play: OpenCE master lists %d verified games (%d joinable network versions)", count, shown);
	}
	pthread_mutex_unlock(&master.lock);
	pthread_mutex_unlock(&p2p_lock);
}

static void *master_thread(void *unused)
{
	(void)unused;
	for (;;)
	{
		int quitting;
		Sleep(250);
		pthread_mutex_lock(&master.lock);
		quitting = master.quitting;
		pthread_mutex_unlock(&master.lock);
		if (quitting) return NULL;
		master_pass();
	}
}

/* Called under p2p_lock. The worker never holds master.lock while taking
p2p_lock; the handoff uses the same lock order as these entry points. */
void p2p_master_browse(int on)
{
	pthread_mutex_lock(&master.lock);
	if (master.browse != !!on)
	{
		master.browse = !!on;
		master.generation++;
		master.refresh = !!on;
	}
	pthread_mutex_unlock(&master.lock);
}

void p2p_master_refresh(void)
{
	pthread_mutex_lock(&master.lock);
	if (master.browse) master.refresh = 1;
	pthread_mutex_unlock(&master.lock);
}

void p2p_master_update(void)
{
	pthread_t thread;
	pthread_mutex_lock(&master.lock);
	if (!master.started && !master.quitting && master.browse && master_allowed() &&
		pthread_create(&thread, NULL, master_thread, NULL) == 0)
	{
		pthread_detach(thread);
		master.started = 1;
	}
	pthread_mutex_unlock(&master.lock);
}

void p2p_master_quit(void)
{
	pthread_mutex_lock(&master.lock);
	master.quitting = 1;
	pthread_mutex_unlock(&master.lock);
}
