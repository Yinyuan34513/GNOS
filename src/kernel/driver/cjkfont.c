/*
 * cjkfont.c — parse the embedded Unifont blob and look glyphs up. (GPLv2)
 *
 * The blob is a header, a table of code point ranges, one width byte per
 * glyph and then the bitmaps, all laid out by tools/mkcjkfont.py (see that
 * file for the field-by-field description).  Nothing is copied or decoded at
 * boot: the ranges are searched in place and the returned pointer aims
 * straight into the kernel's .rodata.
 *
 * The ranges are sorted and disjoint, so the search is a binary one -- which
 * matters more than it looks.  Every character printed to the console that is
 * not plain ASCII comes through here, and a linear walk of twenty-four ranges
 * per glyph would show up as a visibly slower `cat` of a Chinese file.
 */
#include <stdint.h>

#include "cjkfont.h"
#include "debugcon.h"

extern const uint8_t cjkfont_blob[];
extern const uint8_t cjkfont_blob_end[];

#define CJK_MAGIC  0x464B4A43u          /* 'CJKF', little-endian */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t height;
    uint32_t stride;
    uint32_t nglyph;
    uint32_t nrange;
    uint32_t pad0;
    uint32_t pad1;
} cjk_hdr_t;

typedef struct {
    uint32_t first;
    uint32_t last;
    uint32_t index;
} cjk_range_t;

static const cjk_range_t *g_ranges;
static uint32_t           g_nrange;
static const uint8_t     *g_widths;
static const uint8_t     *g_bitmaps;
static uint32_t           g_nglyph;

int cjkfont_init(void)
{
    const cjk_hdr_t *h = (const cjk_hdr_t *)cjkfont_blob;
    uint64_t         n = (uint64_t)(cjkfont_blob_end - cjkfont_blob);

    if (n < sizeof *h || h->magic != CJK_MAGIC || h->version != 1) {
        dbg_puts("CJK: font blob missing or unrecognised\n");
        return 0;
    }
    if (h->height != CJK_HEIGHT || h->stride != CJK_STRIDE) {
        dbg_puts("CJK: font blob has the wrong glyph geometry\n");
        return 0;
    }

    uint64_t roff = sizeof *h;
    uint64_t woff = roff + (uint64_t)h->nrange * sizeof(cjk_range_t);
    uint64_t boff = woff + ((h->nglyph + 3u) & ~3u);
    uint64_t need = boff + (uint64_t)h->nglyph * CJK_BYTES;

    /* A truncated blob would otherwise be discovered one page fault at a time,
     * somewhere deep in a putchar. */
    if (need != n) {
        dbg_puts("CJK: font blob is the wrong length\n");
        return 0;
    }

    g_ranges  = (const cjk_range_t *)(cjkfont_blob + roff);
    g_nrange  = h->nrange;
    g_widths  = cjkfont_blob + woff;
    g_bitmaps = cjkfont_blob + boff;
    g_nglyph  = h->nglyph;

    dbg_puts("CJK: ");
    dbg_puts_dec(g_nglyph);
    dbg_puts(" glyphs in ");
    dbg_puts_dec(g_nrange);
    dbg_puts(" ranges, ");
    dbg_puts_dec((uint32_t)(n / 1024));
    dbg_puts(" KiB\n");
    return 1;
}

/* Which glyph slot holds this code point, or -1. */
static int32_t slot_of(uint32_t cp)
{
    uint32_t lo = 0, hi = g_nrange;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const cjk_range_t *r = &g_ranges[mid];
        if (cp < r->first)
            hi = mid;
        else if (cp > r->last)
            lo = mid + 1;
        else
            return (int32_t)(r->index + (cp - r->first));
    }
    return -1;
}

const uint8_t *cjkfont_glyph(uint32_t cp, int *width)
{
    if (!g_bitmaps)
        return 0;

    int32_t g = slot_of(cp);
    if (g < 0 || (uint32_t)g >= g_nglyph)
        return 0;

    uint8_t w = g_widths[g];
    if (!w)                            /* in range, but the font has no glyph */
        return 0;

    if (width)
        *width = w;
    return g_bitmaps + (uint32_t)g * CJK_BYTES;
}

/*
 * The East Asian Wide and Fullwidth ranges, from Unicode's EastAsianWidth.txt.
 * This is the short form of the table -- the full one has a hundred entries
 * and most of them are for scripts the font does not carry anyway.  Getting
 * this wrong does not corrupt anything; it makes a line of text one column
 * too long or too short.
 */
static int is_wide(uint32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x115F) ||   /* Hangul Jamo initial      */
           (cp >= 0x2E80 && cp <= 0x303E) ||   /* CJK radicals, punctuation */
           (cp >= 0x3041 && cp <= 0x33FF) ||   /* kana, bopomofo, squared   */
           (cp >= 0x3400 && cp <= 0x4DBF) ||   /* CJK extension A           */
           (cp >= 0x4E00 && cp <= 0x9FFF) ||   /* CJK unified ideographs    */
           (cp >= 0xA000 && cp <= 0xA4CF) ||   /* Yi                        */
           (cp >= 0xAC00 && cp <= 0xD7A3) ||   /* Hangul syllables          */
           (cp >= 0xF900 && cp <= 0xFAFF) ||   /* CJK compatibility         */
           (cp >= 0xFE30 && cp <= 0xFE6F) ||   /* CJK compatibility forms   */
           (cp >= 0xFF00 && cp <= 0xFF60) ||   /* fullwidth forms           */
           (cp >= 0xFFE0 && cp <= 0xFFE6) ||
           (cp >= 0x20000 && cp <= 0x3FFFD);   /* the ideographic planes    */
}

/* Combining marks occupy no column of their own.  The console has no way to
 * stack one on the previous cell, so it drops them -- which is still better
 * than letting a combining accent push the rest of the line sideways. */
static int is_combining(uint32_t cp)
{
    return (cp >= 0x0300 && cp <= 0x036F) ||
           (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x20D0 && cp <= 0x20FF) ||
           (cp >= 0xFE00 && cp <= 0xFE0F) ||   /* variation selectors */
           (cp >= 0xFE20 && cp <= 0xFE2F) ||
           cp == 0x200B ||                     /* zero width space    */
           cp == 0xFEFF;                       /* byte order mark     */
}

int cjkfont_width(uint32_t cp)
{
    if (cp == 0)
        return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
        return 0;                              /* C0 / C1 control */
    if (is_combining(cp))
        return 0;
    return is_wide(cp) ? 2 : 1;
}
