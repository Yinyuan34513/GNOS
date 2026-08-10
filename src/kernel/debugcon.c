/*
 * debugcon.c — QEMU debug console output (port 0xE9). (GPLv2)
 */
#include <stdint.h>
#include "debugcon.h"

static void outb(uint16_t port, uint8_t v)
{
    asm volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

void dbg_putc(char c)
{
    if (c == '\n')
        outb(0xE9, '\r');
    outb(0xE9, (uint8_t)c);
}

void dbg_puts(const char *s)
{
    for (; *s; s++)
        dbg_putc(*s);
}

void dbg_puts_dec(uint32_t v)
{
    char buf[12];
    int i = 0;
    if (v == 0)
        buf[i++] = '0';
    while (v) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i--)
        outb(0xE9, (uint8_t)buf[i]);
}

void dbg_puts_hex(uint64_t v)
{
    char buf[19];
    const char *hx = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++)
        buf[2 + i] = hx[(v >> ((15 - i) * 4)) & 0xF];
    buf[18] = 0;
    dbg_puts(buf);
}
