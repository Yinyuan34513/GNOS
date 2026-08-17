#!/usr/bin/env python3
"""
mkcjkfont.py — build the kernel's CJK bitmap font from GNU Unifont.

GNU Unifont is a 16x16 bitmap font that was *later* wrapped in TrueType
outlines, so rasterising it back at exactly 16 pixels reproduces the original
pixels bit for bit -- no hinting, no antialiasing, no guessing.  That is why
this script rasterises a TTF and still calls the result a bitmap font.

Output is src/kernel/driver/cjkfont.bin, which the kernel embeds with .incbin and
parses at boot.  The file is checked into the repository on purpose: building
GNOS must not require Python, Pillow or a font package to be installed.  Run
this by hand (`make cjkfont`) only when the coverage below changes.

Format, all little-endian:

    magic   u32   'CJKF'
    version u32   1
    height  u32   16      pixel rows per glyph
    stride  u32   2       bytes per row (16 pixels, MSB is the leftmost)
    nglyph  u32           total glyphs stored
    nrange  u32           how many code point ranges follow
    pad     u32 x2        header is 32 bytes

    range[nrange]         { first u32, last u32, index u32 }  12 bytes each
                          glyph for code point c in range r lives at
                          r.index + (c - r.first)

    width[nglyph]         one byte per glyph: 8 (halfwidth) or 16 (fullwidth),
                          padded to a 4-byte boundary

    bitmap[nglyph][32]    16 rows of 2 bytes

A glyph the font does not cover is stored as all zeroes with width 0, which
the kernel treats as "not in the font" and draws as a hollow box.
"""

import struct
import sys
from PIL import Image, ImageDraw, ImageFont

FONT = "/usr/share/fonts/truetype/unifont/unifont.ttf"
OUT = "src/kernel/driver/cjkfont.bin"

HEIGHT = 16
STRIDE = 2

# What to include, and why.  Everything below U+00A0 is already covered by the
# built-in 8x16 VGA font, so the ranges start above it.
RANGES = [
    (0x00A0, 0x024F, "Latin-1 supplement and Latin Extended-A/B"),
    (0x0370, 0x03FF, "Greek"),
    (0x0400, 0x04FF, "Cyrillic"),
    (0x2000, 0x206F, "general punctuation -- dashes, quotes, ellipsis"),
    (0x2070, 0x209F, "super/subscripts"),
    (0x20A0, 0x20BF, "currency signs, including U+20AC euro and U+00A5's kin"),
    (0x2100, 0x214F, "letterlike symbols"),
    (0x2190, 0x21FF, "arrows"),
    (0x2200, 0x22FF, "mathematical operators"),
    (0x2500, 0x257F, "box drawing -- what a TUI is made of"),
    (0x2580, 0x259F, "block elements, for progress bars"),
    (0x25A0, 0x25FF, "geometric shapes"),
    (0x2600, 0x26FF, "miscellaneous symbols"),
    (0x2700, 0x27BF, "dingbats -- check marks and crosses"),
    (0x3000, 0x303F, "CJK symbols and punctuation"),
    (0x3040, 0x309F, "hiragana"),
    (0x30A0, 0x30FF, "katakana"),
    (0x3100, 0x312F, "bopomofo"),
    (0x3400, 0x4DBF, "CJK extension A"),
    (0x4E00, 0x9FFF, "CJK unified ideographs -- the bulk of it"),
    (0xF900, 0xFAFF, "CJK compatibility ideographs"),
    (0xFE30, 0xFE4F, "CJK compatibility forms"),
    (0xFF00, 0xFFEF, "halfwidth and fullwidth forms"),
    (0xFFFD, 0xFFFD, "the replacement character, for undecodable input"),
]


def main():
    try:
        font = ImageFont.truetype(FONT, HEIGHT)
    except OSError:
        sys.exit("cannot open %s -- install the `unifont` font package" % FONT)

    # Which code points the font actually has a glyph for.  Asking for a
    # missing one draws .notdef (an empty or hollow box) and would bloat the
    # file with thousands of identical blanks.
    from fontTools.ttLib import TTFont
    cmap = set()
    tt = TTFont(FONT, fontNumber=0, lazy=True)
    for table in tt["cmap"].tables:
        cmap.update(table.cmap.keys())
    tt.close()

    ranges = []
    widths = bytearray()
    bitmap = bytearray()
    index = 0

    img = Image.new("L", (32, HEIGHT * 2), 0)
    draw = ImageDraw.Draw(img)

    for first, last, _why in RANGES:
        ranges.append((first, last, index))
        for cp in range(first, last + 1):
            index += 1
            if cp not in cmap:
                widths.append(0)
                bitmap.extend(b"\0" * (HEIGHT * STRIDE))
                continue

            ch = chr(cp)
            # The advance width is where halfwidth and fullwidth part company;
            # Unifont encodes exactly the distinction the terminal needs.
            adv = font.getlength(ch)
            w = 16 if adv > 8.5 else 8

            draw.rectangle([0, 0, 31, HEIGHT * 2 - 1], fill=0)
            # Unifont's ascent puts the cell top at y=0 when drawn with the
            # default "la" anchor at the origin, which is what we want: the
            # 16 rows below are the glyph box exactly.
            draw.text((0, 0), ch, font=font, fill=255)

            rows = bytearray()
            for y in range(HEIGHT):
                bits = 0
                for x in range(16):
                    if img.getpixel((x, y)) > 127:
                        bits |= 0x8000 >> x
                rows.append((bits >> 8) & 0xFF)
                rows.append(bits & 0xFF)

            if not any(rows):
                # In the font but blank (a space, or a combining mark we
                # cannot place).  Keep the width so the cursor still advances.
                pass
            widths.append(w)
            bitmap.extend(rows)

    nglyph = index
    while len(widths) % 4:
        widths.append(0)

    out = bytearray()
    out += struct.pack("<4sIIIIIII", b"CJKF", 1, HEIGHT, STRIDE,
                       nglyph, len(ranges), 0, 0)
    for first, last, idx in ranges:
        out += struct.pack("<III", first, last, idx)
    out += widths
    out += bitmap

    with open(OUT, "wb") as f:
        f.write(out)

    covered = sum(1 for w in widths if w)
    print("%s: %d glyphs in %d ranges (%d present), %d bytes"
          % (OUT, nglyph, len(ranges), covered, len(out)))


if __name__ == "__main__":
    main()
