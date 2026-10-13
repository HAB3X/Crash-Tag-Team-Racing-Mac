#!/usr/bin/env python3
"""Print the C strings at addresses in an executable: elfstr.py ELF ADDR [ADDR...]"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "recomp"))
from elf import Elf  # noqa: E402

e = Elf(sys.argv[1])
base, image = e.image()
for a in sys.argv[2:]:
    addr = int(a, 0)
    off = addr - base
    end = image.index(b"\0", off)
    print(f"{addr:08x}: {image[off:end].decode('latin-1')!r}")
