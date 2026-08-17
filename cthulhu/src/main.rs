use std::env;
use std::error::Error;
use std::fs;
use std::io::{self, Write};

use cthulhu::codec::{self, Op};
use cthulhu::interp::Machine;

const USAGE: &str = "\
ph'nglui mglw'nafh Cthulhu R'lyeh wgah'nagl fhtagn

Cthulhu 0.1.0 — a tongue of the Outer Gods (Turing complete; written in Rust).

USAGE:
    cthulhu run <file>         sing the hymn: execute the program
    cthulhu declaim <file>     extract what the eight glyphs whisper
    cthulhu lore               recite the table of glyphs
    cthulhu help               this whisper
";

fn main() {
    if let Err(e) = dispatch(env::args().skip(1).collect()) {
        eprintln!("the dream fails: {e}");
        std::process::exit(1);
    }
}

fn dispatch(args: Vec<String>) -> Result<(), Box<dyn Error>> {
    match args.first().map(String::as_str) {
        Some("run") => cmd_run(&args[1..]),
        Some("declaim") => cmd_declaim(&args[1..]),
        Some("lore") => cmd_lore(),
        Some("help") | Some("-h") | Some("--help") => {
            print!("{USAGE}");
            Ok(())
        }
        Some(other) => Err(format!("no such rite: {other}\n{USAGE}").into()),
        None => {
            print!("{USAGE}");
            Ok(())
        }
    }
}

fn file_arg(args: &[String]) -> Result<&str, Box<dyn Error>> {
    args.first()
        .map(String::as_str)
        .ok_or_else(|| "a hymn file is required (see `cthulhu help`)".into())
}

fn cmd_run(args: &[String]) -> Result<(), Box<dyn Error>> {
    let path = file_arg(args)?;
    let src = fs::read(path)?;
    let mut m = Machine::new(&src);
    let stdin = io::stdin();
    let mut input = stdin.lock();
    let stdout = io::stdout();
    let mut output = stdout.lock();
    let stats = m.run(&mut input, &mut output)?;
    output.flush()?;
    eprintln!(
        "[the dream concludes] {} glyphs, {} steps, {} bytes declaimed",
        m.glyphs(),
        stats.steps,
        stats.emitted
    );
    Ok(())
}

fn cmd_declaim(args: &[String]) -> Result<(), Box<dyn Error>> {
    let path = file_arg(args)?;
    let src = fs::read(path)?;
    let glyphs = codec::extract(&src);
    println!("the hymn \"{path}\" holds {} glyphs; the tongue speaks:", glyphs.len());
    let mut tongue = String::new();
    for g in &glyphs {
        println!(
            "  # {:<5}  {:<3}  {:<16} {}",
            g.pos,
            g.op.glyph(),
            g.op.name(),
            g.op.lore()
        );
        tongue.push_str(g.op.glyph());
    }
    println!("\npure tongue: {tongue}");
    Ok(())
}

fn cmd_lore() -> Result<(), Box<dyn Error>> {
    println!("ph'nglui mglw'nafh Cthulhu R'lyeh wgah'nagl fhtagn");
    println!();
    println!("  the eight letters of the tongue — nothing else is heard:");
    println!("  ----------------------------------------------------------");
    for i in 0..8u8 {
        let op = match i {
            0 => Op::Advance,
            1 => Op::Retreat,
            2 => Op::Ascend,
            3 => Op::Descend,
            4 => Op::Declaim,
            5 => Op::Devour,
            6 => Op::Unveil,
            _ => Op::Seclude,
        };
        println!(
            "  {:<4} bf {:<3} {:<16} {}",
            op.glyph(),
            op.bf(),
            op.name(),
            op.lore()
        );
    }
    println!();
    println!("  every other character is chaff: it drifts by, unheard.");
    println!("  the eight letters are exactly Brainfuck, so the tongue");
    println!("  is Turing complete by itself — no translation, no cheating.");
    Ok(())
}