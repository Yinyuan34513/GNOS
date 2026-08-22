/*
 * signalfd.c — signalfd4 (Linux UAPI). (GPLv2)
 *
 * libwayland's wl_event_loop_add_signal() creates one signalfd per signal
 * (SIGINT/SIGTERM for labwc) and drives it through epoll; the compositor
 * never installs a raw handler.  Each anonymous node carries a sigset mask;
 * read() consumes the lowest pending signal that matches the mask as a
 * struct signalfd_siginfo, the same "drain the counter" contract as eventfd.
 * Signal delivery already wakes blocked processes (proc_signal's sched_wake),
 * so a reader parked in the loop below re-checks after every wake.
 */
#include <stddef.h>
#include <stdint.h>
#include "vfs.h"
#include "heap.h"
#include "proc.h"
#include "kstring.h"
#include "syscall.h"
#include "anonfd.h"
#include "signalfd.h"

#define SFD_NONBLOCK  O_NONBLOCK        /* 04000 */
#define SFD_CLOEXEC   O_CLOEXEC         /* 02000000 */

/* struct signalfd_siginfo as libc sees it (x86-64 Linux UAPI, 128 bytes). */
typedef struct {
    uint32_t ssi_signo;
    int32_t  ssi_errno;
    int32_t  ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t  ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t  ssi_status;
    int32_t  ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint16_t ssi_pad2;
    int32_t  ssi_syscall;
    uint64_t ssi_call_addr;
    uint32_t ssi_arch;
    uint8_t  ssi_pad[28];
} signalfd_siginfo_t;                    /* 128 bytes */

typedef struct {
    uint64_t mask;                      /* signals this fd reports */
    int      nonblock;
} signalfd_t;

/* Lowest signal number pending on `p` that the fd's mask selects, or 0.
 * SIGKILL/SIGSTOP are never caught by a signalfd, exactly as on Linux. */
static int signalfd_next(const signalfd_t *s, const proc_t *p)
{
    uint64_t want = p->sig_pending & s->mask &
                    ~(SIGMASK(SIGKILL) | SIGMASK(SIGSTOP));
    if (!want)
        return 0;
    for (int sig = 1; sig < NSIG; sig++)
        if (want & SIGMASK(sig))
            return sig;
    return 0;
}

/* ---- node ops ----------------------------------------------------------- */

static int32_t signalfd_read(vfs_node_t *n, uint64_t off, void *buf,
                             uint32_t len)
{
    (void)off;
    if (len < sizeof(signalfd_siginfo_t))
        return -E_INVAL;
    signalfd_t *s = (signalfd_t *)n->priv;

    for (;;) {
        proc_t *me = proc_current();
        int sig = me ? signalfd_next(s, me) : 0;
        if (sig) {
            /* Consume the signal: it was read as data, so the handler path
             * must not run it afterwards. */
            me->sig_pending &= ~SIGMASK(sig);
            signalfd_siginfo_t si;
            memset(&si, 0, sizeof si);
            si.ssi_signo = (uint32_t)sig;
            memcpy(buf, &si, sizeof si);
            return (int32_t)sizeof si;
        }
        if (s->nonblock)
            return -E_AGAIN;
        sched_block_timeout(WAIT_PIPE, 0);
    }
}

static int32_t signalfd_write(vfs_node_t *n, uint64_t off, const void *buf,
                              uint32_t len)
{
    (void)n; (void)off; (void)buf; (void)len;
    return -E_INVAL;                    /* signalfds are read-only */
}

static int signalfd_poll(vfs_node_t *n, int16_t events, int16_t *revents)
{
    signalfd_t *s = (signalfd_t *)n->priv;
    int16_t r = 0;
    if (events & POLLIN) {
        proc_t *me = proc_current();
        r |= (me && signalfd_next(s, me)) ? POLLIN : 0;
    }
    if (events & POLLOUT)
        r |= POLLOUT;
    *revents = r;
    return 0;
}

static void signalfd_release(vfs_node_t *n)
{
    kfree(n->priv);
    n->priv = NULL;
}

static const vfs_ops_t g_signalfd_ops = {
    .read    = signalfd_read,
    .write   = signalfd_write,
    .poll    = signalfd_poll,
    .release = signalfd_release,
};

/* ---- syscalls ------------------------------------------------------------ */

int64_t sys_signalfd4(uint64_t fd, uint64_t umask, uint64_t sigsetsize,
                      uint64_t flags)
{
    /* musl's sigset_t is 8 bytes on x86-64; libwayland passes 8. */
    if (sigsetsize != 8)
        return -E_INVAL;
    if (flags & ~(SFD_NONBLOCK | SFD_CLOEXEC))
        return -E_INVAL;
    if (!umask || !user_ptr_ok(umask, 8))
        return -E_FAULT;
    uint64_t mask;
    memcpy(&mask, (const void *)(uintptr_t)umask, 8);

    /* fd != -1: replace the mask of an existing signalfd. */
    if (fd != (uint64_t)-1) {
        int h = fd_handle((int)fd);
        if (h < 0)
            return -E_BADF;
        vfs_node_t *n = (vfs_node_t *)vfs_file_node(h);
        if (!n || n->kind != VFS_ANON || n->ops != &g_signalfd_ops)
            return -E_INVAL;
        ((signalfd_t *)n->priv)->mask = mask;
        sched_wake_reason(WAIT_PIPE);
        return 0;
    }

    signalfd_t *s = kmalloc(sizeof(signalfd_t));
    if (!s)
        return -E_NOMEM;
    s->mask = mask;
    s->nonblock = (flags & SFD_NONBLOCK) != 0;

    int h = vfs_anon_open(VFS_ANON, &g_signalfd_ops, s,
                          s->nonblock ? (O_RDWR | O_NONBLOCK) : O_RDWR);
    if (h < 0) {
        kfree(s);
        return h;
    }
    return anon_bind(h, flags & SFD_CLOEXEC);
}

/* fcntl(F_SETFL, O_NONBLOCK) mirror, called from syscall.c. */
int signalfd_is_signalfd(const vfs_node_t *n)
{
    return n && n->kind == VFS_ANON && n->ops == &g_signalfd_ops;
}

void signalfd_set_nonblock(vfs_node_t *n, int nb)
{
    if (signalfd_is_signalfd(n))
        ((signalfd_t *)n->priv)->nonblock = nb;
}
