/*
 * drm_libc.c — freestanding libc bits the Uinxed DRM core assumes exist.
 * (GPLv2)
 *
 * The ported DRM files call the usual C library helpers; GNOS's kernel is
 * deliberately freestanding and provides none of them.  This file supplies
 * the handful the DRM core actually uses, each mapped onto a GNOS kernel
 * facility:
 *
 *   copy_from_user/copy_to_user   user_ptr_ok() + plain copy (the user
 *                                 pointer check is what makes it safe)
 *   nano_time()                   wall/uptime clock in nanoseconds
 *   aligned_alloc(4096, size)     page-aligned physical memory via PMM
 *   aligned_free()                matching release (PMM, not the heap)
 *   strdup()                      kernel-heap duplicate
 *
 * realloc() is provided as krealloc() in src/kernel/core/heap.c and mapped
 * here for the ported files.
 */
#include <stddef.h>
#include <stdint.h>

#include "kstring.h"
#include "heap.h"
#include "pmm.h"
#include "vmm.h"        /* user_ptr_ok */
#include "timer.h"      /* timer_ticks */
#include "drm_port.h"   /* errno maps */

/* Higher-half direct-map base (defined in the kernel's memory setup). */
extern uint64_t g_hhdm;

/* drm_port.h remaps the Uinxed spellings of these two to GNOS-style
 * wrappers via #define; this file provides the real symbols, so the
 * macros must be out of the way while we define them. */
#undef copy_from_user
#undef copy_to_user

/* ---- user-memory copy (same style as the rest of GNOS drivers) -------- */
int copy_from_user(void *k, uint64_t u, uint64_t n)
{
    if (!user_ptr_ok(u, n)) {
        dbg_puts("DRMCFU: FAIL u=0x");
        dbg_puts_hex(u);
        dbg_puts(" n=");
        dbg_puts_dec((uint32_t)n);
        dbg_puts("\r\n");
        return -E_FAULT;
    }
    memcpy(k, (const void *)(uintptr_t)u, n);
    return 0;
}

int copy_to_user(uint64_t u, const void *k, uint64_t n)
{
    if (!user_ptr_ok(u, n)) {
        dbg_puts("DRMCTU: FAIL u=0x");
        dbg_puts_hex(u);
        dbg_puts(" n=");
        dbg_puts_dec((uint32_t)n);
        dbg_puts("\r\n");
        return -E_FAULT;
    }
    memcpy((void *)(uintptr_t)u, k, n);
    return 0;
}

/* ---- monotonic clock in nanoseconds (100 Hz ticks -> 10 ms each) ------ */
uint64_t nano_time(void)
{
    return timer_ticks() * 10000000ULL;
}

/* ---- page-aligned allocation for dumb-buffer backing ------------------
 * GNOS's kernel heap hands out 16-byte-aligned blocks, not pages; the DRM
 * dumb allocator wants page-aligned physical memory (its mmap maps the
 * backing directly).  Allocate whole frames through the PMM and return the
 * HHDM-mapped virtual address, remembering the frame count so the matching
 * free can release exactly what was taken.  The record lives in a tiny
 * side table: dumb buffers are few and the pairing is simple. */
#define ALIGN_SLOTS 64

struct align_slot {
    void    *v;
    uint64_t nframes;
};

static struct align_slot g_align[ALIGN_SLOTS];

void *aligned_alloc(size_t align, size_t size)
{
    uint64_t nframes;

    (void)align;                     /* always page-aligned here */
    if (!size)
        return NULL;
    nframes = ((uint64_t)size + 0xFFF) >> 12;

    uint64_t phys = pmm_alloc_contiguous(nframes);
    if (!phys)
        return NULL;

    void *v = pmm_virt(phys);
    for (int i = 0; i < ALIGN_SLOTS; i++) {
        if (!g_align[i].v) {
            g_align[i].v       = v;
            g_align[i].nframes = nframes;
            return v;
        }
    }
    /* Table full: give the frames back rather than leak them. */
    for (uint64_t i = 0; i < nframes; i++)
        pmm_free(phys + (i << 12));
    return NULL;
}

void aligned_free(void *p)
{
    if (!p)
        return;

    for (int i = 0; i < ALIGN_SLOTS; i++) {
        if (g_align[i].v != p)
            continue;
        uint64_t phys = (uint64_t)(uintptr_t)p - g_hhdm;
        for (uint64_t f = 0; f < g_align[i].nframes; f++)
            pmm_free(phys + (f << 12));
        g_align[i].v       = NULL;
        g_align[i].nframes = 0;
        return;
    }
}

/* ---- kernel-heap string duplicate -------------------------------------- */
char *strdup(const char *s)
{
    size_t len;
    char  *p;

    if (!s)
        return NULL;
    len = strlen(s) + 1;
    p   = kmalloc(len);
    if (p)
        memcpy(p, s, len);
    return p;
}
