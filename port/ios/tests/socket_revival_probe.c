/* The game's sockets made again after a suspension
(port/ios/host/host_socket_revival.c), on a Mac, where nothing is taken
back, so with force: a UDP socket keeps its descriptor, its port, its
options and its non-blocking mode and takes datagrams again; a listening TCP
socket keeps its port and takes connections; a TCP connection is left alone;
a socket closed is forgotten. Run by tools/ios_test.py. */
#include "host_socket_revival.h"

#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int logged;

void host_logf(int priority, const char *format, ...)
{
	va_list arguments;

	(void)priority;
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	putchar('\n');
	logged++;
}

static unsigned short port_of(int fd)
{
	struct sockaddr_in address;
	socklen_t length = sizeof(address);

	assert(getsockname(fd, (struct sockaddr *)&address, &length) == 0);
	return ntohs(address.sin_port);
}

static void loopback(struct sockaddr_in *address, unsigned short port)
{
	memset(address, 0, sizeof(*address));
	address->sin_len = sizeof(*address);
	address->sin_family = AF_INET;
	address->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address->sin_port = htons(port);
}

int main(void)
{
	struct sockaddr_in address;
	int udp = socket(AF_INET, SOCK_DGRAM, 0);
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	int closed = socket(AF_INET, SOCK_DGRAM, 0);
	int one = 1, value = 0;
	socklen_t length = sizeof(value);
	unsigned short udp_port, listener_port;
	char text[8] = { 0 };

	/* a bound UDP socket with an option and non-blocking, as xnet.c's */
	host_socket_opened(udp, AF_INET, SOCK_DGRAM, 0);
	loopback(&address, 0);
	assert(bind(udp, (struct sockaddr *)&address, sizeof(address)) == 0);
	host_socket_bound(udp);
	assert(setsockopt(udp, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) == 0);
	host_socket_option(udp, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	fcntl(udp, F_SETFL, fcntl(udp, F_GETFL) | O_NONBLOCK);
	udp_port = port_of(udp);
	assert(!host_socket_is_defunct(udp));

	/* a listening TCP socket */
	host_socket_opened(listener, AF_INET, SOCK_STREAM, 0);
	assert(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
	host_socket_option(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	loopback(&address, 0);
	assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
	host_socket_bound(listener);
	assert(listen(listener, 4) == 0);
	host_socket_listening(listener, 4);
	listener_port = port_of(listener);

	/* one closed is not made again */
	host_socket_opened(closed, AF_INET, SOCK_DGRAM, 0);
	host_socket_closed(closed);
	close(closed);

	/* nothing taken back: nothing made again (and that said, once) */
	assert(host_socket_revive(0) == 0 && logged == 1);
	logged = 0;
	/* with force: both, under their descriptors, on their ports */
	assert(host_socket_revive(1) == 2);
	assert(port_of(udp) == udp_port && port_of(listener) == listener_port);
	assert(getsockopt(udp, SOL_SOCKET, SO_BROADCAST, &value, &length) == 0 && value);
	assert(fcntl(udp, F_GETFL) & O_NONBLOCK);
	{
		int sender = socket(AF_INET, SOCK_DGRAM, 0);
		int tries;

		loopback(&address, udp_port);
		assert(sendto(sender, "ping", 4, 0, (struct sockaddr *)&address, sizeof(address)) == 4);
		for (tries = 0; tries < 100 && recv(udp, text, sizeof(text) - 1, 0) < 0; tries++)
			usleep(10000);
		assert(!strcmp(text, "ping"));
		close(sender);
	}
	{
		int client = socket(AF_INET, SOCK_STREAM, 0);
		int accepted;

		loopback(&address, listener_port);
		assert(connect(client, (struct sockaddr *)&address, sizeof(address)) == 0);
		accepted = accept(listener, NULL, NULL);
		assert(accepted >= 0);
		/* a connection is not made again: it is counted as gone */
		host_socket_opened(accepted, AF_INET, SOCK_STREAM, 0);
		logged = 0;
		assert(host_socket_revive(1) == 2 && logged == 3);
		close(accepted);
		close(client);
	}
	puts("PASS: sockets made again: descriptor, port, options, non-blocking; listening; connections left");
	return 0;
}
