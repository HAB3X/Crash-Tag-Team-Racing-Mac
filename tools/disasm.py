#!/usr/bin/env python3
"""Disassemble an executable's code: dis.py ELF START [END]   (raw fields, for reading the recompiler's input)"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "recomp"))
from elf import Elf  # noqa: E402
from r5900 import decode  # noqa: E402

REGS = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2",
        "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]

e = Elf(sys.argv[1])
base, image = e.image()
start = int(sys.argv[2], 0)
end = int(sys.argv[3], 0) if len(sys.argv) > 3 else start + 0x40
for a in range(start, end, 4):
    w = struct.unpack_from("<I", image, a - base)[0]
    i = decode(w, a)
    extra = ""
    if i.kind in ("branch", "branch_likely", "jump", "call"):
        extra = f" -> {i.target:08x}"
    print(f"{a:08x}: {w:08x}  {i.name:8s} rs={REGS[i.rs]} rt={REGS[i.rt]} rd={REGS[i.rd]} sa={i.sa} imm={i.imm}{extra}")
