/*
 * debugcon.h — text output to the QEMU debug console (port 0xE9).
 * Useful for headless testing; bytes written here land in the file given
 * to qemu via -debugcon. (GPLv2)
 */
#ifndef GNUCOS_DEBUGCON_H
#define GNUCOS_DEBUGCON_H

void dbg_putc(char c);
void dbg_puts(const char *s);
void dbg_puts_dec(uint32_t v);
void dbg_puts_hex(uint64_t v);

#endif
