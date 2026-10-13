#!/usr/bin/env python3
"""Read files from a PS2 disc image: a 2048-byte .iso, or a raw 2352-byte .bin (MODE1 or MODE2 form 1) named by itself or by
its .cue. Only ISO 9660 is read (no UDF): every PS2 DVD and CD carries an ISO 9660 tree.

    disc.py list  IMAGE [PATH]          list a directory (default the root)
    disc.py tree  IMAGE                 every file with its sector and size
    disc.py get   IMAGE PATH OUT        copy one file out
"""

import os
import struct
import sys


class Disc:
    def __init__(self, path):
        if path.lower().endswith(".cue"):
            path = self._bin_from_cue(path)
        self.path = path
        self.f = open(path, "rb")
        size = os.path.getsize(path)
        self.sector_size, self.data_offset = 2048, 0
        # A raw image starts every sector with the 12-byte sync pattern
        head = self.f.read(16)
        if head[:12] == b"\x00" + b"\xff" * 10 + b"\x00":
            self.sector_size = 2352
            mode = head[15]
            self.data_offset = 24 if mode == 2 else 16
        self.sectors = size // self.sector_size
        pvd = self.read_sectors(16, 1)
        if pvd[1:6] != b"CD001":
            raise ValueError("no ISO 9660 volume descriptor at sector 16")
        self.root = self._parse_record(pvd[156:156 + 34])

    @staticmethod
    def _bin_from_cue(cue):
        folder = os.path.dirname(cue)
        for line in open(cue, encoding="latin-1"):
            line = line.strip()
            if line.upper().startswith("FILE"):
                name = line[line.index('"') + 1:line.rindex('"')]
                return os.path.join(folder, name)
        raise ValueError("no FILE line in the cue sheet")

    def read_sectors(self, lba, count):
        out = bytearray()
        for i in range(count):
            self.f.seek((lba + i) * self.sector_size + self.data_offset)
            out += self.f.read(2048)
        return bytes(out)

    def read_extent(self, lba, size):
        data = self.read_sectors(lba, (size + 2047) // 2048)
        return data[:size]

    @staticmethod
    def _parse_record(rec):
        length = rec[0]
        lba = struct.unpack_from("<I", rec, 2)[0]
        size = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        name_len = rec[32]
        name = rec[33:33 + name_len].decode("latin-1")
        if ";" in name:
            name = name[:name.index(";")]
        return {"lba": lba, "size": size, "dir": bool(flags & 2), "name": name, "len": length}

    def listdir(self, entry):
        data = self.read_extent(entry["lba"], entry["size"])
        pos, out = 0, []
        while pos < len(data):
            length = data[pos]
            if length == 0:  # records don't cross a sector: skip to the next one
                pos = (pos // 2048 + 1) * 2048
                continue
            rec = self._parse_record(data[pos:pos + length])
            if rec["name"] not in ("\x00", "\x01"):
                out.append(rec)
            pos += length
        return out

    def find(self, path):
        entry = self.root
        for part in [p for p in path.replace("\\", "/").split("/") if p]:
            for child in self.listdir(entry):
                if child["name"].upper() == part.upper():
                    entry = child
                    break
            else:
                return None
        return entry

    def walk(self, entry=None, prefix=""):
        entry = entry or self.root
        for child in self.listdir(entry):
            path = prefix + "/" + child["name"]
            yield path, child
            if child["dir"]:
                yield from self.walk(child, path)

    def read_file(self, path):
        entry = self.find(path)
        if entry is None or entry["dir"]:
            raise FileNotFoundError(path)
        return self.read_extent(entry["lba"], entry["size"])


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    cmd, image = argv[1], argv[2]
    disc = Disc(image)
    if cmd == "list":
        entry = disc.find(argv[3]) if len(argv) > 3 else disc.root
        for child in disc.listdir(entry):
            print(f"{'D' if child['dir'] else 'F'} {child['lba']:8d} {child['size']:12d} {child['name']}")
    elif cmd == "tree":
        for path, child in disc.walk():
            print(f"{'D' if child['dir'] else 'F'} {child['lba']:8d} {child['size']:12d} {path}")
    elif cmd == "get":
        data = disc.read_file(argv[3])
        os.makedirs(os.path.dirname(os.path.abspath(argv[4])), exist_ok=True)
        with open(argv[4], "wb") as f:
            f.write(data)
        print(f"{argv[3]}: {len(data)} bytes")
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
