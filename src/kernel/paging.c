/*
 * paging.c — flip page-table permissions so user mode can reach a range.
 * (GPLv2)
 *
 * Access rights in a 4-level walk are the AND of every level, so marking the
 * PML4/PDPT/PD entries U/S is harmless on its own: memory only becomes
 * user-visible once the *leaf* entry also has U/S set.  That lets us open a
 * precise window into the identity map without disturbing anything else.
 */
#include <stddef.h>
#include <stdint.h>

#include "paging.h"
#include "panic.h"
#include "debugcon.h"
#include "kstring.h"

#define PG_P    0x001
#define PG_RW   0x002
#define PG_U    0x004
#define PG_PS   0x080
#define PG_NX   (1ULL << 63)

#define PG_ADDR 0x000FFFFFFFFFF000ULL

/* Filled in by kernel_entry() from the Limine responses. */
extern uint64_t g_hhdm;
extern uint64_t g_kernel_phys;
extern uint64_t g_kernel_virt;

/* Fresh page tables come out of the kernel's own BSS: it is already mapped,
 * and we can convert its addresses to physical exactly because Limine told
 * us where it put the kernel image. */
static uint8_t  g_pool[0x8000] __attribute__((aligned(0x1000)));
static uint32_t g_pool_used;

static uint64_t *phys_to_virt(uint64_t p)
{
    return (uint64_t *)(uintptr_t)(p + g_hhdm);
}

static uint64_t kern_to_phys(const void *v)
{
    return (uint64_t)(uintptr_t)v - g_kernel_virt + g_kernel_phys;
}

static uint64_t *alloc_table(uint64_t *phys_out)
{
    if (g_pool_used + 0x1000 > sizeof(g_pool))
        panic("paging: out of page-table pool");

    uint64_t *t = (uint64_t *)(g_pool + g_pool_used);
    g_pool_used += 0x1000;

    memset(t, 0, 0x1000);
    *phys_out = kern_to_phys(t);
    return t;
}

/* Replace a large-page entry with a table of smaller entries covering the
 * same physical range, preserving its flags. */
static uint64_t *split_large(uint64_t *entry, uint64_t child_page_size,
                             int child_is_large)
{
    uint64_t old   = *entry;
    uint64_t base  = old & PG_ADDR;
    uint64_t flags = old & (PG_RW | PG_U | PG_NX | 0x18);  /* RW U NX PCD PWT */

    uint64_t tbl_phys;
    uint64_t *tbl = alloc_table(&tbl_phys);

    for (unsigned i = 0; i < 512; i++)
        tbl[i] = (base + (uint64_t)i * child_page_size) | flags | PG_P
               | (child_is_large ? PG_PS : 0);

    *entry = tbl_phys | PG_P | PG_RW | PG_U;
    return tbl;
}

void paging_make_user(uint64_t vaddr, uint64_t size)
{
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = phys_to_virt(cr3 & PG_ADDR);

    uint64_t start = vaddr & ~0xFFFULL;
    uint64_t end   = (vaddr + size + 0xFFF) & ~0xFFFULL;

    for (uint64_t va = start; va < end; va += 0x1000) {
        unsigned i4 = (va >> 39) & 0x1FF;
        unsigned i3 = (va >> 30) & 0x1FF;
        unsigned i2 = (va >> 21) & 0x1FF;
        unsigned i1 = (va >> 12) & 0x1FF;

        if (!(pml4[i4] & PG_P))
            panic("paging: user range is not mapped (PML4)");
        pml4[i4] = (pml4[i4] | PG_U | PG_RW) & ~PG_NX;

        uint64_t *pdpt = phys_to_virt(pml4[i4] & PG_ADDR);
        if (!(pdpt[i3] & PG_P))
            panic("paging: user range is not mapped (PDPT)");
        if (pdpt[i3] & PG_PS)                       /* 1 GiB page -> 2 MiB */
            split_large(&pdpt[i3], 0x200000, 1);
        pdpt[i3] = (pdpt[i3] | PG_U | PG_RW) & ~PG_NX;

        uint64_t *pd = phys_to_virt(pdpt[i3] & PG_ADDR);
        if (!(pd[i2] & PG_P))
            panic("paging: user range is not mapped (PD)");
        if (pd[i2] & PG_PS)                         /* 2 MiB page -> 4 KiB */
            split_large(&pd[i2], 0x1000, 0);
        pd[i2] = (pd[i2] | PG_U | PG_RW) & ~PG_NX;

        uint64_t *pt = phys_to_virt(pd[i2] & PG_ADDR);
        if (!(pt[i1] & PG_P))
            panic("paging: user range is not mapped (PT)");
        pt[i1] = (pt[i1] | PG_U | PG_RW | PG_P) & ~PG_NX;

        asm volatile("invlpg (%0)" :: "r"(va) : "memory");
    }

    dbg_puts("GNOS: user window ");
    dbg_puts_hex(start);
    dbg_puts(" .. ");
    dbg_puts_hex(end);
    dbg_puts("\r\n");
}
