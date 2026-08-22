/*
 * timerfd.c — timerfd_create/settime/gettime (Linux UAPI). (GPLv2)
 *
 * One timer per anonymous node, driven by the 100 Hz PIT tick: each armed
 * timer records its next expiry in ticks, and the timer interrupt calls
 * timerfd_tick() once per interrupt to advance expirations and wake the
 * sleepers.  read() drains the expiration counter as an 8-byte value, the
 * same contract as eventfd; poll() reports POLLIN while the counter is
 * non-zero, which is what lets libwayland's event loop drive timers through
 * epoll.
 */
#include <stddef.h>
#include <stdint.h>
#include "vfs.h"
#include "heap.h"
#include "proc.h"
#include "kstring.h"
#include "syscall.h"
#include "timer.h"
#include "anonfd.h"

#define TFD_TIMER_ABSTIME        1
#define TFD_TIMER_CANCEL_ON_SET  2
#define TFD_NONBLOCK             O_NONBLOCK
#define TFD_CLOEXEC              O_CLOEXEC

#define TIMERFD_MAX  32

typedef struct {
    int      active;            /* armed */
    uint64_t next;              /* next expiry, in ticks */
    uint64_t period;            /* interval, in ticks; 0 = one-shot */
    uint64_t expirations;       /* pending expirations, read() drains */
    int      nonblock;
} timerfd_t;

/* Registry the tick handler walks.  Nodes are added at create and removed
 * at release, which happens with interrupts disabled (last unref), so the
 * tick handler never sees a half-built entry. */
static timerfd_t *g_timers[TIMERFD_MAX];

/* Timespec/timerspec as user space sends them (x86-64 Linux ABI). */
typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} k_timespec_t;

typedef struct {
    k_timespec_t it_interval;
    k_timespec_t it_value;
} k_itimerspec_t;                        /* 32 bytes */

/* ticks -> itimerspec value field; a zero timespec is written as 0/0. */
static void ticks_to_ts(uint64_t ticks, k_timespec_t *ts)
{
    ts->tv_sec  = (int64_t)(ticks / SCHED_HZ);
    ts->tv_nsec = (int64_t)((ticks % SCHED_HZ) * 10000000ull);
}

static uint64_t ts_to_ticks(const k_timespec_t *ts)
{
    if (ts->tv_sec < 0 || ts->tv_nsec < 0)
        return 0;
    return (uint64_t)ts->tv_sec * SCHED_HZ + (uint64_t)ts->tv_nsec / 10000000ull;
}

/* ---- node ops ----------------------------------------------------------- */

static int32_t timerfd_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)off;
    if (len < 8)
        return -E_INVAL;
    timerfd_t *t = (timerfd_t *)n->priv;

    for (;;) {
        if (t->expirations > 0) {
            uint64_t v = t->expirations;
            t->expirations = 0;
            memcpy(buf, &v, 8);
            return 8;
        }
        if (t->nonblock)
            return -E_AGAIN;
        sched_block_timeout(WAIT_PIPE, 0);
    }
}

static int32_t timerfd_write(vfs_node_t *n, uint64_t off, const void *buf,
                             uint32_t len)
{
    (void)n; (void)off; (void)buf; (void)len;
    return -E_INVAL;                    /* timerfds are read-only */
}

static int timerfd_poll(vfs_node_t *n, int16_t events, int16_t *revents)
{
    timerfd_t *t = (timerfd_t *)n->priv;
    int16_t    r = 0;
    if (events & POLLIN)
        r |= (t->expirations > 0) ? POLLIN : 0;
    if (events & POLLOUT)
        r |= POLLOUT;
    *revents = r;
    return 0;
}

static void timerfd_release(vfs_node_t *n)
{
    timerfd_t *t = (timerfd_t *)n->priv;
    for (int i = 0; i < TIMERFD_MAX; i++)
        if (g_timers[i] == t)
            g_timers[i] = NULL;
    kfree(t);
    n->priv = NULL;
}

static const vfs_ops_t g_timerfd_ops = {
    .read    = timerfd_read,
    .write   = timerfd_write,
    .poll    = timerfd_poll,
    .release = timerfd_release,
};

/* ---- tick handler -------------------------------------------------------- */

void timerfd_tick(void)
{
    uint64_t now = timer_ticks();
    for (int i = 0; i < TIMERFD_MAX; i++) {
        timerfd_t *t = g_timers[i];
        if (!t || !t->active || now < t->next)
            continue;
        /* Count every missed expiry like Linux does, then rearm. */
        do {
            t->expirations++;
            t->next += t->period ? t->period : 1;
        } while (t->period && t->next <= now);
        if (!t->period)
            t->active = 0;
        sched_wake_reason(WAIT_PIPE);
    }
}

/* ---- syscalls ------------------------------------------------------------ */

int64_t sys_timerfd_create(uint64_t clockid, uint64_t flags)
{
    if (clockid != 1 /* CLOCK_MONOTONIC */)
        return -E_INVAL;
    if (flags & ~(TFD_NONBLOCK | TFD_CLOEXEC))
        return -E_INVAL;

    timerfd_t *t = kmalloc(sizeof(timerfd_t));
    if (!t)
        return -E_NOMEM;
    memset(t, 0, sizeof(*t));
    t->nonblock = (flags & TFD_NONBLOCK) != 0;

    int slot = -1;
    for (int i = 0; i < TIMERFD_MAX; i++)
        if (!g_timers[i]) { slot = i; break; }
    if (slot < 0) {
        kfree(t);
        return -E_NOMEM;
    }
    g_timers[slot] = t;

    int h = vfs_anon_open(VFS_ANON, &g_timerfd_ops, t,
                          t->nonblock ? (O_RDWR | O_NONBLOCK) : O_RDWR);
    if (h < 0) {
        g_timers[slot] = NULL;
        kfree(t);
        return h;
    }
    return anon_bind(h, flags & TFD_CLOEXEC);
}

static timerfd_t *fd_timerfd(int fd)
{
    int h = fd_handle(fd);
    if (h < 0)
        return NULL;
    vfs_node_t *n = vfs_file_node(h);
    if (!n || n->kind != VFS_ANON || n->ops != &g_timerfd_ops)
        return NULL;
    return (timerfd_t *)n->priv;
}

int64_t sys_timerfd_settime(uint64_t fd, uint64_t flags, uint64_t uin,
                            uint64_t uout)
{
    if (flags & ~(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET))
        return -E_INVAL;
    if (!uin || !user_ptr_ok(uin, sizeof(k_itimerspec_t)))
        return -E_FAULT;
    if (uout && !user_ptr_ok(uout, sizeof(k_itimerspec_t)))
        return -E_FAULT;

    timerfd_t *t = fd_timerfd((int)fd);
    if (!t)
        return -E_BADF;

    k_itimerspec_t in, out;
    memcpy(&in, (const void *)(uintptr_t)uin, sizeof(in));

    out.it_interval = in.it_interval;
    if (!t->active)
        out.it_value.tv_sec = out.it_value.tv_nsec = 0;
    else
        ticks_to_ts(t->next > timer_ticks() ? t->next - timer_ticks() : 0,
                    &out.it_value);

    uint64_t value = ts_to_ticks(&in.it_value);
    if (value == 0) {
        t->active = 0;                  /* disarm; counter survives */
    } else {
        t->active    = 1;
        t->period    = ts_to_ticks(&in.it_interval);
        t->next      = (flags & TFD_TIMER_ABSTIME)
                           ? value : timer_ticks() + value;
        t->expirations = 0;             /* arming resets the counter */
    }
    sched_wake_reason(WAIT_PIPE);

    if (uout) {
        memcpy((void *)(uintptr_t)uout, &out, sizeof(out));
        return 0;
    }
    return 0;
}

int64_t sys_timerfd_gettime(uint64_t fd, uint64_t uout)
{
    if (!uout || !user_ptr_ok(uout, sizeof(k_itimerspec_t)))
        return -E_FAULT;
    timerfd_t *t = fd_timerfd((int)fd);
    if (!t)
        return -E_BADF;

    k_itimerspec_t out;
    ticks_to_ts(t->period, &out.it_interval);
    if (!t->active)
        out.it_value.tv_sec = out.it_value.tv_nsec = 0;
    else
        ticks_to_ts(t->next > timer_ticks() ? t->next - timer_ticks() : 0,
                    &out.it_value);
    memcpy((void *)(uintptr_t)uout, &out, sizeof(out));
    return 0;
}

/* fcntl(F_SETFL, O_NONBLOCK) mirror, called from syscall.c. */
int timerfd_is_timerfd(const vfs_node_t *n)
{
    return n && n->kind == VFS_ANON && n->ops == &g_timerfd_ops;
}

void timerfd_set_nonblock(vfs_node_t *n, int nb)
{
    if (timerfd_is_timerfd(n))
        ((timerfd_t *)n->priv)->nonblock = nb;
}