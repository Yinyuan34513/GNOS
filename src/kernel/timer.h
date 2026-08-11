/*
 * timer.h — the 8253/8254 programmable interval timer on IRQ 0. (GPLv2)
 *
 * The timer is what turns cooperative multitasking into pre-emptive
 * multitasking: without it a user program that never makes a system call
 * would own the CPU forever, and Ctrl-C could never be delivered to it.
 */
#ifndef GNUCOS_TIMER_H
#define GNUCOS_TIMER_H

#include <stdint.h>

/*
 * The scheduler tick rate.  This is not a free parameter: it is the value
 * libc reports as sysconf(_SC_CLK_TCK) and the unit that times(2), AT_CLKTCK
 * and c_cc[VTIME] are all denominated in, so the kernel and user space have
 * to agree on it.  100 Hz makes a tick exactly 10 ms.
 */
#define SCHED_HZ  100

/* Program the PIT to `hz` ticks per second and hook it to the scheduler. */
void timer_init(unsigned hz);

/* Ticks since boot. */
uint64_t timer_ticks(void);

/*
 * Seconds between the Unix epoch and the moment the kernel booted, read
 * once from the CMOS real-time clock.  Adding this to the tick counter is
 * what turns a monotonic counter into a wall clock -- without it every file
 * the system writes is stamped 1970 and `make` sees its own output as older
 * than its input.  Returns 0 if the RTC could not be read, which keeps the
 * old boot-relative behaviour rather than inventing a date.
 */
uint64_t timer_boot_epoch(void);

/* Busy-wait for a real number of milliseconds.  Works before timer_init(),
 * with interrupts still masked, because it polls PIT channel 2 rather than
 * counting IRQ 0.  Drivers use it where a device specifies a settling time. */
void timer_delay_ms(unsigned ms);

#endif
