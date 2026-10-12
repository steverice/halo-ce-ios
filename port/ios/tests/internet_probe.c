/* internet play's decisions the menus show (halo_internet.h): whether the
signalling brokers can be reached, and the server browser's line under an
empty list. */
#include "halo_internet.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
	/* the brokers: none contacted until something needs them */
	assert(halo_internet_servers(0, 4, 0, 0, 0) == HALO_INTERNET_SERVERS_NONE);
	assert(halo_internet_servers(0, 4, 4, 0, 60000) == HALO_INTERNET_SERVERS_NONE);
	/* contacted, connecting: trying, until the wait is over */
	assert(halo_internet_servers(1, 4, 0, 0, 0) == HALO_INTERNET_SERVERS_TRYING);
	assert(halo_internet_servers(1, 4, 3, 0, HALO_INTERNET_SERVERS_WAIT - 1) == HALO_INTERNET_SERVERS_TRYING);
	assert(halo_internet_servers(1, 4, 0, 0, HALO_INTERNET_SERVERS_WAIT) == HALO_INTERNET_SERVERS_UNREACHABLE);
	/* every one failed: said at once, without the wait */
	assert(halo_internet_servers(1, 4, 4, 0, 100) == HALO_INTERNET_SERVERS_UNREACHABLE);
	/* an empty brokers file: nothing to reach */
	assert(halo_internet_servers(1, 0, 0, 0, 0) == HALO_INTERNET_SERVERS_UNREACHABLE);
	/* one working is enough, whatever the others do and however long it took */
	assert(halo_internet_servers(1, 4, 3, 1, 0) == HALO_INTERNET_SERVERS_REACHED);
	assert(halo_internet_servers(1, 4, 3, 1, 600000) == HALO_INTERNET_SERVERS_REACHED);
	/* reached, then lost: trying again from when it was last reached */
	assert(halo_internet_servers(1, 1, 0, 0, 500) == HALO_INTERNET_SERVERS_TRYING);
	assert(halo_internet_servers(1, 1, 1, 0, 500) == HALO_INTERNET_SERVERS_UNREACHABLE);

	/* the server browser's empty list: unreachable brokers are said at
	once, since no listing can come */
	assert(halo_internet_browser_empty(HALO_INTERNET_SERVERS_UNREACHABLE, 1, 0, 0) == HALO_INTERNET_BROWSER_NO_SERVERS);
	assert(halo_internet_browser_empty(HALO_INTERNET_SERVERS_UNREACHABLE, 0, 5, 5) == HALO_INTERNET_BROWSER_NO_SERVERS);
	/* still looking: nothing is concluded, whatever was heard so far */
	assert(halo_internet_browser_empty(HALO_INTERNET_SERVERS_TRYING, 1, 0, 0) == HALO_INTERNET_BROWSER_LOOKING);
	assert(halo_internet_browser_empty(HALO_INTERNET_SERVERS_REACHED, 1, 9, 0) == HALO_INTERNET_BROWSER_LOOKING);
	assert(halo_internet_browser_empty(HALO_INTERNET_SERVERS_NONE, 1, 0, 0) == HALO_INTERNET_BROWSER_LOOKING);
	/* looked: games of a newer version mean this build is old, and say so
	before older ones do */
	assert(halo_internet_browser_empty(HALO_INTERNET_SERVERS_REACHED, 0, 27, 14) == HALO_INTERNET_BROWSER_NEWER_ONLY);
	assert(halo_internet_browser_empty(HALO_INTERNET_SERVERS_REACHED, 0, 0, 14) == HALO_INTERNET_BROWSER_OLDER_ONLY);
	assert(halo_internet_browser_empty(HALO_INTERNET_SERVERS_REACHED, 0, 0, 0) == HALO_INTERNET_BROWSER_NONE_FOUND);
	/* (brokers slow to connect, but not given up on: nothing found yet) */
	assert(halo_internet_browser_empty(HALO_INTERNET_SERVERS_TRYING, 0, 0, 0) == HALO_INTERNET_BROWSER_NONE_FOUND);

	puts("internet probe: ok");
	return 0;
}
