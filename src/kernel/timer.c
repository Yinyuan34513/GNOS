/*
 * timer.c — PIT channel 0 driving the scheduler. (GPLv2)
 */
#include <stdint.h>

#include "timer.h"
#include "idt.h"
#include "net.h"
#include "proc.h"
#include "panic.h"
#include "debugcon.h"

#define PIT_CH0    0x40
#define PIT_CH2    0x42
#define PIT_CMD    0x43
#define PIT_BASE   1193182u          /* the ancient 1.193182 MHz crystal */

/* Port 0x61: bit 0 is channel 2's gate input, bit 1 enables the speaker
 * amplifier, and bit 5 reads channel 2's output pin back. */
#define PORT_61    0x61

static volatile uint64_t g_ticks;

static inline void outb(uint16_t port, uint8_t v)
{
    asm volatile("outb %0, %1" :: "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
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
    if ((r->cs & 3) == 3) {
        /*
         * ARP expiry, TCP retransmission and the receive poll ride the same
         * guard, and for a stronger reason than pre-emption: net_poll() also
         * runs inside system calls, and with no locks anywhere, firing it
         * from an interrupt taken in ring 0 could re-enter it mid-way and
         * hand one packet to a socket twice.  A task blocked on a socket
         * polls on its own wakeup (see sock.c), so nothing depends on this
         * running while the machine is idle.
         */
        net_tick();
        sched_tick();
    }
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

/*
 * A delay measured in real time instead of in loop iterations.
 *
 * Channel 0 belongs to the scheduler, so this borrows channel 2 -- the one
 * wired to the PC speaker -- as a one-shot stopwatch: load a count, raise the
 * gate, and poll until the output pin goes high at terminal count.  Bit 1 of
 * port 0x61, the speaker enable, is left clear throughout, so nothing beeps.
 *
 * Two properties matter to callers.  It needs no interrupts, so it is usable
 * during early boot while IRQ 0 is still masked; and a millisecond here is an
 * actual millisecond, which a loop of io_delay() can never promise.  Device
 * bring-up is full of "wait N ms after this write" requirements, and guessing
 * at them with spin counts is how drivers become machine-specific.
 */
void timer_delay_ms(unsigned ms)
{
    const uint16_t count = (uint16_t)(PIT_BASE / 1000u);   /* one millisecond */

    while (ms--) {
        uint8_t p61 = (uint8_t)(inb(PORT_61) & ~0x02u);    /* speaker muted */

        outb(PORT_61, (uint8_t)(p61 & ~0x01u));   /* gate low: counter held */
        outb(PIT_CMD, 0xB0);                      /* ch2, lo/hi, mode 0 */
        outb(PIT_CH2, (uint8_t)(count & 0xFF));
        outb(PIT_CH2, (uint8_t)(count >> 8));
        outb(PORT_61, (uint8_t)(p61 | 0x01u));    /* gate high: counting */

        /* Bounded: on a board that does not route channel 2 back to port
         * 0x61 this must cost a moment, not the boot. */
        uint32_t i = 0;
        while (!(inb(PORT_61) & 0x20)) {
            if (++i == 200000u)
                return;
        }
    }
}
