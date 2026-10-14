/* The Apple host's socket adapter (port/ios/host/posix_net.c) hands the game addresses laid out as
Linux and Winsock have them: a 16-bit family first, where Darwin has a length byte and then a family
byte. Every call that returns an address must convert it: one that does not gives a family of 0x0210,
which xnet.c takes for no IPv4 address, so it rewrites nothing and a datagram from an internet play
peer's stand-in on the loopback reaches the game as 127.0.0.1's, the host's own machine. That was
posix_socket_recvfrom, which reads with recvmsg: a host dropped every joiner 15 seconds into a game. */
#include "posix.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* an IPv4 address as the game's headers have it on every platform */
struct game_address
{
	unsigned short family;
	unsigned short port;
	unsigned int address;
	char zero[8];
};

enum { GAME_AF_INET = 2, GAME_SOCK_STREAM = 1, GAME_SOCK_DGRAM = 2 };

static const unsigned char loopback[4] = { 127, 0, 0, 1 };

static void bind_loopback(int socket, struct game_address *bound)
{
	struct game_address address;
	int length = sizeof(*bound);

	memset(&address, 0, sizeof(address));
	address.family = GAME_AF_INET;
	memcpy(&address.address, loopback, 4);
	assert(posix_socket_bind(socket, &address, sizeof(address)) == 0);
	memset(bound, 0xEE, sizeof(*bound));
	assert(posix_socket_getsockname(socket, bound, &length) == 0);
	assert(length == sizeof(*bound) && bound->family == GAME_AF_INET && bound->port != 0);
	assert(memcmp(&bound->address, loopback, 4) == 0);
}

int main(void)
{
	struct game_address receiver_address, sender_address, from;
	char buffer[16];
	int receiver, sender, listener, client, accepted;
	int length;

	/* a datagram's source address, as the game's recvfrom gets it */
	receiver = posix_socket(GAME_AF_INET, GAME_SOCK_DGRAM, 0);
	sender = posix_socket(GAME_AF_INET, GAME_SOCK_DGRAM, 0);
	assert(receiver >= 0 && sender >= 0);
	bind_loopback(receiver, &receiver_address);
	bind_loopback(sender, &sender_address);
	assert(posix_socket_sendto(sender, "halo", 4, 0, &receiver_address, sizeof(receiver_address)) == 4);
	memset(&from, 0xEE, sizeof(from));
	length = sizeof(from);
	assert(posix_socket_recvfrom(receiver, buffer, sizeof(buffer), 0, &from, &length) == 4);
	assert(memcmp(buffer, "halo", 4) == 0);
	assert(length == sizeof(from));
	assert(from.family == GAME_AF_INET);
	assert(from.port == sender_address.port);
	assert(memcmp(&from.address, loopback, 4) == 0);
	/* (and without an address asked for) */
	assert(posix_socket_sendto(sender, "halo", 4, 0, &receiver_address, sizeof(receiver_address)) == 4);
	assert(posix_socket_recvfrom(receiver, buffer, sizeof(buffer), 0, NULL, NULL) == 4);

	/* a connection's, as accept and getpeername give it */
	listener = posix_socket(GAME_AF_INET, GAME_SOCK_STREAM, 0);
	client = posix_socket(GAME_AF_INET, GAME_SOCK_STREAM, 0);
	assert(listener >= 0 && client >= 0);
	bind_loopback(listener, &receiver_address);
	bind_loopback(client, &sender_address);
	assert(posix_socket_listen(listener, 1) == 0);
	assert(posix_socket_connect(client, &receiver_address, sizeof(receiver_address)) == 0);
	memset(&from, 0xEE, sizeof(from));
	length = sizeof(from);
	accepted = posix_socket_accept(listener, &from, &length);
	assert(accepted >= 0 && from.family == GAME_AF_INET && from.port == sender_address.port);
	memset(&from, 0xEE, sizeof(from));
	length = sizeof(from);
	assert(posix_socket_getpeername(client, &from, &length) == 0);
	assert(from.family == GAME_AF_INET && from.port == receiver_address.port);

	posix_socket_close(accepted);
	posix_socket_close(client);
	posix_socket_close(listener);
	posix_socket_close(sender);
	posix_socket_close(receiver);
	puts("posix net probe: ok");
	return 0;
}
