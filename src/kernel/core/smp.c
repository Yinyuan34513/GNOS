/*
 * smp.c — multi-core bring-up for GNOS. (GPLv2)
 *
 * Brings the APs reported by Limine online and runs them into the shared
 * EEVDF scheduler.  Every core owns its cpu_t slot (found through GS), its
 * GDT/TSS, its stack and -- once lapic_timer_start() has run -- its own
 * 100 Hz clock; the run queue and the process table are shared under
 * g_proc_lock, everything else under the big kernel lock defined here.
 */
#include <stdint.h>
#include <limine.h>

#include "smp.h"
#include "gdt.h"
#include "idt.h"
#include "debugcon.h"
#include "lapic.h"
#include "proc.h"

cpu_t g_cpu[MAX_CPUS];
int   g_ncpus;
int   g_bsp_id;

/* The big kernel lock.  Taken by isr_dispatch on every trap from ring 3
 * and dropped before a process parks; the rest of the kernel stays
 * lock-free exactly as it was on one core. */
spinlock_t g_bkl;

void bkl_acquire(void)
{
    spin_lock(&g_bkl);
}

void bkl_release(void)
{
    spin_unlock(&g_bkl);
}

/* Static per-CPU stacks, ready before smp_init() runs (no heap needed). */
uint8_t g_cpu_stack[MAX_CPUS][PERCPU_KSTACK] __attribute__((aligned(16)));

/* A flat array of stack tops, indexed by cpu, so the AP trampoline can load
 * RSP without knowing the layout of struct cpu. */
void *g_ap_stack_top[MAX_CPUS];

/* Defined in limine_requests.c; NULL if Limine did not honour the request
 * (old firmware / single-core). */
extern volatile struct limine_smp_request smp_request;

int smp_online_count(void)
{
    int n = 0;
    for (int i = 0; i < g_ncpus; i++)
        if (g_cpu[i].online)
            n++;
    return n;
}

void smp_init(void)
{
    if (!smp_request.response) {
        g_ncpus = 1;
        g_cpu[0].id        = 0;
        g_cpu[0].online    = 1;
        g_cpu[0].stack_top = (void *)((uintptr_t)g_cpu_stack[0] + PERCPU_KSTACK);
        dbg_puts("GNOS: SMP disabled (no response from Limine)\r\n");
        return;
    }

    struct limine_smp_response *resp = smp_request.response;
    g_ncpus   = (int)resp->cpu_count;
    g_bsp_id  = (int)resp->bsp_lapic_id;
    if (g_ncpus > MAX_CPUS)
        g_ncpus = MAX_CPUS;

    for (int i = 0; i < g_ncpus; i++) {
        g_cpu[i].id        = i;
        g_cpu[i].lapic_id  = resp->cpus[i]->lapic_id;
        g_cpu[i].stack_top = (void *)((uintptr_t)g_cpu_stack[i] + PERCPU_KSTACK);
        g_ap_stack_top[i]  = g_cpu[i].stack_top;
        /* The BSP (cpus[0]) is already running; everyone else is not online
         * until its trampoline reaches ap_main(). */
        g_cpu[i].online    = (i == 0);
    }

    /* Hand each AP its entry point and the cpu index it should use.  Writing
     * goto_address is what releases the AP: Limine starts it the moment the
     * field is non-NULL. */
    for (int i = 1; i < g_ncpus; i++) {
        resp->cpus[i]->extra_argument = (uint64_t)i;
        resp->cpus[i]->goto_address   = (limine_goto_address)ap_entry;
    }

    /* Wait (bounded) for every AP to report in.  APs are parked afterwards,
     * so a slow or dead core just stays offline rather than hanging boot. */
    for (volatile unsigned t = 0; t < 200000000U; t++) {
        int all = 1;
        for (int i = 1; i < g_ncpus; i++)
            if (!g_cpu[i].online) { all = 0; break; }
        if (all)
            break;
    }

    dbg_puts("GNOS: SMP: ");
    dbg_puts_dec((uint32_t)smp_online_count());
    dbg_puts(" CPU(s) online (BSP lapic ");
    dbg_puts_dec((uint32_t)g_bsp_id);
    dbg_puts(")\r\n");
}

/* C entry for an AP.  RSP already points at this CPU's stack (the trampoline
 * set it before calling), so it is safe to use C here. */
void ap_main(int cpu)
{
    /* Publish the self pointer and point GS at this core's slot *before*
     * anything touches cpu_self() (gdt_build -> tss_set_rsp0 does). */
    g_cpu[cpu].self = &g_cpu[cpu];
    uint64_t base = (uint64_t)(uintptr_t)&g_cpu[cpu];
    asm volatile("wrmsr" :: "c"((uint32_t)IA32_GS_BASE),
                            "a"((uint32_t)(base & 0xFFFFFFFFu)),
                            "d"((uint32_t)(base >> 32))
                 : "memory");

    /* Build and load this core's own GDT/TSS. */
    gdt_build(&g_cpu[cpu]);

    struct gdtr gdtr = { .limit = sizeof(g_cpu[cpu].gdt) - 1,
                         .base  = (uint64_t)(uintptr_t)g_cpu[cpu].gdt };
    asm volatile("lgdt %0" :: "m"(gdtr) : "memory");

    /* Reload every data selector onto the new GDT. */
    asm volatile(
        "mov %0, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "mov %1, %%ax\n\t"
        "ltr %%ax\n\t"
        : : "i"(SEL_KDATA), "i"(SEL_TSS) : "rax", "memory");

    /* `mov %ax,%gs` zeroed IA32_GS_BASE (long-mode selector reload); restore
     * the per-CPU pointer set above or cpu_self() faults at address 0. */
    g_cpu[cpu].self = &g_cpu[cpu];
    asm volatile("wrmsr" :: "c"((uint32_t)IA32_GS_BASE),
                            "a"((uint32_t)(base & 0xFFFFFFFFu)),
                            "d"((uint32_t)(base >> 32))
                 : "memory");

    /* The shared IDT is a safety net until interrupts come on. */
    idt_load();

    g_cpu[cpu].online = 1;

    /* This core's own clock, then join the scheduler.  With an empty run
     * queue schedule() falls back to hlt until the first process is ready;
     * the LAPIC timer makes sure a wake-up tick eventually lands. */
    lapic_init();
    lapic_timer_start();
    sched_start();
    __builtin_unreachable();
}
