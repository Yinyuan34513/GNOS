/*
 * pmm.c — physical page-frame allocator (bitmap). (GPLv2)
 *
 * Two passes over the Limine memory map: the first finds the highest usable
 * address so we know how big the bitmap must be, the second finds a usable
 * region big enough to hold it.  Only after the bitmap exists do we mark the
 * usable frames free -- everything starts out "in use", which is the safe
 * default for holes, MMIO and firmware regions we know nothing about.
 */
#include <stddef.h>
#include <stdint.h>

#include <limine.h>

#include "pmm.h"
#include "panic.h"
#include "debugcon.h"
#include "kstring.h"

extern volatile struct limine_memmap_request memmap_request;
extern uint64_t g_hhdm;

static uint8_t  *g_bitmap;          /* 1 = allocated, 0 = free */
static uint64_t  g_bitmap_bytes;
static uint64_t  g_total;           /* frames covered by the bitmap */
static uint64_t  g_free;
static uint64_t  g_next_hint;       /* where the last search stopped */

void *pmm_virt(uint64_t phys)
{
    return (void *)(uintptr_t)(phys + g_hhdm);
}

static void bit_set(uint64_t f)   { g_bitmap[f >> 3] |=  (uint8_t)(1u << (f & 7)); }
static void bit_clear(uint64_t f) { g_bitmap[f >> 3] &= (uint8_t)~(1u << (f & 7)); }
static int  bit_test(uint64_t f)  { return (g_bitmap[f >> 3] >> (f & 7)) & 1; }

static void mark_used(uint64_t base, uint64_t size)
{
    uint64_t first = base / PAGE_SIZE;
    uint64_t last  = (base + size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t f = first; f < last && f < g_total; f++) {
        if (!bit_test(f)) {
            bit_set(f);
            g_free--;
        }
    }
}

static void mark_free(uint64_t base, uint64_t size)
{
    /* Only whole frames fully inside the region become free. */
    uint64_t first = (base + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t last  = (base + size) / PAGE_SIZE;
    for (uint64_t f = first; f < last && f < g_total; f++) {
        if (bit_test(f)) {
            bit_clear(f);
            g_free++;
        }
    }
}

void pmm_init(void)
{
    if (!memmap_request.response)
        panic("pmm: bootloader gave us no memory map");

    struct limine_memmap_response *mm = memmap_request.response;

    /* Pass 1: how much address space must the bitmap cover? */
    uint64_t highest = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        uint64_t top = e->base + e->length;
        if (top > highest)
            highest = top;
    }
    if (highest == 0)
        panic("pmm: no usable RAM in the memory map");

    g_total        = highest / PAGE_SIZE;
    g_bitmap_bytes = (g_total + 7) / 8;

    /* Pass 2: park the bitmap in the first usable region that fits. */
    uint64_t bm_phys = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        if (e->length >= g_bitmap_bytes) {
            bm_phys = e->base;
            break;
        }
    }
    if (!bm_phys)
        panic("pmm: no single region large enough for the frame bitmap");

    g_bitmap = (uint8_t *)pmm_virt(bm_phys);

    /* Everything is used until proven otherwise. */
    memset(g_bitmap, 0xFF, g_bitmap_bytes);
    g_free = 0;

    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE)
            mark_free(e->base, e->length);
    }

    /* Reclaim nothing below 1 MiB: legacy BIOS structures live there and we
     * would rather not find out which ones still matter. */
    mark_used(0, 0x100000);

    /* And of course the bitmap itself is not up for grabs. */
    mark_used(bm_phys, g_bitmap_bytes);

    dbg_puts("PMM: ");
    dbg_puts_dec((uint32_t)(g_free * PAGE_SIZE / (1024 * 1024)));
    dbg_puts(" MiB free (");
    dbg_puts_dec((uint32_t)g_free);
    dbg_puts(" frames), bitmap ");
    dbg_puts_dec((uint32_t)g_bitmap_bytes);
    dbg_puts(" bytes @");
    dbg_puts_hex(bm_phys);
    dbg_puts("\r\n");
}

uint64_t pmm_alloc(void)
{
    for (int pass = 0; pass < 2; pass++) {
        uint64_t start = pass ? 0 : g_next_hint;
        uint64_t end   = pass ? g_next_hint : g_total;

        for (uint64_t f = start; f < end; f++) {
            if (bit_test(f))
                continue;
            bit_set(f);
            g_free--;
            g_next_hint = f + 1;
            return f * PAGE_SIZE;
        }
    }
    return 0;
}

uint64_t pmm_alloc_zeroed(void)
{
    uint64_t p = pmm_alloc();
    if (p)
        memset(pmm_virt(p), 0, PAGE_SIZE);
    return p;
}

void pmm_free(uint64_t phys)
{
    uint64_t f = phys / PAGE_SIZE;
    if (f >= g_total || !bit_test(f))
        return;
    bit_clear(f);
    g_free++;
    if (f < g_next_hint)
        g_next_hint = f;
}

uint64_t pmm_total_frames(void) { return g_total; }
uint64_t pmm_free_frames(void)  { return g_free; }
