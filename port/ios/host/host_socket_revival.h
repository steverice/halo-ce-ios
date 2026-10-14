/* The game's sockets opened again after the app was suspended.

iOS and tvOS take back a suspended app's sockets ("defunct" them): every
later send or receive on one fails, though its descriptor stays open. The
game's sockets (xnet.c's, through posix_net.c) and internet play's
(p2p.c's tunnel and stand-ins) are made by the guest, which holds their
descriptors. So the host keeps what each was made with (its kind, the
address it is bound to, whether it listens or is connected, the options set
on it) and, when the app comes back to the foreground, puts a new socket made
the same way in place of each one taken back, under the same descriptor
(dup2), bound to the same port where that is free: the guest goes on with
the descriptors it has. A connected TCP socket's connection is gone and is
not made again; its owner sees it fail as a dropped connection.

posix_net.c (the Apple host's) tells this file about each socket;
host_lifecycle.m calls host_socket_revive on entering the foreground.
port/ios/tests/socket_revival_probe.c checks it on a Mac, where nothing is
taken back, with force. */

#ifndef HOST_SOCKET_REVIVAL_H
#define HOST_SOCKET_REVIVAL_H

#include <sys/socket.h>

void host_socket_opened(int fd, int family, int type, int protocol);
/* after a bind, or a send that gave the socket a port */
void host_socket_bound(int fd);
void host_socket_connected(int fd, const struct sockaddr *address, socklen_t length);
void host_socket_listening(int fd, int backlog);
void host_socket_option(int fd, int level, int name, const void *value, socklen_t length);
void host_socket_closed(int fd);

/* whether a socket was taken back: a receive that fails otherwise than
"nothing to read" (or the system's own word, where it answers) */
int host_socket_is_defunct(int fd);

/* each socket taken back (all of them with force; force 2 also breaks
every TCP connection, as the system's taking back does, for a test) made
again, and the
listening ones with them when any was (a listening socket shows nothing to
the receive that tells); returns how many were, and logs each */
int host_socket_revive(int force);

#endif
