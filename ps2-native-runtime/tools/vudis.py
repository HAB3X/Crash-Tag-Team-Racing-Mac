#!/usr/bin/env python3
"""Disassemble VU micro code: vudis.py FILE START [COUNT]   (FILE: raw micro memory, 8 bytes an instruction; START a byte
address). A debugging aid for reading the programs a game uploads."""

import struct
import sys

DEST = lambda w: "".join(c for c, b in zip("xyzw", (24, 23, 22, 21)) if w >> b & 1)
BC = "xyzw"


def s11(v):
    v &= 0x7FF
    return v - 0x800 if v & 0x400 else v


def upper(w):
    op = w & 63
    d, ft, fs, fd = DEST(w), (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    bc = BC[w & 3]
    if op < 28:
        name = ["add", "sub", "madd", "msub", "max", "mini", "mul"][op >> 2]
        return f"{name}{bc}.{d} vf{fd}, vf{fs}, vf{ft}{bc}"
    names1 = {28: "mulq", 29: "maxi", 30: "muli", 31: "minii", 32: "addq", 33: "maddq", 34: "addi", 35: "maddi",
              36: "subq", 37: "msubq", 38: "subi", 39: "msubi", 40: "add", 41: "madd", 42: "mul", 43: "max", 44: "sub",
              45: "msub", 46: "opmsub", 47: "mini"}
    if op in names1:
        return f"{names1[op]}.{d} vf{fd}, vf{fs}, vf{ft}"
    if op >= 60:
        code = (((w >> 6) & 31) << 2) | (w & 3)
        if code < 16:
            name = ["adda", "suba", "madda", "msuba"][code >> 2]
            return f"{name}{BC[code & 3]}.{d} acc, vf{fs}, vf{ft}{BC[code & 3]}"
        if 0x18 <= code <= 0x1B:
            return f"mula{BC[code & 3]}.{d} acc, vf{fs}, vf{ft}{BC[code & 3]}"
        names2 = {0x10: "itof0", 0x11: "itof4", 0x12: "itof12", 0x13: "itof15", 0x14: "ftoi0", 0x15: "ftoi4",
                  0x16: "ftoi12", 0x17: "ftoi15", 0x1C: "mulaq", 0x1D: "abs", 0x1E: "mulai", 0x1F: "clipw",
                  0x20: "addaq", 0x21: "maddaq", 0x22: "addai", 0x23: "maddai", 0x24: "subaq", 0x25: "msubaq",
                  0x26: "subai", 0x27: "msubai", 0x28: "adda", 0x29: "madda", 0x2A: "mula", 0x2C: "suba",
                  0x2D: "msuba", 0x2E: "opmula", 0x2F: "nop"}
        n = names2.get(code, f"?up{code:x}")
        if n == "nop":
            return "nop"
        return f"{n}.{d} vf{ft}, vf{fs}"
    return f"?upper {w:08x}"


def lower(w, pc):
    if w >> 25 == 0x40:
        op = w & 63
        it, is_, id_ = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        d = DEST(w)
        if op == 0x30:
            return f"iadd vi{id_}, vi{is_}, vi{it}"
        if op == 0x31:
            return f"isub vi{id_}, vi{is_}, vi{it}"
        if op == 0x32:
            imm = (w >> 6) & 31
            imm = imm - 32 if imm & 16 else imm
            return f"iaddi vi{it}, vi{is_}, {imm}"
        if op == 0x34:
            return f"iand vi{id_}, vi{is_}, vi{it}"
        if op == 0x35:
            return f"ior vi{id_}, vi{is_}, vi{it}"
        if op >= 0x3C:
            code = (((w >> 6) & 31) << 2) | (w & 3)
            fsf, ftf = BC[(w >> 21) & 3], BC[(w >> 23) & 3]
            t = {0x30: f"move.{d} vf{it}, vf{is_}", 0x31: f"mr32.{d} vf{it}, vf{is_}",
                 0x34: f"lqi.{d} vf{it}, (vi{is_}++)", 0x35: f"sqi.{d} vf{is_}, (vi{it}++)",
                 0x36: f"lqd.{d} vf{it}, (--vi{is_})", 0x37: f"sqd.{d} vf{is_}, (--vi{it})",
                 0x38: f"div q, vf{is_}{fsf}, vf{it}{ftf}", 0x39: f"sqrt q, vf{it}{ftf}",
                 0x3A: f"rsqrt q, vf{is_}{fsf}, vf{it}{ftf}", 0x3B: "waitq", 0x3C: f"mtir vi{it}, vf{is_}{fsf}",
                 0x3D: f"mfir.{d} vf{it}, vi{is_}", 0x3E: f"ilwr.{d} vi{it}, (vi{is_})",
                 0x3F: f"iswr.{d} vi{it}, (vi{is_})", 0x40: f"rnext.{d} vf{it}", 0x41: f"rget.{d} vf{it}",
                 0x42: f"rinit r, vf{is_}{fsf}", 0x43: f"rxor r, vf{is_}{fsf}", 0x64: f"mfp.{d} vf{it}, p",
                 0x68: f"xtop vi{it}", 0x69: f"xitop vi{it}", 0x6C: f"xgkick vi{is_}", 0x70: f"esadd p, vf{is_}",
                 0x71: f"ersadd p, vf{is_}", 0x72: f"eleng p, vf{is_}", 0x73: f"erleng p, vf{is_}",
                 0x74: f"eatanxy p, vf{is_}", 0x75: f"eatanxz p, vf{is_}", 0x76: f"esum p, vf{is_}",
                 0x78: f"esqrt p, vf{is_}{fsf}", 0x79: f"ersqrt p, vf{is_}{fsf}", 0x7A: f"ercpr p, vf{is_}{fsf}",
                 0x7B: "waitp", 0x7C: f"esin p, vf{is_}{fsf}", 0x7D: f"eatan p, vf{is_}{fsf}",
                 0x7E: f"eexp p, vf{is_}{fsf}"}
            return t.get(code, f"?low2 {code:x}")
        return f"?lowop {op:x}"
    op = w >> 25
    it, is_ = (w >> 16) & 31, (w >> 11) & 31
    imm = s11(w)
    d = DEST(w)
    tgt = pc + 8 + imm * 8
    m = {0x00: f"lq.{d} vf{it}, {imm}(vi{is_})", 0x01: f"sq.{d} vf{is_}, {imm}(vi{it})",
         0x04: f"ilw.{d} vi{it}, {imm}(vi{is_})", 0x05: f"isw.{d} vi{it}, {imm}(vi{is_})",
         0x08: f"iaddiu vi{it}, vi{is_}, {((w >> 10) & 0x7800) | (w & 0x7FF)}",
         0x09: f"isubiu vi{it}, vi{is_}, {((w >> 10) & 0x7800) | (w & 0x7FF)}",
         0x10: f"fceq vi1, {w & 0xFFFFFF:x}", 0x11: f"fcset {w & 0xFFFFFF:x}", 0x12: f"fcand vi1, {w & 0xFFFFFF:x}",
         0x13: f"fcor vi1, {w & 0xFFFFFF:x}", 0x14: f"fseq vi{it}, {w & 0xFFF:x}", 0x15: f"fsset {w & 0xFFF:x}",
         0x16: f"fsand vi{it}, {w & 0xFFF:x}", 0x17: f"fsor vi{it}, {w & 0xFFF:x}", 0x18: f"fmeq vi{it}, vi{is_}",
         0x1A: f"fmand vi{it}, vi{is_}", 0x1B: f"fmor vi{it}, vi{is_}", 0x1C: f"fcget vi{it}",
         0x20: f"b {tgt:x}", 0x21: f"bal vi{it}, {tgt:x}", 0x24: f"jr vi{is_}", 0x25: f"jalr vi{it}, vi{is_}",
         0x28: f"ibeq vi{it}, vi{is_}, {tgt:x}", 0x29: f"ibne vi{it}, vi{is_}, {tgt:x}",
         0x2C: f"ibltz vi{is_}, {tgt:x}", 0x2D: f"ibgtz vi{is_}, {tgt:x}", 0x2E: f"iblez vi{is_}, {tgt:x}",
         0x2F: f"ibgez vi{is_}, {tgt:x}"}
    return m.get(op, f"?low {w:08x}")


def main():
    data = open(sys.argv[1], "rb").read()
    start = int(sys.argv[2], 0)
    count = int(sys.argv[3], 0) if len(sys.argv) > 3 else 64
    for n in range(count):
        pc = start + n * 8
        lo, up = struct.unpack_from("<II", data, pc)
        flags = "".join(f for f, b in zip("IEMDT", (31, 30, 29, 28, 27)) if up >> b & 1)
        if up >> 31 & 1:
            low = f"loi {struct.unpack('<f', struct.pack('<I', lo))[0]:g}"
        else:
            low = lower(lo, pc)
        print(f"{pc:05x}: {flags:3s} {upper(up & 0x7FFFFFF if False else up):34s} {low}")


if __name__ == "__main__":
    main()
