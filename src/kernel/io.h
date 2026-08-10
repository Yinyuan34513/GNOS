/*
 * io.h — x86 port I/O primitives. (GPLv2)
 *
 * Every device driver (PCI, E1000, AC97, the PIT, the keyboard) talks to
 * hardware through these.  They are `static inline` so including this header
 * in a translation unit that already defines its own copy (tty.c, timer.c,
 * idt.c each had one) would clash -- so only the new driver files include
 * it; the old local definitions stay put.
 */
#ifndef GNOS_IO_H
#define GNOS_IO_H

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t v)
{
    asm volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outw(uint16_t port, uint16_t v)
{
    asm volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t v;
    asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outl(uint16_t port, uint32_t v)
{
    asm volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    asm volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* A short stall so back-to-back I/O to the same device settles.  Writing to
 * port 0x80 is the classic x86 "pause" for port I/O. */
/* The "memory" clobber is the important part.  Spin loops that wait for a
 * device to update a DMA descriptor call this between polls, and without the
 * clobber gcc is entitled to hoist the descriptor load out of the loop -- the
 * loop body never writes to it, as far as the compiler can see -- and spin
 * forever on a value read once.  Marking the descriptors volatile handles the
 * same hazard from the other side; both are cheap, so do both. */
static inline void io_delay(void)
{
    asm volatile("outb %0, %1" : : "a"((uint8_t)0), "Nd"((uint16_t)0x80)
                 : "memory");
}

#endif
