#!/usr/bin/env python3
"""marinate.py — marinate a Brainfuck program into an eldritch hymn.

Reads a Brainfuck source file and writes a poetic text whose eight
quote-glyphs whisper exactly that program. Every other character is
chaff the interpreter ignores.

Usage: python3 tools/marinate.py examples/hello.bf examples/hymn.txt
"""

import sys

GLYPH = {
    "+": "`",
    "-": "\u00b4",
    ">": "'",
    "<": '"',
    ".": "\u2018",
    ",": "\u2019",
    "[": "\u201c",
    "]": "\u201d",
}

SEED = "Ia! Ia! Cthulhu fhtagn."

FRAG = [
    "in the deep", "beneath the sea", "when the stars are right",
    "the dead whisper", "the elder sign", "of the sunken city",
    "the high priests", "at the end of days", "the sleeping god",
    "dreams of the waking", "trembling at the gate", "the ocean is silent",
    "the moon is pale", "the gates are open", "the shadows gather",
    "in the dark water", "the chant endures", "the spire of Leng",
    "the cold plateau", "the crawling chaos", "the faceless god",
    "under the dead sea", "the ruin of Khem", "beyond the veil",
    "the nameless rite", "the tower of Yeth", "in the drowned land",
    "the stone door", "twice spoken", "never twice alike",
]

_state = 0x5EED

def rnd():
    global _state
    _state = (_state * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
    return _state

def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    with open(sys.argv[1], encoding="utf-8") as f:
        src = f.read()
    ops = [c for c in src if c in GLYPH]
    pieces = [SEED]
    for g in ops:
        frag = FRAG[rnd() % len(FRAG)]
        pieces.append(frag[:-1] + GLYPH[g])
        r = rnd()
        if r % 7 == 0:
            pieces.append(",")
        if r % 11 == 0:
            pieces.append(".")
    pieces.append("fhtagn.")

    words = " ".join(pieces).split()
    lines = []
    cur = ""
    for w in words:
        if len(cur) + len(w) + 1 > 78 and cur:
            lines.append(cur)
            cur = w
        else:
            cur = (cur + " " + w) if cur else w
    if cur:
        lines.append(cur)
    text = "\n".join(lines) + "\n"

    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write(text)
    sys.stderr.write(
        "marinated: {} glyphs into {} bytes of hymn ({} words)\n".format(
            len(ops), len(text.encode("utf-8")), len(words)
        )
    )

if __name__ == "__main__":
    main()