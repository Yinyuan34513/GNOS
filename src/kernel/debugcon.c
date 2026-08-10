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
    dbg_puts("0x");
    dbg_puts_hexn(v, 16);
}

/* Same, but only as wide as the value deserves and with no "0x": a MAC byte
 * printed as 0x000000000000005A tells you nothing you did not know. */
void dbg_puts_hexn(uint64_t v, int digits)
{
    char buf[17];
    const char *hx = "0123456789ABCDEF";
    if (digits < 1) digits = 1;
    if (digits > 16) digits = 16;
    for (int i = 0; i < digits; i++)
        buf[i] = hx[(v >> ((digits - 1 - i) * 4)) & 0xF];
    buf[digits] = 0;
    dbg_puts(buf);
}
