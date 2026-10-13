"""A minimal ELF32 little-endian reader: the sections and the program's loadable segments."""

import struct


class Elf:
    def __init__(self, path):
        self.data = data = open(path, "rb").read()
        if data[:4] != b"\x7fELF":
            raise ValueError("not an ELF file")
        (self.entry, phoff, shoff) = struct.unpack_from("<III", data, 24)
        (phnum,) = struct.unpack_from("<H", data, 44)
        (shnum, shstrndx) = struct.unpack_from("<HH", data, 48)
        self.segments = []
        for n in range(phnum):
            p = struct.unpack_from("<IIIIIIII", data, phoff + n * 32)
            if p[0] == 1:
                self.segments.append({"offset": p[1], "vaddr": p[2], "filesz": p[4], "memsz": p[5]})
        self.sections = {}
        if shoff and shnum:
            heads = [struct.unpack_from("<IIIIIIIIII", data, shoff + n * 40) for n in range(shnum)]
            strtab = heads[shstrndx][4]
            for h in heads:
                name = data[strtab + h[0]:data.index(b"\0", strtab + h[0])].decode()
                if name:
                    self.sections[name] = {"type": h[1], "flags": h[2], "addr": h[3], "offset": h[4], "size": h[5]}

    def image(self):
        """The loaded image: (base address, bytes) covering every PT_LOAD segment's file part."""
        lo = min(s["vaddr"] for s in self.segments)
        hi = max(s["vaddr"] + s["memsz"] for s in self.segments)
        buf = bytearray(hi - lo)
        for s in self.segments:
            buf[s["vaddr"] - lo:s["vaddr"] - lo + s["filesz"]] = self.data[s["offset"]:s["offset"] + s["filesz"]]
        return lo, bytes(buf)
