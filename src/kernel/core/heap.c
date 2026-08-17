/*
 * heap.c — the kernel heap: kmalloc/kfree over a contiguous PMM region. (GPLv2)
 *
 * Design notes (see heap.h): one contiguous physical run, accessed through the
 * HHDM, managed as an explicit doubly-linked free list with boundary tags.
 *
 *   block layout in memory:
 *     [ header ][        payload         ][ footer ]
 *     header:  size (total, incl. hdr+footer, 16-aligned), free flag,
 *              prev/next pointers (free blocks only use the links)
 *     footer:  size (mirror of the header size, for backward coalescing)
 *
 * A free block's links hang off the header; a used block's payload begins
 * right after the header.  Coalescing checks neighbours only when they are
 * inside the heap's single region, so there is never a cross-region merge.
 */
#include <stddef.h>
#include <stdint.h>

#include "heap.h"
#include "pmm.h"
#include "kstring.h"
#include "debugcon.h"
#include "panic.h"

#define KHEAP_HDR_MAGIC  0x4B484452534Au   /* "KHDR"-ish, sanity tag */
#define KHEAP_MIN_BLOCK  32                /* smallest block we will split off */
#define KHEAP_ALIGN     16u

typedef struct kheap_hdr {
    uint64_t size;        /* total block size (header + payload + footer) */
    uint64_t free;        /* 1 = free, 0 = used */
    struct kheap_hdr *prev;
    struct kheap_hdr *next;
} kheap_hdr_t;

#define HDR_SIZE    (sizeof(kheap_hdr_t))   /* 32 bytes on LP64 */
#define FOOT_SIZE   8u                       /* footer holds the size only */

/* Read/write the footer of the block that starts at `h`. */
static uint64_t *footer_of(kheap_hdr_t *h)
{
    return (uint64_t *)((uint8_t *)h + h->size - FOOT_SIZE);
}

static void set_footer(kheap_hdr_t *h)
{
    *footer_of(h) = h->size;
}

/* The block immediately after `h` (may be outside the region). */
static kheap_hdr_t *block_after(kheap_hdr_t *h)
{
    return (kheap_hdr_t *)((uint8_t *)h + h->size);
}

/* The block immediately before `h` is found via the footer size that sits in
 * the 8 bytes just before this header's start; we read that inline in kfree()
 * rather than through a helper. */

static uint8_t      *g_base;
static uint64_t      g_size;
static kheap_hdr_t  *g_free_head;   /* head of the doubly-linked free list */

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(uint64_t)(a - 1);
}

/* Detach `h` from the free list. */
static void free_unlink(kheap_hdr_t *h)
{
    if (h->prev)
        h->prev->next = h->next;
    else
        g_free_head = h->next;
    if (h->next)
        h->next->prev = h->prev;
    h->prev = h->next = NULL;
}

/* Insert `h` at the front of the free list. */
static void free_link(kheap_hdr_t *h)
{
    h->free = 1;
    h->prev = NULL;
    h->next = g_free_head;
    if (g_free_head)
        g_free_head->prev = h;
    g_free_head = h;
}

void kheap_init(void)
{
    /* 16 MiB to start; fall back to smaller sizes if contiguous RAM is tight.
     * The heap is fixed-size and does not grow. */
    static const uint64_t tries[] = { 16, 8, 4, 2, 1 };
    uint64_t chosen = 0;
    for (uint64_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
        uint64_t frames = tries[i] * 1024 * 1024 / PAGE_SIZE;
        uint64_t phys = pmm_alloc_contiguous(frames);
        if (phys) {
            g_base = (uint8_t *)pmm_virt(phys);
            g_size = frames * PAGE_SIZE;
            chosen = tries[i];
            break;
        }
    }
    if (!g_base)
        panic("kheap: could not allocate a contiguous region");

    memset(g_base, 0, g_size);

    /* One free block spanning the whole region, bounded by a permanent
     * sentinel at the very end so forward coalescing never runs off. */
    kheap_hdr_t *b = (kheap_hdr_t *)g_base;
    b->size  = g_size - HDR_SIZE - FOOT_SIZE;
    b->free  = 1;
    b->prev  = NULL;
    b->next  = NULL;
    set_footer(b);

    kheap_hdr_t *sentinel = (kheap_hdr_t *)(g_base + g_size - HDR_SIZE);
    sentinel->size = 0;        /* size 0 marks "end of heap" */
    sentinel->free = 0;
    sentinel->prev = sentinel->next = NULL;

    g_free_head = b;

    dbg_puts("KHEAP: ");
    dbg_puts_dec((uint32_t)chosen);
    dbg_puts(" MiB @");
    dbg_puts_hex((uint64_t)(uintptr_t)g_base);
    dbg_puts("\r\n");
}

void *kmalloc(size_t size)
{
    if (!size)
        size = 1;
    if (!g_base)
        return NULL;

    uint64_t need = align_up((uint64_t)size + HDR_SIZE + FOOT_SIZE, KHEAP_ALIGN);
    if (need < KHEAP_MIN_BLOCK)
        need = KHEAP_MIN_BLOCK;

    for (kheap_hdr_t *b = g_free_head; b; b = b->next) {
        if (b->size < need)
            continue;

        if (b->size - need >= KHEAP_MIN_BLOCK) {
            /* Split: keep `need` for the allocation, carve the remainder as a
             * fresh free block right after it. */
            kheap_hdr_t *rest = (kheap_hdr_t *)((uint8_t *)b + need);
            rest->size  = b->size - need;
            rest->free  = 1;
            rest->prev  = NULL;
            rest->next  = NULL;
            set_footer(rest);
            free_link(rest);

            b->size = need;
        }

        free_unlink(b);
        b->free = 0;
        set_footer(b);
        return (void *)((uint8_t *)b + HDR_SIZE);
    }

    return NULL;
}

void kfree(void *p)
{
    if (!p)
        return;

    kheap_hdr_t *b = (kheap_hdr_t *)((uint8_t *)p - HDR_SIZE);

    /* Coalesce forward: merge with the next block if it is free and inside
     * the heap region. */
    kheap_hdr_t *after = block_after(b);
    uintptr_t after_end = (uintptr_t)after + HDR_SIZE;
    if ((uintptr_t)after >= (uintptr_t)g_base + g_size ||
        after_end > (uintptr_t)g_base + g_size) {
        after = NULL;   /* sentinel / out of region */
    }
    if (after && after->free) {
        free_unlink(after);
        b->size += after->size;
    }

    /* Coalesce backward: merge with the previous block if it is free. */
    if ((uintptr_t)b > (uintptr_t)g_base) {
        uint64_t prev_size = *(uint64_t *)((uint8_t *)b - FOOT_SIZE);
        kheap_hdr_t *before = (kheap_hdr_t *)((uint8_t *)b - prev_size);
        if (before >= (kheap_hdr_t *)(void *)g_base && before->free) {
            free_unlink(before);
            before->size += b->size;
            b = before;
        }
    }

    set_footer(b);
    free_link(b);
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void *kcalloc(size_t n, size_t size)
{
    return kzalloc(n * size);
}

void kheap_self_test(void)
{
    dbg_puts("KHEAP: self-test ... ");

    void *a = kmalloc(100);
    void *b = kmalloc(200);
    void *c = kmalloc(50);
    uintptr_t pa = (uintptr_t)a, pb = (uintptr_t)b, pc = (uintptr_t)c;
    int ok = (a && b && c);
    /* No two live allocations may overlap. */
    if (ok) {
        ok = (pa + 100 <= pb) && (pb + 200 <= pc) &&
             (pa != pb) && (pb != pc) && (pa != pc);
    }
    /* Writes must not fault and must stick. */
    if (ok) {
        memset(a, 0xAB, 100);
        memset(b, 0xCD, 200);
        ok = (*(uint8_t *)a == 0xAB) && (*(uint8_t *)b == 0xCD);
    }
    /* Freeing the middle block and reallocating should coalesce and let a
     * larger request reuse the space without overlapping the survivors. */
    if (ok) {
        kfree(b);
        void *d = kmalloc(150);
        ok = (d != NULL);
        if (ok) {
            uintptr_t pd = (uintptr_t)d;
            ok = (pd + 150 <= pc) && (pd != pa) && (pd != pc);
            memset(d, 0xEF, 150);
            ok = ok && (*(uint8_t *)d == 0xEF);
        }
    }
    /* Free everything; the region should be usable again. */
    if (ok) {
        kfree(a); kfree(c);
        void *e = kmalloc(64);
        ok = (e != NULL);
        if (e)
            kfree(e);
    }

    dbg_puts(ok ? "ok\r\n" : "FAIL\r\n");
}
