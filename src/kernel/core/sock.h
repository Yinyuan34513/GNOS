/*
 * sock.h — BSD sockets on top of net.c and tcp.c. (GPLv2)
 *
 * This is the layer that turns "a TCP control block" or "a UDP port" into
 * the thing a process actually holds: an object with a peer, a receive
 * queue, and blocking semantics.  It exists because three different wire
 * behaviours share one user-visible abstraction:
 *
 *   - SOCK_STREAM   a tcp_pcb_t; reads and writes are byte streams, and the
 *                   hard questions (ordering, retransmission) are tcp.c's.
 *   - SOCK_DGRAM    a bound UDP port plus a queue of whole datagrams; each
 *                   recvfrom() returns exactly one, because datagram
 *                   boundaries are the entire point of UDP.
 *   - SOCK_RAW      everything arriving for one IP protocol, starting at the
 *                   IP header -- the view BusyBox's ping was written against.
 *
 * The interface below is deliberately not the syscall interface: it deals in
 * host-order addresses and kernel buffers, and the syscall layer (which knows
 * about user pointers and struct sockaddr) is a thin translation on top.
 * Socket indices, not pointers, cross that boundary, so a stale fd can never
 * name a recycled object the type system would have believed.
 */
#ifndef GNOS_SOCK_H
#define GNOS_SOCK_H

#include <stdint.h>

/* Address families, types and protocols: the Linux numbers, because musl
 * passes them through verbatim and BusyBox switches on them. */
#define AF_INET        2
#define AF_UNIX        1

#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3

/* Type may arrive OR-ed with these two (musl's resolver opens its DNS socket
 * as SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK); they are flags, not types. */
#define SOCK_NONBLOCK  0x800
#define SOCK_CLOEXEC   0x80000

#define IPPROTO_ICMP   1
#define IPPROTO_TCP    6
#define IPPROTO_UDP    17

#define SOL_SOCKET     1

/* Socket options.  Only SO_BROADCAST and SO_ERROR change observable
 * behaviour here; the rest are accepted and remembered (or silently
 * accepted) because refusing them makes BusyBox's ping fail its setup. */
#define SO_BROADCAST   6
#define SO_ERROR       4
#define SO_RCVBUF      8
#define SO_SNDBUF      7
#define SO_REUSEADDR   2
#define SO_TYPE        3
#define IP_TTL         2            /* level IPPROTO_IP == 0 */

#define SHUT_RD        0
#define SHUT_WR        1
#define SHUT_RDWR      2

#define MSG_PEEK       2
#define MSG_DONTWAIT   0x40

void sock_init(void);

/* Create a socket; returns its index or a negative errno. */
int  sock_create(int domain, int type, int protocol);

/* All of these take the index sock_create() returned.  Addresses are host
 * byte order; port 0 / IP_ANY mean "unspecified". */
int  sock_bind(int s, uint32_t ip, uint16_t port);
int  sock_connect(int s, uint32_t ip, uint16_t port);   /* blocks to completion */
int  sock_listen(int s, int backlog);
int  sock_accept(int s, uint32_t *rip, uint16_t *rport); /* new index or -errno */

int  sock_sendto(int s, const void *buf, uint32_t len, int flags,
                 uint32_t ip, uint16_t port);
int  sock_recvfrom(int s, void *buf, uint32_t len, int flags,
                   uint32_t *ip, uint16_t *port);

int  sock_shutdown(int s, int how);

/* peer == 0: getsockname, else getpeername. */
int  sock_getname(int s, uint32_t *ip, uint16_t *port, int peer);

int  sock_setsockopt(int s, int level, int name, const void *val, uint32_t len);
int  sock_getsockopt(int s, int level, int name, void *val, uint32_t *len);

void sock_close(int s);

/*
 * O_NONBLOCK after the fact.  The common shape in musl and BusyBox is
 * socket() followed by fcntl(F_SETFL, O_NONBLOCK) rather than
 * SOCK_NONBLOCK at creation, so a socket that could only be non-blocking at
 * birth would go on blocking in exactly the programs that took the trouble
 * to ask for the opposite.
 */
int  sock_set_nonblock(int s, int on);
int  sock_is_nonblock(int s);

/* Readiness, for poll/ppoll: readable means a read would not block (data,
 * a pending accept, EOF or an error all count -- a poll that never fires on
 * EOF is a poll that spins forever). */
int  sock_readable(int s);
int  sock_writable(int s);

/* The vfs read/write entry points (read() on a socket is recvfrom without
 * an address, write() is sendto on the connected peer).  Exposed as a node
 * ops table so vfs.c can give socket handles the same treatment as pipes. */
struct vfs_node;
int32_t sock_node_read(struct vfs_node *n, uint64_t off, void *buf, uint32_t len);
int32_t sock_node_write(struct vfs_node *n, uint64_t off, const void *buf,
                        uint32_t len);

/* The upcalls net.c dispatches to, documented in net.h. */
void udp_input(uint32_t src, uint32_t dst, const uint8_t *seg, uint16_t len);
void raw_input(uint32_t src, uint32_t dst, uint8_t proto,
               const uint8_t *packet, uint16_t total_len);

#endif
