/*
 * fbcon.c — framebuffer text console (8x16 font). (GPLv2)
 *
 * Draws directly into the linear framebuffer Limine handed us.  The console
 * scrolls by memmove-ing the pixel rows up one glyph height, which is slow
 * but keeps the whole thing dependency-free.
 */
#include <stdint.h>
#include "bootinfo.h"
#include "fbcon.h"
#include "font8x16.h"
#include "kstring.h"

typedef struct {
    uint32_t *fb;
    uint32_t  w;          /* pixels */
    uint32_t  h;          /* pixels */
    uint32_t  stride;     /* uint32_t per scanline */
    uint32_t  cols;
    uint32_t  rows;
    uint32_t  fg;
    uint32_t  bg;
    uint32_t  cx;         /* glyph column */
    uint32_t  cy;         /* glyph row */
} fbcon_t;

static fbcon_t g;

void fbcon_init(const bootinfo_t *bi)
{
    g.fb     = (uint32_t *)(uintptr_t)bi->fb_addr;
    g.w      = bi->fb_width;
    g.h      = bi->fb_height;
    g.stride = bi->fb_pitch / 4;
    g.fg     = 0x00CCCCCC;
    g.bg     = 0x00000000;
    g.cols   = g.w / FONT_WIDTH;
    g.rows   = g.h / FONT_HEIGHT;
    g.cx     = 0;
    g.cy     = 0;

    fbcon_clear();
}

void fbcon_set_color(uint32_t fg, uint32_t bg)
{
    g.fg = fg;
    g.bg = bg;
}

void fbcon_clear(void)
{
    if (!g.fb)
        return;
    for (uint32_t y = 0; y < g.h; y++)
        for (uint32_t x = 0; x < g.stride; x++)
            g.fb[y * g.stride + x] = g.bg;
    g.cx = 0;
    g.cy = 0;
}

void fbcon_panic(void)
{
    g.bg = 0x00800000;                 /* dark red */
    g.fg = 0x00FFFFFF;
    fbcon_clear();
}

static void scroll(void)
{
    if (!g.fb)
        return;

    uint32_t shift = FONT_HEIGHT * g.stride;
    uint32_t keep  = (g.rows - 1) * FONT_HEIGHT * g.stride;

    memmove(g.fb, g.fb + shift, (uint64_t)keep * 4);
    for (uint32_t i = 0; i < shift; i++)
        g.fb[keep + i] = g.bg;

    g.cy = g.rows - 1;
}

static void draw_glyph(uint32_t col, uint32_t row, uint8_t ch)
{
    if (!g.fb)
        return;

    const unsigned char *glyph = &font8x16[ch * FONT_HEIGHT];
    uint32_t px = col * FONT_WIDTH;
    uint32_t py = row * FONT_HEIGHT;

    for (uint32_t y = 0; y < FONT_HEIGHT; y++) {
        uint8_t bits = glyph[y];
        uint32_t *line = &g.fb[(py + y) * g.stride + px];
        for (uint32_t x = 0; x < FONT_WIDTH; x++)
            line[x] = (bits & (0x80 >> x)) ? g.fg : g.bg;
    }
}

static void newline(void)
{
    g.cx = 0;
    if (++g.cy >= g.rows)
        scroll();
}

void fbcon_putc(char c)
{
    switch (c) {
    case '\n':
        newline();
        return;
    case '\r':
        g.cx = 0;
        return;
    case '\b':
        if (g.cx > 0)
            g.cx--;
        else if (g.cy > 0) {
            g.cy--;
            g.cx = g.cols - 1;
        }
        draw_glyph(g.cx, g.cy, ' ');
        return;
    case '\t':
        do {
            fbcon_putc(' ');
        } while (g.cx & 7);
        return;
    }

    draw_glyph(g.cx, g.cy, (uint8_t)c);
    if (++g.cx >= g.cols)
        newline();
}

void fbcon_puts(const char *s)
{
    for (; *s; s++)
        fbcon_putc(*s);
}

void fbcon_lf(void)
{
    if (++g.cy >= g.rows)
        scroll();
}

void fbcon_size(uint32_t *cols, uint32_t *rows)
{
    if (cols)
        *cols = g.cols;
    if (rows)
        *rows = g.rows;
}
