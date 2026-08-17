/*
 * idt.h — 64-bit interrupt descriptor table. (GPLv2)
 */
#ifndef GNUCOS_IDT_H
#define GNUCOS_IDT_H

#include <stdint.h>
#include "panic.h"

#define IRQ_BASE      0x20    /* PIC vectors are remapped to 0x20..0x2F */
#define SYSCALL_VECTOR 0x80

typedef void (*irq_handler_t)(regs_t *r);

/* Fill in all 256 gates and load the IDT.  Also remaps and masks the PIC. */
void idt_init(void);

/* Reload the (already-built) IDT on the current CPU.  idt_init() calls this
 * once for the BSP; APs call it too so their local IDTR points at the shared
 * table.  Interrupts stay disabled on APs, so this is just a safety net. */
void idt_load(void);

/* Register a handler for a hardware IRQ (0..15) and unmask it. */
void irq_install(unsigned irq, irq_handler_t fn);

/* Register the handler for int 0x80 (the POSIX syscall gate). */
void syscall_install(irq_handler_t fn);

#endif
