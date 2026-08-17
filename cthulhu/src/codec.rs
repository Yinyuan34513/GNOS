//! The glyph alphabet: eight quote-like letters, nothing more.
//!
//! A Cthulhu program is a stream of bytes (typically utf-8 prose). Only
//! the eight glyphs below are heard; every other character is chaff the
//! dream ignores. The eight letters are exactly Brainfuck transliterated
//! into punctuation, which makes the tongue Turing complete.

pub const TAPE_CELLS: usize = 1 << 16;
pub(crate) const TAPE_MASK: usize = TAPE_CELLS - 1;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Op {
    /// `'` — the pointer sinks one cell deeper (bf `>`)
    Advance,
    /// `"` — the pointer rises one cell (bf `<`)
    Retreat,
    /// `` ` `` — the current cell awakens, +1 (bf `+`)
    Ascend,
    /// `´` — the current cell dreams, −1 (bf `−`)
    Descend,
    /// `‘` — the current cell is declaimed as one byte (bf `.`)
    Declaim,
    /// `’` — one byte is devoured from the abyss, EOF is 0 (bf `,`)
    Devour,
    /// `“` — if the cell is empty, leap to the matching Sealing (bf `[`)
    Unveil,
    /// `”` — if the cell is not empty, return to the Unveiling (bf `]`)
    Seclude,
}

impl Op {
    pub fn glyph(self) -> &'static str {
        match self {
            Op::Advance => "'",
            Op::Retreat => "\"",
            Op::Ascend => "`",
            Op::Descend => "\u{00b4}",
            Op::Declaim => "\u{2018}",
            Op::Devour => "\u{2019}",
            Op::Unveil => "\u{201c}",
            Op::Seclude => "\u{201d}",
        }
    }

    pub fn bf(self) -> &'static str {
        match self {
            Op::Advance => ">",
            Op::Retreat => "<",
            Op::Ascend => "+",
            Op::Descend => "-",
            Op::Declaim => ".",
            Op::Devour => ",",
            Op::Unveil => "[",
            Op::Seclude => "]",
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Op::Advance => "the Fisher",
            Op::Retreat => "the Mother",
            Op::Ascend => "the Dreamer",
            Op::Descend => "the Sleeper",
            Op::Declaim => "the Herald",
            Op::Devour => "the Devourer",
            Op::Unveil => "the Unveiling",
            Op::Seclude => "the Sealing",
        }
    }

    pub fn lore(self) -> &'static str {
        match self {
            Op::Advance => "the pointer sinks one cell deeper",
            Op::Retreat => "the pointer rises one cell",
            Op::Ascend => "the current cell awakens by one",
            Op::Descend => "the current cell dreams down by one",
            Op::Declaim => "the current cell is declaimed as one byte",
            Op::Devour => "one byte is devoured from the abyss; dead breath is 0",
            Op::Unveil => "if the cell is empty, leap to the matching Sealing",
            Op::Seclude => "if the cell is not empty, return to the matching Unveiling",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Glyph {
    pub pos: usize,
    pub op: Op,
}

/// Hear a source text: strip the chaff, keep the eight letters.
pub fn extract(src: &[u8]) -> Vec<Glyph> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < src.len() {
        if let Some((op, len)) = glyph_at(&src[i..]) {
            out.push(Glyph { pos: i, op });
            i += len;
        } else {
            i += 1;
        }
    }
    out
}

fn glyph_at(s: &[u8]) -> Option<(Op, usize)> {
    match s {
        [0x27, ..] => Some((Op::Advance, 1)),
        [0x22, ..] => Some((Op::Retreat, 1)),
        [0x60, ..] => Some((Op::Ascend, 1)),
        [0xc2, 0xb4, ..] => Some((Op::Descend, 2)),
        [0xe2, 0x80, 0x98, ..] => Some((Op::Declaim, 3)),
        [0xe2, 0x80, 0x99, ..] => Some((Op::Devour, 3)),
        [0xe2, 0x80, 0x9c, ..] => Some((Op::Unveil, 3)),
        [0xe2, 0x80, 0x9d, ..] => Some((Op::Seclude, 3)),
        _ => None,
    }
}

/// Marinate: turn an op sequence into the bare glyph stream (the pure
/// tongue, no chaff). Inverse of [`extract`] on glyph-only text.
pub fn marinate(ops: &[Op]) -> String {
    let mut s = String::new();
    for &op in ops {
        s.push_str(op.glyph());
    }
    s
}