/*
 * lapic.h — local APIC: enable, EOI and the per-CPU timer. (GPLv2)
 *
 * The BSP keeps the PIT as its 100 Hz clock; every AP runs this timer
 * instead, because the PIT is a single chip that only ever interrupts the
 * core its IRQ line is wired to.  The LAPIC timer is one per core, so it is
 * the natural second clock, and it is what lets every AP drive the shared
 * EEVDF scheduler at the same tick rate.
 */
#ifndef GNUCOS_LAPIC_H
#define GNUCOS_LAPIC_H

#include <stdint.h>
#include "idt.h"            /* for irq_handler_t (regs_t via panic.h) */

/* The LAPIC timer's interrupt vector.  It is a free slot: the PIC owns
 * 0x20..0x2F and the syscall gate owns 0x80. */
#define LAPIC_TIMER_VECTOR 0x40

/* Enable the local APIC (MSR 0x1B, bit 11) and unmask the spurious vector.
 * Called on the BSP after ACPI has been parsed and on every AP in
 * ap_main().  Must run before lapic_timer_start(). */
void lapic_init(void);

/* Start this core's LAPIC timer as a periodic 100 Hz clock.  Calibrates
 * against the PIT's timer_delay_ms() on the first call.  APs only. */
void lapic_timer_start(void);

/* Acknowledge the end of an interrupt.  Every handler for a vector the
 * LAPIC delivered must call this or the core stops taking interrupts. */
void lapic_eoi(void);

/* Register the handler for LAPIC_TIMER_VECTOR (timer.c's ap_timer_irq). */
void lapic_timer_install(irq_handler_t fn);

#endif