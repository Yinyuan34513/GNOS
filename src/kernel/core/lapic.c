/*
 * lapic.c — local APIC driver: enable, EOI, and the per-CPU timer. (GPLv2)
 *
 * Registers are memory-mapped 4 KiB after the base ACPI reported; the HHDM
 * direct map already covers the whole of physical memory, so the LAPIC is
 * just a few volatile pointers into it.  Two registers deserve a note:
 *
 *   - SPURIOUS (0xF0): bit 8 must be set or the LAPIC delivers nothing.
 *     The vector in the low byte has to be a real one, so we use 0x30.
 *   - The timer: when INIT_COUNT hits zero the interrupt fires; with the
 *     periodic bit (0x20000) set in the LVT entry, the count is reloaded
 *     from INIT_COUNT automatically.  The count decrements at bus/16, and
 *     the bus speed is a machine secret, so we calibrate by timing a full
 *     32-bit countdown against the PIT.
 */
#include <stdint.h>

#include "lapic.h"
#include "acpi.h"
#include "timer.h"
#include "idt.h"
#include "debugcon.h"

/* The physical MMIO offset g_hhdm is added to (defined in kernel.c). */
extern uint64_t g_hhdm;

#define MSR_IA32_APIC_BASE   0x1B
#define APIC_BASE_ENABLE     (1ULL << 11)

#define LAPIC_ID         0x020
#define LAPIC_EOI        0x0B0
#define LAPIC_SPURIOUS   0x0F0
#define LAPIC_LVT_TIMER  0x320
#define LAPIC_INIT_COUNT 0x380
#define LAPIC_CUR_COUNT  0x390
#define LAPIC_DIVIDE     0x3E0

#define LVT_TIMER_PERIODIC 0x20000
#define DIVIDE_BY_16       0x3

static volatile uint32_t *g_lapic;   /* mapped base, set by lapic_init() */

/* The calibrated LAPIC timer frequency (counts per second); 0 until the
 * first lapic_timer_start() runs. */
static uint32_t g_lapic_freq;

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    asm volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v),
                 "d"((uint32_t)(v >> 32)) : "memory");
}

static inline uint32_t lapic_read(unsigned off)
{
    return g_lapic[off / 4];
}

static inline void lapic_write(unsigned off, uint32_t v)
{
    g_lapic[off / 4] = v;
}

/* The handler isr_dispatch routes LAPIC_TIMER_VECTOR to (timer.c's
 * ap_timer_irq).  The LAPIC itself knows nothing about it. */
static irq_handler_t g_lapic_timer;

void lapic_timer_install(irq_handler_t fn)
{
    g_lapic_timer = fn;
}

/* isr_dispatch calls this for LAPIC_TIMER_VECTOR. */
irq_handler_t lapic_timer_handler(void)
{
    return g_lapic_timer;
}

void lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

void lapic_init(void)
{
    if (g_lapic)
        return;                         /* already done on this boot */

    uint64_t phys = acpi_lapic_base();
    if (!phys) {
        dbg_puts("LAPIC: no base from ACPI, LAPIC disabled\r\n");
        return;
    }

    g_lapic = (volatile uint32_t *)(uintptr_t)(g_hhdm + phys);

    /* Switch the APIC on (the BSP's is already enabled by the firmware in
     * practice, but the MSR is the one place we can be sure). */
    uint64_t base = rdmsr(MSR_IA32_APIC_BASE);
    if (!(base & APIC_BASE_ENABLE)) {
        wrmsr(MSR_IA32_APIC_BASE, base | APIC_BASE_ENABLE);
        /* Re-read the base after enabling: the APIC's MMIO window moves
         * from 0xFEE00000 to the (same, here) value in the MSR. */
        phys = rdmsr(MSR_IA32_APIC_BASE) & 0xFFFFF000ULL;
        g_lapic = (volatile uint32_t *)(uintptr_t)(g_hhdm + phys);
    }

    /* Bit 8 unmask + a sane spurious vector. */
    lapic_write(LAPIC_SPURIOUS, 0x30 | 0x100);

    dbg_puts("LAPIC: enabled at phys ");
    dbg_puts_hex(phys);
    dbg_puts("\r\n");
}

/* Measure the LAPIC timer's rate with the PIT's delay.  Only meaningful on
 * an AP after lapic_init(); divides by 16 so the 32-bit counter lasts
 * long enough to be timed with 10 ms of granularity. */
static void lapic_timer_calibrate(void)
{
    lapic_write(LAPIC_DIVIDE, DIVIDE_BY_16);
    lapic_write(LAPIC_INIT_COUNT, 0xFFFFFFFFu);

    timer_delay_ms(10);

    uint32_t remaining = lapic_read(LAPIC_CUR_COUNT);
    uint32_t elapsed   = 0xFFFFFFFFu - remaining;
    if (elapsed == 0)
        elapsed = 1;

    /* The count ran down at bus/16 for 10 ms: per second that is 100x. */
    g_lapic_freq = elapsed * 100;

    dbg_puts("LAPIC: timer calibrates at ");
    dbg_puts_dec(g_lapic_freq);
    dbg_puts(" counts/s\r\n");
}

void lapic_timer_start(void)
{
    if (!g_lapic)
        return;
    if (!g_lapic_freq)
        lapic_timer_calibrate();

    lapic_write(LAPIC_DIVIDE, DIVIDE_BY_16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_VECTOR | LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_INIT_COUNT, g_lapic_freq / SCHED_HZ);
}