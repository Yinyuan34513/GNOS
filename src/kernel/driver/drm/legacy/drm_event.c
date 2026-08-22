/*
 * drm_event.c — the kernel->user event queue. (GPLv2)
 *
 * wlroots' legacy uAPI path requests DRM_MODE_PAGE_FLIP_EVENT on every flip
 * and then blocks in drmHandleEvent() reading the flip-complete event back
 * off the fd.  GNOS's flip is synchronous, so the event is queued the moment
 * the ioctl returns and drained by a read() of the expected struct.  A
 * ring buffer keeps several in flight (a client can queue flips ahead of
 * display while later ones complete), and the poll() probe reports
 * readiness so wlroots' epoll integration wakes instead of busy-looping.
 */
#include "drm_internal.h"
#include "proc.h"
#include "kstring.h"

#define DRM_EVQ_CAP 8

typedef struct {
    drm_event_vblank_t ev[DRM_EVQ_CAP];
    unsigned head, count;
    uint32_t seq;
    uint64_t now_ms;
} drm_evq_t;

static drm_evq_t g_evq;

void drm_queue_event(uint64_t user_data)
{
    g_evq.seq++;
    drm_event_vblank_t *e = &g_evq.ev[(g_evq.head + g_evq.count) % DRM_EVQ_CAP];
    e->base.length = sizeof(drm_event_vblank_t);
    e->base.type = DRM_EVENT_FLIP_COMPLETE;
    e->user_data = user_data;
    e->tv_sec = 0;
    e->tv_usec = 0;
    e->sequence = g_evq.seq;
    e->crtc_id = 1;
    if (g_evq.count < DRM_EVQ_CAP)
        g_evq.count++;
    else
        g_evq.head = (g_evq.head + 1) % DRM_EVQ_CAP;
    sched_wake_reason(WAIT_PIPE);       /* a poll() sleeper may be parked */
}

int32_t drm_read(vfs_node_t *n, uint64_t off, void *buf, uint32_t len)
{
    (void)n;
    (void)off;
    if (!buf || len < sizeof(drm_event_t))
        return -E_INVAL;

    /* Block until an event arrives (test-and-sleep like the pipe, atomic
     * against the flip ioctl on a single CPU with cli). */
    proc_t *me = proc_current();
    asm volatile("cli");
    while (g_evq.count == 0) {
        if (!me || proc_pending_signals(me)) {
            asm volatile("sti");
            return -E_INTR;
        }
        sched_block_irqoff(WAIT_PIPE);
    }
    if (len >= sizeof(drm_event_vblank_t)) {
        drm_event_vblank_t e = g_evq.ev[g_evq.head];
        g_evq.head = (g_evq.head + 1) % DRM_EVQ_CAP;
        g_evq.count--;
        asm volatile("sti");
        memcpy(buf, &e, sizeof e);
        return (int32_t)sizeof e;
    }
    drm_event_t e = { .length = g_evq.ev[g_evq.head].base.length,
                      .type = g_evq.ev[g_evq.head].base.type };
    g_evq.head = (g_evq.head + 1) % DRM_EVQ_CAP;
    g_evq.count--;
    asm volatile("sti");
    memcpy(buf, &e, sizeof e);
    return (int32_t)sizeof e;
}

int drm_poll(vfs_node_t *n, int16_t events, int16_t *revents)
{
    (void)n;
    int16_t r = 0;
    if (events & POLLIN)
        r |= (g_evq.count > 0) ? POLLIN : 0;
    *revents = r;
    return 0;
}
