/*
 * gdt.c — build and load our own GDT + TSS. (GPLv2)
 */
#include <stdint.h>

#include "gdt.h"
#include "debugcon.h"

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* The 64-bit TSS: in long mode it no longer holds a task context, only the
 * privilege-level stacks (RSP0..2), the interrupt-stack table and the I/O
 * permission bitmap offset. */
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

/* 7 slots: null, kcode, kdata, udata, ucode + 2 for the 16-byte TSS desc. */
static uint64_t     g_gdt[7];
static struct tss64 g_tss;

/* Stack the CPU switches to when a trap arrives from ring 3. */
static uint8_t g_kstack[0x4000] __attribute__((aligned(16)));

/* Code/data descriptors.  In long mode the base and limit are ignored for
 * these; only P, DPL, S, the type bits and L (64-bit code) have any effect,
 * so the classic flat 4 GiB encodings below are just convention. */
#define DESC_KCODE  0x00AF9A000000FFFFULL   /* P DPL0 S exec rw, L=1 */
#define DESC_KDATA  0x00CF92000000FFFFULL   /* P DPL0 S data rw      */
#define DESC_UDATA  0x00CFF2000000FFFFULL   /* P DPL3 S data rw      */
#define DESC_UCODE  0x00AFFA000000FFFFULL   /* P DPL3 S exec rw, L=1 */

/*
 * A mirror of the TSS's RSP0, readable from assembly.  The `syscall`
 * instruction does no stack switching of its own and never consults the TSS,
 * so syscall_entry has to load the kernel stack by hand; keeping the copy in
 * lockstep here is what stops the two kernel entry paths from disagreeing
 * about which stack the current process owns.
 */
uint64_t g_kernel_rsp0;

void tss_set_rsp0(uint64_t rsp0)
{
    g_tss.rsp[0]  = rsp0;
    g_kernel_rsp0 = rsp0;
}

void gdt_init(void)
{
    g_gdt[0] = 0;
    g_gdt[1] = DESC_KCODE;
    g_gdt[2] = DESC_KDATA;
    g_gdt[3] = DESC_UDATA;
    g_gdt[4] = DESC_UCODE;

    /* TSS descriptor spans two GDT slots in long mode. */
    for (unsigned i = 0; i < sizeof(g_tss); i++)
        ((uint8_t *)&g_tss)[i] = 0;
    tss_set_rsp0((uint64_t)(uintptr_t)g_kstack + sizeof(g_kstack));
    g_tss.iomap_base = sizeof(g_tss);          /* no I/O bitmap */

    uint64_t base  = (uint64_t)(uintptr_t)&g_tss;
    uint32_t limit = sizeof(g_tss) - 1;
    g_gdt[5] = (uint64_t)(limit & 0xFFFF)
             | ((base & 0xFFFFFFULL) << 16)
             | (0x89ULL << 40)                 /* present, 64-bit TSS available */
             | ((uint64_t)((limit >> 16) & 0xF) << 48)
             | (((base >> 24) & 0xFFULL) << 56);
    g_gdt[6] = (base >> 32) & 0xFFFFFFFFULL;

    struct gdtr gdtr = { .limit = sizeof(g_gdt) - 1,
                         .base  = (uint64_t)(uintptr_t)g_gdt };

    /* Load the table, then reload every selector.  CS can only be changed by
     * a far transfer, so we fake one with a far return to the next label. */
    asm volatile(
        "lgdt %0\n\t"
        "mov %1, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "pushq %2\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "mov %3, %%ax\n\t"
        "ltr %%ax\n\t"
        :
        : "m"(gdtr), "i"(SEL_KDATA), "i"(SEL_KCODE), "i"(SEL_TSS)
        : "rax", "memory");

    dbg_puts("GNOS: GDT/TSS installed, rsp0=");
    dbg_puts_hex(g_tss.rsp[0]);
    dbg_puts("\r\n");
}
