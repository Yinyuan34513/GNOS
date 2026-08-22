/*
 * anonfd.c — memfd_create(319) and eventfd(284/290), the two anonymous fds
 * the Wayland plumbing cannot do without. (GPLv2)
 *
 *   - memfd: a nameless, page-backed file with no directory entry.  wl_shm
 *     pools are exactly this: the client creates one, ftruncates it to the
 *     pool size, mmaps it MAP_SHARED, and ships the fd to the compositor
 *     over the socket, where a second mmap lands on the *same* pages.  The
 *     backing store is a physically contiguous span so the existing VFS
 *     mmap model (one physical range per mapping) covers it unchanged.
 *
 *   - eventfd: a 64-bit counter dressed up as a file.  read() drains it
 *     (semaphore or plain), write() adds to it, poll() reports readiness,
 *     and the counter is what makes it a useful cross-process wakeup: a
 *     producer writes 1, a consumer parked in poll() wakes up.
 *
 * The struct layouts, flags and edge cases below are the Linux UAPI:
 * EFD_* flag bits, the read/write semantics, the overflow rule (writing a
 * value that would wrap returns EAGAIN or blocks), and the counter being
 * preserved across fork() because descriptors are shared.
 *
 * Both are anonymous nodes (VFS_ANON): no /dev entry, no path, released
 * through ops->release when the last descriptor closes.
 */
#include <stddef.h>
#include <stdint.h>
#include "vfs.h"
#include "pmm.h"
#include "heap.h"
#include "proc.h"
#include "kstring.h"
#include "syscall.h"

/* ---- eventfd ------------------------------------------------------------ */
#define EFD_SEMAPHORE 0x0001
#define EFD_CLOEXEC   0x80000
#define EFD_NONBLOCK  O_NONBLOCK        /* 0x800, same as Linux */

typedef struct {
    uint64_t count;
    int      flags;               /* EFD_* state, mirrored at open */
} eventfd_t;

static int32_t eventfd_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)off;
    if (len < 8)
        return -E_INVAL;
    eventfd_t *e = (eventfd_t *)n->priv;
    uint64_t   v;

    for (;;) {
        if (e->count > 0) {
            if (e->flags & EFD_SEMAPHORE) {
                v = 1;
                e->count--;
            } else {
                v = e->count;
                e->count = 0;
            }
            memcpy(buf, &v, 8);
            sched_wake_reason(WAIT_PIPE);   /* writers may be parked on overflow */
            return 8;
        }
        if (e->flags & EFD_NONBLOCK)
            return -E_AGAIN;
        sched_block_timeout(WAIT_PIPE, 0);
    }
}

static int32_t eventfd_write(vfs_node_t *n, uint64_t off, const void *buf,
                             uint32_t len)
{
    (void)off;
    if (len < 8)
        return -E_INVAL;
    uint64_t v;
    memcpy(&v, buf, 8);
    if (v == 0xFFFFFFFFFFFFFFFFULL)
        return -E_INVAL;
    eventfd_t *e = (eventfd_t *)n->priv;

    for (;;) {
        if (e->count <= 0xFFFFFFFFFFFFFFFEULL - v) {
            e->count += v;
            sched_wake_reason(WAIT_PIPE);
            return 8;
        }
        if (e->flags & EFD_NONBLOCK)
            return -E_AGAIN;
        sched_block_timeout(WAIT_PIPE, 0);
    }
}

static int eventfd_poll(vfs_node_t *n, int16_t events, int16_t *revents)
{
    eventfd_t *e = (eventfd_t *)n->priv;
    int16_t    r = 0;
    if (events & POLLIN)
        r |= (e->count > 0) ? POLLIN : 0;
    if (events & POLLOUT)
        r |= (e->count < 0xFFFFFFFFFFFFFFFEULL) ? POLLOUT : 0;
    *revents = r;
    return 0;
}

static void eventfd_release(vfs_node_t *n)
{
    kfree(n->priv);
    n->priv = NULL;
}

static const vfs_ops_t g_eventfd_ops = {
    .read    = eventfd_read,
    .write   = eventfd_write,
    .poll    = eventfd_poll,
    .release = eventfd_release,
};

/* ---- memfd -------------------------------------------------------------- */
typedef struct {
    uint64_t size;               /* bytes the fd says it has */
    uint64_t npages;             /* pages actually backed */
    uint64_t first;              /* physical address of page 0 */
} memfd_t;

static void *memfd_virt(memfd_t *m, uint64_t off)
{
    return (uint8_t *)pmm_virt(m->first) + off;
}

static int memfd_grow(memfd_t *m, uint64_t len)
{
    uint64_t need = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (need <= m->npages)
        return 0;

    /* The backing span must be contiguous -- the VFS mmap model hands out
     * one physical range.  A memfd that cannot grow contiguously fails
     * like ENOMEM, which is exactly what Linux reports when the shmem
     * inode cannot extend. */
    uint64_t span = pmm_alloc_contiguous(need);
    if (!span)
        return -E_NOMEM;

    if (m->npages > 0) {
        /* Keep what we had (it may not be adjacent to the new span), then
         * retire the old backing. */
        memcpy(pmm_virt(span), memfd_virt(m, 0), m->npages * PAGE_SIZE);
        pmm_free(m->first);
    }
    /* Freshly extended pages read as zero, the way shmem's ftruncate
     * guarantees -- pmm_alloc_contiguous does not promise a clean span. */
    {
        uint64_t keep = m->npages * PAGE_SIZE;
        if (keep < need * PAGE_SIZE)
            memset((uint8_t *)pmm_virt(span) + keep, 0,
                   (size_t)(need * PAGE_SIZE - keep));
    }
    m->first  = span;
    m->npages = need;
    return 0;
}

static int32_t memfd_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    memfd_t *m = (memfd_t *)n->priv;
    if (off >= m->size)
        return 0;
    uint32_t want = (uint32_t)((m->size - off) < len ? (m->size - off) : len);
    memcpy(buf, memfd_virt(m, off), want);
    return (int32_t)want;
}

static int32_t memfd_write(vfs_node_t *n, uint64_t off, const void *buf,
                           uint32_t len)
{
    memfd_t *m = (memfd_t *)n->priv;
    if (memfd_grow(m, off + len) < 0)
        return -E_NOMEM;
    memcpy(memfd_virt(m, off), buf, len);
    if (off + len > m->size)
        m->size = off + len;
    return (int32_t)len;
}

static int memfd_truncate(vfs_node_t *n, uint64_t len)
{
    memfd_t *m = (memfd_t *)n->priv;
    int e = memfd_grow(m, len);
    if (e < 0)
        return e;
    /* Freshly extended pages must read as zero, the way shmem's ftruncate
     * guarantees. */
    uint64_t old_rounded = (m->size + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
    uint64_t new_rounded = (len + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
    if (new_rounded > old_rounded)
        memset((uint8_t *)memfd_virt(m, 0) + old_rounded, 0,
               (size_t)(new_rounded - old_rounded));
    m->size = len;
    return 0;
}

static int memfd_mmap(vfs_node_t *n, uint64_t offset, uint64_t *phys,
                      uint64_t *size)
{
    memfd_t *m = (memfd_t *)n->priv;
    /* mmap() on a file that has not been written yet must work: wl_shm
     * pools are created empty and only ftruncate()d to size before any
     * pixel lands.  Back at least the page the caller asked for. */
    if (memfd_grow(m, offset + PAGE_SIZE) < 0)
        return -E_NOMEM;
    *phys = m->first + offset;
    *size = m->npages * PAGE_SIZE - offset;
    return 0;
}

static int memfd_poll(vfs_node_t *n, int16_t events, int16_t *revents)
{
    (void)n;
    *revents = (int16_t)(events & (POLLIN | POLLOUT));   /* a file: always ready */
    return 0;
}

static void memfd_release(vfs_node_t *n)
{
    memfd_t *m = (memfd_t *)n->priv;
    if (m) {
        if (m->npages)
            pmm_free(m->first);
        kfree(m);
    }
    n->priv = NULL;
}

static const vfs_ops_t g_memfd_ops = {
    .read     = memfd_read,
    .write    = memfd_write,
    .truncate = memfd_truncate,
    .mmap     = memfd_mmap,
    .poll     = memfd_poll,
    .release  = memfd_release,
};

/* ---- the syscalls ------------------------------------------------------- */
#define MFD_CLOEXEC       0x0001
#define MFD_ALLOW_SEALING 0x0002

int anon_bind(int h, int cloexec)
{
    int fd = fd_alloc(proc_current(), h);
    if (fd < 0) {
        vfs_file_unref(h);
        return fd;
    }
    if (cloexec)
        proc_current()->fd_cloexec |= (1ULL << fd);
    return fd;
}

int64_t sys_memfd_create(uint64_t name, uint64_t flags)
{
    (void)name;                              /* Linux uses it for /proc only */
    if (flags & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING))
        return -E_INVAL;

    memfd_t *m = kmalloc(sizeof(memfd_t));
    if (!m)
        return -E_NOMEM;
    memset(m, 0, sizeof(*m));

    int h = vfs_anon_open(VFS_ANON, &g_memfd_ops, m, O_RDWR);
    if (h < 0) {
        kfree(m);
        return h;
    }
    return anon_bind(h, flags & MFD_CLOEXEC);
}

/* eventfd(284) is the flags-less ancestor; eventfd2(290) takes EFD_*.
 * musl's eventfd() wrapper picks the right number, so both must exist. */
static int64_t eventfd_common(uint64_t count, uint64_t flags)
{
    if (flags & ~(EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK))
        return -E_INVAL;

    eventfd_t *e = kmalloc(sizeof(eventfd_t));
    if (!e)
        return -E_NOMEM;
    memset(e, 0, sizeof(*e));
    e->count = count;
    e->flags = (int)flags;

    int h = vfs_anon_open(VFS_ANON, &g_eventfd_ops, e,
                          (flags & EFD_NONBLOCK) ? (O_RDWR | O_NONBLOCK) : O_RDWR);
    if (h < 0) {
        kfree(e);
        return h;
    }
    return anon_bind(h, flags & EFD_CLOEXEC);
}

int64_t sys_eventfd2(uint64_t count, uint64_t flags)
{
    return eventfd_common(count, flags);
}

int64_t sys_eventfd(uint64_t count)
{
    return eventfd_common(count, 0);
}

/* fcntl(F_SETFL, O_NONBLOCK) mirror -- see syscall.c. */
int anonfd_is_eventfd(const vfs_node_t *n)
{
    return n && n->kind == VFS_ANON && n->ops == &g_eventfd_ops;
}

void anonfd_set_nonblock(vfs_node_t *n, int nb)
{
    if (anonfd_is_eventfd(n))
        ((eventfd_t *)n->priv)->flags = nb ? ((eventfd_t *)n->priv)->flags |
                                             O_NONBLOCK
                                           : ((eventfd_t *)n->priv)->flags &
                                             ~O_NONBLOCK;
}
