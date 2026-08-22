#!/usr/bin/env python3
"""Offline GNU-hash walk of libc.so's dynamic symbol table.

Answers: what does musl's find_sym("__dls3") / find_sym("__dls2b") SHOULD
return, given the file's .gnu.hash / .dynsym / .dynstr content alone?
"""
import struct
import sys

DATA = open("/home/elaina/gnos/build/initrd-root/lib/ld-musl-x86_64.so.1", "rb").read()

GNU_HASH_OFF = 0x2C48   # .gnu.hash file offset (readelf)
DYNSYM_OFF = 0x5E30     # .dynsym file offset
DYNSYM_ENTSIZE = 24
DYNSTR_OFF = 0xF8B0     # .dynstr file offset


def gnu_hash(s: str) -> int:
    h = 5381
    for c in s.encode():
        h = ((h << 5) + h + c) & 0xFFFFFFFF
    return h


def lookup(name: str):
    nbuckets, symoffset, bloom_size, bloom_shift = struct.unpack_from(
        "<IIII", DATA, GNU_HASH_OFF
    )
    # bloom_size is in 8-byte words for ELFCLASS64.
    buckets_off = GNU_HASH_OFF + 16 + bloom_size * 8
    chains_off = buckets_off + nbuckets * 4
    h = gnu_hash(name)
    # musl's gnu_lookup (dynlink.c:273): buckets[h1 % nbuckets] -- the FULL
    # hash, not (h>>1).
    bucket = struct.unpack_from("<I", DATA, buckets_off + (h % nbuckets) * 4)[0]
    print(f"{name}: hash=0x{h:08x} bucket[{h % nbuckets}]={bucket}")
    if not bucket:
        print("  -> no symbols in bucket")
        return
    symindex = bucket
    while True:
        chain = struct.unpack_from(
            "<I", DATA, chains_off + (symindex - symoffset) * 4
        )[0]
        if (h | 1) == (chain & 0xFFFFFFFF | 1):
            ent = struct.unpack_from(
                "<IBBHQQ", DATA, DYNSYM_OFF + symindex * DYNSYM_ENTSIZE
            )
            st_name, st_info, st_other, st_shndx, st_value, st_size = ent
            end = DATA.index(b"\0", DYNSTR_OFF + st_name)
            sname = DATA[DYNSTR_OFF + st_name:end].decode(errors="replace")
            ok = " <== MATCH" if sname == name else ""
            print(f"  idx={symindex:4d} {sname:24s} st_value=0x{st_value:x} "
                  f"st_shndx={st_shndx}{ok}")
            if sname == name:
                return
        if not (chain & 1):
            print("  -> chain continues (bit0=0), next index")
            symindex += 1
            continue
        print("  -> chain end (bit0=1)")
        return


for n in ("__dls3", "__dls2b", "__dls2", "__libc_start_main"):
    lookup(n)
