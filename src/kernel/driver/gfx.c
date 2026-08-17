/*
 * gfx.c — kernel-side framebuffer graphics primitives. (GPLv2)
 *
 * See gfx.h.  All drawing is in XRGB8888 (native little-endian uint32 per
 * pixel), matching the format Limine's linear framebuffer uses and what fbcon
 * already assumes.  Every routine clips to the surface so a bad coordinate can
 * never walk off the end of the framebuffer.
 */
#include <stdint.h>

#include "gfx.h"
#include "font8x16.h"
#include "kstring.h"
#include "debugcon.h"
#include "heap.h"

gfx_surface_t gfx_surface(void *base, uint32_t w, uint32_t h,
                          uint32_t stride_bytes, uint8_t bpp)
{
    gfx_surface_t s;
    s.base   = base;
    s.w      = w;
    s.h      = h;
    s.stride = stride_bytes / 4;   /* we treat scanlines as uint32 words */
    s.bpp    = bpp;
    return s;
}

static inline uint32_t *pixel_at(gfx_surface_t *s, int x, int y)
{
    return (uint32_t *)s->base + (uint64_t)y * s->stride + x;
}

void gfx_clear(gfx_surface_t *s, uint32_t color)
{
    if (!s->base)
        return;
    for (uint32_t y = 0; y < s->h; y++)
        for (uint32_t x = 0; x < s->w; x++)
            pixel_at(s, x, y)[0] = color;
}

void gfx_putpixel(gfx_surface_t *s, int x, int y, uint32_t color)
{
    if (!s->base || x < 0 || y < 0 || (uint32_t)x >= s->w || (uint32_t)y >= s->h)
        return;
    pixel_at(s, x, y)[0] = color;
}

void gfx_fillrect(gfx_surface_t *s, int x, int y, int w, int h, uint32_t color)
{
    if (!s->base)
        return;
    /* Normalise and clip against the surface. */
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0)
        return;
    if ((uint32_t)(x + w) > s->w) w = (int)(s->w - x);
    if ((uint32_t)(y + h) > s->h) h = (int)(s->h - y);

    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            pixel_at(s, xx, yy)[0] = color;
}

void gfx_line(gfx_surface_t *s, int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = (dx > dy ? dx : dy) >> 1;

    for (;;) {
        gfx_putpixel(s, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = err;
        if (e2 > -dx) { err -= dy; x0 += sx; }
        if (e2 <  dy) { err += dx; y0 += sy; }
    }
}

void gfx_putchar(gfx_surface_t *s, int x, int y, char c, uint32_t fg, uint32_t bg)
{
    if (!s->base)
        return;

    const unsigned char *glyph = &font8x16[(uint8_t)c * FONT_HEIGHT];
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            gfx_putpixel(s, x + col, y + row,
                         (bits & (0x80u >> col)) ? fg : bg);
        }
    }
}

void gfx_puts(gfx_surface_t *s, int x, int y, const char *str,
              uint32_t fg, uint32_t bg)
{
    if (!s->base || !str)
        return;
    int cx = x;
    for (; *str; str++) {
        gfx_putchar(s, cx, y, *str, fg, bg);
        cx += FONT_WIDTH;
    }
}

void gfx_self_test(void)
{
    dbg_puts("GFX: self-test ... ");

    /* A scratch 64x64 surface in the heap (no real hardware needed). */
    uint32_t *buf = (uint32_t *)kzalloc(64 * 64 * 4);
    int ok = (buf != NULL);
    gfx_surface_t s = gfx_surface(buf, 64, 64, 64 * 4, 4);

    if (ok) {
        gfx_clear(&s, gfx_rgb(0, 0, 0));
        gfx_putpixel(&s, 10, 20, gfx_rgb(0xFF, 0, 0));
        /* Read the pixel back at the expected offset (stride 64 => y*64+x). */
        uint32_t got = buf[(uint64_t)20 * 64 + 10];
        ok = (got == gfx_rgb(0xFF, 0, 0));

        /* A rectangle of a known colour; verify two interior pixels and that
         * the area outside is still background. */
        if (ok) {
            gfx_fillrect(&s, 5, 5, 20, 20, gfx_rgb(0, 0xFF, 0));
            ok = (buf[(uint64_t)10 * 64 + 10] == gfx_rgb(0, 0xFF, 0)) &&
                 (buf[(uint64_t)24 * 64 + 24] == gfx_rgb(0, 0xFF, 0)) &&
                 (buf[(uint64_t)0  * 64 + 0]  == gfx_rgb(0, 0, 0));
        }

        /* Clipping: an out-of-bounds pixel must not fault or change a corner. */
        if (ok) {
            uint32_t before = buf[0];
            gfx_putpixel(&s, -5, -5, gfx_rgb(1, 2, 3));
            gfx_putpixel(&s, 9999, 9999, gfx_rgb(1, 2, 3));
            ok = (buf[0] == before);
        }

        /* Text: scan the two glyph boxes for a foreground pixel.  Do not test
         * one specific coordinate — the top rows of an 8x16 glyph are blank. */
        if (ok) {
            gfx_puts(&s, 0, 0, "Hi", gfx_rgb(0xFF, 0xFF, 0xFF), gfx_rgb(0, 0, 0));
            int lit = 0;
            for (int yy = 0; yy < FONT_HEIGHT && !lit; yy++)
                for (int xx = 0; xx < 2 * FONT_WIDTH; xx++)
                    if (buf[(uint64_t)yy * 64 + xx] == gfx_rgb(0xFF, 0xFF, 0xFF)) {
                        lit = 1;
                        break;
                    }
            ok = lit;
        }
    }

    if (buf)
        kfree(buf);
    dbg_puts(ok ? "ok\r\n" : "FAIL\r\n");
}
