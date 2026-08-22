/*
 * sock.c — the socket object: three wire behaviours behind one fd. (GPLv2)
 *
 * Layout: the object table and the datagram ring first (the data structure
 * UDP and raw both live on), then the blocking discipline, then each of the
 * sock.h entry points grouped by "things all three types share", "the UDP
 * and raw machinery", and "the stream machinery".
 *
 * The datagram ring deserves its own paragraph.  It is one page per socket,
 * holding records of {header, payload padded to 8 bytes}.  The padding is
 * not for speed: it guarantees that read and write positions are always
 * multiples of 8 in a 4096-byte ring, which guarantees that a wrap marker
 * (a header with len == 0xFFFF) always fits at the end of the buffer, which
 * is what lets the reader follow the writer across the wrap without the two
 * ever disagreeing about where the next record starts.  Records never cross
 * the end of the ring, so neither payload copy ever has to.
 *
 * Blocking follows the pipe's rule (see pipe_node_read in vfs.c): test and
 * sleep are atomic because the whole wait loop runs with interrupts masked
 * and sched_block_irqoff() is the last thing before re-testing.  The one
 * addition here is net_poll() at the top of every retry: on loopback the
 * packet that will wake us is already sitting in a queue, and only a poll
 * turns it into the state change we are waiting for.  Between retries the
 * 100 Hz timer does the same job from interrupt context.
 */
#include "sock.h"
#include "unix.h"
#include "net.h"
#include "tcp.h"
#include "pmm.h"
#include "proc.h"
#include "vfs.h"
#include "kstring.h"

#define SOCK_MAX    16
#define DGRAM_RING  4096                 /* one page per dgram/raw socket */

/* A queued datagram.  src_port doubles as the protocol field on raw
 * sockets, where the payload is the whole IP packet. */
typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t len;                        /* payload bytes; 0xFFFF = wrap marker */
} dgram_hdr_t;
#define DGRAM_WRAP 0xFFFF

typedef struct {
    int      used;
    int      type;                       /* SOCK_STREAM / DGRAM / RAW */
    int      protocol;
    int      nonblock;                   /* SOCK_NONBLOCK at creation */

    uint64_t rx_phys;                    /* the datagram ring page */
    uint32_t rx_head, rx_tail, rx_used;

    uint32_t lip, rip;                   /* local and peer, host order */
    uint16_t lport, rport;
    int      connected;                  /* UDP: a default peer exists */

    int      broadcast;                  /* SO_BROADCAST */
    int      error;                      /* datagram-side async errors */

    tcp_pcb_t *tcp;                      /* SOCK_STREAM only */
} sock_t;

static sock_t g_socks[SOCK_MAX];

void sock_init(void)
{
    memset(g_socks, 0, sizeof(g_socks));
}

static sock_t *sock_at(int s)
{
    if (s < 0 || s >= SOCK_MAX || !g_socks[s].used)
        return NULL;
    return &g_socks[s];
}

/* ---- the datagram ring ------------------------------------------------- */

static uint32_t ring_free(const sock_t *s)
{
    return DGRAM_RING - s->rx_used;
}

/* Append one record; returns 0 when the packet fit and was queued.  A
 * packet that does not fit is dropped -- for UDP and raw sockets that is
 * the specified behaviour, not an error path. */
static int ring_put(sock_t *s, uint32_t src_ip, uint16_t src_port,
                    const void *payload, uint16_t len)
{
    uint8_t *ring = pmm_virt(s->rx_phys);
    uint32_t plen = (uint32_t)(len + 7) & ~7u;
    uint32_t need = 8 + plen;

    if (need > DGRAM_RING - 8 || ring_free(s) < need)
        return 0;

    if (s->rx_head + need > DGRAM_RING) {
        /* Does not fit at the end: leave a wrap marker and start over.
         * Alignment (see the file comment) makes the marker always fit. */
        dgram_hdr_t m = { 0, 0, DGRAM_WRAP };
        memcpy(ring + s->rx_head, &m, 8);
        s->rx_used += 8;
        s->rx_head  = 0;
    }

    dgram_hdr_t h = { src_ip, src_port, len };
    memcpy(ring + s->rx_head, &h, 8);
    if (len)
        memcpy(ring + s->rx_head + 8, payload, len);
    s->rx_head += need;
    if (s->rx_head == DGRAM_RING)
        s->rx_head = 0;
    s->rx_used += need;
    return 1;
}

/* Copy out the oldest record.  Returns the payload length copied (possibly
 * truncated to `cap`), 0 when the ring is empty. */
static uint32_t ring_pop(sock_t *s, uint32_t *src_ip, uint16_t *src_port,
                         void *buf, uint32_t cap, int peek)
{
    if (!s->rx_used)
        return 0;
    const uint8_t *ring = pmm_virt(s->rx_phys);

    dgram_hdr_t h;
    memcpy(&h, ring + s->rx_tail, 8);
    if (h.len == DGRAM_WRAP) {
        s->rx_tail = 0;
        s->rx_used -= 8;
        if (!s->rx_used)
            return 0;
        memcpy(&h, ring + s->rx_tail, 8);
    }

    uint32_t n = h.len < cap ? h.len : cap;
    if (n)
        memcpy(buf, ring + s->rx_tail + 8, n);
    if (src_ip)   *src_ip   = h.src_ip;
    if (src_port) *src_port = h.src_port;

    if (!peek) {
        s->rx_tail += 8 + ((uint32_t)(h.len + 7) & ~7u);
        if (s->rx_tail == DGRAM_RING)
            s->rx_tail = 0;
        s->rx_used -= 8 + ((uint32_t)(h.len + 7) & ~7u);
    }
    return n;
}

/* ---- the blocking discipline -------------------------------------------- */

/*
 * Retry `net_poll()` (so loopback progress does not depend on the timer
 * firing), then wait with test-and-sleep atomic under cli, exactly like the
 * pipe.  The body in between is what each caller supplies.  Returns
 * -E_INTR when a signal should interrupt the call.
 *
 * This is a macro because C has no closures; the condition and the failure
 * test are expressions over the caller's locals.  COND becoming true exits
 * the loop normally; FAIL exits with its value (0 means "return 0").
 */
#define SOCK_WAIT_LOOP(s, COND, FAIL)                                     \
    for (;;) {                                                            \
        net_poll();                                                       \
        sched_wake_reason(WAIT_NET);   /* our poll may have fed a peer */ \
        proc_t *me_ = proc_current();                                     \
        asm volatile("cli");                                              \
        if (COND) { asm volatile("sti"); break; }                         \
        if (FAIL) { asm volatile("sti"); return (FAIL); }                 \
        if ((s)->nonblock) { asm volatile("sti"); return -E_AGAIN; }      \
        if (!me_ || proc_pending_signals(me_)) {                          \
            asm volatile("sti");                                          \
            return -E_INTR;                                               \
        }                                                                 \
        sched_block_irqoff(WAIT_NET);                                     \
        asm volatile("sti");                                              \
    }

/* ---- creation and destruction -------------------------------------------- */

int sock_create(int domain, int type, int protocol)
{
    if (domain != AF_INET)
        return -E_AFNOSUPPORT;

    int nonblock = type & SOCK_NONBLOCK;
    type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (type != SOCK_STREAM && type != SOCK_DGRAM && type != SOCK_RAW)
        return -E_SOCKTNOSUPPORT;
    if (type == SOCK_STREAM && protocol && protocol != IPPROTO_TCP)
        return -E_PROTONOSUPPORT;
    if (type == SOCK_DGRAM && protocol && protocol != IPPROTO_UDP)
        return -E_PROTONOSUPPORT;

    for (int i = 0; i < SOCK_MAX; i++) {
        if (g_socks[i].used)
            continue;
        sock_t *s = &g_socks[i];
        memset(s, 0, sizeof(*s));
        s->type     = type;
        s->protocol = protocol ? protocol
                      : type == SOCK_STREAM ? IPPROTO_TCP
                      : type == SOCK_DGRAM  ? IPPROTO_UDP : 0;
        s->nonblock = nonblock;

        if (type == SOCK_STREAM) {
            s->tcp = tcp_new();
            if (!s->tcp)
                return -E_NOBUFS;
        } else {
            s->rx_phys = pmm_alloc_zeroed();
            if (!s->rx_phys)
                return -E_NOBUFS;
        }
        s->used = 1;
        return i;
    }
    return -E_NOBUFS;
}

void sock_close(int s)
{
    sock_t *sk = sock_at(s);
    if (!sk)
        return;
    if (sk->tcp)
        tcp_destroy(sk->tcp);         /* aborts a live connection (see tcp.h) */
    if (sk->rx_phys)
        pmm_free(sk->rx_phys);
    sk->used = 0;
}

int sock_set_nonblock(int s, int on)
{
    sock_t *sk = sock_at(s);
    if (!sk)
        return -E_BADF;
    sk->nonblock = on != 0;
    return 0;
}

int sock_is_nonblock(int s)
{
    sock_t *sk = sock_at(s);
    return sk ? sk->nonblock : 0;
}

/* ---- binding and naming --------------------------------------------------- */

static int udp_port_in_use(uint32_t ip, uint16_t port)
{
    for (int i = 0; i < SOCK_MAX; i++) {
        sock_t *s = &g_socks[i];
        if (!s->used || s->type != SOCK_DGRAM || s->lport != port)
            continue;
        if (s->lip == IP_ANY || ip == IP_ANY || s->lip == ip)
            return 1;
    }
    return 0;
}

static uint16_t udp_ephemeral_port(void)
{
    static uint16_t next;
    if (next < 49152)
        next = 49152;
    for (int i = 0; i < 65535 - 49152; i++) {
        uint16_t port = next++;
        if (next < 49152)
            next = 49152;
        if (!udp_port_in_use(IP_ANY, port))
            return port;
    }
    return 0;
}

/* Give a UDP socket a local port if it does not have one yet; the first
 * sendto() on an unbound socket needs a source port to receive replies. */
static int udp_ensure_bound(sock_t *s)
{
    if (s->lport)
        return 0;
    s->lport = udp_ephemeral_port();
    return s->lport ? 0 : -E_ADDRINUSE;
}

int sock_bind(int s, uint32_t ip, uint16_t port)
{
    sock_t *sk = sock_at(s);
    if (!sk)
        return -E_BADF;
    if (ip != IP_ANY && !net_is_local(ip))
        return -E_ADDRNOTAVAIL;

    if (sk->type == SOCK_STREAM) {
        if (!port)
            return -E_INVAL;              /* tcp_bind needs an explicit port */
        return tcp_bind(sk->tcp, ip, port);
    }
    if (sk->type == SOCK_RAW) {
        sk->lip = ip;                     /* raw has no ports; bind only filters */
        return 0;
    }

    /* UDP: a zero port means "pick a free ephemeral port" -- musl's DNS
     * resolver binds an all-zero sockaddr (sin_port == 0) before sendto,
     * and rejecting it breaks getaddrinfo(3) with EINVAL. */
    if (!port) {
        port = udp_ephemeral_port();
        if (!port)
            return -E_ADDRINUSE;
    } else if (udp_port_in_use(ip, port)) {
        return -E_ADDRINUSE;
    }
    sk->lip   = ip;
    sk->lport = port;
    return 0;
}

int sock_getname(int s, uint32_t *ip, uint16_t *port, int peer)
{
    sock_t *sk = sock_at(s);
    if (!sk)
        return -E_BADF;

    if (sk->type == SOCK_STREAM) {
        uint32_t lip, rip;
        uint16_t lport, rport;
        tcp_endpoints(sk->tcp, &lip, &lport, &rip, &rport);
        if (peer) {
            if (tcp_state(sk->tcp) != TCPS_ESTABLISHED &&
                tcp_state(sk->tcp) != TCPS_CLOSE_WAIT)
                return -E_NOTCONN;
            *ip = rip; *port = rport;
        } else {
            *ip = lip; *port = lport;
        }
        return 0;
    }
    if (peer) {
        if (!sk->connected)
            return -E_NOTCONN;
        *ip = sk->rip; *port = sk->rport;
    } else {
        *ip = sk->lip; *port = sk->lport;
    }
    return 0;
}

/* ---- UDP and raw: the wire side -------------------------------------------- */

void udp_input(uint32_t src, uint32_t dst, const uint8_t *seg, uint16_t len)
{
    if (len < 8)
        return;
    uint16_t sport = net_get16(seg + 0);
    uint16_t dport = net_get16(seg + 2);
    uint16_t ulen  = net_get16(seg + 4);
    if (ulen < 8 || ulen > len)
        return;
    /* A zero checksum means "not computed", which IPv4 permits. */
    if (net_get16(seg + 6) &&
        net_checksum_pseudo(src, dst, IP_PROTO_UDP, seg, ulen) != 0)
        return;

    for (int i = 0; i < SOCK_MAX; i++) {
        sock_t *s = &g_socks[i];
        if (!s->used || s->type != SOCK_DGRAM || s->lport != dport)
            continue;
        if (s->lip != IP_ANY && s->lip != dst)
            continue;
        if (s->connected && (s->rip != src || s->rport != sport))
            continue;
        ring_put(s, src, sport, seg + 8, (uint16_t)(ulen - 8));
        sched_wake_reason(WAIT_NET);   /* wake poll()/select()/recvfrom waiters */
        return;
    }
    /* No listener: silence.  A full stack would answer with an ICMP port
     * unreachable, which exists mostly so that connect()ed UDP can fail
     * early; nothing in this system relies on it. */
}

void raw_input(uint32_t src, uint32_t dst, uint8_t proto,
               const uint8_t *packet, uint16_t total_len)
{
    (void)dst;
    for (int i = 0; i < SOCK_MAX; i++) {
        sock_t *s = &g_socks[i];
        if (!s->used || s->type != SOCK_RAW)
            continue;
        if (s->protocol && s->protocol != proto)
            continue;
        if (s->lip && s->lip != dst)
            continue;
        ring_put(s, src, 0, packet, total_len);
    }
}

static int is_broadcast_addr(uint32_t ip)
{
    if (ip == IP_BROADCAST)
        return 1;
    netif_t *eth = net_if(NET_IF_ETH);
    if (eth->up && ip == (eth->ip | ~eth->netmask))
        return 1;
    return 0;
}

static int udp_sendto(sock_t *s, const void *buf, uint32_t len,
                      uint32_t ip, uint16_t port)
{
    if (!ip || !port)
        return -E_DESTADDRREQ;
    if (is_broadcast_addr(ip) && !s->broadcast)
        return -E_PERM;
    if (len > NET_MTU - 20 - 8)
        return -E_MSGSIZE;
    int e = udp_ensure_bound(s);
    if (e)
        return e;

    uint32_t src = s->lip ? s->lip : net_route_src(ip);
    if (!src)
        return -E_HOSTUNREACH;

    uint16_t ulen = (uint16_t)(8 + len);
    uint8_t  pkt[8 + NET_MTU - 20 - 8];
    net_put16(pkt + 0, s->lport);
    net_put16(pkt + 2, port);
    net_put16(pkt + 4, ulen);
    net_put16(pkt + 6, 0);
    if (len)
        memcpy(pkt + 8, buf, len);
    uint16_t sum = net_checksum_pseudo(src, ip, IP_PROTO_UDP, pkt, ulen);
    net_put16(pkt + 6, sum ? sum : 0xFFFF);   /* 0 on the wire means "none" */
    e = net_ip_output(ip, IP_PROTO_UDP, pkt, ulen);
    return e ? e : (int)len;
}

/* ---- the entry points -------------------------------------------------------- */

int sock_connect(int s, uint32_t ip, uint16_t port)
{
    sock_t *sk = sock_at(s);
    if (!sk)
        return -E_BADF;
    if (!ip || !port)
        return -E_DESTADDRREQ;

    if (sk->type == SOCK_RAW)
        return -E_OPNOTSUPP;

    if (sk->type == SOCK_DGRAM) {
        /* UDP connect() fixes a default peer and a source port; no packets
         * are exchanged, but a route has to exist. */
        if (!net_route_src(ip))
            return -E_HOSTUNREACH;
        int e = udp_ensure_bound(sk);
        if (e)
            return e;
        sk->rip = ip;
        sk->rport = port;
        sk->connected = 1;
        return 0;
    }

    /* SOCK_STREAM. */
    int state = tcp_state(sk->tcp);
    if (state == TCPS_ESTABLISHED)
        return -E_ISCONN;
    if (state != TCPS_CLOSED && state != TCPS_SYN_SENT)
        return -E_ISCONN;
    if (state == TCPS_CLOSED) {
        int e = tcp_connect(sk->tcp, ip, port);
        if (e)
            return e;
        if (sk->nonblock)
            return -E_INPROGRESS;
    }

    /* Wait out the handshake.  A non-blocking socket that asks again while
     * SYN_SENT is told EALREADY, which is precisely how musl's connect()
     * wrapper expects a re-poll to be answered. */
    for (;;) {
        net_poll();
        sched_wake_reason(WAIT_NET);
        proc_t *me = proc_current();
        asm volatile("cli");
        state = tcp_state(sk->tcp);
        if (state == TCPS_ESTABLISHED) {
            asm volatile("sti");
            return 0;
        }
        if (state == TCPS_CLOSED) {
            int err = tcp_take_error(sk->tcp);
            asm volatile("sti");
            return err ? -err : -E_CONNREFUSED;
        }
        if (sk->nonblock) {
            asm volatile("sti");
            return -E_ALREADY;
        }
        if (!me || proc_pending_signals(me)) {
            asm volatile("sti");
            return -E_INTR;
        }
        sched_block_irqoff(WAIT_NET);
        asm volatile("sti");
    }
}

int sock_listen(int s, int backlog)
{
    sock_t *sk = sock_at(s);
    if (!sk)
        return -E_BADF;
    if (sk->type != SOCK_STREAM)
        return -E_OPNOTSUPP;
    return tcp_listen(sk->tcp, backlog);
}

int sock_accept(int s, uint32_t *rip, uint16_t *rport)
{
    sock_t *sk = sock_at(s);
    if (!sk)
        return -E_BADF;
    if (sk->type != SOCK_STREAM || tcp_state(sk->tcp) != TCPS_LISTEN)
        return -E_OPNOTSUPP;

    SOCK_WAIT_LOOP(sk, tcp_accept_ready(sk->tcp), 0);

    tcp_pcb_t *child = tcp_accept(sk->tcp);
    if (!child)
        return -E_AGAIN;              /* raced ourselves; cannot happen, be safe */

    int ns = sock_create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ns < 0) {
        tcp_destroy(child);
        return ns;
    }
    sock_t *c = sock_at(ns);
    tcp_destroy(c->tcp);              /* replace the fresh pcb with the child */
    c->tcp = child;
    tcp_endpoints(child, &c->lip, &c->lport, &c->rip, &c->rport);
    if (rip)   *rip   = c->rip;
    if (rport) *rport = c->rport;
    return ns;
}

int sock_sendto(int s, const void *buf, uint32_t len, int flags,
                uint32_t ip, uint16_t port)
{
    (void)flags;                      /* MSG_* we honour is MSG_PEEK, read-side */
    sock_t *sk = sock_at(s);
    if (!sk)
        return -E_BADF;

    if (sk->type == SOCK_RAW) {
        if (!ip)
            return -E_DESTADDRREQ;
        if (len > NET_MTU - 20)
            return -E_MSGSIZE;
        int e = net_ip_output(ip, (uint8_t)sk->protocol, buf, (uint16_t)len);
        return e ? e : (int)len;
    }

    if (sk->type == SOCK_DGRAM) {
        if (!ip && sk->connected) {
            ip   = sk->rip;
            port = sk->rport;
        }
        return udp_sendto(sk, buf, len, ip, port);
    }

    /* SOCK_STREAM: a destination address is a category error. */
    if (ip || port)
        return -E_ISCONN;
    int state = tcp_state(sk->tcp);
    if (state != TCPS_ESTABLISHED && state != TCPS_CLOSE_WAIT) {
        int err = tcp_take_error(sk->tcp);
        return err ? -err : -E_NOTCONN;
    }

    for (;;) {
        int n = tcp_write(sk->tcp, buf, len);
        if (n > 0)
            return n;
        if (n < 0)
            return n;
        /* No window space: wait for an ACK to open some.  If the connection
         * left us instead, that is a broken pipe, with the signal the
         * writer's shell is expecting. */
        SOCK_WAIT_LOOP(sk, (tcp_tx_space(sk->tcp) > 0),
                       (tcp_state(sk->tcp) != TCPS_ESTABLISHED &&
                        tcp_state(sk->tcp) != TCPS_CLOSE_WAIT
                            ? (proc_signal(proc_current(), SIGPIPE), -E_PIPE)
                            : 0));
    }
}

int sock_recvfrom(int s, void *buf, uint32_t len, int flags,
                  uint32_t *ip, uint16_t *port)
{
    sock_t *sk = sock_at(s);
    if (!sk)
        return -E_BADF;

    if (sk->type == SOCK_DGRAM || sk->type == SOCK_RAW) {
        SOCK_WAIT_LOOP(sk, (sk->rx_used > 0), 0);
        return (int)ring_pop(sk, ip, port, buf, len, flags & MSG_PEEK);
    }

    /* SOCK_STREAM. */
    for (;;) {
        if (tcp_rx_avail(sk->tcp) > 0) {
            if (ip || port) {
                uint32_t lip; uint16_t lport;
                tcp_endpoints(sk->tcp, &lip, &lport, ip, port);
            }
            return tcp_read(sk->tcp, buf, len, flags & MSG_PEEK);
        }
        if (tcp_eof(sk->tcp))
            return 0;
        int state = tcp_state(sk->tcp);
        if (state == TCPS_CLOSED) {
            int e = tcp_take_error(sk->tcp);
            return e ? -e : 0;        /* clean close with nothing left: EOF */
        }
        if (state == TCPS_LISTEN)
            return -E_OPNOTSUPP;
        SOCK_WAIT_LOOP(sk,
                       (tcp_rx_avail(sk->tcp) > 0 || tcp_eof(sk->tcp) ||
                        tcp_state(sk->tcp) == TCPS_CLOSED),
                       0);
    }
}

int sock_shutdown(int s, int how)
{
    sock_t *sk = sock_at(s);
    if (!sk)
        return -E_BADF;
    if (how < SHUT_RD || how > SHUT_RDWR)
        return -E_INVAL;
    if (sk->type != SOCK_STREAM)
        return 0;                     /* meaningless on a datagram socket */
    if (how == SHUT_WR || how == SHUT_RDWR) {
        int e = tcp_close_write(sk->tcp);
        if (e)
            return e;
    }
    if (how == SHUT_RD || how == SHUT_RDWR)
        tcp_shutdown_read(sk->tcp);
    return 0;
}

/* ---- options ------------------------------------------------------------- */

int sock_setsockopt(int s, int level, int name, const void *val, uint32_t len)
{
    sock_t *sk = sock_at(s);
    if (!sk)
        return -E_BADF;
    if (!val || len < 4)
        return -E_INVAL;
    uint32_t v;
    memcpy(&v, val, 4);

    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_BROADCAST:
            sk->broadcast = v != 0;
            return 0;
        case SO_TYPE:
            return -E_INVAL;          /* read-only */
        default:
            /* SO_RCVBUF, SO_SNDBUF, SO_REUSEADDR, ...: accepted, ignored.
             * Refusing them breaks BusyBox's setup paths for no benefit;
             * the buffers are fixed-size and that is simply what they get. */
            return 0;
        }
    }
    /* IPPROTO_IP (level 0) and anything else: same policy. */
    return 0;
}

int sock_getsockopt(int s, int level, int name, void *val, uint32_t *len)
{
    sock_t *sk = sock_at(s);
    if (!sk)
        return -E_BADF;
    if (!val || !len || *len < 4)
        return -E_INVAL;

    uint32_t v = 0;
    if (level == SOL_SOCKET) {
        switch (name) {
        case SO_TYPE:
            v = (uint32_t)sk->type;
            break;
        case SO_ERROR:
            v = (uint32_t)(sk->tcp ? tcp_take_error(sk->tcp) : sk->error);
            sk->error = 0;
            break;
        case SO_BROADCAST:
            v = (uint32_t)sk->broadcast;
            break;
        default:
            v = 0;
            break;
        }
    }
    memcpy(val, &v, 4);
    *len = 4;
    return 0;
}

/* ---- readiness ------------------------------------------------------------ */

int sock_readable(int s)
{
    sock_t *sk = sock_at(s);
    if (!sk)
        return 1;                     /* a dead fd is "ready": read it and see */
    if (sk->type != SOCK_STREAM)
        return sk->rx_used > 0;
    int state = tcp_state(sk->tcp);
    if (state == TCPS_LISTEN)
        return tcp_accept_ready(sk->tcp);
    return tcp_rx_avail(sk->tcp) > 0 || tcp_eof(sk->tcp) ||
           state == TCPS_CLOSED;
}

int sock_writable(int s)
{
    sock_t *sk = sock_at(s);
    if (!sk)
        return 1;
    if (sk->type != SOCK_STREAM)
        return 1;
    int state = tcp_state(sk->tcp);
    return (state == TCPS_ESTABLISHED || state == TCPS_CLOSE_WAIT) &&
           tcp_tx_space(sk->tcp) > 0;
}

/* ---- vfs node ops ----------------------------------------------------------- */

int32_t sock_node_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)off;
    int s = (int)(uintptr_t)n->priv;
    if (s < 0)                          /* -2 - u: an AF_UNIX socket */
        return unix_node_read(n, off, buf, len);
    return sock_recvfrom(s, buf, len, 0, NULL, NULL);
}

int32_t sock_node_write(vfs_node_t *n, uint64_t off, const void *buf,
                        uint32_t len)
{
    (void)off;
    int s = (int)(uintptr_t)n->priv;
    if (s < 0)
        return unix_node_write(n, off, buf, len);
    return sock_sendto(s, buf, len, 0, 0, 0);
}
