/*
 * idt.c — interrupt descriptor table, PIC remap and trap dispatch. (GPLv2)
 *
 * Every vector points at the matching stub in isr.asm; the stubs funnel into
 * isr_dispatch() below with a uniform register frame.  CPU exceptions that we
 * cannot recover from end up in panic_from_frame(), which is the whole reason
 * an IDT is worth installing this early: without one, a stray #UD or #PF just
 * escalates to a triple fault and the machine silently resets.
 */
#include <stdint.h>

#include "idt.h"
#include "gdt.h"
#include "panic.h"
#include "proc.h"
#include "vmm.h"
#include "debugcon.h"

struct idt_entry {
    uint16_t off_lo;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern uint8_t isr_stub_base[];   /* isr.asm, 16 bytes per vector */

static struct idt_entry g_idt[256];
static irq_handler_t    g_irq[16];
static irq_handler_t    g_syscall;

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

/* ---- 8259A PIC ------------------------------------------------------- */
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

static void pic_remap(void)
{
    uint8_t m1 = inb(PIC1_DATA), m2 = inb(PIC2_DATA);
    (void)m1; (void)m2;

    outb(PIC1_CMD, 0x11);  outb(PIC2_CMD, 0x11);   /* ICW1: init + ICW4  */
    outb(PIC1_DATA, IRQ_BASE);                     /* ICW2: vector bases */
    outb(PIC2_DATA, IRQ_BASE + 8);
    outb(PIC1_DATA, 0x04);                         /* ICW3: slave on IR2 */
    outb(PIC2_DATA, 0x02);
    outb(PIC1_DATA, 0x01);  outb(PIC2_DATA, 0x01); /* ICW4: 8086 mode    */

    outb(PIC1_DATA, 0xFF);                         /* mask everything    */
    outb(PIC2_DATA, 0xFF);
}

static void pic_unmask(unsigned irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (uint8_t)(1u << (irq & 7));
    outb(port, (uint8_t)(inb(port) & ~bit));
    if (irq >= 8)                                  /* also open the cascade */
        outb(PIC1_DATA, (uint8_t)(inb(PIC1_DATA) & ~(1u << 2)));
}

static void pic_eoi(unsigned irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

/* ---- gates ----------------------------------------------------------- */
static void idt_set(unsigned vec, uint64_t handler, uint8_t dpl)
{
    g_idt[vec].off_lo    = (uint16_t)(handler & 0xFFFF);
    g_idt[vec].selector  = SEL_KCODE;
    g_idt[vec].ist       = 0;
    /* 0x8E = present, 64-bit interrupt gate (IF cleared on entry). */
    g_idt[vec].type_attr = (uint8_t)(0x8E | ((dpl & 3) << 5));
    g_idt[vec].off_mid   = (uint16_t)((handler >> 16) & 0xFFFF);
    g_idt[vec].off_hi    = (uint32_t)(handler >> 32);
    g_idt[vec].zero      = 0;
}

void irq_install(unsigned irq, irq_handler_t fn)
{
    if (irq >= 16)
        return;
    g_irq[irq] = fn;
    pic_unmask(irq);
}

void syscall_install(irq_handler_t fn)
{
    g_syscall = fn;
}

/* Minimal, framebuffer-free fatal-fault reporter.  Used for ring-0 faults
 * and double faults so that the diagnostic reaches the debug console even
 * when the full panic path would itself fault and escalate to a triple
 * fault (which would silently reset the CPU before anything was printed). */
static void __attribute__((noreturn)) fault_halt(regs_t *r, const char *tag)
{
    uint64_t cr2 = 0;
    if (r && r->vector == 14)
        asm volatile("mov %%cr2, %0" : "=r"(cr2));

    dbg_puts(tag);
    dbg_puts(" vector=");
    dbg_puts_dec((uint32_t)(r ? r->vector : 0));
    dbg_puts(" err=");
    dbg_puts_hex(r ? r->errcode : 0);
    dbg_puts(" rip=");
    dbg_puts_hex(r ? r->rip : 0);
    dbg_puts(" cr2=");
    dbg_puts_hex(cr2);
    dbg_puts(" cs=");
    dbg_puts_hex(r ? r->cs : 0);
    dbg_puts("\r\n");

    for (;;)
        asm volatile("cli; hlt");
}

/* ---- dispatch -------------------------------------------------------- */
void isr_dispatch(regs_t *r)
{
    /* A double fault means the normal fault handler faulted again; report it
     * and park the CPU instead of letting it escalate to a triple fault. */
    if (r->vector == 8) {
        fault_halt(r, "GNOS: DOUBLE FAULT");
    }

    if (r->vector < 32) {
        /* #BP is the only fault we let through: it is how a debugger stops. */
        if (r->vector == 3) {
            dbg_puts("GNOS: breakpoint at ");
            dbg_puts_hex(r->rip);
            dbg_puts("\r\n");
            return;
        }

        /*
         * A fault in ring 3 is the user program's problem, not the kernel's:
         * turn it into SIGSEGV and let the default action kill just that
         * process.  Only a fault taken in ring 0 is fatal to the system.
         */
        if ((r->cs & 3) == 3 && proc_current()) {
            /*
             * A page fault just below the stack is not an error, it is the
             * stack growing.  Map the page and retry the instruction; only
             * if the address is outside the stack window does this fall
             * through to the SIGSEGV path below.
             */
            if (r->vector == 14) {
                uint64_t cr2;
                asm volatile("mov %%cr2, %0" : "=r"(cr2));
                if (vmm_grow_stack(proc_current()->as, cr2))
                    return;
            }

            dbg_puts("GNOS: fault in user pid ");
            dbg_puts_dec((uint32_t)proc_current()->pid);
            dbg_puts(" vector=");
            dbg_puts_dec((uint32_t)r->vector);
            dbg_puts(" rip=");
            dbg_puts_hex(r->rip);
            /* CR2 and the stack say far more than RIP alone: a jump through a
             * null function pointer and a wild data write both arrive here as
             * "vector=14", and only the faulting address tells them apart. */
            if (r->vector == 14) {
                uint64_t cr2;
                asm volatile("mov %%cr2, %0" : "=r"(cr2));
                dbg_puts(" cr2=");
                dbg_puts_hex(cr2);
                dbg_puts(" err=");
                dbg_puts_hex(r->errcode);
            }
            dbg_puts(" rsp=");
            dbg_puts_hex(r->rsp);
            dbg_puts("\r\n");
            proc_signal(proc_current(), SIGSEGV);
            proc_check_signals(r);
            return;
        }

        fault_halt(r, "GNOS: RING0 FAULT");
    }

    if (r->vector == SYSCALL_VECTOR) {
        if (g_syscall)
            g_syscall(r);
        else
            r->rax = (uint64_t)-38;      /* -ENOSYS */
        proc_check_signals(r);
        return;
    }

    if (r->vector >= IRQ_BASE && r->vector < IRQ_BASE + 16) {
        unsigned irq = (unsigned)(r->vector - IRQ_BASE);

        /* Acknowledge first.  A handler may switch tasks and not come back
         * for a long time; leaving the PIC un-acked would stop every further
         * interrupt on that line, the timer included. */
        pic_eoi(irq);
        if (g_irq[irq])
            g_irq[irq](r);
        proc_check_signals(r);
        return;
    }

    /* Anything else is a spurious software interrupt; ignore it. */
}

void idt_init(void)
{
    pic_remap();

    for (unsigned v = 0; v < 256; v++) {
        uint64_t stub = (uint64_t)(uintptr_t)isr_stub_base + (uint64_t)v * 16;
        /* int 0x80 must be reachable from ring 3, everything else must not. */
        idt_set(v, stub, (v == SYSCALL_VECTOR) ? 3 : 0);
    }

    struct idtr idtr = { .limit = sizeof(g_idt) - 1,
                         .base  = (uint64_t)(uintptr_t)g_idt };
    asm volatile("lidt %0" :: "m"(idtr) : "memory");

    dbg_puts("GNOS: IDT installed, stubs@");
    dbg_puts_hex((uint64_t)(uintptr_t)isr_stub_base);
    dbg_puts("\r\n");
}
