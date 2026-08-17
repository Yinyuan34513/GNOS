/*
 * tcp.h — a small, honest TCP. (GPLv2)
 *
 * "Small" here means a specific set of decisions, all of which trade
 * throughput for the ability to read the state machine in one sitting:
 *
 *   - **In-order only.**  A segment that arrives out of order is dropped, not
 *     held in a reassembly queue.  This is legal -- the sender will retransmit
 *     -- and it removes the single largest data structure a TCP normally
 *     needs.  It costs a round trip whenever the network reorders, which on a
 *     QEMU slirp link and on loopback is never.
 *   - **One outstanding retransmission timer** for the whole connection,
 *     re-armed from the oldest unacknowledged byte, with a fixed RTO rather
 *     than a Jacobson/Karels estimate.
 *   - **No congestion control.**  The send window is whatever the peer
 *     advertised, bounded by our transmit buffer.  There is no slow start and
 *     no fast retransmit.  On a link that does not drop packets these do
 *     nothing; on one that does, this TCP will be a bad citizen, which is
 *     worth knowing before pointing it at the internet.
 *   - **No SACK, no window scaling, no timestamps.**  Options are parsed only
 *     far enough to find the MSS, and only MSS is ever sent.
 *
 * What is *not* compromised is the state machine and the sequence-number
 * arithmetic, because those are what make TCP TCP: the three-way handshake,
 * the four-way close with its two independent half-connections, TIME_WAIT,
 * and the rule that every comparison of sequence numbers is a signed
 * difference so that the 32-bit space can wrap without anybody noticing.
 */
#ifndef GNOS_TCP_H
#define GNOS_TCP_H

#include <stdint.h>

/* RFC 793's states, in its order.  Kept as an enum the socket layer can read
 * because "am I connected yet?" is a question only this table can answer. */
#define TCPS_CLOSED       0
#define TCPS_LISTEN       1
#define TCPS_SYN_SENT     2
#define TCPS_SYN_RCVD     3
#define TCPS_ESTABLISHED  4
#define TCPS_FIN_WAIT_1   5
#define TCPS_FIN_WAIT_2   6
#define TCPS_CLOSE_WAIT   7
#define TCPS_CLOSING      8
#define TCPS_LAST_ACK     9
#define TCPS_TIME_WAIT   10

typedef struct tcp_pcb tcp_pcb_t;

void tcp_init(void);

/* Called at 100 Hz from net_tick(): retransmission, TIME_WAIT expiry, and
 * the connect() timeout. */
void tcp_tick(void);

/* ---- the interface the socket layer uses ------------------------------- */
tcp_pcb_t *tcp_new(void);

/*
 * Release a control block.  A connection that is still open is *aborted*
 * (RST), not closed politely: this is what close() on an unread socket does,
 * and pretending otherwise would leave half-dead PCBs nobody can reach.
 */
void tcp_destroy(tcp_pcb_t *p);

int  tcp_bind(tcp_pcb_t *p, uint32_t ip, uint16_t port);
int  tcp_listen(tcp_pcb_t *p, int backlog);

/* Dequeue one fully established child, or NULL if none is waiting. */
tcp_pcb_t *tcp_accept(tcp_pcb_t *p);

/* Start the handshake.  Returns 0 -- completion is observed through
 * tcp_state(), because connect() may or may not be allowed to block. */
int  tcp_connect(tcp_pcb_t *p, uint32_t ip, uint16_t port);

/* Copy into the send buffer and transmit what the window allows.  Returns the
 * number of bytes accepted (may be less than `len`), or a negative errno. */
int  tcp_write(tcp_pcb_t *p, const void *buf, uint32_t len);

/* Copy out of the receive buffer.  0 means "nothing right now"; use
 * tcp_eof() to tell that apart from end of stream. */
int  tcp_read(tcp_pcb_t *p, void *buf, uint32_t len, int peek);

/* shutdown(SHUT_WR): send a FIN once the send buffer drains. */
int  tcp_close_write(tcp_pcb_t *p);
void tcp_shutdown_read(tcp_pcb_t *p);

int  tcp_state(const tcp_pcb_t *p);

/* The pending asynchronous error (ECONNREFUSED, ECONNRESET, ETIMEDOUT...),
 * consumed by the read -- exactly SO_ERROR's contract. */
int  tcp_take_error(tcp_pcb_t *p);

int  tcp_rx_avail(const tcp_pcb_t *p);
int  tcp_tx_space(const tcp_pcb_t *p);
int  tcp_eof(const tcp_pcb_t *p);            /* peer sent FIN and we drained */
int  tcp_accept_ready(const tcp_pcb_t *p);

void tcp_endpoints(const tcp_pcb_t *p, uint32_t *lip, uint16_t *lport,
                   uint32_t *rip, uint16_t *rport);

#endif
