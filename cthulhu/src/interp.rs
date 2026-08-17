//! The engine that dreams.

use std::error::Error as StdError;
use std::fmt;
use std::io::{self, Read, Write};

use crate::codec::{extract, Glyph, Op, TAPE_CELLS, TAPE_MASK};

#[derive(Debug)]
pub enum Error {
    Unsealed { pos: usize },
    Unveiled { pos: usize },
    Io(io::Error),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Unsealed { pos } => {
                write!(f, "an Unveiling at glyph #{pos} has no matching Sealing")
            }
            Error::Unveiled { pos } => {
                write!(f, "a Sealing at glyph #{pos} has no matching Unveiling")
            }
            Error::Io(e) => write!(f, "the dream chokes on I/O: {e}"),
        }
    }
}

impl StdError for Error {}

impl From<io::Error> for Error {
    fn from(e: io::Error) -> Self {
        Error::Io(e)
    }
}

#[derive(Debug, Clone, Copy)]
pub struct Stats {
    pub steps: u64,
    pub emitted: u64,
}

pub struct Machine {
    prog: Vec<Glyph>,
    tape: Vec<u8>,
    ptr: usize,
    steps: u64,
}

impl Machine {
    pub fn new(src: &[u8]) -> Machine {
        Machine {
            prog: extract(src),
            tape: vec![0; TAPE_CELLS],
            ptr: 0,
            steps: 0,
        }
    }

    pub fn glyphs(&self) -> usize {
        self.prog.len()
    }

    pub fn run<R: Read, W: Write>(&mut self, input: &mut R, output: &mut W) -> Result<Stats, Error> {
        let mut pc = 0usize;
        let mut emitted = 0u64;
        while pc < self.prog.len() {
            let op = self.prog[pc].op;
            self.steps += 1;
            match op {
                Op::Ascend => self.tape[self.ptr] = self.tape[self.ptr].wrapping_add(1),
                Op::Descend => self.tape[self.ptr] = self.tape[self.ptr].wrapping_sub(1),
                Op::Advance => self.ptr = (self.ptr + 1) & TAPE_MASK,
                Op::Retreat => self.ptr = (self.ptr + TAPE_MASK) & TAPE_MASK,
                Op::Declaim => {
                    output.write_all(&[self.tape[self.ptr]])?;
                    emitted += 1;
                }
                Op::Devour => {
                    let mut b = [0u8; 1];
                    self.tape[self.ptr] = match input.read(&mut b) {
                        Ok(0) | Err(_) => 0,
                        Ok(_) => b[0],
                    };
                }
                Op::Unveil => {
                    if self.tape[self.ptr] == 0 {
                        pc = Self::jump_forward(&self.prog, pc)?;
                    }
                }
                Op::Seclude => {
                    if self.tape[self.ptr] != 0 {
                        pc = Self::jump_back(&self.prog, pc)?;
                    }
                }
            }
            pc += 1;
        }
        Ok(Stats {
            steps: self.steps,
            emitted,
        })
    }

    fn jump_forward(prog: &[Glyph], from: usize) -> Result<usize, Error> {
        let mut depth = 0usize;
        for j in from + 1..prog.len() {
            match prog[j].op {
                Op::Unveil => depth += 1,
                Op::Seclude => {
                    if depth == 0 {
                        return Ok(j);
                    }
                    depth -= 1;
                }
                _ => {}
            }
        }
        Err(Error::Unsealed { pos: prog[from].pos })
    }

    fn jump_back(prog: &[Glyph], from: usize) -> Result<usize, Error> {
        let mut depth = 0usize;
        for j in (0..from).rev() {
            match prog[j].op {
                Op::Seclude => depth += 1,
                Op::Unveil => {
                    if depth == 0 {
                        return Ok(j);
                    }
                    depth -= 1;
                }
                _ => {}
            }
        }
        Err(Error::Unveiled { pos: prog[from].pos })
    }
}