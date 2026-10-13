"""The VU's micro instructions decoded the way src/platform/native/graphics/ee/vu.cpp's interpreter decodes them (Vu::Decode):
the same classes, operand fields, read and write sets, pipelines and latencies. vu1_translate.py builds its C++ from these, so
the two must agree; the translation's checks (native/GRAPHICS.md, "VU1 translation") compare them instruction by instruction.
"""

from dataclasses import dataclass, field

FLAG_STATUS = 2
FLAG_CLIP = 4


def dest_mask(word):
    # x is bit 0 (the word's bit 24), w bit 3 (bit 21)
    return ((word >> 24) & 1) | ((word >> 23) & 1) << 1 | ((word >> 22) & 1) << 2 | ((word >> 21) & 1) << 3


def imm11(word):
    return (word | 0xFFFFF800) - (1 << 32) if word & 0x400 else word & 0x3FF


@dataclass
class Instr:
    address: int
    lower_word: int
    upper_word: int
    ibit: bool = False
    ebit: bool = False
    mbit: bool = False
    dbit: bool = False
    tbit: bool = False
    # upper
    upper: str = 'nop'      # nop arith itof ftoi abs clip opmula opmsub max mini unknown
    op: str = ''            # add sub mul madd msub
    source: str = ''        # vector broadcast i q
    to_acc: bool = False
    bc: int = 0
    shift: int = 0
    dest: int = 0
    fd: int = 0
    fs: int = 0
    ft: int = 0
    # lower
    lower: str = 'nop'
    pipe: str = 'none'      # none fmac fdiv efu ialu branch
    ldest: int = 0
    lfs: int = 0
    lft: int = 0
    lfd: int = 0
    fsf: int = 0
    ftf: int = 0
    latency: int = 0
    # reads and writes
    upper_write_vf: int = 0
    upper_write_mask: int = 0
    upper_read0: int = 0
    upper_read0_mask: int = 0
    upper_read1: int = 0
    upper_read1_mask: int = 0
    lower_write_vf: int = 0
    lower_write_mask: int = 0
    lower_read0: int = 0
    lower_read0_mask: int = 0
    lower_read1: int = 0
    lower_read1_mask: int = 0
    lower_write_vi: int = 0
    lower_read_vi: int = 0
    upper_flags: int = 0
    lower_flags: int = 0

    @property
    def is_branch(self):
        return not self.ibit and self.pipe == 'branch'

    @property
    def upper_fmac(self):
        return self.upper not in ('nop', 'unknown')

    def text(self):
        return disassemble(self)


_LOWER_TABLE = [
    [None] * 12 + ['move', 'lqi', 'div', 'mtir', 'rnext'] + [None] * 8 + ['mfp', 'xtop', 'xgkick', 'esadd', 'eatanxy', 'esqrt', 'esin'],
    [None] * 12 + ['mr32', 'sqi', 'sqrt', 'mfir', 'rget'] + [None] * 8 + [None, 'xitop', None, 'ersadd', 'eatanxz', 'ersqrt', 'eatan'],
    [None] * 12 + [None, 'lqd', 'rsqrt', 'ilwr', 'rinit'] + [None] * 8 + [None, None, None, 'eleng', 'esum', 'ercpr', 'eexp'],
    [None] * 12 + [None, 'sqd', 'waitq', 'iswr', 'rxor'] + [None] * 8 + [None, None, None, 'erleng', None, 'waitp', None],
]

_LOWER_OP7 = {
    0x00: 'lq', 0x01: 'sq', 0x04: 'ilw', 0x05: 'isw', 0x08: 'iaddiu', 0x09: 'isubiu', 0x10: 'fceq', 0x11: 'fcset',
    0x12: 'fcand', 0x13: 'fcor', 0x14: 'fseq', 0x15: 'fsset', 0x16: 'fsand', 0x17: 'fsor', 0x18: 'fmeq', 0x1A: 'fmand',
    0x1B: 'fmor', 0x1C: 'fcget', 0x20: 'b', 0x21: 'bal', 0x24: 'jr', 0x25: 'jalr', 0x28: 'ibeq', 0x29: 'ibne',
    0x2C: 'ibltz', 0x2D: 'ibgtz', 0x2E: 'iblez', 0x2F: 'ibgez',
}

_EFU_LATENCY = {
    'esadd': 11, 'ersadd': 18, 'eleng': 18, 'ersqrt': 18, 'erleng': 24, 'eatanxy': 54, 'eatanxz': 54, 'eatan': 54,
    'esum': 12, 'ercpr': 12, 'esqrt': 12, 'esin': 29, 'eexp': 44,
}


def decode(address, pair):
    lw = pair & 0xFFFFFFFF
    uw = pair >> 32
    d = Instr(address, lw, uw)
    d.ibit = bool((uw >> 31) & 1)
    d.ebit = bool((uw >> 30) & 1)
    d.mbit = bool((uw >> 29) & 1)
    d.dbit = bool((uw >> 28) & 1)
    d.tbit = bool((uw >> 27) & 1)
    d.dest = dest_mask(uw)
    d.fd = (uw >> 6) & 0x1F
    d.fs = (uw >> 11) & 0x1F
    d.ft = (uw >> 16) & 0x1F
    d.bc = uw & 3

    def arith(op, source, to_acc):
        d.upper = 'arith'
        d.op = op
        d.source = source
        d.to_acc = to_acc

    op6 = uw & 0x3F
    if op6 < 0x3C:
        group = op6 >> 2
        if group == 0:
            arith('add', 'broadcast', False)
        elif group == 1:
            arith('sub', 'broadcast', False)
        elif group == 2:
            arith('madd', 'broadcast', False)
        elif group == 3:
            arith('msub', 'broadcast', False)
        elif group == 4:
            d.upper, d.source = 'max', 'broadcast'
        elif group == 5:
            d.upper, d.source = 'mini', 'broadcast'
        elif group == 6:
            arith('mul', 'broadcast', False)
        else:
            table = {
                0x1C: ('arith', 'mul', 'q'), 0x1D: ('max', '', 'i'), 0x1E: ('arith', 'mul', 'i'), 0x1F: ('mini', '', 'i'),
                0x20: ('arith', 'add', 'q'), 0x21: ('arith', 'madd', 'q'), 0x22: ('arith', 'add', 'i'),
                0x23: ('arith', 'madd', 'i'), 0x24: ('arith', 'sub', 'q'), 0x25: ('arith', 'msub', 'q'),
                0x26: ('arith', 'sub', 'i'), 0x27: ('arith', 'msub', 'i'), 0x28: ('arith', 'add', 'vector'),
                0x29: ('arith', 'madd', 'vector'), 0x2A: ('arith', 'mul', 'vector'), 0x2B: ('max', '', 'vector'),
                0x2C: ('arith', 'sub', 'vector'), 0x2D: ('arith', 'msub', 'vector'), 0x2E: ('opmsub', '', ''),
                0x2F: ('mini', '', 'vector'),
            }
            entry = table.get(op6)
            if entry is None:
                d.upper = 'unknown'
            elif entry[0] == 'arith':
                arith(entry[1], entry[2], False)
            else:
                d.upper = entry[0]
                d.source = entry[2]
    else:
        index = (uw >> 6) & 0x1F
        bc = uw & 3
        shifts = [0, 4, 12, 15]
        if index == 0:
            arith('add', 'broadcast', True)
        elif index == 1:
            arith('sub', 'broadcast', True)
        elif index == 2:
            arith('madd', 'broadcast', True)
        elif index == 3:
            arith('msub', 'broadcast', True)
        elif index == 4:
            d.upper = 'itof'
            d.shift = shifts[bc]
        elif index == 5:
            d.upper = 'ftoi'
            d.shift = shifts[bc]
        elif index == 6:
            arith('mul', 'broadcast', True)
        elif index == 7:
            if bc == 0:
                arith('mul', 'q', True)
            elif bc == 1:
                d.upper = 'abs'
            elif bc == 2:
                arith('mul', 'i', True)
            else:
                d.upper = 'clip'
        elif index == 8:
            arith(['add', 'madd', 'add', 'madd'][bc], ['q', 'q', 'i', 'i'][bc], True)
        elif index == 9:
            arith(['sub', 'msub', 'sub', 'msub'][bc], ['q', 'q', 'i', 'i'][bc], True)
        elif index == 10:
            if bc == 3:
                d.upper = 'unknown'
            else:
                arith(['add', 'madd', 'mul'][bc], 'vector', True)
        elif index == 11:
            if bc == 0:
                arith('sub', 'vector', True)
            elif bc == 1:
                arith('msub', 'vector', True)
            elif bc == 2:
                d.upper = 'opmula'
            else:
                d.upper = 'nop'
        else:
            d.upper = 'unknown'

    if d.upper in ('arith', 'max', 'mini'):
        d.upper_read0, d.upper_read0_mask = d.fs, d.dest
        if d.source == 'vector':
            d.upper_read1, d.upper_read1_mask = d.ft, d.dest
        elif d.source == 'broadcast':
            d.upper_read1, d.upper_read1_mask = d.ft, 1 << d.bc
        if not (d.upper == 'arith' and d.to_acc):
            d.upper_write_vf, d.upper_write_mask = d.fd, d.dest
    elif d.upper in ('itof', 'ftoi', 'abs'):
        d.upper_read0, d.upper_read0_mask = d.fs, d.dest
        d.upper_write_vf, d.upper_write_mask = d.ft, d.dest
    elif d.upper == 'clip':
        d.upper_read0, d.upper_read0_mask = d.fs, 7
        d.upper_read1, d.upper_read1_mask = d.ft, 8
        d.upper_flags = FLAG_CLIP
    elif d.upper in ('opmula', 'opmsub'):
        d.upper_read0, d.upper_read0_mask = d.fs, 7
        d.upper_read1, d.upper_read1_mask = d.ft, 7
        if d.upper == 'opmsub':
            d.upper_write_vf, d.upper_write_mask = d.fd, 7

    if d.ibit:
        d.lower = 'nop'
        d.pipe = 'none'
        if d.upper_write_vf == 0:
            # (the interpreter leaves the mask when the I bit is set; nothing reads it then)
            pass
        return d

    d.ldest = dest_mask(lw)
    d.lfs = (lw >> 11) & 0x1F
    d.lft = (lw >> 16) & 0x1F
    d.lfd = (lw >> 6) & 0x1F
    d.fsf = (lw >> 21) & 3
    d.ftf = (lw >> 23) & 3
    op7 = lw >> 25
    lower = 'unknown'
    if op7 == 0x40:
        f6 = lw & 0x3F
        if f6 == 0x30:
            lower = 'iadd'
        elif f6 == 0x31:
            lower = 'isub'
        elif f6 == 0x32:
            lower = 'iaddi'
        elif f6 == 0x34:
            lower = 'iand'
        elif f6 == 0x35:
            lower = 'ior'
        elif f6 in (0x3C, 0x3D, 0x3E, 0x3F):
            index = (lw >> 6) & 0x1F
            lower = _LOWER_TABLE[lw & 3][index] or 'unknown'
    else:
        lower = _LOWER_OP7.get(op7, 'unknown')
    d.lower = lower

    is_ = d.lfs & 0xF
    it = d.lft & 0xF
    id_ = d.lfd & 0xF
    pipe = 'none'
    if lower in ('lq', 'lqi', 'lqd'):
        pipe = 'fmac'
        d.lower_write_vf, d.lower_write_mask = d.lft, d.ldest
        d.lower_read_vi = 1 << is_
        if lower != 'lq':
            d.lower_write_vi = is_
    elif lower in ('sq', 'sqi', 'sqd'):
        pipe = 'fmac'
        d.lower_read0, d.lower_read0_mask = d.lfs, d.ldest
        d.lower_read_vi = 1 << it
        if lower != 'sq':
            d.lower_write_vi = it
    elif lower in ('move', 'mr32'):
        pipe = 'fmac'
        d.lower_read0 = d.lfs
        d.lower_read0_mask = d.ldest if lower == 'move' else 0xF
        d.lower_write_vf, d.lower_write_mask = d.lft, d.ldest
    elif lower == 'mfir':
        pipe = 'fmac'
        d.lower_write_vf, d.lower_write_mask = d.lft, d.ldest
        d.lower_read_vi = 1 << is_
    elif lower == 'mtir':
        pipe = 'fmac'
        d.lower_read0, d.lower_read0_mask = d.lfs, 1 << d.fsf
        d.lower_write_vi = it
    elif lower in ('rget', 'rnext', 'mfp'):
        pipe = 'fmac'
        d.lower_write_vf, d.lower_write_mask = d.lft, d.ldest
    elif lower in ('rinit', 'rxor'):
        pipe = 'fmac'
        d.lower_read0, d.lower_read0_mask = d.lfs, 1 << d.fsf
    elif lower == 'fcset':
        pipe = 'fmac'
        d.lower_flags = FLAG_CLIP
    elif lower == 'fsset':
        pipe = 'fmac'
        d.lower_flags = FLAG_STATUS
    elif lower in ('div', 'sqrt', 'rsqrt'):
        pipe = 'fdiv'
        d.latency = 13 if lower == 'rsqrt' else 7
        if lower != 'sqrt':
            d.lower_read0, d.lower_read0_mask = d.lfs, 1 << d.fsf
        d.lower_read1, d.lower_read1_mask = d.lft, 1 << d.ftf
    elif lower == 'waitq':
        pipe = 'fdiv'
    elif lower in ('esadd', 'ersadd', 'eleng', 'erleng', 'eatanxy', 'eatanxz', 'esum'):
        pipe = 'efu'
        d.lower_read0, d.lower_read0_mask = d.lfs, 0xF
    elif lower in ('ercpr', 'esqrt', 'ersqrt', 'esin', 'eatan', 'eexp'):
        pipe = 'efu'
        d.lower_read0, d.lower_read0_mask = d.lfs, 1 << d.fsf
    elif lower == 'waitp':
        pipe = 'efu'
    elif lower in ('ilw', 'ilwr'):
        pipe = 'ialu'
        d.latency = 4
        d.lower_write_vi = it
        d.lower_read_vi = 1 << is_
    elif lower in ('isw', 'iswr'):
        pipe = 'ialu'
        d.lower_read_vi = 1 << is_ | 1 << it
    elif lower in ('iadd', 'isub', 'iand', 'ior'):
        pipe = 'ialu'
        d.lower_write_vi = id_
        d.lower_read_vi = 1 << is_ | 1 << it
    elif lower in ('iaddi', 'iaddiu', 'isubiu'):
        pipe = 'ialu'
        d.lower_write_vi = it
        d.lower_read_vi = 1 << is_
    elif lower in ('fseq', 'fsand', 'fsor', 'fcget', 'xtop', 'xitop'):
        d.lower_write_vi = it
    elif lower in ('fmeq', 'fmand', 'fmor'):
        d.lower_write_vi = it
        d.lower_read_vi = 1 << is_
    elif lower in ('fceq', 'fcand', 'fcor'):
        d.lower_write_vi = 1
    elif lower in ('ibeq', 'ibne'):
        pipe = 'branch'
        d.lower_read_vi = 1 << is_ | 1 << it
    elif lower in ('ibltz', 'ibgtz', 'iblez', 'ibgez', 'jr'):
        pipe = 'branch'
        d.lower_read_vi = 1 << is_
    elif lower == 'jalr':
        pipe = 'branch'
        d.lower_read_vi = 1 << is_
        d.lower_write_vi = it
    elif lower == 'bal':
        pipe = 'branch'
        d.lower_write_vi = it
    elif lower == 'b':
        pipe = 'branch'
    elif lower == 'xgkick':
        d.lower_read_vi = 1 << is_

    if lower in _EFU_LATENCY:
        d.latency = _EFU_LATENCY[lower]

    if d.lower_write_vf == 0:
        d.lower_write_mask = 0
    if d.upper_write_vf == 0:
        d.upper_write_mask = 0
    d.pipe = pipe
    return d


def _mask_text(mask):
    return ''.join(c for n, c in enumerate('xyzw') if mask & (1 << n))


def disassemble(d):
    flags = ''.join(f for f, on in (('[I]', d.ibit), ('[E]', d.ebit), ('[M]', d.mbit), ('[D]', d.dbit), ('[T]', d.tbit)) if on)
    m = _mask_text(d.dest)
    bcn = 'xyzw'[d.bc]
    if d.upper == 'arith':
        name = d.op + ('a' if d.to_acc else '')
        name += {'broadcast': bcn, 'i': 'i', 'q': 'q', 'vector': ''}[d.source]
        dst = 'ACC' if d.to_acc else f'vf{d.fd:02}'
        src2 = {'broadcast': f'vf{d.ft:02}{bcn}', 'i': 'I', 'q': 'Q', 'vector': f'vf{d.ft:02}'}[d.source]
        up = f'{name}.{m} {dst}, vf{d.fs:02}, {src2}'
    elif d.upper in ('max', 'mini'):
        name = d.upper + {'broadcast': bcn, 'i': 'i', 'vector': ''}[d.source]
        src2 = {'broadcast': f'vf{d.ft:02}{bcn}', 'i': 'I', 'vector': f'vf{d.ft:02}'}[d.source]
        up = f'{name}.{m} vf{d.fd:02}, vf{d.fs:02}, {src2}'
    elif d.upper in ('itof', 'ftoi'):
        up = f'{d.upper}{d.shift}.{m} vf{d.ft:02}, vf{d.fs:02}'
    elif d.upper == 'abs':
        up = f'abs.{m} vf{d.ft:02}, vf{d.fs:02}'
    elif d.upper == 'clip':
        up = f'clipw.xyz vf{d.fs:02}, vf{d.ft:02}w'
    elif d.upper == 'opmula':
        up = f'opmula.xyz ACC, vf{d.fs:02}, vf{d.ft:02}'
    elif d.upper == 'opmsub':
        up = f'opmsub.xyz vf{d.fd:02}, vf{d.fs:02}, vf{d.ft:02}'
    elif d.upper == 'nop':
        up = 'nop'
    else:
        up = f'??? {d.upper_word:08x}'
    if d.ibit:
        lo = f'loi {d.lower_word:#010x}'
    else:
        lo = d.lower
        lm = _mask_text(d.ldest)
        is_, it, id_ = d.lfs & 15, d.lft & 15, d.lfd & 15
        imm = imm11(d.lower_word)
        if d.lower in ('lq', 'sq'):
            lo = f'lq.{lm} vf{d.lft:02}, {imm}(vi{is_:02})' if d.lower == 'lq' else f'sq.{lm} vf{d.lfs:02}, {imm}(vi{it:02})'
        elif d.lower in ('lqi', 'lqd'):
            lo = f'{d.lower}.{lm} vf{d.lft:02}, (vi{is_:02})'
        elif d.lower in ('sqi', 'sqd'):
            lo = f'{d.lower}.{lm} vf{d.lfs:02}, (vi{it:02})'
        elif d.lower in ('ilw', 'isw'):
            lo = f'{d.lower}.{lm} vi{it:02}, {imm}(vi{is_:02})'
        elif d.lower in ('ilwr', 'iswr'):
            lo = f'{d.lower}.{lm} vi{it:02}, (vi{is_:02})'
        elif d.lower in ('iaddiu', 'isubiu'):
            imm15 = ((d.lower_word >> 10) & 0x7800) | (d.lower_word & 0x7FF)
            lo = f'{d.lower} vi{it:02}, vi{is_:02}, {imm15:#x}'
        elif d.lower == 'iaddi':
            imm5 = (d.lower_word >> 6) & 0x1F
            imm5 = imm5 - 32 if imm5 & 0x10 else imm5
            lo = f'iaddi vi{it:02}, vi{is_:02}, {imm5}'
        elif d.lower in ('iadd', 'isub', 'iand', 'ior'):
            lo = f'{d.lower} vi{id_:02}, vi{is_:02}, vi{it:02}'
        elif d.lower in ('b', 'bal'):
            target = d.address + 1 + imm
            lo = f'{d.lower} {"vi%02d, " % it if d.lower == "bal" else ""}{target:#06x}'
        elif d.lower in ('ibeq', 'ibne'):
            lo = f'{d.lower} vi{it:02}, vi{is_:02}, {d.address + 1 + imm:#06x}'
        elif d.lower in ('ibltz', 'ibgtz', 'iblez', 'ibgez'):
            lo = f'{d.lower} vi{is_:02}, {d.address + 1 + imm:#06x}'
        elif d.lower == 'jr':
            lo = f'jr vi{is_:02}'
        elif d.lower == 'jalr':
            lo = f'jalr vi{it:02}, vi{is_:02}'
        elif d.lower in ('move', 'mr32'):
            lo = f'{d.lower}.{lm} vf{d.lft:02}, vf{d.lfs:02}'
        elif d.lower == 'mfir':
            lo = f'mfir.{lm} vf{d.lft:02}, vi{is_:02}'
        elif d.lower == 'mtir':
            lo = f'mtir vi{it:02}, vf{d.lfs:02}{"xyzw"[d.fsf]}'
        elif d.lower in ('div', 'rsqrt'):
            lo = f'{d.lower} Q, vf{d.lfs:02}{"xyzw"[d.fsf]}, vf{d.lft:02}{"xyzw"[d.ftf]}'
        elif d.lower == 'sqrt':
            lo = f'sqrt Q, vf{d.lft:02}{"xyzw"[d.ftf]}'
        elif d.lower == 'xgkick':
            lo = f'xgkick vi{is_:02}'
        elif d.lower in ('xtop', 'xitop', 'fcget'):
            lo = f'{d.lower} vi{it:02}'
        elif d.lower in ('fcand', 'fcor', 'fceq', 'fcset'):
            lo = f'{d.lower} {"vi01, " if d.lower != "fcset" else ""}{d.lower_word & 0xFFFFFF:#08x}'
        elif d.lower in ('fsand', 'fsor', 'fseq', 'fsset'):
            imm12 = ((d.lower_word >> 21) & 1) << 11 | (d.lower_word & 0x7FF)
            lo = f'{d.lower} vi{it:02}, {imm12:#05x}'
        elif d.lower in ('fmand', 'fmor', 'fmeq'):
            lo = f'{d.lower} vi{it:02}, vi{is_:02}'
        elif d.pipe == 'efu' and d.lower not in ('waitp',):
            lo = f'{d.lower} P, vf{d.lfs:02}' + ('' if d.lower_read0_mask == 0xF else 'xyzw'[d.fsf])
        elif d.lower == 'mfp':
            lo = f'mfp.{lm} vf{d.lft:02}, P'
        elif d.lower == 'unknown':
            lo = f'??? {d.lower_word:08x}'
    return f'{up:36} {lo} {flags}'.rstrip()
