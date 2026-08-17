/*
 * net.h — the link and internet layers: ethernet, ARP, IPv4, ICMP. (GPLv2)
 *
 * e1000.c moves frames.  This file decides what they mean.  The split is the
 * usual one and it is worth being explicit about where the seam is: the
 * driver knows about descriptors and DMA and nothing about addresses; this
 * layer knows about addresses and nothing about descriptors.
 *
 * What is here is the minimum that lets a BSD socket work:
 *
 *   ethernet   demultiplex on ethertype, and prepend the 14-byte header on
 *              the way out.
 *   ARP        a fixed-size cache of IP -> MAC.  A send to an address we
 *              have not learned yet parks the datagram in a small pending
 *              queue, fires a request, and delivers it when the reply lands
 *              -- rather than dropping it and hoping TCP notices.
 *   IPv4       header build/parse, checksum, and a routing decision that has
 *              exactly three outcomes: loopback, on-link, or via the gateway.
 *   ICMP       echo reply, so the machine answers ping, and delivery of
 *              everything to raw sockets, so it can *send* one.
 *
 * What is deliberately not here: fragmentation (datagrams larger than the MTU
 * are refused rather than split, and inbound fragments are dropped), options
 * in the IP header (parsed past, never generated), and IPv6.
 *
 * ---- where this code runs -------------------------------------------------
 *
 * net_poll() is this kernel's version of a softirq, except that it is not
 * soft: it is called straight from the e1000 and timer interrupt handlers as
 * well as from socket system calls.  That is safe here for two reasons that
 * will stop being true the moment either changes:
 *
 *   1. Interrupt gates leave IF clear and no handler re-enables it, so an
 *      interrupt cannot nest and cannot pre-empt itself.
 *   2. The kernel is single-CPU and non-preemptible in kernel mode, so the
 *      only way two contexts can touch a socket at once is an interrupt
 *      landing inside a system call.  Syscall-side code that mutates shared
 *      state therefore runs under cli/sti, and net_poll() holds a re-entrancy
 *      flag so an interrupt that arrives mid-poll simply defers its work to
 *      the next tick instead of corrupting the one in progress.
 */
#ifndef GNOS_NET_H
#define GNOS_NET_H

#include <stdint.h>
#include "sysnum.h"           /* sockaddr_in_t for struct ifreq */

#define ETH_ALEN       6
#define ETH_HDR_LEN    14
#define NET_MTU        1500
#define NET_FRAME_MAX  1518        /* MTU + header + 802.1Q room */

#define ETH_P_IP       0x0800
#define ETH_P_ARP      0x0806

#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

/* Addresses are kept in host byte order everywhere above the wire format;
 * conversion happens exactly at the point a header is built or parsed.  Mixed
 * conventions inside a stack are the single most productive source of bugs
 * that only show up on one endianness of test data. */
#define IPV4(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(c) << 8)  |  (uint32_t)(d))

#define IP_ANY        0u
#define IP_BROADCAST  0xFFFFFFFFu
#define IP_LOOPBACK   IPV4(127, 0, 0, 1)

static inline uint16_t net_htons(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

static inline uint32_t net_htonl(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

#define net_ntohs net_htons
#define net_ntohl net_htonl

/* Unaligned big-endian accessors.  Protocol headers are byte streams, not
 * structs: an IP header with options makes every field after it unaligned,
 * and a packed struct would only hide that from the compiler, not the CPU. */
static inline uint16_t net_get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t net_get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static inline void net_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void net_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* An interface.  There are exactly two and they are never created or
 * destroyed, which is why this is a plain struct and not a refcounted
 * object: "lo" is synthetic and "eth0" is the e1000 or nothing. */
typedef struct {
    char     name[8];
    uint8_t  mac[ETH_ALEN];
    uint32_t ip, netmask, gateway;
    int      up;
    int      loopback;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes;
    uint64_t rx_dropped, tx_dropped;
} netif_t;

#define NET_IF_LO   0
#define NET_IF_ETH  1
#define NET_IF_MAX  2

/* Bring the stack up on top of whatever e1000_init() found.  Safe to call
 * when there is no NIC: "lo" still works, which is all the self-test and
 * getaddrinfo's /etc/hosts path need. */
void net_init(void);

netif_t *net_if(int idx);
netif_t *net_if_by_name(const char *name);

/*
 * Network ioctls (SIOCGIF* / SIOCSIF*) that ifconfig(8) and route(8) issue on
 * an AF_INET datagram socket.  The command numbers and the ifreq/ifconf
 * layout match Linux exactly, because BusyBox was built against Linux's
 * <linux/if.h> and will not tolerate a near-miss.
 */
#define SIOCGIFNAME     0x8910
#define SIOCGIFCONF     0x8912
#define SIOCGIFFLAGS    0x8913
#define SIOCSIFFLAGS    0x8914
#define SIOCGIFADDR     0x8915
#define SIOCSIFADDR     0x8916
#define SIOCGIFDSTADDR  0x8917
#define SIOCSIFDSTADDR  0x8918
#define SIOCGIFBRDADDR  0x8919
#define SIOCSIFBRDADDR  0x891a
#define SIOCGIFNETMASK  0x891b
#define SIOCSIFNETMASK  0x891c
#define SIOCGIFMETRIC   0x891d
#define SIOCSIFMETRIC   0x891e
#define SIOCGIFMTU      0x8921
#define SIOCSIFMTU      0x8922
#define SIOCSIFHWADDR   0x8924
#define SIOCGIFHWADDR   0x8927
#define SIOCGIFINDEX    0x8933

#define IFF_UP          0x0001
#define IFF_BROADCAST   0x0002
#define IFF_LOOPBACK    0x0008
#define IFF_RUNNING     0x0040
#define IFF_MULTICAST   0x1000
#define ARPHRD_ETHER    1

#define IFNAMSIZ 16

/* ifreq: a 16-byte name followed by a 16-byte union.  The address fields are
 * sockaddr_in-shaped (family in host order, address in network order); the
 * hardware-address field is raw bytes with ARPHRD in the first two and the MAC
 * in the next six, which is exactly where BusyBox reads sa_data from. */
typedef struct {
    char ifr_name[IFNAMSIZ];
    union {
        sockaddr_in_t addr;
        sockaddr_in_t netmask;
        sockaddr_in_t broadaddr;
        int16_t       flags;
        int32_t       metric;
        int32_t       mtu;
        int32_t       ifindex;
        uint8_t       hw[16];
    } u;
} ifreq_t;

/* ifconf: { int len; (4 pad); void *buf; } -- the pointer sits at offset 8. */
typedef struct {
    int32_t  ifc_len;
    int32_t  _pad;
    uint64_t ifc_buf;
} ifconf_t;

/* Handle a SIOC* request coming from a socket fd.  Returns a negative errno,
 * or -E_NOTTY for commands this layer does not recognise. */
int net_if_ioctl(uint64_t cmd, uint64_t arg);

/*
 * Drain the receive ring and the loopback queue and dispatch everything
 * found.  Idempotent, bounded, and re-entrancy safe (see the header comment).
 */
void net_poll(void);

/* 100 Hz: ARP ageing, TCP retransmission, and a poll so that an otherwise
 * idle machine still empties the receive ring. */
void net_tick(void);

/* ---- checksums --------------------------------------------------------- */
/* The internet checksum: one's complement sum of 16-bit big-endian words,
 * complemented.  Returns the value ready to be stored in a header field. */
uint16_t net_checksum(const void *data, uint32_t len);

/*
 * The same sum taken over TCP/UDP's imaginary pseudo-header (source and
 * destination address, protocol, length) followed by the segment.  It exists
 * so that a segment delivered to the wrong host or the wrong protocol fails
 * its checksum instead of being accepted -- IP's own checksum covers only the
 * IP header, so without this the transport layer would trust a header it has
 * no other way to verify.
 */
uint16_t net_checksum_pseudo(uint32_t src, uint32_t dst, uint8_t proto,
                             const void *seg, uint32_t len);

/* ---- output ------------------------------------------------------------ */
/*
 * Send one IPv4 datagram.  `payload` is the transport header plus its data;
 * the 20-byte IP header is prepended here.  Returns 0 on success or a
 * negative errno -- E_MSGSIZE for anything that would need fragmenting,
 * E_HOSTUNREACH when there is no route and no gateway.
 *
 * A send to an address whose MAC is not yet known does not fail: the datagram
 * is queued behind an ARP request and leaves when the reply arrives.
 */
int net_ip_output(uint32_t dst, uint8_t proto, const void *payload, uint16_t len);

/* The address this machine would use as a source when talking to `dst`. */
uint32_t net_route_src(uint32_t dst);

/* True if `dst` names this machine (any interface address, or 127/8). */
int net_is_local(uint32_t dst);

/* ---- upcalls, implemented by the transport layers ---------------------- */
/* `seg` points at the transport header; `len` is the transport length. */
void udp_input(uint32_t src, uint32_t dst, const uint8_t *seg, uint16_t len);
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, uint16_t len);

/*
 * Raw sockets see the whole packet starting at the IP header, because that
 * is what Linux gives them and therefore what BusyBox's ping parses: it
 * reads ihl out of the first byte and skips that many words to find the ICMP
 * echo reply.
 */
void raw_input(uint32_t src, uint32_t dst, uint8_t proto,
               const uint8_t *packet, uint16_t total_len);

#endif
