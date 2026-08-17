/*
 * gdt.h — 64-bit GDT + TSS. (GPLv2)
 *
 * Limine hands us its own GDT, which we are not allowed to keep once we
 * start switching privilege levels: we need ring-3 descriptors and a TSS
 * whose RSP0 points at a kernel stack, so that a trap taken in user mode
 * lands on a safe stack instead of the user's.
 *
 * Selector layout (the order matters: SYSRET wants user data at base+8 and
 * user code at base+16, so keep kernel data / user data / user code adjacent):
 *
 *   0x00  null
 *   0x08  kernel code   (ring 0, 64-bit)
 *   0x10  kernel data   (ring 0)
 *   0x18  user data     (ring 3)
 *   0x20  user code     (ring 3, 64-bit)
 *   0x28  TSS           (16-byte system descriptor, occupies 0x28 and 0x30)
 */
#ifndef GNUCOS_GDT_H
#define GNUCOS_GDT_H

#include <stdint.h>

#define SEL_KCODE   0x08
#define SEL_KDATA   0x10
#define SEL_UDATA   (0x18 | 3)   /* 0x1B — RPL 3 */
#define SEL_UCODE   (0x20 | 3)   /* 0x23 — RPL 3 */
#define SEL_TSS     0x28

/* The 64-bit TSS: in long mode it no longer holds a task context, only the
 * privilege-level stacks (RSP0..2), the interrupt-stack table and the I/O
 * permission bitmap offset.  Lives here (not gdt.c) so smp.c's per-CPU
 * struct can embed one per core. */
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Defined fully in smp.h; only the pointer matters here. */
struct cpu;

/* Install the BSP's GDT/TSS and reload every segment register. */
void gdt_init(void);

/* Build a complete GDT + TSS into a per-CPU structure (used for the BSP and
 * for every AP).  The TSS's RSP0 is pointed at the CPU's own kernel stack. */
void gdt_build(struct cpu *c);

/* Point TSS.RSP0 at the stack a ring-3 -> ring-0 trap should switch to. */
void tss_set_rsp0(uint64_t rsp0);

/* The same value, mirrored where isr.asm's syscall_entry can reach it: the
 * `syscall` instruction never looks at the TSS, so it loads RSP from here. */
extern uint64_t g_kernel_rsp0;

#endif
