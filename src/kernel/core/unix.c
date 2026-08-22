/*
 * unix.c — AF_UNIX stream sockets. (GPLv2)
 *
 * The transport every Wayland client and wlroots itself rides on: libwayland
 * connects to a pathname socket (/tmp/wayland-0), pushes wl_buffer fds
 * across with SCM_RIGHTS, and drives the whole thing from epoll.  This
 * module provides exactly that: pathname bind/connect, listen/accept, a
 * byte ring at each peer end, blocking and O_NONBLOCK reads, poll
 * readiness, and fd passing through sendmsg/recvmsg.
 *
 * Sockets live in their own table like sock.c's; the syscall layer hands
 * out a *negative* index (vfs priv = -2 - u) so the two domains share the
 * VFS socket kind without ever colliding.  Data travels in kernel memory:
 * write() into the peer's ring, read() out of one's own, so the network
 * stack never gets involved.
 */
#include <stddef.h>
#include <stdint.h>
#include "vfs.h"
#include "heap.h"
#include "proc.h"
#include "kstring.h"
#include "syscall.h"
#include "sock.h"

#define UNIX_MAX   16
#define UNIX_RING  8192
#define UNIX_BACKLOG 8

#define UNIX_NAME_MAX 108

/* SCM_RIGHTS queue depth, in file handles. */
#define UNIX_FDS_MAX 16

typedef struct unix_sock {
    int      used;
    int      nonblock;

    uint8_t *rx;                     /* byte ring: incoming data */
    uint32_t head, tail, rused;
    uint32_t cap;

    int      fds[UNIX_FDS_MAX];      /* handles awaiting recvmsg (SCM_RIGHTS) */
    int      n_fds;

    char     bound[UNIX_NAME_MAX];   /* pathname we are bound to */
    int      has_path;               /* bound to a pathname (vs socketpair) */

    int      listening;
    struct unix_sock *peer;          /* connected stream peer */
    struct unix_sock *backlog[UNIX_BACKLOG];
    int      n_backlog;

    int      shut_rx, shut_tx;       /* shutdown() halves */
    int      peer_closed;            /* the peer end went away / shut down */
} unix_t;

static unix_t g_unix[UNIX_MAX];

static unix_t *unix_at(int u)
{
    if (u < 0 || u >= UNIX_MAX || !g_unix[u].used)
        return NULL;
    return &g_unix[u];
}

/* ---- the byte ring ------------------------------------------------------- */

static void ring_wake(void)
{
    sched_wake_reason(WAIT_PIPE);
}

static void ring_put(unix_t *u, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) {
        u->rx[u->head] = p[i];
        u->head = (u->head + 1) % u->cap;
    }
    u->rused += len;
}

static void ring_get(unix_t *u, void *out, uint32_t len)
{
    uint8_t *p = (uint8_t *)out;
    for (uint32_t i = 0; i < len; i++) {
        p[i] = u->rx[u->tail];
        u->tail = (u->tail + 1) % u->cap;
    }
    u->rused -= len;
}

/* ---- lifecycle ------------------------------------------------------------ */

int unix_create(int type, int protocol)
{
    int nonblock = type & SOCK_NONBLOCK;
    type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (type != SOCK_STREAM)
        return -E_SOCKTNOSUPPORT;
    if (protocol && protocol != 1)
        return -E_PROTONOSUPPORT;

    for (int i = 0; i < UNIX_MAX; i++) {
        if (g_unix[i].used)
            continue;
        unix_t *u = &g_unix[i];
        memset(u, 0, sizeof(*u));
        u->rx       = kmalloc(UNIX_RING);
        if (!u->rx)
            return -E_NOMEM;
        u->cap      = UNIX_RING;
        u->nonblock = nonblock;
        u->used     = 1;
        return i;
    }
    return -E_NOBUFS;
}

void unix_close(int u)
{
    unix_t *s = unix_at(u);
    if (!s)
        return;
#ifdef SYSTRACE
    {
        extern void dbg_puts(const char *);
        extern void dbg_puts_dec(uint32_t);
        extern void dbg_puts_hex(unsigned long);
        dbg_puts("UCLOSE p=");
        dbg_puts_dec((uint32_t)(proc_current() ? proc_current()->pid : 0));
        dbg_puts(" u=");
        dbg_puts_dec((uint32_t)u);
        dbg_puts(" lst=");
        dbg_puts_dec((uint32_t)s->listening);
        dbg_puts(" path=");
        dbg_puts(s->has_path ? s->bound : "-");
        dbg_puts("\n");
    }
#endif

    /* Wake the peer so a parked reader sees EOF; break its back-pointer so
     * a later write from that side gets -EPIPE instead of a stale pointer.
     * Pending SCM_RIGHTS handles were ref'd for the receiver, so the unrefs
     * here balance them if nobody ever recvmsg'd them. */
    if (s->peer) {
        s->peer->peer_closed = 1;
        s->peer->peer = NULL;
        ring_wake();
    }
    for (int i = 0; i < s->n_fds; i++)
        vfs_file_unref(s->fds[i]);
    kfree(s->rx);
    s->used = 0;
}

/* ---- address handling (pathname) ----------------------------------------- */

static int unix_bind(int u, const char *path, uint32_t len)
{
    unix_t *s = unix_at(u);
    if (!s)
        return -E_BADF;
    if (s->has_path || s->listening || s->peer)
        return -E_INVAL;
    if (len == 0 || len > UNIX_NAME_MAX - 1)
        return -E_NAMETOOLONG;
    if (path[0] != '/')
        return -E_INVAL;                /* no abstract sockets here */

    for (int i = 0; i < UNIX_MAX; i++)
        if (g_unix[i].used && g_unix[i].has_path &&
            !memcmp(g_unix[i].bound, path, len + 1) && i != u)
            return -E_ADDRINUSE;

    memcpy(s->bound, path, len);
    s->bound[len] = 0;
    s->has_path = 1;
    return 0;
}

/* Find the listening socket bound to `path`. */
static unix_t *unix_listener(const char *path, uint32_t len)
{
    for (int i = 0; i < UNIX_MAX; i++) {
        unix_t *s = &g_unix[i];
        if (s->used && s->has_path && s->listening &&
            !memcmp(s->bound, path, len + 1))
            return s;
    }
    return NULL;
}

/* ---- connect / listen / accept ------------------------------------------- */

int unix_listen(int u, int backlog)
{
    unix_t *s = unix_at(u);
    if (!s)
        return -E_BADF;
    if (s->peer)
        return -E_ISCONN;
    if (!s->has_path)
        return -E_INVAL;
    if (backlog > UNIX_BACKLOG)
        backlog = UNIX_BACKLOG;
    s->listening = 1;
    (void)backlog;                      /* one deep queue of fixed size */
    return 0;
}

int unix_connect(int u, const char *path, uint32_t len)
{
    unix_t *s = unix_at(u);
    if (!s)
        return -E_BADF;
    if (s->peer)
        return -E_ISCONN;
    if (s->listening)
        return -E_INVAL;
    if (len == 0 || len > UNIX_NAME_MAX - 1)
        return -E_NAMETOOLONG;

    unix_t *l = unix_listener(path, len);
    if (!l)
        return -E_CONNREFUSED;
    if (l->n_backlog >= UNIX_BACKLOG)
        return -E_CONNREFUSED;

    /* Pair the socket with a fresh child owned by the listener.  The child
     * is what accept() will hand back, so this is where the byte rings
     * meet: writes from the client land in the child's ring. */
    int ci = unix_create(SOCK_STREAM, 0);
    if (ci < 0)
        return ci;
    unix_t *c = unix_at(ci);
    c->peer          = s;
    c->peer_closed   = 0;
    s->peer          = c;
    l->backlog[l->n_backlog++] = c;
    ring_wake();
    return 0;
}

int unix_accept(int u)
{
    unix_t *s = unix_at(u);
    if (!s || !s->listening)
        return -E_BADF;
    if (s->n_backlog == 0) {
        if (s->nonblock)
            return -E_AGAIN;
        for (;;) {
            sched_block_timeout(WAIT_PIPE, 0);
            if (s->n_backlog > 0)
                break;
            if (!s->used)               /* listener closed under us */
                return -E_BADF;
        }
    }

    unix_t *c = s->backlog[0];
    for (int i = 1; i < s->n_backlog; i++)
        s->backlog[i - 1] = s->backlog[i];
    s->n_backlog--;

    for (int i = 0; i < UNIX_MAX; i++)
        if (&g_unix[i] == c)
            return i;
    return -E_IO;                       /* unreachable */
}

/* ---- read / write -------------------------------------------------------- */

/* Fill `len` bytes of `buf` from the socket.  Returns bytes copied (0 =
 * EOF when the peer is gone), or a negative errno. */
static int unix_recv_data(unix_t *s, void *buf, uint32_t len, int peek,
                          int nonblock)
{
    uint32_t got = 0;
    while (got < len) {
        if (s->rused > 0) {
            uint32_t chunk = len - got;
            if (chunk > s->rused)
                chunk = s->rused;
            if (peek) {
                uint8_t *out = (uint8_t *)buf + got;
                for (uint32_t i = 0; i < chunk; i++)
                    out[i] = s->rx[(s->tail + i) % s->cap];
                got += chunk;
                break;                  /* peek is a one-shot snapshot */
            }
            ring_get(s, (uint8_t *)buf + got, chunk);
            got += chunk;
            ring_wake();                /* writers may be parked on full */
            continue;
        }
        if (got > 0)
            break;                      /* deliver the partial read */
        if (s->peer_closed || s->shut_rx) {
#ifdef SYSTRACE
            {
                extern void dbg_puts(const char *);
                extern void dbg_puts_dec(uint32_t);
                static unsigned ue;
                if (++ue < 30) {
                    dbg_puts("UEOF u=");
                    dbg_puts_dec((uint32_t)(s - g_unix));
                    dbg_puts(" pc=");
                    dbg_puts_dec((uint32_t)s->peer_closed);
                    dbg_puts(" srx=");
                    dbg_puts_dec((uint32_t)s->shut_rx);
                    dbg_puts(" lst=");
                    dbg_puts_dec((uint32_t)s->listening);
                    dbg_puts("\n");
                }
            }
#endif
            return 0;                   /* EOF */
        }
        if (nonblock || s->nonblock)
            return -E_AGAIN;
        sched_block_timeout(WAIT_PIPE, 0);
        if (!s->rused)
            continue;
    }
    return (int)got;
}

/* The fds queued for the next recvmsg.  Sends ref'd handles; the receiver
 * installs them in its table (or, on a plain read()/bad buffer, drops
 * them -- Linux does the same with MSG_CTRUNC). */
static int unix_queue_fds(unix_t *s, const int *handles, int n)
{
    if (s->n_fds + n > UNIX_FDS_MAX)
        return -E_MSGSIZE;
    memcpy(&s->fds[s->n_fds], handles, n * sizeof(int));
    s->n_fds += n;
    return 0;
}

static void unix_drop_fds(unix_t *s, int n)
{
    while (n-- > 0 && s->n_fds > 0) {
        vfs_file_unref(s->fds[s->n_fds - 1]);
        s->n_fds--;
    }
}

/* Send `len` bytes into the peer's ring, blocking until it fits. */
static int unix_send_data(unix_t *s, const void *buf, uint32_t len)
{
    unix_t *d = s->peer;
    if (!d || s->shut_tx || d->peer_closed || d->shut_rx)
        return -E_PIPE;
    uint32_t sent = 0;
    while (sent < len) {
        uint32_t free = d->cap - d->rused;
        if (free > 0) {
            uint32_t chunk = len - sent;
            if (chunk > free)
                chunk = free;
            ring_put(d, (const uint8_t *)buf + sent, chunk);
            sent += chunk;
            ring_wake();
            continue;
        }
        if (s->nonblock)
            return sent > 0 ? (int)sent : -E_AGAIN;
        sched_block_timeout(WAIT_PIPE, 0);
        if (!s->peer || s->peer != d || d->peer_closed)
            return sent > 0 ? (int)sent : -E_PIPE;
    }
    return (int)sent;
}

int unix_readable(int u)
{
    unix_t *s = unix_at(u);
    if (!s)
        return 0;
    return s->rused > 0 || s->peer_closed || s->shut_rx ||
           (s->listening && s->n_backlog > 0) || s->n_fds > 0;
}

int unix_writable(int u)
{
    unix_t *s = unix_at(u);
    if (!s || !s->peer || s->shut_tx || s->peer_closed)
        return 0;
    return s->peer->cap - s->peer->rused > 0;
}

int unix_set_nonblock(int u, int on)
{
    unix_t *s = unix_at(u);
    if (!s)
        return -E_BADF;
    s->nonblock = on;
    return 0;
}

int unix_is_nonblock(int u)
{
    unix_t *s = unix_at(u);
    return s ? s->nonblock : 0;
}

int unix_shutdown(int u, int how)
{
    unix_t *s = unix_at(u);
    if (!s)
        return -E_BADF;
    if (how == 0 || how == 2)
        s->shut_rx = 1;
    if (how == 1 || how == 2) {
        s->shut_tx = 1;
        if (s->peer) {
            s->peer->peer_closed = 1;
            s->peer->peer = NULL;       /* and stop draining into it */
        }
    }
    ring_wake();
    return 0;
}

int unix_getname(int u, char *path, uint32_t *len, int peer)
{
    unix_t *s = unix_at(u);
    if (!s)
        return -E_BADF;
    const char *p = "";
    if (!peer && s->has_path)
        p = s->bound;
    else if (peer && s->peer && s->peer->has_path)
        p = s->peer->bound;
    uint32_t n = (uint32_t)strlen(p);
    if (n + 1 > *len)
        return -E_OVERFLOW;
    memcpy(path, p, n + 1);
    *len = n + 1;
    return 0;
}

/* ---- syscall-facing entry points ----------------------------------------- */

int unix_bind_sys(int u, const char *path, uint32_t len)
{
    return unix_bind(u, path, len);
}

int unix_connect_sys(int u, const char *path, uint32_t len)
{
    return unix_connect(u, path, len);
}

int unix_listen_sys(int u, int backlog)
{
    return unix_listen(u, backlog);
}

int unix_accept_sys(int u)
{
    return unix_accept(u);
}

/* socketpair: bind two fresh sockets to each other. */
void unix_link(int a, int b)
{
    unix_t *ua = unix_at(a);
    unix_t *ub = unix_at(b);
    if (!ua || !ub)
        return;
    ua->peer = ub;
    ub->peer = ua;
    ua->peer_closed = 0;
    ub->peer_closed = 0;
    ring_wake();
}

/* read(2) on a socket node.  Pending fds are consumed and dropped, the way
 * Linux delivers MSG_CTRUNC. */
int32_t unix_node_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)off;
    unix_t *s = unix_at(-2 - (int)(uintptr_t)n->priv);
    if (!s)
        return -E_BADF;
    if (s->n_fds > 0)
        unix_drop_fds(s, s->n_fds);
    return unix_recv_data(s, buf, len, 0, 0);
}

int32_t unix_node_write(vfs_node_t *n, uint64_t off, const void *buf,
                        uint32_t len)
{
    (void)off;
    unix_t *s = unix_at(-2 - (int)(uintptr_t)n->priv);
    if (!s)
        return -E_BADF;
    return unix_send_data(s, buf, len);
}

/* sendmsg(46)/recvmsg(47) with SCM_RIGHTS.  `handles` is the kernel-side
 * fd list; control data is built/parsed by the syscall layer.  MSG_PEEK /
 * MSG_DONTWAIT come from sock.h with their Linux values. */

int unix_sendmsg(int u, const void *buf, uint32_t len, const int *fds, int nfds)
{
    unix_t *s = unix_at(u);
    if (!s)
        return -E_BADF;
    if (nfds > 0) {
        if (!s->peer)
            return -E_PIPE;
        int r = unix_queue_fds(s->peer, fds, nfds);
        if (r < 0)
            return r;
    }
    return unix_send_data(s, buf, len);
}

/* Returns bytes read, and fills `fds` (up to *nfds) with handles to
 * install in the receiver's table.  The caller owns them after this call.
 * MSG_PEEK leaves both data and fds queued. */
int unix_recvmsg(int u, void *buf, uint32_t len, int *fds, int *nfds, int flags)
{
    unix_t *s = unix_at(u);
    if (!s)
        return -E_BADF;

    if (!(flags & MSG_PEEK)) {
        int room = *nfds;
        int take = s->n_fds > room ? room : s->n_fds;
        if (take > 0)
            memcpy(fds, s->fds, take * sizeof(int));
        unix_drop_fds(s, s->n_fds - take);
        *nfds = take;
    } else {
        *nfds = 0;
    }

    return unix_recv_data(s, buf, len, (flags & MSG_PEEK) != 0,
                          (flags & MSG_DONTWAIT) != 0);
}