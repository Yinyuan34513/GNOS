/*
 * gdt.c — build and load our own GDT + TSS, one copy per CPU. (GPLv2)
 */
#include <stdint.h>

#include "gdt.h"
#include "smp.h"
#include "debugcon.h"

/* Code/data descriptors.  In long mode the base and limit are ignored for
 * these; only P, DPL, S, the type bits and L (64-bit code) have any effect,
 * so the classic flat 4 GiB encodings below are just convention. */
#define DESC_KCODE  0x00AF9A000000FFFFULL   /* P DPL0 S exec rw, L=1 */
#define DESC_KDATA  0x00CF92000000FFFFULL   /* P DPL0 S data rw      */
#define DESC_UDATA  0x00CFF2000000FFFFULL   /* P DPL3 S data rw      */
#define DESC_UCODE  0x00AFFA000000FFFFULL   /* P DPL3 S exec rw, L=1 */

/*
 * Point the current CPU's TSS.RSP0 -- and, in lockstep, the per-CPU slot
 * isr.asm's syscall_entry loads -- at the current process's kernel stack.
 * Keeping the two mirrors together is what stops the interrupt path (which
 * the CPU routes through the TSS) and the `syscall` path (which loads RSP
 * by hand) from disagreeing about which stack the process owns.
 */
void tss_set_rsp0(uint64_t rsp0)
{
    cpu_t *c = cpu_self();
    c->tss.rsp[0] = rsp0;
    c->kernel_rsp0 = rsp0;
}

/* Fill c->gdt and c->tss with a complete descriptor set for one CPU.  The
 * seven GDT slots are: null, kcode, kdata, udata, ucode, then the 16-byte
 * TSS descriptor spanning slots 5 and 6. */
void gdt_build(struct cpu *c)
{
    for (unsigned i = 0; i < 7; i++)
        c->gdt[i] = 0;

    c->gdt[1] = DESC_KCODE;
    c->gdt[2] = DESC_KDATA;
    c->gdt[3] = DESC_UDATA;
    c->gdt[4] = DESC_UCODE;

    for (unsigned i = 0; i < sizeof(c->tss); i++)
        ((uint8_t *)&c->tss)[i] = 0;

    uint64_t tss_base  = (uint64_t)(uintptr_t)&c->tss;
    uint32_t tss_limit = sizeof(c->tss) - 1;
    c->gdt[5] = (uint64_t)(tss_limit & 0xFFFF)
             | ((tss_base & 0xFFFFFFULL) << 16)
             | (0x89ULL << 40)                 /* present, 64-bit TSS avail */
             | ((uint64_t)((tss_limit >> 16) & 0xF) << 48)
             | (((tss_base >> 24) & 0xFFULL) << 56);
    c->gdt[6] = (tss_base >> 32) & 0xFFFFFFFFULL;

    c->tss.rsp[0]    = (uint64_t)(uintptr_t)c->stack_top;
    c->tss.iomap_base = sizeof(c->tss);        /* no I/O bitmap */
}

void gdt_init(void)
{
    /* The BSP's per-CPU slot must exist before we build its GDT.  smp_init()
     * fills the rest; here we just make sure CPU 0 has a stack to point at. */
    if (!g_cpu[0].stack_top)
        g_cpu[0].stack_top = (void *)((uintptr_t)g_cpu_stack[0] + PERCPU_KSTACK);

    /* The BSP finds its own cpu_t through %gs:0, so point GS at it before
     * anything touches cpu_self().  APs do the same in ap_main(). */
    g_cpu[0].self = &g_cpu[0];
    uint64_t gsbase = (uint64_t)(uintptr_t)&g_cpu[0];
    asm volatile("wrmsr" :: "c"((uint32_t)IA32_GS_BASE),
                            "a"((uint32_t)(gsbase & 0xFFFFFFFFu)),
                            "d"((uint32_t)(gsbase >> 32))
                 : "memory");
    uint32_t gl, gh;
    asm volatile("rdmsr" : "=a"(gl), "=d"(gh) : "c"((uint32_t)IA32_GS_BASE));
    dbg_puts("GDT: gsbase=");
    dbg_puts_hex(((uint64_t)gh << 32) | gl);
    dbg_puts(" (cpu 0)\r\n");

    gdt_build(&g_cpu[0]);

    struct gdtr gdtr = { .limit = sizeof(g_cpu[0].gdt) - 1,
                         .base  = (uint64_t)(uintptr_t)g_cpu[0].gdt };

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

    dbg_puts("GNOS: GDT/TSS installed (BSP), rsp0=");
    dbg_puts_hex(g_cpu[0].tss.rsp[0]);
    dbg_puts("\r\n");
}
