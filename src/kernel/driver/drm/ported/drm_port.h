/*
 * drm_port.h — porting shim for the Uinxed DRM core files under ported/.
 * (GPLv2)
 *
 * The Uinxed DRM code was written against that kernel's facilities; this
 * header maps them onto GNOS equivalents so the files under ported/ compile
 * and behave unchanged:
 *
 *   Uinxed                          GNOS
 *   -------                         ----
 *   <stdint.h>             <stdint.h>
 *   <stddef.h>             <stddef.h>
 *   "kstring.h"             "kstring.h"  (memcpy/memset/strlen/strncmp)
 *   <stdbool.h>            <stdbool.h>
 *   "heap.h"  malloc/free      "heap.h"     kmalloc/kfree
 *   "smp.h"              "smp.h"      spinlock_t/spin_lock/unlock
 *   "vfs.h"                "vfs.h"      E_* (mapped below)
 *   "debugcon.h"  plogk        "debugcon.h" dbg_puts (see drm_print.c)
 *   <proc/uaccess.h>                drm_internal.h copy_from_user/to_user
 *
 * errno: Uinxed uses Linux-style ENOENT/ENOMEM/...; GNOS spells them
 * E_NOENT/E_NOMEM/...  Map the Linux names to the GNOS values so the ported
 * code can keep its own spelling.
 */
#ifndef GNUCOS_DRM_PORT_H
#define GNUCOS_DRM_PORT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "kstring.h"    /* memcpy/memset/strlen/strncmp */
#include "heap.h"       /* kmalloc/kfree */
#include "smp.h"        /* spinlock_t, spin_lock, spin_unlock */
#include "vfs.h"        /* E_* errno values */
#include "debugcon.h"   /* dbg_puts & friends */
#include "intrusive_list.h" /* ilist_node_t (wait_queue_t below) */
#include "proc.h"       /* proc_current/sched_block/sched_wake_queue */

/* ---- user-memory copy (Uinxed <proc/uaccess.h>) ---------------------
 * Uinxed spells the helpers copy_from_user(dst, void *src, size) /
 * copy_to_user(void *dst, src, size), both returning non-zero on failure;
 * GNOS spells them copy_from_user(k, uint64_t u, n) with 0 / -errno.  The
 * semantics line up (non-zero means failure), so wrap GNOS's helpers under
 * the Uinxed names for the ported code.  uaccess.h is not a GNOS header;
 * the declarations below come from drm_internal.h (GNOS DRM). */
extern int copy_from_user(void *k, uint64_t u, uint64_t n);
extern int copy_to_user(uint64_t u, const void *k, uint64_t n);

static inline int drm_port_copy_from_user(void *dst, const void *src, size_t size)
{
    return copy_from_user(dst, (uint64_t)(uintptr_t)src, size);
}

static inline int drm_port_copy_to_user(void *dst, const void *src, size_t size)
{
    return copy_to_user((uint64_t)(uintptr_t)dst, src, size);
}

#define copy_from_user(dst, src, size) drm_port_copy_from_user((dst), (src), (size))
#define copy_to_user(dst, src, size)   drm_port_copy_to_user((dst), (src), (size))

/* user_access_ok / strncpy_from_user are not used by the ported files. */

/* ---- Uinxed allocator names -> GNOS kernel heap --------------------- */
#define malloc  kmalloc
#define free    kfree
#define realloc krealloc

/* ---- freestanding libc helpers supplied by drm_libc.c ---------------- */
uint64_t nano_time(void);
void *aligned_alloc(size_t align, size_t size);
void aligned_free(void *p);
char *strdup(const char *s);

/* ---- Linux errno spelling -> GNOS values ---------------------------- */
#ifndef ENOENT
#define ENOENT  E_NOENT
#endif
#ifndef ESRCH
#define ESRCH   E_SRCH
#endif
#ifndef EINTR
#define EINTR   E_INTR
#endif
#ifndef EIO
#define EIO     E_IO
#endif
#ifndef ENOMEM
#define ENOMEM  E_NOMEM
#endif
#ifndef EACCES
#define EACCES  E_ACCES
#endif
#ifndef EFAULT
#define EFAULT  E_FAULT
#endif
#ifndef EBUSY
#define EBUSY   E_BUSY
#endif
#ifndef EEXIST
#define EEXIST  E_EXIST
#endif
#ifndef ENODEV
#define ENODEV  E_NODEV
#endif
#ifndef EINVAL
#define EINVAL  E_INVAL
#endif
#ifndef ENOSPC
#define ENOSPC  E_NOSPC
#endif
#ifndef ERANGE
#define ERANGE  E_RANGE
#endif
#ifndef EBADF
#define EBADF   E_BADF
#endif
#ifndef ENOSYS
#define ENOSYS  E_NOSYS
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP E_OPNOTSUPP
#endif
#ifndef EOVERFLOW
#define EOVERFLOW E_OVERFLOW
#endif
/* EDEADLK(35) is used by the modeset lock backoff path; GNOS has no errno
 * table entry for it yet, so give the ported code the Linux number. */
#ifndef EDEADLK
#define EDEADLK 35
#endif

/* Uinxed spinlocks carry an rflags field for irqsave; GNOS spinlock_t is a
 * bare volatile counter.  Code that pokes lock->lock directly (the modeset
 * lock init path) is adapted in place; everything else just calls
 * spin_lock()/spin_unlock() which both kernels provide. */

/* ---- wait_queue_t ---------------------------------------------------
 * Uinxed's vblank/commit/event wait queues (proc/task.h) are a task list
 * under a spinlock.  GNOS has no wait-queue type, so the DRM core gets a
 * minimal structural equivalent here; the blocking primitive maps onto
 * GNOS's scheduler (sched_block/sched_wake) in the ported vblank code.
 * The members match Uinxed's layout so drm_device.h can embed it. */
typedef struct wait_queue {
    ilist_node_t tasks;
    spinlock_t   lock;
} wait_queue_t;

static inline void wait_queue_init(wait_queue_t *q)
{
    ilist_init(&q->tasks);
    q->lock.v = 0;
}

/* Block the current task on @q.  GNOS's scheduler keys sleepers by a
 * wait-reason plus an optional wait_queue cookie (proc_t.wait_q), and
 * sched_wake_queue() wakes exactly the tasks parked on that queue. */
static inline void wait_queue_prepare(wait_queue_t *q)
{
    proc_t *me = proc_current();
    if (me)
        me->wait_q = (void *)q;
}

static inline void wait_queue_sleep(void)
{
    sched_block(WAIT_DRM);
}

static inline void wait_queue_wake_all(wait_queue_t *q)
{
    sched_wake_queue((void *)q);
}

#endif /* GNUCOS_DRM_PORT_H */
