/*
 * timer.c — PIT channel 0 driving the scheduler. (GPLv2)
 */
#include <stdint.h>

#include "timer.h"
#include "timerfd.h"
#include "idt.h"
#include "net.h"
#include "proc.h"
#include "panic.h"
#include "debugcon.h"
#include "lapic.h"

#define PIT_CH0    0x40
#define PIT_CH2    0x42
#define PIT_CMD    0x43
#define PIT_BASE   1193182u          /* the ancient 1.193182 MHz crystal */

/* Port 0x61: bit 0 is channel 2's gate input, bit 1 enables the speaker
 * amplifier, and bit 5 reads channel 2's output pin back. */
#define PORT_61    0x61

static volatile uint64_t g_ticks;

/* Defined below, next to the rest of the real-time-clock code. */
static void rtc_read_boot_epoch(void);

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
    timerfd_tick();

/*
 * Only pre-empt a task that was interrupted in user mode.  The kernel's
 * shared state is serialised by the big kernel lock, but a pre-empted
 * kernel-mode frame has already parked that process's BKL claim in
 * sched_tick; switching away from a task that is halfway through a system
 * call is still unsafe for other reasons (its syscall bookkeeping), so
 * kernel code only ever gives up the CPU where it says so itself
 * (sched_block / sched_yield).
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

/*
 * The APs' clock: the same housekeeping as timer_irq, minus the global tick
 * counter, which the BSP's PIT owns.  The LAPIC itself already did the EOI
 * (idt.c routes LAPIC_TIMER_VECTOR).  sched_tick() on an AP parks and
 * resumes tasks with the BKL handed over like any other core.
 */
static void ap_timer_irq(regs_t *r)
{
    sched_expire_timeouts();
    timerfd_tick();

    if ((r->cs & 3) == 3) {
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
    lapic_timer_install(ap_timer_irq);
    rtc_read_boot_epoch();

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

/* ---- the CMOS real-time clock -----------------------------------------
 *
 * The PIT counts, but it does not know what time it is.  The date comes
 * from the battery-backed clock behind ports 0x70/0x71, read exactly once
 * at boot: it is a slow, byte-at-a-time interface, and re-reading it on
 * every gettimeofday() would make the cheapest system call in the set one
 * of the most expensive.  Wall-clock time is therefore this epoch plus the
 * tick counter, which also makes time strictly monotonic -- something the
 * RTC on its own does not guarantee.
 */
#define CMOS_ADDR  0x70
#define CMOS_DATA  0x71

static uint8_t cmos_read(uint8_t reg)
{
    /* Bit 7 of the index port is the NMI disable line; leave it clear. */
    outb(CMOS_ADDR, reg & 0x7F);
    return inb(CMOS_DATA);
}

static int cmos_updating(void)
{
    return (cmos_read(0x0A) & 0x80) != 0;
}

static uint32_t bcd_to_bin(uint8_t v)
{
    return (v & 0x0F) + ((v >> 4) * 10);
}

/* Days from 1970-01-01 to the first of the given month, proleptic Gregorian.
 * Written as a closed form rather than a loop over years because it has to
 * be correct for dates well past 2038 and a loop is only correct until
 * someone stops testing it. */
static uint64_t days_from_civil(uint32_t y, uint32_t m, uint32_t d)
{
    y -= m <= 2;
    uint32_t era = y / 400;
    uint32_t yoe = y - era * 400;                           /* [0, 399] */
    uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;   /* [0, 146096] */
    /* 719468 is the number of days from 0000-03-01 to 1970-01-01. */
    return (uint64_t)era * 146097 + doe - 719468;
}

static uint64_t g_boot_epoch;

static void rtc_read_boot_epoch(void)
{
    /*
     * Read the whole clock twice and only accept a pair of identical
     * readings taken outside an update cycle.  A single read can catch the
     * chip mid-carry and return 01:59:60 rolling into 02:00:00 -- the
     * classic way to be an hour wrong once in a very long while.
     */
    uint8_t s = 0, mi = 0, h = 0, d = 0, mo = 0, y = 0, cent = 0, regb = 0;

    for (int tries = 0; tries < 1000; tries++) {
        while (cmos_updating())
            ;

        uint8_t s2 = cmos_read(0x00), mi2 = cmos_read(0x02);
        uint8_t h2 = cmos_read(0x04), d2  = cmos_read(0x07);
        uint8_t mo2 = cmos_read(0x08), y2 = cmos_read(0x09);
        uint8_t c2 = cmos_read(0x32);

        if (tries && s == s2 && mi == mi2 && h == h2 && d == d2 &&
            mo == mo2 && y == y2) {
            cent = c2;
            regb = cmos_read(0x0B);
            break;
        }
        s = s2; mi = mi2; h = h2; d = d2; mo = mo2; y = y2;
        cent = c2;
        regb = cmos_read(0x0B);
    }

    if (!mo || mo > 12 || !d || d > 31)
        return;                         /* no usable clock; stay at the epoch */

    uint32_t hour24 = h;
    /* Bit 2 of register B: set means the values are already binary. */
    if (!(regb & 0x04)) {
        s  = (uint8_t)bcd_to_bin(s);
        mi = (uint8_t)bcd_to_bin(mi);
        d  = (uint8_t)bcd_to_bin(d);
        mo = (uint8_t)bcd_to_bin(mo);
        y  = (uint8_t)bcd_to_bin(y);
        cent = (uint8_t)bcd_to_bin(cent);
        /* In 12-hour BCD mode bit 7 is the PM flag and has to come off
         * before the digits are converted. */
        hour24 = bcd_to_bin((uint8_t)(h & 0x7F));
    }
    /* Bit 1 of register B clear means 12-hour mode: 12 AM is 0, 12 PM is 12. */
    if (!(regb & 0x02)) {
        if (hour24 == 12)
            hour24 = 0;
        if (h & 0x80)
            hour24 += 12;
    }

    /* The century register is optional and reads as garbage on plenty of
     * machines; only trust something plausible. */
    uint32_t year = (cent >= 19 && cent <= 21) ? cent * 100 + y : 2000 + y;

    g_boot_epoch = days_from_civil(year, mo, d) * 86400ULL +
                   (uint64_t)hour24 * 3600 + (uint64_t)mi * 60 + s;

    /* The RTC is in whatever zone the firmware keeps it in, most often UTC.
     * We have no timezone database, so it is taken as UTC and left there. */
    dbg_puts("TIMER: RTC epoch ");
    dbg_puts_dec((uint32_t)g_boot_epoch);
    dbg_puts("\r\n");
}

uint64_t timer_boot_epoch(void)
{
    return g_boot_epoch;
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
