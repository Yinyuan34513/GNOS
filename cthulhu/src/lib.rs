//! # Cthulhu — a tongue of the Outer Gods
//!
//! A Cthulhu program is ordinary text. Eight quote-like glyphs are the
//! only letters of the tongue; every other character is chaff the dream
//! ignores. You may hide a program inside a psalm, a letter, a poem —
//! the interpreter hears only the eight glyphs.
//!
//! The eight letters are exactly Brainfuck, transliterated into
//! punctuation, so the tongue is Turing complete *by itself*:
//!
//! | glyph | bf  | name            |
//! |-------|-----|-----------------|
//! | `'`   | `>` | the Fisher      |
//! | `"`   | `<` | the Mother      |
//! | `` ` ``| `+` | the Dreamer    |
//! | `´`   | `-` | the Sleeper     |
//! | `‘`   | `.` | the Herald      |
//! | `’`   | `,` | the Devourer    |
//! | `“`   | `[` | the Unveiling   |
//! | `”`   | `]` | the Sealing     |
//!
//! The mapping is a bijection, so no translation step exists: the glyphs
//! *are* the program.

pub mod codec;
pub mod interp;