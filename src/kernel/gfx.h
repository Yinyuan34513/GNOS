/*
 * gfx.h — kernel-side framebuffer graphics primitives. (GPLv2)
 *
 * A gfx_surface_t is a thin view of a linear framebuffer: a base pointer, its
 * pixel dimensions, the number of 32-bit words per scanline (stride) and the
 * bytes-per-pixel.  Everything here draws into that surface in XRGB8888
 * (the format Limine's framebuffer hands us, which is what fbcon already
 * assumes), so the primitives are independent of whatever device owns the
 * memory -- fbdev hands them its /dev/fb0 surface, a boot logo hands them a
 * temporary one, and a self-test hands them a throwaway buffer.
 */
#ifndef GNUCOS_GFX_H
#define GNUCOS_GFX_H

#include <stdint.h>

typedef struct {
    void    *base;     /* pixel (0,0) */
    uint32_t w;        /* pixels across */
    uint32_t h;        /* pixels down */
    uint32_t stride;   /* uint32_t words per scanline */
    uint8_t  bpp;      /* bytes per pixel (we require 4) */
} gfx_surface_t;

/* Build a surface over a raw framebuffer.  bpp must be 4 (XRGB8888). */
gfx_surface_t gfx_surface(void *base, uint32_t w, uint32_t h,
                          uint32_t stride_bytes, uint8_t bpp);

/* Pack R,G,B (each 0..255) into the XRGB8888 word gfx writes. */
static inline uint32_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint32_t)((0u << 24) | ((uint32_t)r << 16) |
                      ((uint32_t)g << 8) | (uint32_t)b);
}

/* Whole-surface fill. */
void gfx_clear(gfx_surface_t *s, uint32_t color);
/* Single pixel (clipped to the surface). */
void gfx_putpixel(gfx_surface_t *s, int x, int y, uint32_t color);
/* Axis-aligned filled rectangle, clipped. */
void gfx_fillrect(gfx_surface_t *s, int x, int y, int w, int h, uint32_t color);
/* Bresenham line, clipped per-endpoint. */
void gfx_line(gfx_surface_t *s, int x0, int y0, int x1, int y1, uint32_t color);
/* One glyph from the bundled 8x16 font at pixel (x,y). */
void gfx_putchar(gfx_surface_t *s, int x, int y, char c, uint32_t fg, uint32_t bg);
/* NUL-terminated string in the 8x16 font, advancing in 8-pixel steps. */
void gfx_puts(gfx_surface_t *s, int x, int y, const char *str,
              uint32_t fg, uint32_t bg);

/* Non-fatal self check: draw a coloured pixel and a rectangle into a scratch
 * surface and read the bytes back to confirm the format/addressing.  Prints a
 * one-line result to the debug console. */
void gfx_self_test(void);

#endif
