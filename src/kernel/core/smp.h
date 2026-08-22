/*
 * smp.h — SMP support for GNOS. (GPLv2)
 *
 * Two-stage: smp_init() brings the APs online through Limine, and each AP
 * then joins the shared EEVDF scheduler from ap_main().  The per-CPU state
 * -- the current process, the kernel stack the ring-3 entry paths switch
 * to, and the idle context's park point -- lives in the first slots of
 * struct cpu, whose address every core keeps in GS.  The offsets of the
 * two slots isr.asm touches are fixed and documented below; they are part
 * of the C/asm ABI.
 *
 * Locking: the run queue and the process table are guarded by a spinlock
 * (g_proc_lock, private to proc.c).  Everything else the kernel shares is
 * serialised by the big kernel lock, so the rest of the tree stays
 * lock-free exactly as it was on one core.
 */
#ifndef GNUCOS_SMP_H
#define GNUCOS_SMP_H

#include <stdint.h>
#include "gdt.h"        /* for struct tss64 */

#define MAX_CPUS       16
#define PERCPU_KSTACK  0x4000

/* The kernel's GS base.  Each core points IA32_GS_BASE at its own cpu_t;
 * `self` (the first member) is what lets C find the structure via %gs:0. */
#define IA32_GS_BASE   0xC0000101ULL

/* One per core.  The first slot (index 0) is always the BSP. */
typedef struct cpu {
    struct cpu    *self;      /* == &g_cpu[cpu]; reachable as %gs:0  */
    int            id;        /* our index into g_cpu[]              */
    uint32_t       lapic_id;  /* the APIC id Limine reported         */
    uint8_t        online;    /* 1 once the AP has reached ap_main   */

    /* ---- per-CPU runtime slots --------------------------------------
     * Offsets are fixed and mirrored in isr.asm (CPU_KERNEL_RSP0 and
     * CPU_USER_RSP): syscall_entry loads kernel_rsp0 and stashes the
     * user RSP in user_rsp through GS, and the scheduler parks each
     * core's idle context in sched_rsp. */
    struct proc   *current;    /* +24: the process running on this core  */
    uint64_t       kernel_rsp0;/* +32: stack a ring-3 entry switches to  */
    uint64_t       user_rsp;   /* +40: syscall scratch (the user RSP)    */
    uint64_t       sched_rsp;  /* +48: where switch_context parks idle   */

    void          *stack_top;  /* top of this CPU's kernel stack  */
    uint64_t       gdt[7];     /* private GDT copy                */
    struct tss64   tss;        /* private TSS                     */
} cpu_t;

/* This core's own cpu_t.  Every CPU points IA32_GS_BASE at &g_cpu[cpu]
 * (BSP in gdt_init, APs in ap_main), so `mov %gs:0` reads the self pointer
 * and a single memory load finds the whole structure. */
static inline cpu_t *cpu_self(void)
{
    cpu_t *c;
    asm volatile("movq %%gs:0, %0" : "=r"(c) : : "memory");
    return c;
}

/* ---- spinlocks --------------------------------------------------------
 * The run queue and the process table are the one structure several cores
 * can touch at once; the kernel's other shared state is serialised by the
 * big kernel lock (BKL) instead, keeping the rest of the tree lock-free
 * exactly as before. */
typedef struct {
    volatile uint32_t v;
} spinlock_t;

static inline void spin_lock(spinlock_t *l)
{
    uint32_t x = 1;
    asm volatile(
        "1: xchgl %0, %1\n\t"
        "testl %0, %0\n\t"
        "jnz 1b"
        : "+r"(x), "+m"(l->v) : : "memory");
}

static inline void spin_unlock(spinlock_t *l)
{
    asm volatile("" ::: "memory");
    l->v = 0;
}

/* The big kernel lock.  Taken on every trap/syscall entry from ring 3 and
 * released on the way back; a process that blocks drops it before parking
 * so it never rides a parked context switch (see sched_block in proc.c). */
extern spinlock_t g_bkl;
void bkl_acquire(void);
void bkl_release(void);

extern cpu_t g_cpu[MAX_CPUS];
extern int    g_ncpus;         /* how many cores Limine reported  */
extern int    g_bsp_id;        /* lapic id of the bootstrap CPU   */

/* Per-CPU kernel stacks (defined in smp.c); gdt.c points the BSP's TSS.RSP0
 * at g_cpu_stack[0]. */
extern uint8_t g_cpu_stack[MAX_CPUS][PERCPU_KSTACK];

/* Discover the APs and start them.  Called once, on the BSP, after the
 * allocators and ACPI (the APs run the scheduler from ap_main on). */
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
