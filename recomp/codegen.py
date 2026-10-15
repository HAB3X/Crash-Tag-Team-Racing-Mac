"""C for R5900 code: one C function per MIPS function, the registers in the Ps2Cpu struct, memory through the runtime's access
functions (runtime/include/ps2/recomp.h), branches as gotos with their delay slots run in the MIPS order (the condition read
first, then the delay slot, then the jump), calls as C calls, and `jr ra` as a C return.
"""

S64 = "c->r[{}].sd[0]"
U64 = "c->r[{}].ud[0]"
S32 = "c->r[{}].sw[0]"
U32 = "c->r[{}].uw[0]"


def s64(n):
    return "0" if n == 0 else S64.format(n)


def u64(n):
    return "0" if n == 0 else U64.format(n)


def s32(n):
    return "0" if n == 0 else S32.format(n)


def u32(n):
    return "0u" if n == 0 else U32.format(n)


def set64(n, expr):
    return "" if n == 0 else f"{S64.format(n)} = (s64)({expr});"


def set32(n, expr):
    return "" if n == 0 else f"{S64.format(n)} = (s64)(s32)(u32)({expr});"


def addr(i):
    return f"(u32)({u32(i.rs)} + {i.imm})"


FPU_BINARY = {"add.s": "fpu_add", "sub.s": "fpu_sub", "mul.s": "fpu_mul", "div.s": "fpu_div"}


def emit_plain(i, ctx):
    """The C for an instruction that isn't a branch or jump."""
    n = i.name
    rs, rt, rd, sa = i.rs, i.rt, i.rd, i.sa
    # --- SPECIAL
    if n == "sll":
        if rd == 0:
            return ""
        return set32(rd, f"{u32(rt)} << {sa}")
    if n == "srl":
        return set32(rd, f"{u32(rt)} >> {sa}")
    if n == "sra":
        return set32(rd, f"{s32(rt)} >> {sa}")
    if n == "sllv":
        return set32(rd, f"{u32(rt)} << ({u32(rs)} & 31)")
    if n == "srlv":
        return set32(rd, f"{u32(rt)} >> ({u32(rs)} & 31)")
    if n == "srav":
        return set32(rd, f"{s32(rt)} >> ({u32(rs)} & 31)")
    if n == "dsll":
        return set64(rd, f"{u64(rt)} << {sa}")
    if n == "dsrl":
        return set64(rd, f"{u64(rt)} >> {sa}")
    if n == "dsra":
        return set64(rd, f"{s64(rt)} >> {sa}")
    if n == "dsll32":
        return set64(rd, f"{u64(rt)} << {sa + 32}")
    if n == "dsrl32":
        return set64(rd, f"{u64(rt)} >> {sa + 32}")
    if n == "dsra32":
        return set64(rd, f"{s64(rt)} >> {sa + 32}")
    if n == "dsllv":
        return set64(rd, f"{u64(rt)} << ({u32(rs)} & 63)")
    if n == "dsrlv":
        return set64(rd, f"{u64(rt)} >> ({u32(rs)} & 63)")
    if n == "dsrav":
        return set64(rd, f"{s64(rt)} >> ({u32(rs)} & 63)")
    if n == "movz":
        return "" if rd == 0 else f"if ({u64(rt)} == 0) {S64.format(rd)} = {s64(rs)};"
    if n == "movn":
        return "" if rd == 0 else f"if ({u64(rt)} != 0) {S64.format(rd)} = {s64(rs)};"
    if n == "mfhi":
        return set64(rd, "c->hi.sd[0]")
    if n == "mflo":
        return set64(rd, "c->lo.sd[0]")
    if n == "mthi":
        return f"c->hi.sd[0] = {s64(rs)};"
    if n == "mtlo":
        return f"c->lo.sd[0] = {s64(rs)};"
    if n == "mfhi1":
        return set64(rd, "c->hi.sd[1]")
    if n == "mflo1":
        return set64(rd, "c->lo.sd[1]")
    if n == "mthi1":
        return f"c->hi.sd[1] = {s64(rs)};"
    if n == "mtlo1":
        return f"c->lo.sd[1] = {s64(rs)};"
    if n in ("mult", "mult1"):
        k = 1 if n.endswith("1") else 0
        out = f"{{ s64 p = (s64){s32(rs)} * (s64){s32(rt)}; c->lo.sd[{k}] = (s32)p; c->hi.sd[{k}] = (s32)(p >> 32);"
        if rd:
            out += f" {S64.format(rd)} = c->lo.sd[{k}];"
        return out + " }"
    if n in ("multu", "multu1"):
        k = 1 if n.endswith("1") else 0
        out = (f"{{ u64 p = (u64){u32(rs)} * (u64){u32(rt)}; c->lo.sd[{k}] = (s32)(u32)p; "
               f"c->hi.sd[{k}] = (s32)(u32)(p >> 32);")
        if rd:
            out += f" {S64.format(rd)} = c->lo.sd[{k}];"
        return out + " }"
    if n in ("madd", "madd1", "maddu", "maddu1"):
        k = 1 if n.endswith("1") else 0
        if n.startswith("maddu"):
            prod = f"(u64){u32(rs)} * (u64){u32(rt)}"
        else:
            prod = f"(u64)((s64){s32(rs)} * (s64){s32(rt)})"
        out = (f"{{ u64 a = ((u64)c->hi.uw[{2 * k}] << 32) | c->lo.uw[{2 * k}]; a += {prod}; "
               f"c->lo.sd[{k}] = (s32)(u32)a; c->hi.sd[{k}] = (s32)(u32)(a >> 32);")
        if rd:
            out += f" {S64.format(rd)} = c->lo.sd[{k}];"
        return out + " }"
    if n in ("div", "div1"):
        return f"ps2_div(c, {s32(rs)}, {s32(rt)}, {1 if n.endswith('1') else 0});"
    if n in ("divu", "divu1"):
        return f"ps2_divu(c, {u32(rs)}, {u32(rt)}, {1 if n.endswith('1') else 0});"
    if n in ("add", "addu"):
        return set32(rd, f"{u32(rs)} + {u32(rt)}")
    if n in ("sub", "subu"):
        return set32(rd, f"{u32(rs)} - {u32(rt)}")
    if n in ("dadd", "daddu"):
        return set64(rd, f"{u64(rs)} + {u64(rt)}")
    if n in ("dsub", "dsubu"):
        return set64(rd, f"{u64(rs)} - {u64(rt)}")
    if n == "and":
        return set64(rd, f"{u64(rs)} & {u64(rt)}")
    if n == "or":
        return set64(rd, f"{u64(rs)} | {u64(rt)}")
    if n == "xor":
        return set64(rd, f"{u64(rs)} ^ {u64(rt)}")
    if n == "nor":
        return set64(rd, f"~({u64(rs)} | {u64(rt)})")
    if n == "slt":
        return set64(rd, f"{s64(rs)} < {s64(rt)}")
    if n == "sltu":
        return set64(rd, f"{u64(rs)} < {u64(rt)}")
    if n == "mfsa":
        return set64(rd, "c->sa")
    if n == "mtsa":
        return f"c->sa = {u32(rs)};"
    if n == "mtsab":
        return f"c->sa = ({u32(rs)} & 15) ^ ({i.imm} & 15);"
    if n == "mtsah":
        return f"c->sa = (({u32(rs)} & 7) ^ ({i.imm} & 7)) << 1;"
    if n in ("sync", "cache", "pref", "tge", "tgeu", "tlt", "tltu", "teq", "tne", "tgei", "tgeiu", "tlti", "tltiu",
             "teqi", "tnei", "tlbr", "tlbwi", "tlbwr", "tlbp"):
        return ""
    if n == "syscall":
        return f"ps2_syscall(c, 0x{i.addr:08x});"
    if n == "break":
        return f"ps2_break(c, 0x{i.addr:08x});"
    # --- immediates
    if n in ("addi", "addiu"):
        return set32(rt, f"{u32(rs)} + {i.imm}")
    if n in ("daddi", "daddiu"):
        return set64(rt, f"{u64(rs)} + (u64)(s64){i.imm}")
    if n == "slti":
        return set64(rt, f"{s64(rs)} < (s64){i.imm}")
    if n == "sltiu":
        return set64(rt, f"{u64(rs)} < (u64)(s64){i.imm}")
    if n == "andi":
        return set64(rt, f"{u64(rs)} & 0x{i.uimm:x}u")
    if n == "ori":
        return set64(rt, f"{u64(rs)} | 0x{i.uimm:x}u")
    if n == "xori":
        return set64(rt, f"{u64(rs)} ^ 0x{i.uimm:x}u")
    if n == "lui":
        return set64(rt, f"(s32)0x{(i.uimm << 16) & 0xFFFFFFFF:08x}u")
    # --- loads and stores
    a = addr(i)
    if n == "lb":
        return f"{{ s64 v = (s8)rd8({a}); {set64(rt, 'v')} }}"
    if n == "lbu":
        return f"{{ u64 v = rd8({a}); {set64(rt, 'v')} }}"
    if n == "lh":
        return f"{{ s64 v = (s16)rd16({a}); {set64(rt, 'v')} }}"
    if n == "lhu":
        return f"{{ u64 v = rd16({a}); {set64(rt, 'v')} }}"
    if n == "lw":
        return f"{{ s64 v = (s32)rd32({a}); {set64(rt, 'v')} }}"
    if n == "lwu":
        return f"{{ u64 v = rd32({a}); {set64(rt, 'v')} }}"
    if n == "ld":
        return f"{{ u64 v = rd64({a}); {set64(rt, 'v')} }}"
    if n == "lq":
        return f"{{ u128 v = rd128({a} & ~15u); {'' if rt == 0 else f'c->r[{rt}].q = v;'} }}"
    if n in ("lwl", "lwr", "ldl", "ldr"):
        return "" if rt == 0 else f"ps2_{n}(c, {rt}, {a});"
    if n == "sb":
        return f"wr8({a}, (u8){u32(rt)});"
    if n == "sh":
        return f"wr16({a}, (u16){u32(rt)});"
    if n == "sw":
        return f"wr32({a}, {u32(rt)});"
    if n == "sd":
        return f"wr64({a}, {u64(rt)});"
    if n == "sq":
        return f"wr128({a} & ~15u, {'(u128)0' if rt == 0 else f'c->r[{rt}].q'});"
    if n in ("swl", "swr", "sdl", "sdr"):
        return f"ps2_{n}(c, {rt}, {a});"
    if n == "lwc1":
        return f"c->f[{i.ft}].u = rd32({a});"
    if n == "swc1":
        return f"wr32({a}, c->f[{i.ft}].u);"
    if n == "lqc2":
        return f"vu0_lqc2({i.ft}, rd128({a} & ~15u));"
    if n == "sqc2":
        return f"wr128({a} & ~15u, vu0_sqc2({i.ft}));"
    # --- COP0
    if n == "mfc0":
        return set32(rt, f"ps2_mfc0(c, {rd})")
    if n == "mtc0":
        return f"ps2_mtc0(c, {rd}, {u32(rt)});"
    if n == "ei":
        return "ps2_ei(c);"
    if n == "di":
        return "ps2_di(c);"
    # --- COP1
    if n == "mfc1":
        return set32(rt, f"c->f[{i.fs}].u")
    if n == "mtc1":
        return f"c->f[{i.fs}].u = {u32(rt)};"
    if n == "cfc1":
        return set32(rt, f"ps2_cfc1(c, {i.fs})")
    if n == "ctc1":
        return f"ps2_ctc1(c, {i.fs}, {u32(rt)});"
    fs, ft, fd = i.fs, i.ft, i.fd
    F = "c->f[{}].f"
    if n in FPU_BINARY:
        return f"{F.format(fd)} = {FPU_BINARY[n]}({F.format(fs)}, {F.format(ft)});"
    if n == "sqrt.s":
        return f"{F.format(fd)} = fpu_sqrt({F.format(ft)});"
    if n == "rsqrt.s":
        return f"{F.format(fd)} = fpu_rsqrt({F.format(fs)}, {F.format(ft)});"
    if n == "abs.s":
        return f"c->f[{fd}].u = c->f[{fs}].u & 0x7FFFFFFFu;"
    if n == "neg.s":
        return f"c->f[{fd}].u = c->f[{fs}].u ^ 0x80000000u;"
    if n == "mov.s":
        return f"c->f[{fd}].u = c->f[{fs}].u;"
    if n == "adda.s":
        return f"c->acc.f = fpu_add({F.format(fs)}, {F.format(ft)});"
    if n == "suba.s":
        return f"c->acc.f = fpu_sub({F.format(fs)}, {F.format(ft)});"
    if n == "mula.s":
        return f"c->acc.f = fpu_mul({F.format(fs)}, {F.format(ft)});"
    if n == "madd.s":
        return f"{F.format(fd)} = fpu_add(c->acc.f, fpu_mul({F.format(fs)}, {F.format(ft)}));"
    if n == "msub.s":
        return f"{F.format(fd)} = fpu_sub(c->acc.f, fpu_mul({F.format(fs)}, {F.format(ft)}));"
    if n == "madda.s":
        return f"c->acc.f = fpu_add(c->acc.f, fpu_mul({F.format(fs)}, {F.format(ft)}));"
    if n == "msuba.s":
        return f"c->acc.f = fpu_sub(c->acc.f, fpu_mul({F.format(fs)}, {F.format(ft)}));"
    if n == "cvt.w.s":
        return f"c->f[{fd}].s = fpu_cvtw({F.format(fs)});"
    if n == "cvt.s.w":
        return f"{F.format(fd)} = (f32)c->f[{fs}].s;"
    if n == "max.s":
        return f"{F.format(fd)} = fpu_max({F.format(fs)}, {F.format(ft)});"
    if n == "min.s":
        return f"{F.format(fd)} = fpu_min({F.format(fs)}, {F.format(ft)});"
    if n == "c.f.s":
        return "c->fcr31 &= ~0x00800000u;"
    if n in ("c.eq.s", "c.lt.s", "c.le.s"):
        op = {"c.eq.s": "==", "c.lt.s": "<", "c.le.s": "<="}[n]
        return (f"if ({F.format(fs)} {op} {F.format(ft)}) c->fcr31 |= 0x00800000u; "
                f"else c->fcr31 &= ~0x00800000u;")
    # --- COP2
    if n == "qmfc2":
        return "" if rt == 0 else f"c->r[{rt}].q = vu0_sqc2({i.fs});"
    if n == "qmtc2":
        return f"vu0_lqc2({i.fs}, c->r[{rt}].q);"
    if n == "cfc2":
        return set32(rt, f"vu0_cfc2({i.fs})")
    if n == "ctc2":
        return f"vu0_ctc2({i.fs}, {u32(rt)});"
    if n.startswith("v"):
        return emit_vu(i)
    # --- MMI
    if n.startswith("p") or n in ("qfsrv", "plzcw"):
        if rd == 0 and n not in ("pmthi", "pmtlo", "pmthl", "pmaddw", "pmsubw", "pmadduw", "pmultw", "pmultuw",
                                 "pdivw", "pdivuw", "pdivbw", "pmaddh", "pmsubh", "phmadh", "phmsbh", "pmulth"):
            return ""
        if n in ("psllh", "psrlh", "psrah", "psllw", "psrlw", "psraw", "pmfhl", "pmthl"):
            return f"mmi_{n}(c, {rd}, {rt}, {rs}, {sa});"
        return f"mmi_{n}(c, {rd}, {rs}, {rt});"
    raise ValueError(f"no C for {i!r}")


def vec(i, kind):
    """The second operand of a VU FMAC instruction: ft, ft's broadcast component, Q or I."""
    if kind == "bc":
        return f"VBC({i.ft}, {i.bc})"
    if kind == "q":
        return "VQ()"
    if kind == "i":
        return "VI_()"
    return f"VF({i.ft})"


def emit_vu(i):
    n = i.name
    d = i.dest
    # The FMAC family: name = op [a] [bc|q|i]
    for op in ("madd", "msub", "add", "sub", "mul", "max", "mini"):
        for acc in ("a", ""):
            for kind, suffix in (("bc", "bc"), ("q", "q"), ("i", "i"), ("", "")):
                if n == f"v{op}{acc}{suffix}":
                    if op in ("max", "mini") and acc:
                        continue
                    dst = "-1" if acc else str(i.fd)
                    return f"vu0_{op}({d}, {dst}, VF({i.fs}), {vec(i, kind)});"
    if n == "vopmula":
        return f"vu0_opmula(VF({i.fs}), VF({i.ft}));"
    if n == "vopmsub":
        return f"vu0_opmsub({i.fd}, VF({i.fs}), VF({i.ft}));"
    if n.startswith("vftoi"):
        return f"vu0_ftoi({d}, {i.ft}, VF({i.fs}), {n[5:]});"
    if n.startswith("vitof"):
        return f"vu0_itof({d}, {i.ft}, VF({i.fs}), {n[5:]});"
    if n == "vabs":
        return f"vu0_abs({d}, {i.ft}, VF({i.fs}));"
    if n == "vmove":
        return f"vu0_move({d}, {i.ft}, VF({i.fs}));"
    if n == "vmr32":
        return f"vu0_mr32({d}, {i.ft}, VF({i.fs}));"
    if n == "vclipw":
        return f"vu0_clip(VF({i.fs}), VF({i.ft}));"
    if n in ("vnop", "vwaitq"):
        return ""
    if n == "vdiv":
        return f"vu0_div(VF({i.fs}).f[{i.fsf}], VF({i.ft}).f[{i.ftf}]);"
    if n == "vsqrt":
        return f"vu0_sqrt(VF({i.ft}).f[{i.ftf}]);"
    if n == "vrsqrt":
        return f"vu0_rsqrt(VF({i.fs}).f[{i.fsf}], VF({i.ft}).f[{i.ftf}]);"
    if n == "vmtir":
        return f"vu0_seti({i.ft}, VF({i.fs}).u[{i.fsf}]);"
    if n == "vmfir":
        return f"vu0_mfir({d}, {i.ft}, {i.fs});"
    if n == "viadd":
        return f"vu0_seti({i.fd}, VI({i.fs}) + VI({i.ft}));"
    if n == "visub":
        return f"vu0_seti({i.fd}, VI({i.fs}) - VI({i.ft}));"
    if n == "viaddi":
        return f"vu0_seti({i.ft}, VI({i.fs}) + ({i.imm}));"
    if n == "viand":
        return f"vu0_seti({i.fd}, VI({i.fs}) & VI({i.ft}));"
    if n == "vior":
        return f"vu0_seti({i.fd}, VI({i.fs}) | VI({i.ft}));"
    if n == "vlqi":
        return f"vu0_lqi({d}, {i.ft}, {i.fs}, 1);"
    if n == "vlqd":
        return f"vu0_lqi({d}, {i.ft}, {i.fs}, -1);"
    if n == "vsqi":
        return f"vu0_sqi({d}, {i.fs}, {i.ft}, 1);"
    if n == "vsqd":
        return f"vu0_sqi({d}, {i.fs}, {i.ft}, -1);"
    if n == "vilwr":
        return f"vu0_ilwr({d}, {i.ft}, {i.fs});"
    if n == "viswr":
        return f"vu0_iswr({d}, {i.ft}, {i.fs});"
    if n == "vcallms":
        return f"vu0_callms({i.uimm * 8});"
    if n == "vcallmsr":
        return "vu0_callms(ps2_vu0_cmsar0 * 8);"
    if n in ("vrnext", "vrget", "vrinit", "vrxor"):
        return f"vu0_{n[1:]}({d}, {i.ft}, VF({i.fs}).u[{i.fsf}]);"
    raise ValueError(f"no C for {i!r}")


def branch_condition(i):
    n = i.name[:-1] if i.kind == "branch_likely" else i.name
    if n == "beq":
        return f"{u64(i.rs)} == {u64(i.rt)}"
    if n == "bne":
        return f"{u64(i.rs)} != {u64(i.rt)}"
    if n == "blez":
        return f"{s64(i.rs)} <= 0"
    if n == "bgtz":
        return f"{s64(i.rs)} > 0"
    if n in ("bltz", "bltzal"):
        return f"{s64(i.rs)} < 0"
    if n in ("bgez", "bgezal"):
        return f"{s64(i.rs)} >= 0"
    if n == "bc1f":
        return "!(c->fcr31 & 0x00800000u)"
    if n == "bc1t":
        return "(c->fcr31 & 0x00800000u)"
    if n == "bc0f":
        return "!ps2_cpcond0(c)"
    if n == "bc0t":
        return "ps2_cpcond0(c)"
    if n == "bc2f":
        return "!vu0_busy()"
    if n == "bc2t":
        return "vu0_busy()"
    raise ValueError(f"no condition for {i!r}")


class FunctionEmitter:
    def __init__(self, program, start, end, names, overrides, setjmps=frozenset(), encrypted=False):
        self.p = program
        self.start = start
        self.end = end
        self.names = names            # address -> C name of every function (overrides already applied)
        self.overrides = overrides
        self.setjmps = setjmps        # the game's setjmp functions (the config's)
        # Code that decrypts parts of itself as it runs (an overlay the config marks): a word that isn't an instruction
        # stops the game if it's reached, instead of the recompile
        self.encrypted = encrypted

    def plain(self, i):
        if i.name == "unknown" and self.encrypted:
            return f"ps2_undecodable(c, 0x{i.addr:08x}u, 0x{i.word:08x}u);"
        return emit_plain(i, None)

    def inside(self, a):
        return self.start <= a < self.end

    def call_expr(self, target):
        name = self.names.get(target)
        if name:
            return f"{name}(c);"
        return f"ps2_call(c, 0x{target:08x}u);"

    def emit(self):
        p = self.p
        reached = sorted(p.reachable(self.start, self.end))
        reached_set = set(reached)
        labels = set()
        backward = set()
        delay_slots = set()
        for a in reached:
            i = p.insn(a)
            if i.kind in ("branch", "branch_likely", "jump", "call", "jr", "jalr"):
                delay_slots.add(a + 4)
            if i.kind in ("branch", "branch_likely", "jump") and self.inside(i.target):
                labels.add(i.target)
                if i.target <= a:
                    backward.add(i.target)
            if i.kind == "jr" and a in p.jump_tables:
                for t in p.jump_tables[a]:
                    if self.inside(t):
                        labels.add(t)
        # Labels on delay slots are reached through out-of-line copies
        slot_labels = labels & delay_slots
        for a in slot_labels:
            labels.add(a + 4)
        lines = [f"void f_{self.start:08x}(Ps2Cpu* c)", "{"]
        a_prev = None
        for a in reached:
            if a in delay_slots:
                continue
            i = p.insn(a)
            if a in labels and a not in slot_labels:
                lines.append(f"L_{a:08x}:;")
                if a in backward:
                    lines.append("    PS2_POLL(c);")
            lines.append(f"    // {a:08x}")
            lines.extend("    " + x for x in self.emit_insn(i, reached_set) if x)
            a_prev = a
        # Falling off the end of the region: on into the next function
        if reached:
            flow = [x for x in reached if x not in delay_slots]
            if not self.ends_flow(p.insn(flow[-1])):
                lines.append(f"    {self.call_expr(reached[-1] + 4)} return;")
        for a in sorted(slot_labels):
            i = p.insn(a)
            lines.append(f"L_{a:08x}:;")
            lines.extend("    " + x for x in [self.plain(i)] if x)
            lines.append(f"    goto L_{a + 4:08x};")
        lines.append("}")
        return "\n".join(lines)

    @staticmethod
    def ends_flow(i):
        if i.kind in ("jump", "jr", "eret"):
            return True
        if i.kind in ("branch", "branch_likely") and not i.link:
            from analysis import is_unconditional
            return is_unconditional(i)
        return False

    def delay(self, i):
        d = self.p.insn(i.addr + 4)
        if d.kind:
            return [f"/* branch in a delay slot at {d.addr:08x} ignored */"]
        return [self.plain(d)]

    def goto_or_tail(self, target):
        if self.inside(target):
            return f"goto L_{target:08x};"
        return f"{{ {self.call_expr(target)} return; }}"

    def emit_insn(self, i, reached):
        k = i.kind
        if not k:
            return [self.plain(i)]
        if k in ("branch", "branch_likely"):
            from analysis import is_unconditional
            out = []
            link = ""
            if i.link:
                link = f"c->r[31].sd[0] = (s32)0x{i.addr + 8:08x}u;"
            if is_unconditional(i) and not i.link:
                out += self.delay(i)
                out.append(self.goto_or_tail(i.target))
                return out
            cond = branch_condition(i)
            if i.link:
                # A call (bal and friends): to a function, or a jump within this one with ra set
                if self.inside(i.target):
                    taken = f"goto L_{i.target:08x};"
                else:
                    taken = self.call_expr(i.target)
                if k == "branch":
                    return [f"{{ int t = {cond};"] + ["  " + x for x in self.delay(i)] + \
                           [f"  {link} if (t) {{ {taken} }} }}"]
                return [f"if ({cond}) {{"] + ["  " + x for x in self.delay(i)] + [f"  {link} {taken} }}"]
            if k == "branch":
                return [f"{{ int t = {cond};"] + ["  " + x for x in self.delay(i)] + \
                       [f"  if (t) {self.goto_or_tail(i.target)} }}"]
            return [f"if ({cond}) {{"] + ["  " + x for x in self.delay(i)] + [f"  {self.goto_or_tail(i.target)} }}"]
        if k == "jump":
            return self.delay(i) + [self.goto_or_tail(i.target)]
        if k == "call":
            call = self.call_expr(i.target)
            if i.target in self.setjmps:
                # The host's place is kept here, in the caller's frame, for longjmp to come back to (ps2_longjmp); the game's
                # setjmp keeps the registers
                call = f"if (_setjmp(*ps2_setjmp_buffer(c, {u32(4)})) == 0) {call}"
            return self.delay(i) + [f"c->r[31].sd[0] = (s32)0x{i.addr + 8:08x}u;", call]
        if k == "jr":
            if i.rs == 31:
                return self.delay(i) + ["return;"]
            out = [f"{{ u32 t = {u32(i.rs)};"] + ["  " + x for x in self.delay(i)]
            table = self.p.jump_tables.get(i.addr)
            if table:
                out.append("  switch (t) {")
                for tgt in sorted(set(table)):
                    if self.inside(tgt):
                        out.append(f"  case 0x{tgt:08x}u: goto L_{tgt:08x};")
                out.append("  }")
            out.append(f"  ps2_call(c, t); return; }}")
            return out
        if k == "jalr":
            out = [f"{{ u32 t = {u32(i.rs)};"] + ["  " + x for x in self.delay(i)]
            if i.rd:
                out.append(f"  c->r[{i.rd}].sd[0] = (s32)0x{i.addr + 8:08x}u;")
            out.append("  ps2_call(c, t); }")
            return out
        if k == "syscall":
            return [self.plain(i)]
        if k == "eret":
            # Back from kernel mode to the address in ErrorEPC or EPC: a game's own kernel-mode code (CTTR's libkernel puts its
            # return address in ErrorEPC to write a timer with ERL set) goes back to its caller as JR RA would, or on to a
            # function start
            return [f"{{ u32 t = ps2_eret(c, 0x{i.addr:08x}u);",
                    f"  if (t == {u32(31)}) return;",
                    "  ps2_call(c, t); return; }"]
        raise ValueError(f"unhandled {i!r}")
