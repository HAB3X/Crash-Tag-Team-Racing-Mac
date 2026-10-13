"""The R5900 (Emotion Engine core) instruction decoder: MIPS III plus the R5900's own instructions (three-operand MULT/MULTU,
MADD, the pipeline-1 HI/LO instructions, the 128-bit MMI set, LQ/SQ, the SA register), COP0, COP1 (the single-precision FPU
with its accumulator) and COP2 (VU0 in macro mode). Each instruction becomes an Insn with its mnemonic and fields; codegen.py
gives them their meaning in C.
"""

from dataclasses import dataclass, field


@dataclass
class Insn:
    addr: int
    word: int
    name: str
    rs: int = 0
    rt: int = 0
    rd: int = 0
    sa: int = 0
    imm: int = 0       # sign-extended 16-bit immediate
    uimm: int = 0      # zero-extended 16-bit immediate
    target: int = 0    # branch or jump target address
    fs: int = 0
    ft: int = 0
    fd: int = 0
    dest: int = 0      # VU dest mask: bit 0 = x ... bit 3 = w (our order, not the encoding's)
    bc: int = 0        # VU broadcast field
    fsf: int = 0
    ftf: int = 0
    kind: str = ""     # "branch", "branch_likely", "jump", "call", "jr", "jalr", "syscall", "" (plain)
    link: bool = False

    def __repr__(self):
        return f"{self.addr:08x}: {self.name} {self.word:08x}"


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


SPECIAL = {
    0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav", 8: "jr", 9: "jalr", 10: "movz", 11: "movn",
    12: "syscall", 13: "break", 15: "sync", 16: "mfhi", 17: "mthi", 18: "mflo", 19: "mtlo", 20: "dsllv", 22: "dsrlv",
    23: "dsrav", 24: "mult", 25: "multu", 26: "div", 27: "divu", 32: "add", 33: "addu", 34: "sub", 35: "subu",
    36: "and", 37: "or", 38: "xor", 39: "nor", 40: "mfsa", 41: "mtsa", 42: "slt", 43: "sltu", 44: "dadd",
    45: "daddu", 46: "dsub", 47: "dsubu", 48: "tge", 49: "tgeu", 50: "tlt", 51: "tltu", 52: "teq", 54: "tne",
    56: "dsll", 58: "dsrl", 59: "dsra", 60: "dsll32", 62: "dsrl32", 63: "dsra32",
}

REGIMM = {
    0: "bltz", 1: "bgez", 2: "bltzl", 3: "bgezl", 8: "tgei", 9: "tgeiu", 10: "tlti", 11: "tltiu", 12: "teqi",
    14: "tnei", 16: "bltzal", 17: "bgezal", 18: "bltzall", 19: "bgezall", 24: "mtsab", 25: "mtsah",
}

PRIMARY = {
    2: "j", 3: "jal", 4: "beq", 5: "bne", 6: "blez", 7: "bgtz", 8: "addi", 9: "addiu", 10: "slti", 11: "sltiu",
    12: "andi", 13: "ori", 14: "xori", 15: "lui", 20: "beql", 21: "bnel", 22: "blezl", 23: "bgtzl", 24: "daddi",
    25: "daddiu", 26: "ldl", 27: "ldr", 30: "lq", 31: "sq", 32: "lb", 33: "lh", 34: "lwl", 35: "lw", 36: "lbu",
    37: "lhu", 38: "lwr", 39: "lwu", 40: "sb", 41: "sh", 42: "swl", 43: "sw", 44: "sdl", 45: "sdr", 46: "swr",
    47: "cache", 49: "lwc1", 51: "pref", 54: "lqc2", 55: "ld", 57: "swc1", 62: "sqc2", 63: "sd",
}

MMI = {
    0: "madd", 1: "maddu", 4: "plzcw", 16: "mfhi1", 17: "mthi1", 18: "mflo1", 19: "mtlo1", 24: "mult1",
    25: "multu1", 26: "div1", 27: "divu1", 32: "madd1", 33: "maddu1", 48: "pmfhl", 49: "pmthl", 52: "psllh",
    54: "psrlh", 55: "psrah", 60: "psllw", 62: "psrlw", 63: "psraw",
}
MMI0 = {
    0: "paddw", 1: "psubw", 2: "pcgtw", 3: "pmaxw", 4: "paddh", 5: "psubh", 6: "pcgth", 7: "pmaxh", 8: "paddb",
    9: "psubb", 10: "pcgtb", 16: "paddsw", 17: "psubsw", 18: "pextlw", 19: "ppacw", 20: "paddsh", 21: "psubsh",
    22: "pextlh", 23: "ppach", 24: "paddsb", 25: "psubsb", 26: "pextlb", 27: "ppacb", 30: "pext5", 31: "ppac5",
}
MMI1 = {
    1: "pabsw", 2: "pceqw", 3: "pminw", 4: "padsbh", 5: "pabsh", 6: "pceqh", 7: "pminh", 10: "pceqb", 16: "padduw",
    17: "psubuw", 18: "pextuw", 20: "padduh", 21: "psubuh", 22: "pextuh", 24: "paddub", 25: "psubub", 26: "pextub",
    27: "qfsrv",
}
MMI2 = {
    0: "pmaddw", 2: "psllvw", 3: "psrlvw", 4: "pmsubw", 8: "pmfhi", 9: "pmflo", 10: "pinth", 12: "pmultw",
    13: "pdivw", 14: "pcpyld", 16: "pmaddh", 17: "phmadh", 18: "pand", 19: "pxor", 20: "pmsubh", 21: "phmsbh",
    26: "pexeh", 27: "prevh", 28: "pmulth", 29: "pdivbw", 30: "pexew", 31: "prot3w",
}
MMI3 = {
    0: "pmadduw", 3: "psravw", 8: "pmthi", 9: "pmtlo", 10: "pinteh", 12: "pmultuw", 13: "pdivuw", 14: "pcpyud",
    18: "por", 19: "pnor", 26: "pexch", 27: "pcpyh", 30: "pexcw",
}

COP1_S = {
    0: "add.s", 1: "sub.s", 2: "mul.s", 3: "div.s", 4: "sqrt.s", 5: "abs.s", 6: "mov.s", 7: "neg.s", 22: "rsqrt.s",
    24: "adda.s", 25: "suba.s", 26: "mula.s", 28: "madd.s", 29: "msub.s", 30: "madda.s", 31: "msuba.s",
    36: "cvt.w.s", 40: "max.s", 41: "min.s", 48: "c.f.s", 50: "c.eq.s", 52: "c.lt.s", 54: "c.le.s",
}

# VU0 macro mode: the upper-type instructions (funct 0-59) and the special2 table (funct 60-63)
VU_SPECIAL1 = {
    28: "vmulq", 29: "vmaxi", 30: "vmuli", 31: "vminii", 32: "vaddq", 33: "vmaddq", 34: "vaddi", 35: "vmaddi",
    36: "vsubq", 37: "vmsubq", 38: "vsubi", 39: "vmsubi", 40: "vadd", 41: "vmadd", 42: "vmul", 43: "vmax",
    44: "vsub", 45: "vmsub", 46: "vopmsub", 47: "vmini", 48: "viadd", 49: "visub", 50: "viaddi", 52: "viand",
    53: "vior", 56: "vcallms", 57: "vcallmsr",
}
VU_BC = {0: "vaddbc", 1: "vsubbc", 2: "vmaddbc", 3: "vmsubbc", 4: "vmaxbc", 5: "vminibc", 6: "vmulbc"}
VU_SPECIAL2 = {
    0x10: "vitof0", 0x11: "vitof4", 0x12: "vitof12", 0x13: "vitof15", 0x14: "vftoi0", 0x15: "vftoi4",
    0x16: "vftoi12", 0x17: "vftoi15", 0x1C: "vmulaq", 0x1D: "vabs", 0x1E: "vmulai", 0x1F: "vclipw", 0x20: "vaddaq",
    0x21: "vmaddaq", 0x22: "vaddai", 0x23: "vmaddai", 0x24: "vsubaq", 0x25: "vmsubaq", 0x26: "vsubai",
    0x27: "vmsubai", 0x28: "vadda", 0x29: "vmadda", 0x2A: "vmula", 0x2C: "vsuba", 0x2D: "vmsuba", 0x2E: "vopmula",
    0x2F: "vnop", 0x30: "vmove", 0x31: "vmr32", 0x34: "vlqi", 0x35: "vsqi", 0x36: "vlqd", 0x37: "vsqd",
    0x38: "vdiv", 0x39: "vsqrt", 0x3A: "vrsqrt", 0x3B: "vwaitq", 0x3C: "vmtir", 0x3D: "vmfir", 0x3E: "vilwr",
    0x3F: "viswr", 0x40: "vrnext", 0x41: "vrget", 0x42: "vrinit", 0x43: "vrxor",
}
VU_BC2 = {0: "vaddabc", 1: "vsubabc", 2: "vmaddabc", 3: "vmsubabc", 6: "vmulabc"}

BRANCHES = {"beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal", "bc1f", "bc1t", "bc0f", "bc0t", "bc2f",
            "bc2t"}
BRANCHES_LIKELY = {"beql", "bnel", "blezl", "bgtzl", "bltzl", "bgezl", "bltzall", "bgezall", "bc1fl", "bc1tl",
                   "bc0fl", "bc0tl", "bc2fl", "bc2tl"}


def decode(word, addr):
    op = word >> 26
    rs = (word >> 21) & 31
    rt = (word >> 16) & 31
    rd = (word >> 11) & 31
    sa = (word >> 6) & 31
    funct = word & 63
    i = Insn(addr, word, "unknown", rs=rs, rt=rt, rd=rd, sa=sa, imm=sext16(word & 0xFFFF), uimm=word & 0xFFFF)
    if op == 0:
        i.name = SPECIAL.get(funct, "unknown")
        if i.name == "jr":
            i.kind = "jr"
        elif i.name == "jalr":
            i.kind = "jalr"
            i.link = True
        elif i.name == "syscall":
            i.kind = "syscall"
    elif op == 1:
        i.name = REGIMM.get(rt, "unknown")
        if i.name.startswith("b"):
            i.target = (addr + 4 + (i.imm << 2)) & 0xFFFFFFFF
            i.link = "al" in i.name
    elif op in (2, 3):
        i.name = PRIMARY[op]
        i.target = ((addr + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
        i.kind = "jump" if op == 2 else "call"
        i.link = op == 3
    elif op == 16:
        if rs == 0:
            i.name = "mfc0"
        elif rs == 4:
            i.name = "mtc0"
        elif rs == 8:
            i.name = {0: "bc0f", 1: "bc0t", 2: "bc0fl", 3: "bc0tl"}.get(rt, "unknown")
            i.target = (addr + 4 + (i.imm << 2)) & 0xFFFFFFFF
        elif rs == 16:
            i.name = {1: "tlbr", 2: "tlbwi", 6: "tlbwr", 8: "tlbp", 24: "eret", 56: "ei", 57: "di"}.get(funct, "unknown")
    elif op == 17:
        i.ft, i.fs, i.fd = rt, rd, sa
        if rs == 0:
            i.name = "mfc1"
        elif rs == 2:
            i.name = "cfc1"
        elif rs == 4:
            i.name = "mtc1"
        elif rs == 6:
            i.name = "ctc1"
        elif rs == 8:
            i.name = {0: "bc1f", 1: "bc1t", 2: "bc1fl", 3: "bc1tl"}.get(rt, "unknown")
            i.target = (addr + 4 + (i.imm << 2)) & 0xFFFFFFFF
        elif rs == 16:
            i.name = COP1_S.get(funct, "unknown")
        elif rs == 20 and funct == 32:
            i.name = "cvt.s.w"
    elif op == 18:
        decode_cop2(i, word)
    elif op == 28:
        if funct == 8:
            i.name = MMI0.get(sa, "unknown")
        elif funct == 40:
            i.name = MMI1.get(sa, "unknown")
        elif funct == 9:
            i.name = MMI2.get(sa, "unknown")
        elif funct == 41:
            i.name = MMI3.get(sa, "unknown")
        else:
            i.name = MMI.get(funct, "unknown")
    else:
        i.name = PRIMARY.get(op, "unknown")
        if op in (4, 5, 6, 7, 20, 21, 22, 23):
            i.target = (addr + 4 + (i.imm << 2)) & 0xFFFFFFFF
        if op in (49, 57):
            i.ft = rt
        if op in (54, 62):
            i.ft = rt
    if i.name in BRANCHES:
        i.kind = "branch"
        i.link = i.name.endswith("al")
    elif i.name in BRANCHES_LIKELY:
        i.kind = "branch_likely"
        i.link = i.name.endswith("all")
    return i


def decode_cop2(i, word):
    rs = (word >> 21) & 31
    i.ft = (word >> 16) & 31
    i.fs = (word >> 11) & 31
    i.fd = (word >> 6) & 31
    if not (word & (1 << 25)):
        i.name = {1: "qmfc2", 2: "cfc2", 5: "qmtc2", 6: "ctc2"}.get(rs, "unknown")
        if rs == 8:
            i.name = {0: "bc2f", 1: "bc2t", 2: "bc2fl", 3: "bc2tl"}.get(i.rt, "unknown")
            i.target = (i.addr + 4 + (i.imm << 2)) & 0xFFFFFFFF
        return
    # The dest field's bits 24..21 are x, y, z, w: our mask has x in bit 0
    enc = (word >> 21) & 15
    i.dest = ((enc >> 3) & 1) | (((enc >> 2) & 1) << 1) | (((enc >> 1) & 1) << 2) | ((enc & 1) << 3)
    i.bc = word & 3
    i.fsf = (word >> 21) & 3
    i.ftf = (word >> 23) & 3
    funct = word & 63
    if funct < 28:
        i.name = VU_BC.get(funct >> 2, "unknown")
    elif funct < 60:
        i.name = VU_SPECIAL1.get(funct, "unknown")
        if i.name == "viaddi":
            imm5 = (word >> 6) & 31
            i.imm = imm5 - 32 if imm5 & 16 else imm5
        if i.name in ("vcallms",):
            i.uimm = (word >> 6) & 0x7FFF
    else:
        code = (((word >> 6) & 31) << 2) | (word & 3)
        if code < 0x10:
            i.name = VU_BC2.get(code >> 2, "unknown")
            i.bc = code & 3
        elif 0x18 <= code <= 0x1B:
            i.name = "vmulabc"
            i.bc = code & 3
        else:
            i.name = VU_SPECIAL2.get(code, "unknown")
