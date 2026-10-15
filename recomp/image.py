"""The code one recompile covers: its text, the loaded bytes around it, its entry point and the ranges holding its data.

An executable's text is its .text section, or the config's ranges when the executable has no section table (Biohazard
Outbreak's are stripped down to their program headers, and its VU microcode and some data sit between two runs of code). A
code overlay (a file the game loads at a fixed address and runs) is the file's bytes at that address, its text range from the
config.

The text is one span from its first range's start to its last range's end; the gaps between ranges are holes: no code is
there (their words read as zero and no function starts in them), and they're searched for pointers as data.
"""

import struct


class CodeImage:
    def __init__(self, base, data, text_ranges, entry, data_ranges):
        """text_ranges: the (start, end) runs of code, in order"""
        for start, end in text_ranges:
            if start % 4 or end % 4 or not base <= start < end <= base + len(data):
                raise ValueError(f"text {start:#x}-{end:#x} isn't inside {base:#x}-{base + len(data):#x}")
        for (_, end), (start, _) in zip(text_ranges, text_ranges[1:]):
            if start < end:
                raise ValueError(f"text ranges out of order or overlapping at {start:#x}")
        self.image_base = base
        self.image = data
        self.text_start = text_ranges[0][0]
        self.text_end = text_ranges[-1][1]
        self.holes = [(end, start) for (_, end), (start, _) in zip(text_ranges, text_ranges[1:]) if end < start]
        self.entry = entry              # None for an overlay: its starts come from its calls and pointers
        self.data_ranges = data_ranges + self.holes  # (start, end) addresses of the words that may point at code

    def in_hole(self, address):
        return any(start <= address < end for start, end in self.holes)

    def text_words(self):
        off = self.text_start - self.image_base
        words = list(struct.unpack_from(f"<{(self.text_end - self.text_start) // 4}I", self.image, off))
        for start, end in self.holes:
            words[(start - self.text_start) // 4:(end - self.text_start) // 4] = [0] * ((end - start) // 4)
        return words

    @classmethod
    def from_elf(cls, elf, text=None, vutext=None):
        """An executable: its .text section, or `text` ([(start, end), ...]) when it has no sections; `vutext` (start, end),
        its VU microcode when it has no sections, isn't searched for pointers."""
        base, data = elf.image()
        section = elf.sections.get(".text")
        if section is not None:
            if text is not None:
                raise ValueError("the executable has a .text section: the config's text range isn't wanted")
            text = [(section["addr"], section["addr"] + section["size"])]
            ranges = [(s["addr"], s["addr"] + s["size"]) for name, s in elf.sections.items()
                      if name not in (".text", ".vutext") and s["type"] == 1 and s["addr"]]
        else:
            if text is None:
                raise ValueError("the executable has no section table: give its code's ranges in the config "
                                 "([executable] text = [[start, end], ...])")
            ranges = [(base, text[0][0]), (text[-1][1], base + len(data))]
        code = cls(base, data, text, elf.entry, ranges)
        if vutext is not None:
            code.data_ranges = [r for d in code.data_ranges for r in subtract(d, vutext)]
        return code

    @classmethod
    def from_overlay(cls, data, base, text):
        """An overlay's file loaded at `base`, its text `text` (start, end); the rest of the file is its data."""
        start, end = text
        return cls(base, data, [text], None, [(base, start), (end, base + len(data))])


def subtract(r, cut):
    """Range r without range cut: zero, one or two ranges"""
    out = []
    if r[0] < min(r[1], cut[0]):
        out.append((r[0], min(r[1], cut[0])))
    if max(r[0], cut[1]) < r[1]:
        out.append((max(r[0], cut[1]), r[1]))
    return out
