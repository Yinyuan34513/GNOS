/*
 * epoll.c — epoll_create1(291)/epoll_ctl(233)/epoll_wait(232)/
 * epoll_pwait(281). (GPLv2)
 *
 * wlroots and libwayland drive their event loops with epoll, so the
 * compositor cannot run on a poll-only kernel.  The implementation is the
 * honest teaching-OS one: epoll_wait scans the registered descriptors with
 * the same readiness machinery as poll(2) and sleeps in the same wait
 * channel, so an epoll loop and a poll loop behave identically -- right
 * down to the spurious wakeups, which both resolve by re-scanning.
 *
 * The struct layouts and numbers are the Linux UAPI: EPOLL* event bits,
 * EPOLL_CTL_*, epoll_event as a *packed* 12-byte pair (the Linux ABI --
 * data directly follows events with no padding), EPOLL_CLOEXEC on the
 * instance fd.  A descriptor that disappears between epoll_ctl and
 * epoll_wait reports nothing, which is Linux's behaviour too: a closed fd
 * is silently removed from the interest set.
 */
#include <stddef.h>
#include <stdint.h>
#include "vfs.h"
#include "heap.h"
#include "proc.h"
#include "vmm.h"
#include "kstring.h"
#include "syscall.h"

#define EPOLLIN      0x00000001
#define EPOLLOUT     0x00000004
#define EPOLLERR     0x00000008
#define EPOLLHUP     0x00000010
#define EPOLLRDHUP   0x00002000
#define EPOLLRDNORM  0x00000040
#define EPOLLRDBAND  0x00000080
#define EPOLLWRNORM  0x00000100
#define EPOLLWRBAND  0x00000200
#define EPOLLET      0x80000000
#define EPOLLONESHOT 0x40000000

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLL_CLOEXEC 0x00080000

/* The Linux ABI: events and data back to back, 12 bytes total. */
typedef struct {
    uint32_t events;
    uint64_t data;
} __attribute__((packed)) epoll_event_t;

/* An interest set entry: the descriptor number plus the events the caller
 * asked about.  data rides along untouched and is reported verbatim. */
typedef struct {
    int      used;
    int      fd;
    uint32_t events;
    uint64_t data;
} epoll_entry_t;

#define EPOLL_MAX_ENTRIES 256

typedef struct {
    epoll_entry_t entries[EPOLL_MAX_ENTRIES];
} epoll_inst_t;

static int inst_of(int fd, epoll_inst_t **out)
{
    int h = fd_handle(fd);
    if (h < 0)
        return -E_BADF;
    const vfs_node_t *n = vfs_file_node(h);
    if (!n || n->kind != VFS_ANON || !n->ops || !n->priv)
        return -E_INVAL;
    *out = (epoll_inst_t *)(uintptr_t)n->priv;
    return 0;
}

static void epoll_release(vfs_node_t *n)
{
    kfree(n->priv);
    n->priv = NULL;
}

static const vfs_ops_t g_epoll_ops = {
    .release = epoll_release,
};

int64_t sys_epoll_create1(uint64_t flags)
{
    if (flags & ~EPOLL_CLOEXEC)
        return -E_INVAL;

    epoll_inst_t *inst = kmalloc(sizeof(epoll_inst_t));
    if (!inst)
        return -E_NOMEM;
    memset(inst, 0, sizeof(*inst));

    int h = vfs_anon_open(VFS_ANON, &g_epoll_ops, inst, O_RDWR);
    if (h < 0) {
        kfree(inst);
        return h;
    }
    int fd = fd_alloc(proc_current(), h);
    if (fd < 0) {
        vfs_file_unref(h);
        return fd;
    }
    if (flags & EPOLL_CLOEXEC)
        proc_current()->fd_cloexec |= (1ULL << fd);
    return fd;
}

int64_t sys_epoll_create(uint64_t flags)
{
    (void)flags;
    return sys_epoll_create1(0);
}

int64_t sys_epoll_ctl(int epfd, int op, int fd, uint64_t up_event)
{
    epoll_inst_t *inst;
    int e = inst_of(epfd, &inst);
    if (e < 0)
        return e;
    if (fd < 0)
        return -E_BADF;

    epoll_event_t ev = {0};
    if (op != EPOLL_CTL_DEL) {
        if (!user_ptr_ok(up_event, sizeof(ev)))
            return -E_FAULT;
        memcpy(&ev, (const void *)(uintptr_t)up_event, sizeof(ev));
    }

    /* Adding the epoll fd to itself is a loop the kernel must refuse. */
    if (op == EPOLL_CTL_ADD && fd == epfd)
        return -E_INVAL;
    /* Only descriptors the process actually holds can be watched. */
    if (fd_handle(fd) < 0)
        return -E_BADF;

    int slot = -1;
    for (int i = 0; i < EPOLL_MAX_ENTRIES; i++) {
        if (inst->entries[i].used && inst->entries[i].fd == fd) {
            slot = i;
            break;
        }
        if (slot < 0 && !inst->entries[i].used)
            slot = i;
    }

    switch (op) {
    case EPOLL_CTL_ADD:
        if (slot < 0 || (inst->entries[slot].used && inst->entries[slot].fd == fd))
            return -E_EXIST;
        inst->entries[slot].used   = 1;
        inst->entries[slot].fd     = fd;
        inst->entries[slot].events = ev.events;
        inst->entries[slot].data   = ev.data;
        return 0;
    case EPOLL_CTL_MOD:
        if (slot < 0 || !inst->entries[slot].used)
            return -E_NOENT;
        inst->entries[slot].events = ev.events;
        inst->entries[slot].data   = ev.data;
        return 0;
    case EPOLL_CTL_DEL:
        if (slot < 0 || !inst->entries[slot].used)
            return -E_NOENT;
        inst->entries[slot].used = 0;
        return 0;
    }
    return -E_INVAL;
}

/* The pollfd layout do_ppoll speaks: fd, events, revents.  Built in kernel
 * scratch so the user's epoll_event array never has to look like one. */
typedef struct {
    int32_t  fd;
    int16_t  events;
    int16_t  revents;
} pollfd_k_t;

static uint32_t epoll_bit(int16_t rev)
{
    uint32_t r = 0;
    if (rev & POLLIN)  r |= EPOLLIN;
    if (rev & POLLOUT) r |= EPOLLOUT;
    if (rev & POLLERR) r |= EPOLLERR;
    if (rev & POLLHUP) r |= EPOLLHUP;
    return r;
}

static int64_t epoll_wait_common(int epfd, uint64_t uevents, int maxevents,
                                 int64_t ms)
{
    epoll_inst_t *inst;
    int e = inst_of(epfd, &inst);
    if (e < 0)
        return e;
    if (maxevents <= 0 || maxevents > EPOLL_MAX_ENTRIES)
        return -E_INVAL;
    if (!user_ptr_ok(uevents, (uint64_t)maxevents * sizeof(epoll_event_t)))
        return -E_FAULT;

    int64_t ticks;
    if (ms < 0)
        ticks = -1;
    else if (ms == 0)
        ticks = 0;
    else
        ticks = (ms + 9) / 10;

    pollfd_k_t pf[EPOLL_MAX_ENTRIES];
    int want = 0;
    for (int i = 0; i < EPOLL_MAX_ENTRIES; i++)
        if (inst->entries[i].used) {
            pf[want].fd      = inst->entries[i].fd;
            pf[want].events  = (int16_t)(inst->entries[i].events & 0xFFFF);
            pf[want].revents = 0;
            want++;
        }

    int64_t n = do_ppoll((uint8_t *)pf, (uint64_t)want, ticks);
    if (n <= 0)
        return n;

    epoll_event_t *out = (epoll_event_t *)(uintptr_t)uevents;
    int nout = 0;
    for (int i = 0; i < want && nout < maxevents; i++) {
        if (!pf[i].revents)
            continue;
        /* A descriptor that has gone away since epoll_ctl reports nothing,
         * exactly as though it had been closed and auto-removed. */
        if (pf[i].revents & POLLNVAL)
            continue;
        /* Find the entry again -- the interest set may have changed while
         * we slept -- and pass its data through untouched. */
        for (int k = 0; k < EPOLL_MAX_ENTRIES; k++)
            if (inst->entries[k].used && inst->entries[k].fd == pf[i].fd) {
                out[nout].events = epoll_bit(pf[i].revents);
                out[nout].data   = inst->entries[k].data;
                nout++;
                break;
            }
    }
    return nout;
}

int64_t sys_epoll_wait(int epfd, uint64_t uevents, int maxevents, int ms)
{
    return epoll_wait_common(epfd, uevents, maxevents, ms);
}

int64_t sys_epoll_pwait(int epfd, uint64_t uevents, int maxevents, int ms,
                        uint64_t usigmask)
{
    (void)usigmask;
    return epoll_wait_common(epfd, uevents, maxevents, ms);
}
