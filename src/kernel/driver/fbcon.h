/*
 * fbcon.h — framebuffer text consoles using the bundled 8x16 font. (GPLv2)
 *
 * There is one framebuffer and several consoles sharing it.  Each console
 * owns a character cell buffer, a cursor and a colour pair; exactly one of
 * them is *active* at a time and is the only one whose writes reach the
 * screen.  Switching consoles repaints the framebuffer from the incoming
 * console's cells, which is what makes Ctrl-Alt-F<n> feel instant and lets a
 * program keep printing to a terminal nobody is looking at.
 */
#ifndef GNUCOS_FBCON_H
#define GNUCOS_FBCON_H

#include <stdint.h>
#include "bootinfo.h"

/* How many consoles can exist at once.  One per virtual terminal. */
#define FBCON_MAX_CONS 6

/*
 * Cell-buffer bounds.  Limine picks the mode, so the console geometry is not
 * known at compile time; these are the largest text grid we will keep a
 * back buffer for, and anything larger is clamped (the extra pixels simply
 * stay blank).  256x96 covers every mode up to 2048x1536.
 */
#define FBCON_MAX_COLS 256
#define FBCON_MAX_ROWS 96

/* Initialise from the bootloader-provided framebuffer.  Console 0 exists
 * from this point on and is the active one; it is where the kernel talks. */
void fbcon_init(const bootinfo_t *bi);

/* Claim a console.  Returns its id, or -1 when they are all taken.  Console 0
 * is allocated by fbcon_init() and must not be claimed again. */
int  fbcon_alloc(void);

/* Put `con` on the screen and repaint from its cells.  A no-op if it is
 * already active or the id is not a live console. */
void fbcon_activate(int con);

/* The console currently on screen. */
int  fbcon_active(void);

/* ---- per-console output ------------------------------------------------ */
/* Write one character.  Understands \n, \r, \b and \t, and scrolls. */
void fbcon_putc_on(int con, char c);
/* Move down one row without touching the column -- a bare line feed, which is
 * what a terminal does with '\n' when OPOST/ONLCR are off. */
void fbcon_lf_on(int con);
/* Blank the console with its background colour and home the cursor. */
void fbcon_clear_on(int con);
/* Set a console's foreground / background colours (x8r8g8b8). */
void fbcon_set_color_on(int con, uint32_t fg, uint32_t bg);

/* ---- console 0, the kernel's own ---------------------------------------
 * Thin wrappers so panic(), the boot path and anything else that just wants
 * "print to the screen" does not have to know consoles exist. */
void fbcon_putc(char c);
void fbcon_puts(const char *s);
void fbcon_lf(void);
void fbcon_clear(void);
void fbcon_set_color(uint32_t fg, uint32_t bg);

/* Report the console size in glyphs, for TIOCGWINSZ.  Every console is the
 * size of the framebuffer, so this needs no id.  Either pointer may be NULL. */
void fbcon_size(uint32_t *cols, uint32_t *rows);

/* The pixel geometry of the current mode (any pointer may be NULL). */
void fbcon_geometry(uint32_t *w, uint32_t *h, uint32_t *pitch);
/* The framebuffer base pointer and scanline pitch in bytes, for drivers
 * that blit into the console's own memory (DRM SETCRTC). */
void    *fbcon_fb(void);
uint32_t fbcon_pitch(void);

/* A modeset changed the resolution: re-derive the grid, clamp every
 * console's cursor and scroll region, and repaint the active console. */
void fbcon_resize(uint32_t w, uint32_t h, uint32_t pitch);

/*
 * Switch to the panic colour scheme (white on red) and clear.  This forces
 * console 0 to the front first: a panic on a machine sitting on tty3 has to
 * be visible, and the kernel's messages only ever go to console 0.
 */
void fbcon_panic(void);

#endif
