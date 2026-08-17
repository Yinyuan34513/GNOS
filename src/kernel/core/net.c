/*
 * net.c — ethernet, ARP, IPv4 and ICMP. (GPLv2)
 *
 * See net.h for the shape of this layer and for the note on which contexts
 * net_poll() runs in, which is the only genuinely subtle thing in the file.
 */
#include "net.h"
#include "tcp.h"             /* tcp_tick(); the transport layers fill the upcalls */
#include "sock.h"            /* sock_init() */
#include "e1000.h"
#include "vfs.h"            /* errno numbers */
#include "vmm.h"            /* user_ptr_ok() */
#include "kstring.h"
#include "proc.h"
#include "timer.h"
#include "debugcon.h"

/* ---- interfaces -------------------------------------------------------- */
static netif_t g_ifs[NET_IF_MAX];

netif_t *net_if(int idx)
{
    return (idx >= 0 && idx < NET_IF_MAX) ? &g_ifs[idx] : (netif_t *)0;
}

netif_t *net_if_by_name(const char *name)
{
    for (int i = 0; i < NET_IF_MAX; i++)
        if (!strcmp(g_ifs[i].name, name))
            return &g_ifs[i];
    return (netif_t *)0;
}

/* ---- SIOC* ioctls for ifconfig(8) / route(8) ------------------------- */
static int net_if_idx(const char *name)
{
    for (int i = 0; i < NET_IF_MAX; i++)
        if (!strcmp(g_ifs[i].name, name))
            return i;
    return -1;
}

int net_if_ioctl(uint64_t ucmd, uint64_t uarg)
{
    uint32_t cmd = (uint32_t)ucmd;
    if (!uarg || !user_ptr_ok(uarg, 1))
        return -E_FAULT;
    dbg_puts("net_ioctl: cmd="); dbg_puts_hex(cmd);
    dbg_puts(" uarg="); dbg_puts_hex((uint64_t)uarg); dbg_puts("\r\n");

    switch (cmd) {
    /* Enumerate every interface as a struct ifreq.  ifconfig uses this to
     * decide what to print; we always report both "lo" and "eth0". */
    case SIOCGIFCONF: {
        ifconf_t ic;
        if (!user_ptr_ok(uarg, sizeof(ic)))
            return -E_FAULT;
        memcpy(&ic, (const void *)(uintptr_t)uarg, sizeof(ic));
        dbg_puts("  SIOCGIFCONF: maxlen="); dbg_puts_hex((uint64_t)ic.ifc_len);
        dbg_puts(" buf="); dbg_puts_hex(ic.ifc_buf); dbg_puts("\r\n");
        int max  = ic.ifc_len;
        int used = 0;
        uint8_t *buf = (uint8_t *)(uintptr_t)ic.ifc_buf;
        for (int i = 0; i < NET_IF_MAX; i++) {
            if (used + (int)sizeof(ifreq_t) > max)
                break;
            if (buf && !user_ptr_ok((uint64_t)buf + used, sizeof(ifreq_t)))
                return -E_FAULT;
            ifreq_t r;
            memset(&r, 0, sizeof(r));
            strncpy(r.ifr_name, g_ifs[i].name, IFNAMSIZ - 1);
            r.u.addr.sin_family = AF_INET;
            r.u.addr.sin_addr   = net_htonl(g_ifs[i].ip);
            if (buf)
                memcpy(buf + used, &r, sizeof(r));
            used += sizeof(r);
        }
        ic.ifc_len = used;
        memcpy((void *)(uintptr_t)uarg, &ic, sizeof(ic));
        dbg_puts("  SIOCGIFCONF: used="); dbg_puts_hex((uint64_t)used);
        dbg_puts(" name0=["); dbg_puts(((ifreq_t*)buf)->ifr_name);
        dbg_puts("]\r\n");
        return 0;
    }

    /* Read-only getters for one named interface. */
    case SIOCGIFADDR:
    case SIOCGIFDSTADDR:
    case SIOCGIFNETMASK:
    case SIOCGIFBRDADDR:
    case SIOCGIFHWADDR:
    case SIOCGIFFLAGS:
    case SIOCGIFMTU:
    case SIOCGIFINDEX:
    case SIOCGIFMETRIC: {
        ifreq_t r;
        if (!user_ptr_ok(uarg, sizeof(r)))
            return -E_FAULT;
        memcpy(&r, (const void *)(uintptr_t)uarg, sizeof(r));
        int idx = net_if_idx(r.ifr_name);
        dbg_puts("  name=["); dbg_puts(r.ifr_name); dbg_puts("] idx=");
        dbg_puts_hex((uint64_t)idx); dbg_puts("\r\n");
        if (idx < 0)
            return -E_NODEV;
        netif_t *n = &g_ifs[idx];
        if (cmd == SIOCGIFADDR || cmd == SIOCGIFDSTADDR) {
            r.u.addr.sin_family = AF_INET;
            r.u.addr.sin_addr   = net_htonl(n->ip);
        } else if (cmd == SIOCGIFNETMASK) {
            r.u.netmask.sin_family = AF_INET;
            r.u.netmask.sin_addr   = net_htonl(n->netmask);
        } else if (cmd == SIOCGIFBRDADDR) {
            r.u.broadaddr.sin_family = AF_INET;
            r.u.broadaddr.sin_addr = net_htonl(n->netmask
                                              ? (n->ip | ~n->netmask) : 0);
        } else if (cmd == SIOCGIFHWADDR) {
            r.u.hw[0] = (uint8_t)ARPHRD_ETHER;
            r.u.hw[1] = 0;
            memcpy(r.u.hw + 2, n->mac, ETH_ALEN);
        } else if (cmd == SIOCGIFFLAGS) {
            int f = n->up ? IFF_UP : 0;
            f |= IFF_BROADCAST;
            if (n->loopback) f |= IFF_LOOPBACK;
            else f |= IFF_RUNNING | IFF_MULTICAST;
            r.u.flags = (int16_t)f;
        } else if (cmd == SIOCGIFMTU) {
            r.u.mtu = 1500;
        } else if (cmd == SIOCGIFINDEX) {
            r.u.ifindex = idx;
        } else {                        /* SIOCGIFMETRIC */
            r.u.metric = 1;
        }
        memcpy((void *)(uintptr_t)uarg, &r, sizeof(r));
        return 0;
    }

    /* Setters: enough for `ifconfig eth0 10.0.2.15 netmask ... up`. */
    case SIOCSIFADDR:
    case SIOCSIFNETMASK:
    case SIOCSIFFLAGS: {
        ifreq_t r;
        if (!user_ptr_ok(uarg, sizeof(r)))
            return -E_FAULT;
        memcpy(&r, (const void *)(uintptr_t)uarg, sizeof(r));
        int idx = net_if_idx(r.ifr_name);
        if (idx < 0)
            return -E_NODEV;
        netif_t *n = &g_ifs[idx];
        if (cmd == SIOCSIFADDR)
            n->ip = net_ntohl(r.u.addr.sin_addr);
        else if (cmd == SIOCSIFNETMASK)
            n->netmask = net_ntohl(r.u.netmask.sin_addr);
        else /* SIOCSIFFLAGS */
            n->up = (r.u.flags & IFF_UP) ? 1 : 0;
        return 0;
    }

    default:
        return -E_NOTTY;
    }
}

/* ---- checksums --------------------------------------------------------- */
/* Accumulate into 32 bits and fold at the end: the carries cannot be lost,
 * and one fold is enough because the sum of 16-bit words over a 64 KiB
 * packet cannot overflow into a third fold. */
static uint32_t sum16(const uint8_t *p, uint32_t len, uint32_t acc)
{
    while (len > 1) {
        acc += net_get16(p);
        p += 2;
        len -= 2;
    }
    if (len)
        acc += (uint32_t)p[0] << 8;      /* odd tail is padded on the right */
    return acc;
}

static uint16_t fold(uint32_t acc)
{
    while (acc >> 16)
        acc = (acc & 0xFFFF) + (acc >> 16);
    return (uint16_t)~acc;
}

uint16_t net_checksum(const void *data, uint32_t len)
{
    return fold(sum16((const uint8_t *)data, len, 0));
}

uint16_t net_checksum_pseudo(uint32_t src, uint32_t dst, uint8_t proto,
                             const void *seg, uint32_t len)
{
    uint32_t acc = 0;
    acc += (src >> 16) & 0xFFFF;
    acc += src & 0xFFFF;
    acc += (dst >> 16) & 0xFFFF;
    acc += dst & 0xFFFF;
    acc += proto;
    acc += len & 0xFFFF;
    return fold(sum16((const uint8_t *)seg, len, acc));
}

/* ---- ARP --------------------------------------------------------------- */
#define ARP_CACHE   16
#define ARP_TIMEOUT_TICKS  (60 * 100)    /* an entry is good for a minute */
#define ARP_RETRY_TICKS    100           /* re-request at most once a second */

#define ARP_FREE    0
#define ARP_PENDING 1
#define ARP_VALID   2

typedef struct {
    uint32_t ip;
    uint8_t  mac[ETH_ALEN];
    uint8_t  state;
    uint64_t stamp;         /* tick the entry became valid, or was requested */
} arp_entry_t;

static arp_entry_t g_arp[ARP_CACHE];

/*
 * Datagrams waiting for an ARP reply.  Four slots is not a design compromise
 * so much as an observation: the only traffic that ever hits an unresolved
 * address is the first packet of a new conversation, and by the second one
 * the cache is warm.  Dropping the *first* SYN of a connection is survivable
 * (TCP retransmits) but it costs a full retransmit timeout, which is exactly
 * the kind of "the network is mysteriously slow" behaviour worth avoiding.
 */
#define ARP_QUEUE 4
typedef struct {
    int      used;
    uint32_t nexthop;
    uint16_t len;
    uint8_t  packet[NET_MTU];       /* a complete IP datagram */
} arp_pending_t;

static arp_pending_t g_arpq[ARP_QUEUE];

static const uint8_t bcast_mac[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static arp_entry_t *arp_find(uint32_t ip)
{
    for (int i = 0; i < ARP_CACHE; i++)
        if (g_arp[i].state != ARP_FREE && g_arp[i].ip == ip)
            return &g_arp[i];
    return (arp_entry_t *)0;
}

/* Evict the oldest entry when the cache is full.  A cache this small on a
 * machine with one gateway will essentially never evict anything. */
static arp_entry_t *arp_slot(uint32_t ip)
{
    arp_entry_t *e = arp_find(ip);
    if (e)
        return e;

    arp_entry_t *oldest = &g_arp[0];
    for (int i = 0; i < ARP_CACHE; i++) {
        if (g_arp[i].state == ARP_FREE)
            return &g_arp[i];
        if (g_arp[i].stamp < oldest->stamp)
            oldest = &g_arp[i];
    }
    return oldest;
}

static int eth_output(const uint8_t *dmac, uint16_t ethertype,
                      const void *payload, uint16_t len);

static void arp_request(uint32_t target)
{
    netif_t *nif = &g_ifs[NET_IF_ETH];
    uint8_t pkt[28];

    net_put16(pkt + 0, 1);              /* hardware type: ethernet */
    net_put16(pkt + 2, ETH_P_IP);       /* protocol type */
    pkt[4] = ETH_ALEN;
    pkt[5] = 4;
    net_put16(pkt + 6, 1);              /* opcode: request */
    memcpy(pkt + 8, nif->mac, ETH_ALEN);
    net_put32(pkt + 14, nif->ip);
    memset(pkt + 18, 0, ETH_ALEN);      /* the answer we are asking for */
    net_put32(pkt + 24, target);

    eth_output(bcast_mac, ETH_P_ARP, pkt, sizeof(pkt));
}

static void arp_reply(const uint8_t *req)
{
    netif_t *nif = &g_ifs[NET_IF_ETH];
    uint8_t pkt[28];

    net_put16(pkt + 0, 1);
    net_put16(pkt + 2, ETH_P_IP);
    pkt[4] = ETH_ALEN;
    pkt[5] = 4;
    net_put16(pkt + 6, 2);              /* opcode: reply */
    memcpy(pkt + 8, nif->mac, ETH_ALEN);
    net_put32(pkt + 14, nif->ip);
    memcpy(pkt + 18, req + 8, ETH_ALEN);        /* back to whoever asked */
    net_put32(pkt + 24, net_get32(req + 14));

    eth_output(req + 8, ETH_P_ARP, pkt, sizeof(pkt));
}

static int ip_transmit(uint32_t nexthop, const uint8_t *packet, uint16_t len);

/* An address became known: release anything that was waiting on it. */
static void arp_flush_queue(uint32_t ip)
{
    for (int i = 0; i < ARP_QUEUE; i++) {
        if (!g_arpq[i].used || g_arpq[i].nexthop != ip)
            continue;
        g_arpq[i].used = 0;             /* clear first: ip_transmit may re-queue */
        ip_transmit(ip, g_arpq[i].packet, g_arpq[i].len);
    }
}

static void arp_learn(uint32_t ip, const uint8_t *mac)
{
    if (!ip || ip == IP_BROADCAST)
        return;

    arp_entry_t *e = arp_slot(ip);
    e->ip = ip;
    memcpy(e->mac, mac, ETH_ALEN);
    e->state = ARP_VALID;
    e->stamp = timer_ticks();
    arp_flush_queue(ip);
}

static void arp_input(const uint8_t *pkt, uint16_t len)
{
    if (len < 28)
        return;
    if (net_get16(pkt + 0) != 1 || net_get16(pkt + 2) != ETH_P_IP)
        return;
    if (pkt[4] != ETH_ALEN || pkt[5] != 4)
        return;

    uint16_t op   = net_get16(pkt + 6);
    uint32_t sip  = net_get32(pkt + 14);
    uint32_t tip  = net_get32(pkt + 24);

    /* Learn from every ARP packet that crosses us, request or reply: the
     * sender is by definition reachable and has just told us its MAC. */
    arp_learn(sip, pkt + 8);

    if (op == 1 && tip == g_ifs[NET_IF_ETH].ip && g_ifs[NET_IF_ETH].up)
        arp_reply(pkt);
}

/* ---- ethernet ---------------------------------------------------------- */
static int eth_output(const uint8_t *dmac, uint16_t ethertype,
                      const void *payload, uint16_t len)
{
    netif_t *nif = &g_ifs[NET_IF_ETH];
    static uint8_t frame[NET_FRAME_MAX];

    if (!nif->up || len > NET_MTU)
        return -E_MSGSIZE;

    memcpy(frame + 0, dmac, ETH_ALEN);
    memcpy(frame + 6, nif->mac, ETH_ALEN);
    net_put16(frame + 12, ethertype);
    memcpy(frame + ETH_HDR_LEN, payload, len);

    uint16_t total = (uint16_t)(ETH_HDR_LEN + len);
    /* The card pads to the 60-byte minimum itself (TCTL.PSP), so a 28-byte
     * ARP does not need padding here. */
    if (!e1000_send(frame, total)) {
        nif->tx_dropped++;
        return -E_IO;
    }
    nif->tx_packets++;
    nif->tx_bytes += total;
    return 0;
}

/* ---- loopback ---------------------------------------------------------- */
/*
 * Loopback is not a fake NIC here, it is a queue of IP datagrams: there is no
 * ethernet header to build and immediately throw away, and no MTU to respect
 * beyond the one the socket layer already checked.
 *
 * It is a queue rather than a direct call because the alternative is
 * recursion -- tcp_output() -> loopback -> tcp_input() -> tcp_output() for
 * the ACK -> ... -- with a depth that depends on what the peer decides to do.
 * Deferring to net_poll() bounds it at one.
 */
#define LOOP_QUEUE 8
typedef struct {
    uint16_t len;
    uint8_t  packet[NET_MTU];
} loop_slot_t;

static loop_slot_t g_loopq[LOOP_QUEUE];
static unsigned    g_loop_head, g_loop_tail;

static int loop_enqueue(const uint8_t *packet, uint16_t len)
{
    unsigned next = (g_loop_tail + 1) % LOOP_QUEUE;
    if (next == g_loop_head || len > NET_MTU) {
        g_ifs[NET_IF_LO].tx_dropped++;
        return -E_NOBUFS;
    }
    memcpy(g_loopq[g_loop_tail].packet, packet, len);
    g_loopq[g_loop_tail].len = len;
    g_loop_tail = next;
    g_ifs[NET_IF_LO].tx_packets++;
    g_ifs[NET_IF_LO].tx_bytes += len;
    return 0;
}

/* ---- IPv4 -------------------------------------------------------------- */
#define IP_HDR_LEN 20

static uint16_t g_ip_id;

int net_is_local(uint32_t dst)
{
    if ((dst >> 24) == 127)
        return 1;
    for (int i = 0; i < NET_IF_MAX; i++)
        if (g_ifs[i].up && g_ifs[i].ip && g_ifs[i].ip == dst)
            return 1;
    return 0;
}

uint32_t net_route_src(uint32_t dst)
{
    if ((dst >> 24) == 127)
        return IP_LOOPBACK;
    if (net_is_local(dst))
        return dst;
    return g_ifs[NET_IF_ETH].ip;
}

/* Hand a fully formed IP datagram to the link layer, resolving the next hop's
 * MAC first.  On a cache miss the datagram is parked and a request goes out;
 * the reply path calls back in here through arp_flush_queue(). */
static int ip_transmit(uint32_t nexthop, const uint8_t *packet, uint16_t len)
{
    if (nexthop == IP_BROADCAST)
        return eth_output(bcast_mac, ETH_P_IP, packet, len);

    arp_entry_t *e = arp_find(nexthop);
    if (e && e->state == ARP_VALID)
        return eth_output(e->mac, ETH_P_IP, packet, len);

    uint64_t now = timer_ticks();
    if (!e || e->state == ARP_FREE) {
        e = arp_slot(nexthop);
        e->ip    = nexthop;
        e->state = ARP_PENDING;
        e->stamp = now;
        arp_request(nexthop);
    } else if (now - e->stamp >= ARP_RETRY_TICKS) {
        e->stamp = now;
        arp_request(nexthop);
    }

    for (int i = 0; i < ARP_QUEUE; i++) {
        if (g_arpq[i].used)
            continue;
        g_arpq[i].used    = 1;
        g_arpq[i].nexthop = nexthop;
        g_arpq[i].len     = len;
        memcpy(g_arpq[i].packet, packet, len);
        return 0;
    }

    g_ifs[NET_IF_ETH].tx_dropped++;
    return -E_NOBUFS;
}

int net_ip_output(uint32_t dst, uint8_t proto, const void *payload, uint16_t len)
{
    static uint8_t packet[NET_MTU];

    if ((uint32_t)len + IP_HDR_LEN > NET_MTU)
        return -E_MSGSIZE;

    uint32_t src = net_route_src(dst);
    uint16_t total = (uint16_t)(IP_HDR_LEN + len);

    packet[0] = 0x45;                   /* IPv4, 5 words of header */
    packet[1] = 0;                      /* no DSCP, no ECN */
    net_put16(packet + 2, total);
    net_put16(packet + 4, ++g_ip_id);
    net_put16(packet + 6, 0x4000);      /* don't fragment, offset 0 */
    packet[8]  = 64;                    /* TTL */
    packet[9]  = proto;
    net_put16(packet + 10, 0);          /* checksum, filled in below */
    net_put32(packet + 12, src);
    net_put32(packet + 16, dst);
    net_put16(packet + 10, net_checksum(packet, IP_HDR_LEN));

    memcpy(packet + IP_HDR_LEN, payload, len);

    if (net_is_local(dst))
        return loop_enqueue(packet, total);

    netif_t *eth = &g_ifs[NET_IF_ETH];
    if (!eth->up)
        return -E_NETUNREACH;

    /* Three-line routing table: broadcast, on-link, or via the gateway. */
    uint32_t nexthop;
    if (dst == IP_BROADCAST || (eth->netmask && dst == (eth->ip | ~eth->netmask)))
        nexthop = IP_BROADCAST;
    else if (eth->netmask && (dst & eth->netmask) == (eth->ip & eth->netmask))
        nexthop = dst;
    else if (eth->gateway)
        nexthop = eth->gateway;
    else
        return -E_HOSTUNREACH;

    return ip_transmit(nexthop, packet, total);
}

/* ---- ICMP -------------------------------------------------------------- */
#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

static void icmp_input(uint32_t src, uint32_t dst, const uint8_t *msg, uint16_t len)
{
    static uint8_t reply[NET_MTU - IP_HDR_LEN];

    if (len < 8 || net_checksum(msg, len) != 0)
        return;
    if (msg[0] != ICMP_ECHO_REQUEST)
        return;                          /* replies are for raw sockets only */
    if (dst == IP_BROADCAST)
        return;                          /* do not answer broadcast pings */
    if (len > sizeof(reply))
        return;

    /* An echo reply is the request with the type changed and the checksum
     * recomputed: identifier, sequence and payload all come straight back,
     * which is how the sender matches the answer to its question. */
    memcpy(reply, msg, len);
    reply[0] = ICMP_ECHO_REPLY;
    net_put16(reply + 2, 0);
    net_put16(reply + 2, net_checksum(reply, len));

    net_ip_output(src, IP_PROTO_ICMP, reply, len);
}

/* ---- IPv4 input -------------------------------------------------------- */
static void ip_input(netif_t *nif, const uint8_t *packet, uint16_t len)
{
    if (len < IP_HDR_LEN)
        return;
    if ((packet[0] >> 4) != 4)
        return;

    uint16_t ihl = (uint16_t)((packet[0] & 0x0F) * 4);
    if (ihl < IP_HDR_LEN || ihl > len)
        return;
    if (net_checksum(packet, ihl) != 0)
        return;

    uint16_t total = net_get16(packet + 2);
    if (total < ihl || total > len)
        return;                          /* truncated, or a lying length */

    /* No reassembly: a fragment is neither the whole datagram nor safe to
     * hand up as one.  MF set or a non-zero offset means drop. */
    if (net_get16(packet + 6) & 0x3FFF)
        return;

    uint8_t  proto = packet[9];
    uint32_t src   = net_get32(packet + 12);
    uint32_t dst   = net_get32(packet + 16);

    /* Accept anything addressed to us, to broadcast, or to the subnet
     * broadcast.  Everything else on the wire is somebody else's. */
    if (!net_is_local(dst) && dst != IP_BROADCAST &&
        !(nif->netmask && dst == (nif->ip | ~nif->netmask)))
        return;

    nif->rx_packets++;
    nif->rx_bytes += total;

    const uint8_t *seg = packet + ihl;
    uint16_t seglen = (uint16_t)(total - ihl);

    /* Raw sockets see everything, before and regardless of what the cooked
     * protocols make of it -- including the ICMP echo replies that are the
     * entire point of ping. */
    raw_input(src, dst, proto, packet, total);

    switch (proto) {
    case IP_PROTO_ICMP: icmp_input(src, dst, seg, seglen); break;
    case IP_PROTO_UDP:  udp_input(src, dst, seg, seglen);  break;
    case IP_PROTO_TCP:  tcp_input(src, dst, seg, seglen);  break;
    default: break;
    }
}

static void eth_input(const uint8_t *frame, uint16_t len)
{
    if (len < ETH_HDR_LEN)
        return;

    uint16_t ethertype = net_get16(frame + 12);
    const uint8_t *payload = frame + ETH_HDR_LEN;
    uint16_t plen = (uint16_t)(len - ETH_HDR_LEN);

    switch (ethertype) {
    case ETH_P_ARP:
        arp_input(payload, plen);
        break;
    case ETH_P_IP:
        ip_input(&g_ifs[NET_IF_ETH], payload, plen);
        break;
    default:
        break;
    }
}

/* ---- the poll loop ----------------------------------------------------- */
void net_poll(void)
{
    /*
     * Re-entrancy guard.  This is the whole concurrency design of the stack:
     * an interrupt that lands while a system call is halfway through a poll
     * turns into a no-op, and its frames are collected by the loop already
     * running or by the next timer tick.  Nothing is lost because the frames
     * stay in the card's ring until somebody takes them.
     */
    static volatile int busy;
    static uint8_t frame[NET_FRAME_MAX];

    if (busy)
        return;
    busy = 1;

    /* Bounded so that a flood cannot hold an interrupt handler forever. */
    for (int n = 0; n < 32; n++) {
        uint16_t len = e1000_recv(frame, sizeof(frame));
        if (!len)
            break;
        eth_input(frame, len);
    }

    for (int n = 0; n < LOOP_QUEUE && g_loop_head != g_loop_tail; n++) {
        loop_slot_t *s = &g_loopq[g_loop_head];
        g_loop_head = (g_loop_head + 1) % LOOP_QUEUE;
        g_ifs[NET_IF_LO].rx_packets++;
        g_ifs[NET_IF_LO].rx_bytes += s->len;
        ip_input(&g_ifs[NET_IF_LO], s->packet, s->len);
    }

    busy = 0;
}

void net_tick(void)
{
    uint64_t now = timer_ticks();

    /* Retire stale ARP entries.  A pending one that was never answered is
     * dropped sooner, so that a later send retries the request instead of
     * silently queueing behind an address nobody is ever going to claim. */
    for (int i = 0; i < ARP_CACHE; i++) {
        if (g_arp[i].state == ARP_VALID && now - g_arp[i].stamp > ARP_TIMEOUT_TICKS)
            g_arp[i].state = ARP_FREE;
        else if (g_arp[i].state == ARP_PENDING && now - g_arp[i].stamp > 5 * ARP_RETRY_TICKS)
            g_arp[i].state = ARP_FREE;
    }

    tcp_tick();
    net_poll();
}

/* ---- bring-up ---------------------------------------------------------- */
/*
 * The addresses are QEMU's user-mode network, hard-coded because there is no
 * DHCP client and 10.0.2.15 is what slirp hands out anyway.  `ifconfig` can
 * change them at run time; this is only the state the machine boots into so
 * that the self-test has something to work with.
 */
void net_init(void)
{
    memset(g_ifs, 0, sizeof(g_ifs));
    memset(g_arp, 0, sizeof(g_arp));
    memset(g_arpq, 0, sizeof(g_arpq));
    g_loop_head = g_loop_tail = 0;

    netif_t *lo = &g_ifs[NET_IF_LO];
    strncpy(lo->name, "lo", sizeof(lo->name) - 1);
    lo->ip       = IP_LOOPBACK;
    lo->netmask  = IPV4(255, 0, 0, 0);
    lo->up       = 1;
    lo->loopback = 1;

    netif_t *eth = &g_ifs[NET_IF_ETH];
    strncpy(eth->name, "eth0", sizeof(eth->name) - 1);
    if (e1000_present()) {
        memcpy(eth->mac, e1000_mac(), ETH_ALEN);
        eth->ip      = IPV4(10, 0, 2, 15);
        eth->netmask = IPV4(255, 255, 255, 0);
        eth->gateway = IPV4(10, 0, 2, 2);
        eth->up      = 1;
    }

    sock_init();

    dbg_puts("NET: lo 127.0.0.1/8");
    if (eth->up) {
        dbg_puts(", eth0 10.0.2.15/24 via 10.0.2.2");
    } else {
        dbg_puts(", eth0 down (no NIC)");
    }
    dbg_puts("\r\n");
}
