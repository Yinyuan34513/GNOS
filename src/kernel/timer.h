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

/* Program the PIT to `hz` ticks per second and hook it to the scheduler. */
void timer_init(unsigned hz);

/* Ticks since boot. */
uint64_t timer_ticks(void);

#endif
