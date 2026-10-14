/* Adapt Linux socket helpers to Darwin's sockaddr length byte and socket flags. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "host_socket_revival.h"

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0x80000
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
/* (an address is Linux's when the guest made it: a 16-bit family, whose second byte is 0 for every
family the game uses. One posix_net.c made itself is Darwin's already, a length byte and then the
family: posix_local_ipv4_address's route probe. Read as Linux's, its family came out 0, the probe's
connect did nothing, and the address was always the first interface's, not the default route's) */
static struct sockaddr_storage sockaddr_inward(const void *input,socklen_t size) {
    struct sockaddr_storage address={0};
    if(size>sizeof(address))size=sizeof(address);
    if(input && size>=2){
        const unsigned char *bytes=input;
        memcpy(&address,input,size);address.ss_len=size;address.ss_family=bytes[1]?bytes[1]:bytes[0];
    }
    return address;
}
static void sockaddr_outward(void *address,socklen_t size) {
    if(address && size>=2){uint16_t family=((struct sockaddr *)address)->sa_family;memcpy(address,&family,2);}
}
/* (each socket's making kept, to make it again after the app was suspended: host_socket_revival.c) */
static int ios_socket(int family,int type,int protocol) {
    int fd=socket(family,type&~SOCK_CLOEXEC,protocol);
    if(fd>=0){int one=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));fcntl(fd,F_SETFD,FD_CLOEXEC);
        host_socket_opened(fd,family,type&~SOCK_CLOEXEC,protocol);}
    return fd;
}
static int ios_bind(int fd,const void *p,socklen_t n) {
    struct sockaddr_storage a=sockaddr_inward(p,n);int r=bind(fd,(struct sockaddr *)&a,n);if(!r)host_socket_bound(fd);return r;
}
static int ios_connect(int fd,const void *p,socklen_t n) {
    struct sockaddr_storage a=sockaddr_inward(p,n);int r=connect(fd,(struct sockaddr *)&a,n);
    if(!r || errno==EINPROGRESS)host_socket_connected(fd,(struct sockaddr *)&a,n);
    return r;
}
static int ios_listen(int fd,int backlog) {int r=listen(fd,backlog);if(!r)host_socket_listening(fd,backlog);return r;}
static int ios_setsockopt(int fd,int level,int name,const void *value,socklen_t length) {
    int r=setsockopt(fd,level,name,value,length);if(!r)host_socket_option(fd,level,name,value,length);return r;
}
static int ios_close(int fd) {host_socket_closed(fd);return close(fd);}
static int ios_accept4(int fd,void *p,socklen_t *n,int flags) {
    (void)flags;int out=accept(fd,p,n);if(out>=0){fcntl(out,F_SETFD,FD_CLOEXEC);if(n)sockaddr_outward(p,*n);}return out;
}
static ssize_t ios_sendto(int fd,const void *p,size_t n,int flags,const void *address,socklen_t size) {
    struct sockaddr_storage a=sockaddr_inward(address,size);ssize_t r=sendto(fd,p,n,flags,(struct sockaddr *)&a,size);
    /* (a first send gives an unbound socket its port: kept, as a bind's is) */
    host_socket_bound(fd);
    return r;
}
static ssize_t ios_recvfrom(int fd,void *p,size_t n,int flags,void *address,socklen_t *size) {
    ssize_t out=recvfrom(fd,p,n,flags,address,size);if(out>=0 && size)sockaddr_outward(address,*size);return out;
}
/* (posix_socket_recvfrom reads with recvmsg, for MSG_TRUNC: its source address is Darwin's too.
Left as it came, its family read 0x0210, xnet.c took it for no IPv4 address and rewrote nothing, and
a datagram from an internet play peer's stand-in reached the game as 127.0.0.1's, the host's own) */
static ssize_t ios_recvmsg(int fd,struct msghdr *message,int flags) {
    ssize_t out=recvmsg(fd,message,flags);
    if(out>=0 && message && message->msg_name)sockaddr_outward(message->msg_name,message->msg_namelen);
    return out;
}
static int ios_getsockname(int fd,void *p,socklen_t *n) {int r=getsockname(fd,p,n);if(!r)sockaddr_outward(p,*n);return r;}
static int ios_getpeername(int fd,void *p,socklen_t *n) {int r=getpeername(fd,p,n);if(!r)sockaddr_outward(p,*n);return r;}
static ssize_t ios_getrandom(void *p,size_t size,unsigned flags) {(void)flags;arc4random_buf(p,size);return size;}
#define socket ios_socket
#define listen ios_listen
#define setsockopt ios_setsockopt
#define close ios_close
#define bind ios_bind
#define connect ios_connect
#define accept4 ios_accept4
#define sendto ios_sendto
#define recvfrom ios_recvfrom
#define recvmsg ios_recvmsg
#define getsockname ios_getsockname
#define getpeername ios_getpeername
#define getrandom ios_getrandom
#include "../../linux/src/posix_net.c"

/* ---------- a router's forwarding: NAT-PMP or PCP (host_natpmp.c). UPnP
(posix_upnp.c, with miniupnpc) finds its router by multicast, which an Apple
device may send only with an entitlement Apple grants on request. p2p.c calls
these from one thread at a time; ports are in network byte order here */
#include "host_natpmp.h"

static int forward_method = POSIX_UPNP_NAT_PMP, forward_renew_seconds;
/* the internal port forwarded (a removal names it), 0 if none */
static unsigned short forwarded_port;

int posix_upnp_forward_udp(unsigned short port, unsigned short preferred_port, posix_ulong *external_address,
	unsigned short *external_port, char *error, int error_size)
{
	uint32_t address = 0, lifetime = 0;
	uint16_t mapped = 0;
	int method = 0;
	char why[160] = "";

	/* (the tunnel's socket on another port since: its old forwarding goes) */
	if (forwarded_port && forwarded_port != port)
	{
		natpmp_unmap_udp(ntohs(forwarded_port), 0);
		forwarded_port = 0;
	}
	if (!natpmp_map_udp(ntohs(port), ntohs(preferred_port), &address, &mapped, &lifetime, &method, why,
		sizeof(why)))
	{
		forward_method = method == 2 ? POSIX_UPNP_PCP : method == 3 ? POSIX_UPNP_NAT_PMP_OR_PCP : POSIX_UPNP_NAT_PMP;
		if (error && error_size > 0)
			snprintf(error, (size_t)error_size, "%s", why);
		return 0;
	}
	forward_method = method == 2 ? POSIX_UPNP_PCP : POSIX_UPNP_NAT_PMP;
	/* (renewed after half of the lifetime the router gave, when that is
	shorter than p2p.c's half hour) */
	forward_renew_seconds = lifetime && lifetime < 3600 ? (int)(lifetime / 2) : 0;
	forwarded_port = port;
	*external_address = address;
	*external_port = htons(mapped);
	return 1;
}

void posix_upnp_stop_forwarding_udp(unsigned short external_port)
{
	(void)external_port;
	if (!forwarded_port)
		return;
	natpmp_unmap_udp(ntohs(forwarded_port), 0);
	forwarded_port = 0;
}

/* ---------- the app in the background (host_lifecycle.m) */

int host_lifecycle_in_background(void);
unsigned long long host_lifecycle_background_milliseconds(void);

int posix_app_in_background(void)
{
	return host_lifecycle_in_background();
}

posix_ulong posix_background_milliseconds(void)
{
	return (posix_ulong)host_lifecycle_background_milliseconds();
}

int posix_upnp_method(void)
{
	return forward_method;
}

int posix_upnp_renew_seconds(void)
{
	return forward_renew_seconds;
}
