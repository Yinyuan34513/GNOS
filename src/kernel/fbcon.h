/*
 * fbcon.h — tiny framebuffer text console using the bundled 8x16 font.
 * Draws glyphs into the linear framebuffer supplied by the bootloader.
 * (GPLv2)
 */
#ifndef GNUCOS_FBCON_H
#define GNUCOS_FBCON_H

#include <stdint.h>
#include "bootinfo.h"

/* Initialise from the bootloader-provided framebuffer.  Clears the screen. */
void fbcon_init(const bootinfo_t *bi);

/* Write one character.  Understands \n, \r, \b and \t, and scrolls. */
void fbcon_putc(char c);

/* Write a NUL-terminated string at the current cursor position. */
void fbcon_puts(const char *s);

/* Move down one row without touching the column -- a bare line feed, which is
 * what a terminal does with '\n' when OPOST/ONLCR are off. */
void fbcon_lf(void);

/* Report the console size in glyphs, for TIOCGWINSZ.  Either pointer may be
 * NULL. */
void fbcon_size(uint32_t *cols, uint32_t *rows);

/* Set the foreground / background colours (x8r8g8b8). */
void fbcon_set_color(uint32_t fg, uint32_t bg);

/* Blank the screen with the current background and home the cursor. */
void fbcon_clear(void);

/* Switch to the panic colour scheme (white on red) and clear. */
void fbcon_panic(void);

#endif
