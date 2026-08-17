/*
 * panic.c — kernel panic implementation. (GPLv2)
 */
#include <stddef.h>
#include <stdint.h>

#include "panic.h"
#include "debugcon.h"
#include "fbcon.h"

/* Names for the CPU exception vectors we route through panic(). */
static const char *exc_name(uint64_t v)
{
    switch (v) {
    case  0: return "#DE divide error";
    case  2: return "#NMI";
    case  3: return "#BP breakpoint";
    case  4: return "#OF overflow";
    case  5: return "#BR bound range";
    case  6: return "#UD invalid opcode";
    case  7: return "#NM device not available";
    case  8: return "#DF double fault";
    case 10: return "#TS invalid TSS";
    case 11: return "#NP segment not present";
    case 12: return "#SS stack fault";
    case 13: return "#GP general protection";
    case 14: return "#PF page fault";
    case 16: return "#MF x87 error";
    case 17: return "#AC alignment check";
    case 18: return "#MC machine check";
    case 19: return "#XF simd error";
    default: return "unknown exception";
    }
}

static void put_hex(uint64_t v)
{
    char buf[19];
    const char *hex = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
    buf[18] = 0;
    dbg_puts(buf);
}

void panic_from_frame(const char *msg, const regs_t *r)
{
    /* Red screen so the crash is obvious on the framebuffer. */
    fbcon_panic();

    dbg_puts("\r\n!!! KERNEL PANIC !!!\r\n");
    dbg_puts(msg);
    dbg_puts("\r\n");
    if (r) {
        dbg_puts("vector: ");
        if (r->vector < 32)
            dbg_puts(exc_name(r->vector));
        else
            dbg_puts("syscall/irq");
        dbg_puts(" (");
        dbg_puts_dec((uint32_t)r->vector);
        dbg_puts(")\r\n");

        if (r->vector == 14) {
            uint64_t cr2;
            asm volatile("mov %%cr2, %0" : "=r"(cr2));
            dbg_puts("cr2="); put_hex(cr2); dbg_puts("\r\n");
        }
        dbg_puts("rip="); put_hex(r->rip); dbg_puts(" err="); put_hex(r->errcode);
        dbg_puts("\r\n");
        dbg_puts("rsp="); put_hex(r->rsp); dbg_puts(" rfl="); put_hex(r->rflags);
        dbg_puts("\r\n");
        dbg_puts("cs ="); put_hex(r->cs);  dbg_puts(" ss ="); put_hex(r->ss);
        dbg_puts("\r\n");
        dbg_puts("rax="); put_hex(r->rax); dbg_puts(" rbx="); put_hex(r->rbx);
        dbg_puts("\r\n");
        dbg_puts("rcx="); put_hex(r->rcx); dbg_puts(" rdx="); put_hex(r->rdx);
        dbg_puts("\r\n");
        dbg_puts("rsi="); put_hex(r->rsi); dbg_puts(" rdi="); put_hex(r->rdi);
        dbg_puts("\r\n");
        dbg_puts("rbp="); put_hex(r->rbp); dbg_puts(" r8 ="); put_hex(r->r8);
        dbg_puts("\r\n");
    }

    fbcon_puts("\n*** KERNEL PANIC ***\n");
    fbcon_puts(msg);
    fbcon_puts("\nsystem halted -- see debug console (port 0xE9)\n");

    for (;;)
        asm volatile("cli; hlt");
}

void panic(const char *msg)
{
    panic_from_frame(msg, NULL);
}
