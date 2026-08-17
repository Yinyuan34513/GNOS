/*
 * smp.h — minimal multi-core bring-up for GNOS. (GPLv2)
 *
 * This is the *first* stage of SMP: it discovers the APs through Limine and
 * brings them online, but they are parked in a cli/hlt loop and do no work.
 * The BSP still runs everything.  Per-CPU scheduling, the LAPIC timer and
 * inter-processor interrupts are deliberately out of scope here — the kernel
 * has no locks yet, so letting two cores touch the process table would be a
 * bug.  See the plan for the next stages.
 */
#ifndef GNUCOS_SMP_H
#define GNUCOS_SMP_H

#include <stdint.h>
#include "gdt.h"        /* for struct tss64 */

#define MAX_CPUS       16
#define PERCPU_KSTACK  0x4000

/* One per core.  The first slot (index 0) is always the BSP. */
typedef struct cpu {
    int            id;         /* our index into g_cpu[]          */
    uint32_t       lapic_id;   /* the APIC id Limine reported     */
    uint8_t        online;     /* 1 once the AP has reached ap_main */
    void          *stack_top;  /* top of this CPU's kernel stack  */
    uint64_t       gdt[7];     /* private GDT copy                */
    struct tss64   tss;        /* private TSS                     */
} cpu_t;

extern cpu_t g_cpu[MAX_CPUS];
extern int    g_ncpus;         /* how many cores Limine reported  */
extern int    g_bsp_id;        /* lapic id of the bootstrap CPU   */

/* Per-CPU kernel stacks (defined in smp.c); gdt.c points the BSP's TSS.RSP0
 * at g_cpu_stack[0]. */
extern uint8_t g_cpu_stack[MAX_CPUS][PERCPU_KSTACK];

/* Discover the APs and start them.  Called once, on the BSP, right after
 * gdt_init()/idt_init().  APs that fail to come up are simply left parked. */
void smp_init(void);

/* C entry point for an AP; called from ap_trampoline.asm once its own stack
 * and GDT are loaded.  Does not return. */
void ap_main(int cpu);

/* AP reset vector, defined in ap_trampoline.asm and handed to Limine as each
 * core's goto_address.  Declared here so smp_init() can take its address. */
void ap_entry(void);

/* Number of cores that actually came online (BSP always counts). */
int smp_online_count(void);

#endif
