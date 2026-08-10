/*
 * tcp.c — the state machine tcp.h promised. (GPLv2)
 *
 * The file is organised the way a segment travels: allocation and the send
 * path first, then tcp_input() (which is most of the file, because receiving
 * is where every state transition lives), then the timers.
 *
 * Two conventions carry the whole design:
 *
 *   - Every sequence-number comparison goes through seq_lt()/seq_leq(), which
 *     subtract and look at the sign.  That is the entire trick behind a
 *     32-bit sequence space that wraps: "a before b" means a - b, read as a
 *     signed integer, is negative.  Never compare them directly.
 *
 *   - The two buffers are byte rings indexed by sequence number, not by
 *     position: slot = seq & (TCP_BUF_SIZE - 1).  Because both directions are
 *     strictly in-order, each ring holds one contiguous half-open interval of
 *     the sequence space -- receive holds [rcv_read, rcv_nxt), send holds
 *     [snd_una, snd_end) -- and no other bookkeeping is needed.  Wraparound
 *     of the ring and of the sequence space falls out of the same mask.
 *
 * Locks: none, on purpose; see "where this code runs" in net.h.  tcp_input()
 * executes inside net_poll(), from an interrupt or from a system call that
 * polled; the socket layer wraps its own calls in cli/sti; tcp_tick() runs
 * from the timer interrupt.  The one rule that keeps this sound is that
 * nothing here blocks and nothing here can be re-entered: net_ip_output()
 * queues on loopback rather than recursing (see net.c), which is what makes
 * "send an ACK from inside tcp_input()" safe.
 */
#include "tcp.h"
#include "net.h"
#include "pmm.h"
#include "timer.h"
#include "vfs.h"
#include "kstring.h"

/* Segment flags, as they appear in the flags byte. */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

#define TCP_MSS       (NET_MTU - 40)   /* 1460: 20-byte IP and TCP headers */
#define TCP_BUF_SIZE  4096             /* one page each way, indexed by seq */
#define TCP_MAX_PCB   8
#define TCP_BACKLOG   4

/* All timers in 100 Hz ticks.  The RTO is fixed (tcp.h explains why); one
 * second is a defensible value on a LAN or a slirp link, and five retries
 * is where we concede. */
#define TCP_RTO_TICKS    100
#define TCP_MAX_RETRIES  5
#define TCP_TIME_WAIT    200           /* 2 s standing in for 2*MSL */
#define TCP_CONN_TICKS   500           /* connect() gives up after 5 s */

#define EPHEMERAL_FIRST  49152

struct tcp_pcb {
    int      used;
    int      state;
    uint32_t lip, rip;          /* host byte order, like everywhere above L3 */
    uint16_t lport, rport;

    /* Send side: [snd_una, snd_end) lives in the tx ring, with snd_nxt
     * between them marking what has actually been transmitted. */
    uint32_t snd_una, snd_nxt, snd_end;
    uint16_t snd_wnd;           /* the peer's latest advertised window */
    uint16_t snd_mss;           /* the peer's MSS, clamped to ours */
    int      fin_queued;        /* shutdown(SHUT_WR): FIN goes out at snd_end */
    int      fin_sent;          /* FIN occupies sequence number snd_end */

    /* Receive side: [rcv_read, rcv_nxt) waits in the rx ring for the
     * application.  In-order only, so this interval is the whole story. */
    uint32_t rcv_nxt, rcv_read;
    int      fin_seen;          /* the peer's FIN consumed a sequence number */
    int      rx_shutdown;       /* shutdown(SHUT_RD): ACK, but keep nothing */

    uint64_t rx_phys, tx_phys;  /* the pages behind the rings */

    uint64_t rto_at;            /* retransmission deadline, 0 = disarmed */
    int      retries;
    uint64_t tw_at;             /* TIME_WAIT expiry, 0 = not in TIME_WAIT */
    uint64_t conn_at;           /* connect() deadline, 0 = none */

    int      error;             /* pending async error, SO_ERROR style */

    /* Listen bookkeeping: a child remembers its parent until accepted; the
     * parent keeps a fixed queue of established children.  A full queue
     * means the application is not accept()ing fast enough, and the child
     * is RST -- exactly what a real stack does at backlog overflow. */
    struct tcp_pcb *parent;
    struct tcp_pcb *accept_q[TCP_BACKLOG];
    int             accept_n;
};

static tcp_pcb_t g_pcbs[TCP_MAX_PCB];

/* "a is before b" in a 32-bit sequence space that wraps. */
static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static int seq_leq(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

static uint32_t min3(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t m = a < b ? a : b;
    return m < c ? m : c;
}

/* Something that changes every connection and is not trivially predictable
 * from the last one; RFC 6528 would like a keyed hash, ticks plus one turn
 * of a golden-ratio LCG will do here. */
static uint32_t next_iss(void)
{
    static uint32_t salt = 0x9E3779B9u;
    salt = salt * 1664525u + 1013904223u;
    return (uint32_t)timer_ticks() * 64000u + salt;
}

/* ---- allocation --------------------------------------------------------- */

static void pcb_free(tcp_pcb_t *p)
{
    if (p->rx_phys) pmm_free(p->rx_phys);
    if (p->tx_phys) pmm_free(p->tx_phys);
    p->used = 0;               /* the rest of the struct is now stale by definition */
}

static tcp_pcb_t *pcb_alloc(void)
{
    for (int i = 0; i < TCP_MAX_PCB; i++) {
        if (g_pcbs[i].used)
            continue;
        tcp_pcb_t *p = &g_pcbs[i];
        memset(p, 0, sizeof(*p));
        p->rx_phys = pmm_alloc_zeroed();
        p->tx_phys = pmm_alloc_zeroed();
        if (!p->rx_phys || !p->tx_phys) {
            pcb_free(p);       /* tolerates the half-allocated case */
            return NULL;
        }
        p->used = 1;
        p->state = TCPS_CLOSED;
        p->snd_wnd = TCP_BUF_SIZE;   /* until the peer says otherwise */
        p->snd_mss = TCP_MSS;
        return p;
    }
    return NULL;
}

tcp_pcb_t *tcp_new(void)
{
    return pcb_alloc();
}

void tcp_destroy(tcp_pcb_t *p)
{
    /* An open connection is aborted, not closed: close() on a socket with
     * unread data is precisely the case where the peer must hear about it. */
    if (p->state == TCPS_SYN_RCVD   || p->state == TCPS_ESTABLISHED ||
        p->state == TCPS_FIN_WAIT_1 || p->state == TCPS_FIN_WAIT_2  ||
        p->state == TCPS_CLOSE_WAIT || p->state == TCPS_CLOSING     ||
        p->state == TCPS_LAST_ACK) {
        uint32_t src = p->lip ? p->lip : net_route_src(p->rip);
        uint8_t seg[20];
        memset(seg, 0, sizeof(seg));
        net_put16(seg + 0, p->lport);
        net_put16(seg + 2, p->rport);
        net_put32(seg + 4, p->snd_nxt);
        net_put32(seg + 8, p->rcv_nxt);
        seg[12] = 5 << 4;
        seg[13] = TCP_RST | TCP_ACK;
        net_put16(seg + 16, net_checksum_pseudo(src, p->rip, IP_PROTO_TCP,
                                                seg, sizeof(seg)));
        net_ip_output(p->rip, IP_PROTO_TCP, seg, sizeof(seg));
    }

    /* Unlink from a parent's accept queue if we were never collected. */
    if (p->parent) {
        tcp_pcb_t *par = p->parent;
        for (int i = 0; i < par->accept_n; i++)
            if (par->accept_q[i] == p) {
                par->accept_q[i] = par->accept_q[--par->accept_n];
                break;
            }
    }
    pcb_free(p);
}

/* ---- ports and binding --------------------------------------------------- */

static int port_in_use(uint32_t ip, uint16_t port)
{
    for (int i = 0; i < TCP_MAX_PCB; i++) {
        tcp_pcb_t *p = &g_pcbs[i];
        if (!p->used || p->lport != port)
            continue;
        /* A conflict needs matching addresses too, and ANY matches anything
         * -- the same rule bind() has always had. */
        if (p->lip == IP_ANY || ip == IP_ANY || p->lip == ip)
            return 1;
    }
    return 0;
}

int tcp_bind(tcp_pcb_t *p, uint32_t ip, uint16_t port)
{
    if (p->state != TCPS_CLOSED)
        return -E_INVAL;
    if (ip != IP_ANY && !net_is_local(ip))
        return -E_ADDRNOTAVAIL;
    if (port == 0)
        return -E_INVAL;
    if (port_in_use(ip, port))
        return -E_ADDRINUSE;
    p->lip = ip;
    p->lport = port;
    return 0;
}

/* An unbound connect() or listen() gets an ephemeral port.  Linear probe
 * from a rotating start; the pool is eight PCBs, so this terminates. */
static uint16_t ephemeral_port(void)
{
    static uint16_t next;
    if (next < EPHEMERAL_FIRST)
        next = EPHEMERAL_FIRST;
    for (int i = 0; i < 65535 - EPHEMERAL_FIRST; i++) {
        uint16_t port = next++;
        if (next < EPHEMERAL_FIRST)
            next = EPHEMERAL_FIRST;
        if (!port_in_use(IP_ANY, port))
            return port;
    }
    return 0;
}

/* ---- segment construction ------------------------------------------------- */

/*
 * Build and emit one segment.  `data` may be NULL for a pure control
 * segment.  SYNs carry our MSS option and nothing else; everything else is
 * a plain 20-byte header.
 */
static void tcp_emit(tcp_pcb_t *p, uint32_t seq, uint8_t flags,
                     const void *data, uint32_t len)
{
    uint8_t  hdr[24];
    uint32_t hdrlen = (flags & TCP_SYN) ? 24 : 20;
    uint32_t total  = hdrlen + len;
    uint8_t  seg[20 + 4 + TCP_MSS];
    uint32_t src = p->lip ? p->lip : net_route_src(p->rip);
    uint32_t wnd = TCP_BUF_SIZE - (p->rcv_nxt - p->rcv_read);

    memset(hdr, 0, sizeof(hdr));
    net_put16(hdr + 0, p->lport);
    net_put16(hdr + 2, p->rport);
    net_put32(hdr + 4, seq);
    net_put32(hdr + 8, (flags & TCP_ACK) ? p->rcv_nxt : 0);
    hdr[12] = (uint8_t)((hdrlen / 4) << 4);
    hdr[13] = flags;
    net_put16(hdr + 14, (uint16_t)wnd);
    if (flags & TCP_SYN) {
        hdr[20] = 2;                    /* MSS option */
        hdr[21] = 4;
        net_put16(hdr + 22, TCP_MSS);
    }

    memcpy(seg, hdr, hdrlen);
    if (len)
        memcpy(seg + hdrlen, data, len);
    net_put16(seg + 16, net_checksum_pseudo(src, p->rip, IP_PROTO_TCP,
                                            seg, total));
    net_ip_output(p->rip, IP_PROTO_TCP, seg, (uint16_t)total);
}

static void tcp_send_ack(tcp_pcb_t *p)
{
    tcp_emit(p, p->snd_nxt, TCP_ACK, NULL, 0);
}

/* RFC 793's reset generation for a segment that matched no socket: never
 * answer a RST, steal the ACK's sequence number when there is one. */
static void tcp_send_rst(uint32_t src, uint32_t dst, const uint8_t *seg,
                         uint32_t dlen, uint8_t flags)
{
    if (flags & TCP_RST)
        return;
    tcp_pcb_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.lip   = dst;
    tmp.lport = net_get16(seg + 2);
    tmp.rip   = src;
    tmp.rport = net_get16(seg + 0);
    if (flags & TCP_ACK) {
        tcp_emit(&tmp, net_get32(seg + 8), TCP_RST, NULL, 0);
    } else {
        tmp.rcv_nxt = net_get32(seg + 4) + dlen +
                      ((flags & TCP_SYN) ? 1 : 0) + ((flags & TCP_FIN) ? 1 : 0);
        tcp_emit(&tmp, 0, TCP_RST | TCP_ACK, NULL, 0);
    }
}

static void rto_arm(tcp_pcb_t *p)
{
    p->rto_at = timer_ticks() + TCP_RTO_TICKS;
}

/* ---- the send path --------------------------------------------------------- */

/*
 * Push out whatever the window allows from the unsent part of the tx ring,
 * then the FIN if one is queued and all data has gone.  One segment at a
 * time, called after every write and every window-opening ACK; there is no
 * Nagle here because there is no second segment to wait for -- the buffer
 * is one page.
 */
static void tcp_output(tcp_pcb_t *p)
{
    if (p->state != TCPS_ESTABLISHED && p->state != TCPS_CLOSE_WAIT)
        return;

    uint32_t in_flight = p->snd_nxt - p->snd_una;
    uint32_t wnd_left  = p->snd_wnd > in_flight ? p->snd_wnd - in_flight : 0;
    uint32_t unsent    = p->snd_end - p->snd_nxt;
    uint32_t n         = min3(unsent, wnd_left, p->snd_mss);

    if (n) {
        /* The ring may wrap inside this segment; emit byte-by-byte into a
         * scratch buffer rather than teaching tcp_emit about rings.  With
         * 4 KiB buffers this is at most one page of copying. */
        uint8_t *tx = pmm_virt(p->tx_phys);
        uint8_t  scratch[TCP_MSS];
        for (uint32_t i = 0; i < n; i++)
            scratch[i] = tx[(p->snd_nxt + i) & (TCP_BUF_SIZE - 1)];
        tcp_emit(p, p->snd_nxt, TCP_ACK | TCP_PSH, scratch, n);
        p->snd_nxt += n;
        rto_arm(p);
    }

    if (p->fin_queued && !p->fin_sent && p->snd_nxt == p->snd_end) {
        tcp_emit(p, p->snd_nxt, TCP_ACK | TCP_FIN, NULL, 0);
        p->fin_sent = 1;
        p->snd_nxt++;                 /* the FIN owns a sequence number */
        rto_arm(p);
        if (p->state == TCPS_ESTABLISHED)
            p->state = TCPS_FIN_WAIT_1;
        else if (p->state == TCPS_CLOSE_WAIT)
            p->state = TCPS_LAST_ACK;
    }
}

int tcp_write(tcp_pcb_t *p, const void *buf, uint32_t len)
{
    if (p->state != TCPS_ESTABLISHED && p->state != TCPS_CLOSE_WAIT)
        return p->error ? -p->error : -E_NOTCONN;
    if (p->fin_queued)
        return -E_PIPE;

    uint32_t space = TCP_BUF_SIZE - (p->snd_end - p->snd_una);
    uint32_t n = len < space ? len : space;
    if (!n)
        return 0;                     /* windowed out; the caller may block */

    uint8_t *tx = pmm_virt(p->tx_phys);
    const uint8_t *src = buf;
    for (uint32_t i = 0; i < n; i++)
        tx[(p->snd_end + i) & (TCP_BUF_SIZE - 1)] = src[i];
    p->snd_end += n;
    tcp_output(p);
    return (int)n;
}

int tcp_close_write(tcp_pcb_t *p)
{
    if (p->state != TCPS_ESTABLISHED && p->state != TCPS_CLOSE_WAIT)
        return -E_NOTCONN;
    p->fin_queued = 1;
    tcp_output(p);
    return 0;
}

void tcp_shutdown_read(tcp_pcb_t *p)
{
    p->rcv_read = p->rcv_nxt;         /* drop whatever is buffered */
    p->rx_shutdown = 1;
}

/* ---- the receive path ------------------------------------------------------- */

static void mss_from_options(tcp_pcb_t *p, const uint8_t *seg, uint32_t hdrlen)
{
    uint32_t i = 20;
    while (i + 2 <= hdrlen) {
        uint8_t kind = seg[i];
        if (kind == 0)
            break;
        if (kind == 1) { i++; continue; }
        uint8_t olen = seg[i + 1];
        if (olen < 2 || i + olen > hdrlen)
            break;
        if (kind == 2 && olen == 4) {
            uint16_t mss = net_get16(seg + i + 2);
            p->snd_mss = mss < TCP_MSS ? mss : TCP_MSS;
        }
        i += olen;
    }
}

/* Find the pcb a segment belongs to: an exact four-tuple match first, a
 * listener on the port second.  Returns the *matched* pcb, which for a
 * listener is the listener itself. */
static tcp_pcb_t *pcb_lookup(uint32_t src, uint32_t dst,
                             uint16_t sport, uint16_t dport)
{
    tcp_pcb_t *listener = NULL;
    for (int i = 0; i < TCP_MAX_PCB; i++) {
        tcp_pcb_t *p = &g_pcbs[i];
        if (!p->used || p->lport != dport)
            continue;
        if (p->state == TCPS_LISTEN) {
            if (p->lip == IP_ANY || p->lip == dst)
                listener = p;
            continue;
        }
        if (p->state == TCPS_CLOSED)
            continue;
        if (p->rport == sport && p->rip == src &&
            (p->lip == IP_ANY || p->lip == dst))
            return p;
    }
    return listener;
}

static void syn_arrived(tcp_pcb_t *listener, uint32_t src, uint32_t dst,
                        const uint8_t *seg, uint32_t hdrlen)
{
    tcp_pcb_t *c = pcb_alloc();
    if (!c)
        return;                       /* no resources: stay silent, they'll retry */

    c->lip     = dst;
    c->lport   = listener->lport;
    c->rip     = src;
    c->rport   = net_get16(seg + 0);
    c->rcv_nxt = net_get32(seg + 4) + 1;
    c->rcv_read = c->rcv_nxt;
    c->snd_una = c->snd_nxt = c->snd_end = next_iss();
    c->snd_wnd = net_get16(seg + 14);
    c->parent  = listener;
    c->state   = TCPS_SYN_RCVD;
    mss_from_options(c, seg, hdrlen);

    tcp_emit(c, c->snd_una, TCP_SYN | TCP_ACK, NULL, 0);
    c->snd_nxt++;                     /* the SYN owns a sequence number */
    rto_arm(c);
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, uint16_t len)
{
    if (len < 20)
        return;
    if (net_checksum_pseudo(src, dst, IP_PROTO_TCP, seg, len) != 0)
        return;                       /* a bad checksum means the segment never happened */

    uint16_t sport  = net_get16(seg + 0);
    uint16_t dport  = net_get16(seg + 2);
    uint32_t sseq   = net_get32(seg + 4);
    uint32_t sack   = net_get32(seg + 8);
    uint32_t hdrlen = (uint32_t)(seg[12] >> 4) * 4;
    uint8_t  flags  = seg[13];
    if (hdrlen < 20 || hdrlen > len)
        return;
    uint32_t dlen = len - hdrlen;

    tcp_pcb_t *p = pcb_lookup(src, dst, sport, dport);
    if (!p) {
        tcp_send_rst(src, dst, seg, dlen, flags);
        return;
    }

    /* ---- LISTEN: only a SYN is interesting ---------------------------- */
    if (p->state == TCPS_LISTEN) {
        if (flags & TCP_RST)
            return;
        if (flags & TCP_ACK) {
            tcp_send_rst(src, dst, seg, dlen, flags);
            return;
        }
        if (flags & TCP_SYN)
            syn_arrived(p, src, dst, seg, hdrlen);
        return;
    }

    /* ---- RST: fatal everywhere except LISTEN ---------------------------- */
    if (flags & TCP_RST) {
        if (p->state == TCPS_SYN_SENT)
            p->error = E_CONNREFUSED;
        else
            p->error = E_CONNRESET;
        p->state = TCPS_CLOSED;       /* the struct lingers for SO_ERROR */
        p->rto_at = 0;
        return;
    }

    /* ---- SYN_SENT: the handshake's second leg --------------------------- */
    if (p->state == TCPS_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
            sack == p->snd_nxt) {
            p->rcv_nxt  = sseq + 1;
            p->rcv_read = p->rcv_nxt;
            p->snd_una  = sack;
            p->snd_wnd  = net_get16(seg + 14);
            p->rto_at   = 0;
            p->conn_at  = 0;
            p->state    = TCPS_ESTABLISHED;
            mss_from_options(p, seg, hdrlen);
            tcp_send_ack(p);
        }
        return;                       /* anything else: wait for retransmit */
    }

    /* Every state from here on acknowledges. */
    if (flags & TCP_ACK) {
        if (p->state == TCPS_SYN_RCVD) {
            if (sack != p->snd_nxt) {
                tcp_send_rst(src, dst, seg, dlen, flags);
                return;
            }
            p->snd_una = sack;
            p->rto_at  = 0;
            p->state   = TCPS_ESTABLISHED;
            tcp_pcb_t *par = p->parent;
            /* par->used: a closed listener's pcb may already be recycled. */
            if (par && par->used && par->state == TCPS_LISTEN &&
                par->accept_n < TCP_BACKLOG)
                par->accept_q[par->accept_n++] = p;
            /* else: the pcb stays SYN_RCVD-less but established; a full
             * backlog is the application's problem to drain, the connection
             * itself is fine. */
        } else if (seq_lt(p->snd_una, sack) && seq_leq(sack, p->snd_nxt)) {
            p->snd_una = sack;
            p->retries = 0;
            if (p->snd_una == p->snd_nxt)
                p->rto_at = 0;        /* everything acknowledged */
            else
                rto_arm(p);           /* partial progress: fresh timer */
            tcp_output(p);            /* the window may have opened */
        }

        /* FIN-acknowledged transitions.  fin_sent means the FIN owns
         * snd_nxt - 1, so snd_una == snd_nxt covers it. */
        if (p->fin_sent && p->snd_una == p->snd_nxt) {
            if (p->state == TCPS_FIN_WAIT_1)
                p->state = TCPS_FIN_WAIT_2;
            else if (p->state == TCPS_CLOSING) {
                p->state = TCPS_TIME_WAIT;
                p->tw_at = timer_ticks() + TCP_TIME_WAIT;
            } else if (p->state == TCPS_LAST_ACK)
                p->state = TCPS_CLOSED;
        }
    }

    if (p->state == TCPS_CLOSED)
        return;

    /* ---- sequence check: this TCP accepts only the next expected byte ---- */
    uint32_t consumes = dlen + ((flags & TCP_FIN) ? 1 : 0);
    if (sseq != p->rcv_nxt) {
        /* A retransmission of what we already have, or something from the
         * future we choose not to hold.  Either way the peer needs our
         * current rcv_nxt; either way nothing changes here.  Pure-ACK
         * keepalives (one byte behind) are answered but not counted. */
        if (consumes || seq_leq(sseq + consumes, p->rcv_nxt))
            tcp_send_ack(p);
        return;
    }

    if (dlen) {
        uint32_t space = TCP_BUF_SIZE - (p->rcv_nxt - p->rcv_read);
        uint32_t n = dlen < space ? dlen : space;
        if (!p->rx_shutdown && n) {
            uint8_t *rx = pmm_virt(p->rx_phys);
            for (uint32_t i = 0; i < n; i++)
                rx[(p->rcv_nxt + i) & (TCP_BUF_SIZE - 1)] = seg[hdrlen + i];
        }
        /* Truncation (space < dlen) is legal: the rest is retransmitted and
         * will be "out of order" until the application drains the ring. */
        p->rcv_nxt += p->rx_shutdown ? dlen : n;
    }

    if (flags & TCP_FIN) {
        p->rcv_nxt++;                 /* the FIN owns a sequence number */
        p->fin_seen = 1;
        if (p->state == TCPS_ESTABLISHED)
            p->state = TCPS_CLOSE_WAIT;
        else if (p->state == TCPS_FIN_WAIT_1) {
            /* Our FIN may already be acknowledged; if so both halves are
             * done and this is TIME_WAIT, otherwise CLOSING. */
            if (p->fin_sent && p->snd_una == p->snd_nxt) {
                p->state = TCPS_TIME_WAIT;
                p->tw_at = timer_ticks() + TCP_TIME_WAIT;
            } else
                p->state = TCPS_CLOSING;
        } else if (p->state == TCPS_FIN_WAIT_2) {
            p->state = TCPS_TIME_WAIT;
            p->tw_at = timer_ticks() + TCP_TIME_WAIT;
        }
    }

    if (dlen || (flags & TCP_FIN))
        tcp_send_ack(p);
}

/* ---- the timers ------------------------------------------------------------- */

static void tcp_retransmit(tcp_pcb_t *p)
{
    if (++p->retries > TCP_MAX_RETRIES) {
        p->error = E_TIMEDOUT;
        p->state = TCPS_CLOSED;
        p->rto_at = 0;
        return;
    }

    if (p->state == TCPS_SYN_SENT) {
        tcp_emit(p, p->snd_una, TCP_SYN, NULL, 0);
    } else if (p->state == TCPS_SYN_RCVD) {
        tcp_emit(p, p->snd_una, TCP_SYN | TCP_ACK, NULL, 0);
    } else {
        /* Data from snd_una, with the FIN re-attached if it was the last
         * thing out.  snd_una == snd_end with fin_sent means the FIN alone
         * is outstanding, and this degenerates to a bare FIN. */
        uint32_t have = p->snd_end - p->snd_una;
        uint32_t n = have < p->snd_mss ? have : p->snd_mss;
        int with_fin = p->fin_sent && (p->snd_una + n == p->snd_end);
        uint8_t flags = TCP_ACK | (n ? TCP_PSH : 0) | (with_fin ? TCP_FIN : 0);
        if (n) {
            uint8_t *tx = pmm_virt(p->tx_phys);
            uint8_t  scratch[TCP_MSS];
            for (uint32_t i = 0; i < n; i++)
                scratch[i] = tx[(p->snd_una + i) & (TCP_BUF_SIZE - 1)];
            tcp_emit(p, p->snd_una, flags, scratch, n);
        } else {
            tcp_emit(p, p->snd_una, flags, NULL, 0);
        }
    }
    rto_arm(p);
}

void tcp_tick(void)
{
    uint64_t now = timer_ticks();
    for (int i = 0; i < TCP_MAX_PCB; i++) {
        tcp_pcb_t *p = &g_pcbs[i];
        if (!p->used)
            continue;
        if (p->rto_at && now >= p->rto_at)
            tcp_retransmit(p);
        if (p->conn_at && now >= p->conn_at) {
            p->error  = E_TIMEDOUT;
            p->state  = TCPS_CLOSED;
            p->conn_at = 0;
            p->rto_at = 0;
        }
        if (p->tw_at && now >= p->tw_at) {
            p->state = TCPS_CLOSED;
            p->tw_at = 0;
        }
    }
}

/* ---- the rest of the socket-layer interface -------------------------------- */

int tcp_listen(tcp_pcb_t *p, int backlog)
{
    (void)backlog;                    /* the queue is fixed at TCP_BACKLOG */
    if (p->state != TCPS_CLOSED)
        return -E_INVAL;
    if (!p->lport) {
        p->lport = ephemeral_port();
        if (!p->lport)
            return -E_ADDRINUSE;
    }
    p->state = TCPS_LISTEN;
    return 0;
}

int tcp_connect(tcp_pcb_t *p, uint32_t ip, uint16_t port)
{
    if (p->state != TCPS_CLOSED)
        return p->state == TCPS_ESTABLISHED ? -E_ISCONN : -E_ALREADY;
    if (!p->lport) {
        p->lport = ephemeral_port();
        if (!p->lport)
            return -E_ADDRINUSE;
    }
    if (!net_route_src(ip))
        return -E_HOSTUNREACH;

    p->rip     = ip;
    p->rport   = port;
    p->snd_una = p->snd_nxt = p->snd_end = next_iss();
    p->state   = TCPS_SYN_SENT;
    tcp_emit(p, p->snd_una, TCP_SYN, NULL, 0);
    p->snd_nxt++;                     /* the SYN owns a sequence number */
    rto_arm(p);
    p->conn_at = timer_ticks() + TCP_CONN_TICKS;
    return 0;
}

tcp_pcb_t *tcp_accept(tcp_pcb_t *p)
{
    if (p->state != TCPS_LISTEN || !p->accept_n)
        return NULL;
    tcp_pcb_t *c = p->accept_q[0];
    for (int i = 1; i < p->accept_n; i++)
        p->accept_q[i - 1] = p->accept_q[i];
    p->accept_n--;
    c->parent = NULL;                 /* accepted children outlive the listener */
    return c;
}

int tcp_read(tcp_pcb_t *p, void *buf, uint32_t len, int peek)
{
    uint32_t avail = p->rcv_nxt - p->rcv_read;
    uint32_t n = len < avail ? len : avail;
    if (n) {
        const uint8_t *rx = pmm_virt(p->rx_phys);
        uint8_t *dst = buf;
        for (uint32_t i = 0; i < n; i++)
            dst[i] = rx[(p->rcv_read + i) & (TCP_BUF_SIZE - 1)];
        if (!peek)
            p->rcv_read += n;
    }
    return (int)n;
}

int tcp_state(const tcp_pcb_t *p) { return p->state; }

int tcp_take_error(tcp_pcb_t *p)
{
    int e = p->error;
    p->error = 0;
    return e;
}

int tcp_rx_avail(const tcp_pcb_t *p)
{
    return (int)(p->rcv_nxt - p->rcv_read);
}

int tcp_tx_space(const tcp_pcb_t *p)
{
    return (int)(TCP_BUF_SIZE - (p->snd_end - p->snd_una));
}

int tcp_eof(const tcp_pcb_t *p)
{
    return p->fin_seen && p->rcv_read == p->rcv_nxt;
}

int tcp_accept_ready(const tcp_pcb_t *p)
{
    return p->accept_n > 0;
}

void tcp_endpoints(const tcp_pcb_t *p, uint32_t *lip, uint16_t *lport,
                   uint32_t *rip, uint16_t *rport)
{
    if (lip)   *lip   = p->lip ? p->lip : net_route_src(p->rip);
    if (lport) *lport = p->lport;
    if (rip)   *rip   = p->rip;
    if (rport) *rport = p->rport;
}
