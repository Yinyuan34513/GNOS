/*
 * timer.c — PIT channel 0 driving the scheduler. (GPLv2)
 */
#include <stdint.h>

#include "timer.h"
#include "idt.h"
#include "proc.h"
#include "panic.h"
#include "debugcon.h"

#define PIT_CH0    0x40
#define PIT_CMD    0x43
#define PIT_BASE   1193182u          /* the ancient 1.193182 MHz crystal */

static volatile uint64_t g_ticks;

static inline void outb(uint16_t port, uint8_t v)
{
    asm volatile("outb %0, %1" :: "a"(v), "Nd"(port));
}

static void timer_irq(regs_t *r)
{
    g_ticks++;

    /* Deadlines have to be checked even when the interrupt landed in kernel
     * mode: a VTIME sleeper is often the *only* thing on the machine, so the
     * idle loop -- which runs at ring 0 -- is exactly who we interrupt. */
    sched_expire_timeouts();

    /*
     * Only pre-empt a task that was interrupted in user mode.  The kernel
     * has no locks, so switching away from a task that is halfway through a
     * system call could leave the process table or the open-file table in a
     * state the next task would trip over.  Kernel code therefore only ever
     * gives up the CPU where it says so itself (sched_block / sched_yield).
     */
    if ((r->cs & 3) == 3)
        sched_tick();
}

void timer_init(unsigned hz)
{
    if (hz == 0)
        hz = 100;

    uint32_t divisor = PIT_BASE / hz;
    if (divisor == 0)
        divisor = 1;
    if (divisor > 0xFFFF)
        divisor = 0xFFFF;

    /* 0x36 = channel 0, lobyte/hibyte, mode 3 (square wave), binary. */
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)(divisor >> 8));

    irq_install(0, timer_irq);

    dbg_puts("TIMER: PIT at ");
    dbg_puts_dec(hz);
    dbg_puts(" Hz (divisor ");
    dbg_puts_dec(divisor);
    dbg_puts(")\r\n");
}

uint64_t timer_ticks(void)
{
    return g_ticks;
}
