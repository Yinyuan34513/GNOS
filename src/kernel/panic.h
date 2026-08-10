/*
 * panic.h — kernel panic / fatal fault handling. (GPLv2)
 *
 * panic() is the last resort: it prints a message and the CPU register
 * state, paints the framebuffer red so the failure is visible even on a
 * headless boot, and halts.  The IDT exception stubs funnel unrecoverable
 * faults (GPF, page fault, etc.) into panic() too.
 */
#ifndef GNUCOS_PANIC_H
#define GNUCOS_PANIC_H

#include <stdint.h>

/* Register frame captured by the ISR stubs (see isr.asm / idt.c). */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, errcode;
    uint64_t rip, cs, rflags, rsp, ss;
} regs_t;

/* Halt-and-catch-fire fatal error.  Never returns. */
void panic(const char *msg) __attribute__((noreturn));

/* Exception entry point called from the ISR dispatcher. */
void panic_from_frame(const char *msg, const regs_t *r) __attribute__((noreturn));

/* Hex/decimal helpers shared with the console. */
void dbg_puts(const char *s);
void dbg_puts_dec(uint32_t v);

#endif
