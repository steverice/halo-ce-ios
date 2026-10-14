/* The game's sockets opened again after the app was suspended
(host_socket_revival.h). */

#include "host_socket_revival.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

/* (host.h's, declared here so that the probe builds this file alone) */
void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
enum { HOST_LOG_INFO = 4, HOST_LOG_WARN = 5 };

enum
{
	/* the descriptors kept (the game's are few and low) */
	MAXIMUM_DESCRIPTORS = 1024,
	MAXIMUM_OPTIONS = 8,
	MAXIMUM_OPTION_SIZE = 16,
	/* Darwin's own answer to whether a socket was taken back
	(getsockopt's SO_ISDEFUNCT, which the SDK headers leave out) */
	SOCKET_IS_DEFUNCT = 0x1101,
};

struct socket_option
{
	int level, name;
	unsigned char value[MAXIMUM_OPTION_SIZE];
	socklen_t length;
};

static struct
{
	unsigned char used, bound, connected, listening;
	int family, type, protocol, backlog;
	struct sockaddr_storage bound_address, connected_address;
	socklen_t bound_length, connected_length;
	struct socket_option options[MAXIMUM_OPTIONS];
	int option_count;
} sockets[MAXIMUM_DESCRIPTORS];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int known(int fd)
{
	return fd >= 0 && fd < MAXIMUM_DESCRIPTORS && sockets[fd].used;
}

void host_socket_opened(int fd, int family, int type, int protocol)
{
	if (fd < 0 || fd >= MAXIMUM_DESCRIPTORS)
		return;
	pthread_mutex_lock(&lock);
	memset(&sockets[fd], 0, sizeof(sockets[fd]));
	sockets[fd].used = 1;
	sockets[fd].family = family;
	sockets[fd].type = type;
	sockets[fd].protocol = protocol;
	pthread_mutex_unlock(&lock);
}

void host_socket_bound(int fd)
{
	struct sockaddr_storage address;
	socklen_t length = sizeof(address);

	pthread_mutex_lock(&lock);
	/* (the address the system gave it: a port chosen for port 0, or by a
	send, is the one to take again) */
	if (known(fd) && !sockets[fd].bound && getsockname(fd, (struct sockaddr *)&address, &length) == 0)
	{
		sockets[fd].bound_address = address;
		sockets[fd].bound_length = length;
		sockets[fd].bound = 1;
	}
	pthread_mutex_unlock(&lock);
}

void host_socket_connected(int fd, const struct sockaddr *address, socklen_t length)
{
	pthread_mutex_lock(&lock);
	if (known(fd) && length <= sizeof(sockets[fd].connected_address))
	{
		memcpy(&sockets[fd].connected_address, address, length);
		sockets[fd].connected_length = length;
		sockets[fd].connected = 1;
	}
	pthread_mutex_unlock(&lock);
	host_socket_bound(fd);
}

void host_socket_listening(int fd, int backlog)
{
	pthread_mutex_lock(&lock);
	if (known(fd))
	{
		sockets[fd].listening = 1;
		sockets[fd].backlog = backlog;
	}
	pthread_mutex_unlock(&lock);
}

void host_socket_option(int fd, int level, int name, const void *value, socklen_t length)
{
	int index;

	if (length > MAXIMUM_OPTION_SIZE)
		return;
	pthread_mutex_lock(&lock);
	if (known(fd))
	{
		/* (set again: the latest value) */
		for (index = 0; index < sockets[fd].option_count; index++)
		{
			if (sockets[fd].options[index].level == level && sockets[fd].options[index].name == name)
				break;
		}
		if (index < MAXIMUM_OPTIONS)
		{
			sockets[fd].options[index].level = level;
			sockets[fd].options[index].name = name;
			memcpy(sockets[fd].options[index].value, value, length);
			sockets[fd].options[index].length = length;
			if (index == sockets[fd].option_count)
				sockets[fd].option_count++;
		}
	}
	pthread_mutex_unlock(&lock);
}

void host_socket_closed(int fd)
{
	pthread_mutex_lock(&lock);
	if (known(fd))
		sockets[fd].used = 0;
	pthread_mutex_unlock(&lock);
}

int host_socket_is_defunct(int fd)
{
	int defunct = 0;
	socklen_t length = sizeof(defunct);
	char byte;
	int type = 0;
	socklen_t type_length = sizeof(type);

	if (getsockopt(fd, SOL_SOCKET, SOCKET_IS_DEFUNCT, &defunct, &length) == 0 && defunct)
		return 1;
	/* (a listening socket has nothing to receive: only the system says) */
	if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_length) == 0 && type == SOCK_STREAM)
		return 0;
	if (recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) >= 0)
		return 0;
	return errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
}

static unsigned short port_of(const struct sockaddr_storage *address)
{
	if (address->ss_family == AF_INET)
		return ntohs(((const struct sockaddr_in *)address)->sin_port);
	if (address->ss_family == AF_INET6)
		return ntohs(((const struct sockaddr_in6 *)address)->sin6_port);
	return 0;
}

/* one socket made again under its descriptor: 1 on the same port, 2 on
another (it was free no longer), 0 if it could not be */
static int revive(int fd)
{
	int flags = fcntl(fd, F_GETFL);
	int descriptor_flags = fcntl(fd, F_GETFD);
	int fresh = socket(sockets[fd].family, sockets[fd].type, sockets[fd].protocol);
	int one = 1;
	int index;
	int result = 1;

	if (fresh < 0)
		return 0;
	/* (the descriptor is never free in between: another thread opening a
	file could take it otherwise) */
	if (dup2(fresh, fd) < 0)
	{
		close(fresh);
		return 0;
	}
	close(fresh);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags);
	if (descriptor_flags >= 0)
		fcntl(fd, F_SETFD, descriptor_flags);
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	for (index = 0; index < sockets[fd].option_count; index++)
	{
		struct socket_option const *option = &sockets[fd].options[index];

		setsockopt(fd, option->level, option->name, option->value, option->length);
	}
	if (sockets[fd].bound &&
		bind(fd, (const struct sockaddr *)&sockets[fd].bound_address, sockets[fd].bound_length) < 0)
	{
		struct sockaddr_storage any = sockets[fd].bound_address;
		socklen_t length = sizeof(any);

		if (any.ss_family == AF_INET)
			((struct sockaddr_in *)&any)->sin_port = 0;
		else if (any.ss_family == AF_INET6)
			((struct sockaddr_in6 *)&any)->sin6_port = 0;
		if (bind(fd, (const struct sockaddr *)&any, sockets[fd].bound_length) < 0)
			return 0;
		getsockname(fd, (struct sockaddr *)&sockets[fd].bound_address, &length);
		result = 2;
	}
	if (sockets[fd].listening && listen(fd, sockets[fd].backlog) < 0)
		return 0;
	if (sockets[fd].connected && sockets[fd].type == SOCK_DGRAM)
		connect(fd, (const struct sockaddr *)&sockets[fd].connected_address, sockets[fd].connected_length);
	return result;
}

int host_socket_revive(int force)
{
	int count = 0, failed = 0, lost = 0;
	int fd;

	unsigned char defunct[MAXIMUM_DESCRIPTORS] = { 0 };
	int any = 0;

	pthread_mutex_lock(&lock);
	for (fd = 0; fd < MAXIMUM_DESCRIPTORS; fd++)
	{
		if (sockets[fd].used && (force || host_socket_is_defunct(fd)))
			any = defunct[fd] = 1;
	}
	for (fd = 0; fd < MAXIMUM_DESCRIPTORS; fd++)
	{
		unsigned short port;
		int result;

		/* (a listening socket shows nothing to a receive: it is made again
		with the others when any was taken back) */
		if (!sockets[fd].used || !(defunct[fd] || (any && sockets[fd].listening)))
			continue;
		port = sockets[fd].bound ? port_of(&sockets[fd].bound_address) : 0;
		/* (a TCP connection taken back is gone: its owner sees it fail; a
		test's, force 2, is broken as the system's would be) */
		if (sockets[fd].type == SOCK_STREAM && !sockets[fd].listening)
		{
			if (force == 2)
				shutdown(fd, SHUT_RDWR);
			lost++;
			continue;
		}
		result = revive(fd);
		if (!result)
			failed++;
		else
			count++;
		host_logf(result ? HOST_LOG_INFO : HOST_LOG_WARN, "network: socket %d (%s%s, port %u) %s", fd,
			sockets[fd].type == SOCK_STREAM ? "TCP" : "UDP", sockets[fd].listening ? ", listening" : "", port,
			result == 1 ? "opened again on the same port" : result == 2 ?
			"opened again on another port (its own was taken)" : "could not be opened again");
	}
	pthread_mutex_unlock(&lock);
	if (!force && !any)
	{
		int open = 0;

		for (fd = 0; fd < MAXIMUM_DESCRIPTORS; fd++)
			open += sockets[fd].used;
		if (open)
			host_logf(HOST_LOG_INFO, "network: none of the game's %d sockets was taken back", open);
	}
	if (count || failed || lost)
	{
		host_logf(HOST_LOG_INFO, "network: %d of the game's sockets were taken back while the app was suspended "
			"(opened again: %d; could not be: %d; connections lost: %d)", count + failed + lost, count, failed, lost);
	}
	return count;
}
