/*
 * cjkfont.h — glyph lookup in the embedded Unifont bitmaps. (GPLv2)
 *
 * The built-in 8x16 VGA font covers exactly the 256 characters a PC BIOS knew
 * about.  Everything else -- Chinese, box drawing, arrows, the replacement
 * character -- comes from here, addressed by Unicode code point rather than
 * by byte value.
 */
#ifndef GNUOS_CJKFONT_H
#define GNUOS_CJKFONT_H

#include <stdint.h>

/* Every glyph in this font is 16 rows of 16 pixels, stored two bytes per row
 * with the leftmost pixel in the high bit of the first byte.  Halfwidth
 * glyphs simply leave the right-hand eight columns clear. */
#define CJK_HEIGHT  16
#define CJK_STRIDE  2
#define CJK_BYTES   (CJK_HEIGHT * CJK_STRIDE)

/* Validate the blob and report how many glyphs it holds (0 if it is missing
 * or malformed, in which case every lookup below fails and the console falls
 * back to drawing a hollow box). */
int cjkfont_init(void);

/*
 * Find the bitmap for a code point.  Returns NULL when the font has nothing
 * for it.  On success *width is 8 or 16 -- the number of columns the glyph
 * really occupies, which is also how many console cells it needs.
 */
const uint8_t *cjkfont_glyph(uint32_t cp, int *width);

/*
 * How wide a code point is on a character terminal, without needing its
 * bitmap: 0 for a combining mark or a control character, 2 for the East Asian
 * wide ranges, 1 for everything else.  This is the same question wcwidth(3)
 * answers, and the console has to answer it before it knows whether the glyph
 * will fit on the line.
 */
int cjkfont_width(uint32_t cp);

#endif
