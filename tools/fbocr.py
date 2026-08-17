#!/usr/bin/env python3
# fbocr.py — decode a GNOS framebuffer screenshot back into text.
#
# The headless-test counterpart of a pair of eyes: QEMU's screendump gives a
# PPM of the framebuffer, this script re-decodes the console cells with the
# same fonts the kernel draws (font8x16.h and cjkfont.bin), and prints the
# screen as text -- which is the only way a headless run can verify that
# Chinese (or fastfetch's logo) actually rendered, as opposed to merely
# reaching the tty.
#
# Usage: python3 tools/fbocr.py screenshot.ppm [cols rows]
# The pixel format of the GNOS fb is 0x00RRGGBB (little-endian uint32), which
# QEMU emits as RGB in the PPM.
import struct, sys

SRC = sys.argv[0].rsplit('/', 1)[0] + '/..' if '/' in sys.argv[0] else '..'

def load_font8x16():
    """Parse font8x16.h's C array into glyphs[256][16]."""
    txt = open(f'{SRC}/src/kernel/driver/font8x16.h').read()
    txt = txt[txt.index('{'):]
    vals = []
    for tok in txt.replace('{', ' ').replace('}', ' ').replace(',', ' ').split():
        if tok.startswith('0x'):
            vals.append(int(tok, 16))
    assert len(vals) >= 256 * 16, f'font8x16 short: {len(vals)}'
    return [bytes(vals[i*16:(i+1)*16]) for i in range(256)]

def load_cjk():
    """Return (glyph2cp, widths) from cjkfont.bin."""
    d = open(f'{SRC}/src/kernel/driver/cjkfont.bin', 'rb').read()
    magic, ver, h, stride, ng, nr = struct.unpack_from('<6I', d, 0)
    assert magic == 0x464B4A43 and ver == 1 and h == 16 and stride == 2
    ranges = [struct.unpack_from('<3I', d, 32 + i*12) for i in range(nr)]
    woff = 32 + nr*12
    boff = woff + ((ng + 3) & ~3)
    g2c = {}
    for first, last, idx in ranges:
        for cp in range(first, last + 1):
            w = d[woff + idx + (cp - first)]
            if w:
                g2c[bytes(d[boff + (idx + (cp - first))*32 : boff + (idx + (cp - first))*32 + 32])] = (cp, w)
    return g2c

def main():
    ppm = sys.argv[1]
    with open(ppm, 'rb') as f:
        assert f.readline().strip() == b'P6'
        line = f.readline()
        while line.startswith(b'#'):
            line = f.readline()
        w, h = map(int, line.split())
        maxv = int(f.readline())
        assert maxv == 255
        raw = f.read()

    cols = int(sys.argv[2]) if len(sys.argv) > 2 else w // 8
    rows = int(sys.argv[3]) if len(sys.argv) > 3 else h // 16

    def px(x, y):
        o = (y * w + x) * 3
        return (raw[o], raw[o+1], raw[o+2])

    font = load_font8x16()
    g2c = load_cjk()
    text8 = {glyph: chr(i) for i, glyph in enumerate(font)}

    out = []
    for r in range(rows):
        line = []
        c = 0
        while c < cols:
            x0, y0 = c * 8, r * 16
            # cell background = majority of the four corners
            corners = [px(x0, y0), px(x0+7, y0), px(x0, y0+15), px(x0+7, y0+15)]
            bg = max(set(corners), key=corners.count)
            bits = []
            for y in range(16):
                row = 0
                for x in range(8):
                    if px(x0+x, y0+y) != bg:
                        row |= 1 << (7 - x)
                bits.append(row)
            glyph = bytes(bits)
            if all(b == 0 for b in bits):
                line.append(' ')
                c += 1
                continue
            if glyph in text8:
                line.append(text8[glyph])
                c += 1
                continue
            # maybe a CJK glyph: grab the right half too
            bits2 = []
            for y in range(16):
                row = 0
                for x in range(8):
                    if px(x0+8+x, y0+y) != bg:
                        row |= 1 << (7 - x)
                bits2.append(row)
            full = bytes(bits) + bytes(bits2)
            if full in g2c:
                cp, _w = g2c[full]
                line.append(chr(cp))
                c += 2
                continue
            line.append('?')
            c += 1
        out.append(''.join(line).rstrip())
    print('\n'.join(out))

if __name__ == '__main__':
    main()
