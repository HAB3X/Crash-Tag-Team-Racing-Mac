"""Finding the functions of a stripped executable.

Starts come from: the entry point, every JAL target, every J target outside the function it's in, the config's extra starts,
and words in the data sections that point at code no function reaches (function pointers). The text is cut at the starts; each
piece's control flow is followed from its start, and code it never reaches (after padding) starts a new function. That repeats
until nothing changes. Jump tables (`lui/addu/lw/jr` switches) are found by their pattern and give a `jr`'s targets.
"""

import struct

from r5900 import decode


class Program:
    def __init__(self, code, config):
        """code: the CodeImage (image.py) to analyse; config: its extra and not starts"""
        self.code = code
        self.config = config
        self.text_start = code.text_start
        self.text_end = code.text_end
        self.words = code.text_words()
        self.insns = [decode(w, self.text_start + 4 * n) for n, w in enumerate(self.words)]
        self.image_base, self.image = code.image_base, code.image
        self.jump_tables = {}   # jr address -> list of targets
        self.computed_starts = set()
        self.decode_cache = {}
        self.functions = []     # sorted (start, end)

    # --- helpers
    def in_text(self, addr):
        return self.text_start <= addr < self.text_end and addr % 4 == 0 and not self.code.in_hole(addr)

    def insn(self, addr):
        return self.insns[(addr - self.text_start) // 4]

    def read32(self, addr):
        off = addr - self.image_base
        if 0 <= off <= len(self.image) - 4:
            return struct.unpack_from("<I", self.image, off)[0]
        return None

    def data_pointers(self):
        """Words in the data that point into the text."""
        out = set()
        for start, end in self.code.data_ranges:
            off = start - self.image_base
            for k in range(off, off + (end - start) - 3, 4):
                w = struct.unpack_from("<I", self.image, k)[0]
                if self.in_text(w):
                    out.add(w)
        return out

    def code_pointers(self):
        """Addresses in the text that code builds with LUI and ADDIU or ORI on the same register (a function's address handed
        on: a thread's entry, a callback)."""
        out = set()
        for n, i in enumerate(self.insns):
            if i.name != "lui" or i.rt == 0:
                continue
            for j in self.insns[n + 1:n + 12]:
                if j.name in ("addiu", "daddiu", "ori") and j.rs == i.rt:
                    low = j.uimm if j.name == "ori" else j.imm
                    v = ((i.uimm << 16) + low) & 0xFFFFFFFF
                    if self.in_text(v) and v % 4 == 0:
                        out.add(v)
                    break
                if writes_gpr(j) == i.rt:
                    break
        return out

    def runs_into(self, a):
        """Whether the code before a runs on into it: a's not where a function can begin. A call's return lands there too,
        unless the call doesn't return (exit, an assert's stop) and a function's prologue follows; padding (zero words) and
        the end of a function (a jump, JR, an unconditional branch, ERET) don't."""
        if not self.in_text(a - 8):
            return False
        before = self.insn(a - 8)
        if before.kind in ("branch", "branch_likely", "jump", "call", "jr", "jalr"):
            if before.kind in ("jump", "jr") or (before.kind == "branch" and not before.link and is_unconditional(before)):
                return False
            if before.link:
                i = self.insn(a)
                return not (i.name == "addiu" and i.rt == 29 and i.rs == 29 and i.imm < 0)
            return True
        last = self.insn(a - 4)
        return last.word != 0 and last.kind != "eret"

    # --- jump tables
    def find_jump_table(self, jr_addr, lo, hi):
        """The targets of a `jr reg` that ends a switch: lw reg, %lo(T)(base) where base = lui %hi(T) [+ index]."""
        jr = self.insn(jr_addr)
        reg = jr.rs
        load = None
        a = jr_addr - 4
        # The load that defines the register, a few instructions back
        while a >= lo and a >= jr_addr - 40:
            i = self.insn(a)
            if i.name == "lw" and i.rt == reg:
                load = i
                break
            if writes_gpr(i) == reg:
                return None
            a -= 4
        if load is None:
            return None
        regs = {load.rs}
        offset = load.imm
        hi16 = None
        bound = None
        a2 = load.addr - 4
        while a2 >= lo and a2 >= load.addr - 64:
            i = self.insn(a2)
            if i.name == "lui" and i.rt in regs:
                hi16 = i.uimm
                break
            if i.name in ("addu", "daddu") and i.rd in regs:
                regs.discard(i.rd)
                regs |= {i.rs, i.rt}
            elif i.name in ("addiu", "daddiu") and i.rt in regs:
                regs.discard(i.rt)
                regs.add(i.rs)
                offset += i.imm
            a2 -= 4
        if hi16 is None:
            return None
        # The bound: sltiu x, idx, N before it
        a3 = a2
        while a3 >= lo and a3 >= a2 - 48:
            i = self.insn(a3)
            if i.name == "sltiu":
                bound = i.uimm
                break
            a3 -= 4
        table = ((hi16 << 16) + offset) & 0xFFFFFFFF
        targets = []
        n = 0
        while True:
            if bound is not None and n >= bound:
                break
            if bound is None and n >= 512:
                break
            w = self.read32(table + 4 * n)
            if w is None or not (lo <= w < hi) or w % 4:
                break
            targets.append(w)
            n += 1
        return targets or None

    def find_computed_jump(self, jr_addr):
        """A `jr reg` into code: reg = lui/addi base + an index (hand-written asm's unrolled loops, entered part way). The
        targets are every instruction from the base to the first `jr ra` after it."""
        jr = self.insn(jr_addr)
        regs = {jr.rs}
        offset = 0
        hi16 = None
        a = jr_addr - 4
        while a >= jr_addr - 48 and a >= self.text_start:
            i = self.insn(a)
            if i.name in ("lw", "ld") and i.rt in regs:
                return None
            if i.name == "lui" and i.rt in regs:
                hi16 = i.uimm
                break
            if i.name in ("add", "addu", "daddu", "dadd") and i.rd in regs:
                regs.discard(i.rd)
                regs |= {i.rs, i.rt}
            elif i.name in ("addi", "addiu", "daddiu") and i.rt in regs:
                if i.rs != i.rt:
                    regs.discard(i.rt)
                    regs.add(i.rs)
                offset += i.imm
            a -= 4
        if hi16 is None:
            return None
        base = ((hi16 << 16) + offset) & 0xFFFFFFFF
        if not self.in_text(base):
            return None
        targets = []
        if base < jr_addr:
            # Blocks before the jump (each its own small routine): every instruction there may be entered, so each is a
            # function start, reached by an indirect tail call
            self.computed_starts |= set(range(base, jr_addr, 4))
            return None
        t = base
        while self.in_text(t) and len(targets) < 1024:
            targets.append(t)
            i = self.insn(t)
            if i.kind == "jr" and i.rs == 31:
                break
            t += 4
        return targets

    def after_padding(self, a):
        """The first code after the zero words at a (up to 16), or None when a isn't padding or nothing follows it"""
        if self.words[(a - self.text_start) // 4] != 0:
            return None
        for b in range(a + 4, min(a + 68, self.text_end), 4):
            if self.in_text(b) and self.words[(b - self.text_start) // 4] != 0:
                return b
        return None

    def decodes(self, a):
        """Whether everything that would run from a decodes: if not, a is data kept in the text, not a function. Followed to
        the end of the text, not of a region, as how far a region goes depends on the starts found so far."""
        if a not in self.decode_cache:
            self.decode_cache[a] = all(self.insn(x).name != "unknown" for x in self.reachable(a, self.text_end))
        return self.decode_cache[a]

    # --- control flow within one region
    def reachable(self, start, end):
        seen = set()
        work = [start]
        while work:
            a = work.pop()
            while start <= a < end and a not in seen:
                i = self.insn(a)
                seen.add(a)
                k = i.kind
                if k in ("branch", "branch_likely"):
                    seen.add(a + 4)
                    if start <= i.target < end:
                        work.append(i.target)
                    if not i.link and is_unconditional(i):
                        break
                    a += 8
                    continue
                if k == "jump":
                    seen.add(a + 4)
                    if start <= i.target < end:
                        work.append(i.target)
                    break
                if k == "jr":
                    seen.add(a + 4)
                    if i.rs != 31:
                        t = self.jump_tables.get(a)
                        if t:
                            work.extend(x for x in t if start <= x < end)
                    break
                if k == "eret":
                    break  # a jump to EPC or ErrorEPC, with no delay slot
                if k in ("call", "jalr"):
                    seen.add(a + 4)
                    a += 8
                    continue
                a += 4
        return seen

    def region(self, s, e):
        """The code reached from a start within its region, and the new starts it shows"""
        seen = self.reachable(s, e)
        found = set()
        # Jumps and branches leaving the region start functions
        for a in seen:
            i = self.insn(a)
            if i.kind in ("jump", "branch", "branch_likely") and not i.link:
                if self.in_text(i.target) and not (s <= i.target < e):
                    found.add(i.target)
        # Flow running past the region's end (after a call or branch whose delay slot is the last instruction, or into the
        # next function) continues at a function start
        for a in seen:
            i = self.insn(a)
            if i.kind in ("jump", "jr", "eret") or (i.kind in ("branch", "branch_likely") and not i.link and is_unconditional(i)):
                continue
            nxt = a + 8 if i.kind else a + 4
            if (a - 4) in seen and self.insn(a - 4).kind:
                continue  # a delay slot: its branch decides
            if nxt >= e and self.in_text(nxt):
                found.add(nxt)
        # Code after the reached part that nothing reaches starts a function (padding skipped). Only after it: an unreached
        # word between reached ones is dead code the compiler left inside the function (Biohazard Outbreak File 1's demo
        # overlay has one in the middle of a switch, at 0x540550), and a start there cut the function off from its later
        # cases. Words that don't decode, in what would run from there, make it data kept in the text (Biohazard Outbreak's
        # libkernel keeps its syscall patch tables between its functions): no function, and the search goes on past it
        a = max(seen) + 4 if seen else s
        skipped = set()
        while a < e:
            if a not in seen and a not in skipped and self.words[(a - self.text_start) // 4] != 0:
                if self.decodes(a):
                    found.add(a)
                    break
                skipped |= self.reachable(a, self.text_end)
            a += 4
        return seen, found

    def analyse(self):
        starts = {self.code.entry} if self.code.entry is not None else set()
        # The text's first code (after any zero padding) begins a function: an overlay has no entry, and nothing in it may
        # point at its first function (Biohazard Outbreak's overlays begin with 0x40 zero bytes, then their entry, which the
        # game calls through a pointer of its own)
        first = next((n for n, w in enumerate(self.words) if w != 0), None)
        if first is not None:
            starts.add(self.text_start + 4 * first)
        starts |= set(self.config.get("extra_functions", []))
        not_starts = set(self.config.get("not_functions", []))
        for i in self.insns:
            if i.kind == "call" and self.in_text(i.target):
                starts.add(i.target)
            if i.kind == "branch" and i.link and self.in_text(i.target):
                pass
        # Jump tables first, over the whole text: their targets are labels, never function starts
        for i in self.insns:
            if i.kind == "jr" and i.rs != 31:
                t = self.find_jump_table(i.addr, max(self.text_start, i.addr - 64), self.text_end)
                if not t:
                    t = self.find_computed_jump(i.addr)
                if t:
                    self.jump_tables[i.addr] = t
        jt_targets = set()
        for t in self.jump_tables.values():
            jt_targets |= set(t)
        # A jump table's targets are labels, unless the config names one a function start: a game's handler tables are
        # jumped through (JR) as well as called, and their entries are whole functions
        not_starts |= jt_targets - set(self.config.get("extra_functions", []))
        starts |= self.computed_starts
        pointers = self.data_pointers() - jt_targets
        # A switch's cases are one function's labels: a data word that points between them (a number in a table that looks
        # like an address) isn't a function start unless something calls it. Split there, the function would lose the cases
        # past the split (WoC's menu drawing: a sound table's 0x1E7F00 in the middle of a 35 case switch)
        called = {i.target for i in self.insns if i.kind == "call"}
        for jr, targets in self.jump_tables.items():
            low, high = min(min(targets), jr), max(targets)
            pointers -= {p for p in pointers if low < p <= high and p not in called}
        # A number in the data that lands where code runs on into it (the middle of a function) isn't a function pointer:
        # compiled functions begin after the end of the one before. CTTR's GS register tables hold words like 0x00350800
        # that cut a switch off from its own jump table's cases
        pointers = {p for p in pointers if p in called or not self.runs_into(p)}
        # A function's address built in code is a start too, where one can begin (CTTR's 0x3B8B10, a thread's entry, comes
        # after two unreached instructions that would otherwise run into it)
        pointers |= {p for p in self.code_pointers() - jt_targets if not self.runs_into(p)}
        # A pointer to words that don't decode points at a table kept in the text (code builds its address too)
        pointers = {p for p in pointers if p in called or self.decodes(p)}
        starts = {s for s in starts if self.in_text(s)} - not_starts
        starts |= pointers
        # A start on zero padding (a pointer or a config's start one word early) runs on into the code after the padding;
        # that code begins a function of its own too, as anything calling it calls it there (Biohazard Outbreak File 1's
        # 0x1C0120, an item action's handler, was left inside the start at its padding 0x1C0118)
        starts |= {c for c in (self.after_padding(s) for s in starts) if c is not None} - not_starts
        cache = {}
        for rnd in range(50):
            ordered = sorted(starts)
            new = set()
            all_reached = set()
            for n, s in enumerate(ordered):
                e = ordered[n + 1] if n + 1 < len(ordered) else self.text_end
                if (s, e) not in cache:
                    cache[(s, e)] = self.region(s, e)
                seen, found = cache[(s, e)]
                all_reached |= seen
                new |= found
            # Data pointers into the code (function pointers): every one starts a function, even where other code runs into
            # it (that code then calls it as a tail call)
            new |= pointers
            new -= not_starts
            new -= starts
            if not new:
                break
            starts |= new
        ordered = sorted(starts)
        self.functions = []
        for n, s in enumerate(ordered):
            e = ordered[n + 1] if n + 1 < len(ordered) else self.text_end
            self.functions.append((s, e))
        return self.functions


def is_unconditional(i):
    if i.name in ("beq", "beql") and i.rs == i.rt:
        return True
    if i.name in ("bgez", "bgezl", "blez", "blezl") and i.rs == 0:
        return True
    return False


def writes_gpr(i):
    """The GPR an instruction writes (0 for none), for the jump table's backward scan."""
    n = i.name
    if n in ("lw", "lb", "lbu", "lh", "lhu", "lwu", "ld", "lq", "lui", "addiu", "addi", "ori", "andi", "xori",
             "slti", "sltiu", "daddiu", "lwl", "lwr", "ldl", "ldr", "mfc1", "cfc1", "qmfc2", "cfc2", "mfc0"):
        return i.rt
    if n in ("addu", "add", "subu", "sub", "daddu", "dsubu", "and", "or", "xor", "nor", "slt", "sltu", "sll", "srl",
             "sra", "sllv", "srlv", "srav", "dsll", "dsrl", "dsra", "dsll32", "dsrl32", "dsra32", "mfhi", "mflo",
             "movz", "movn", "jalr"):
        return i.rd
    return 0
