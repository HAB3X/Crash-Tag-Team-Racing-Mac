#!/usr/bin/env python3
"""Recompile a PS2 executable's MIPS code to C.

    recomp.py CONFIG.toml ELF OUTDIR [--files N] [--disc IMAGE]

The config (a game's own) names extra function starts, addresses that aren't starts, and overrides: functions the runtime
or the game replaces with host code (the generated body is kept as f_XXXXXXXX for the override to call). The output is C
that includes the runtime's ps2/recomp.h: funcs.h (every function's prototype), table.c (address to function, for indirect
calls) and code_NNN.c. Nothing of the executable's data is written: the runtime loads the executable from the disc at start.

Code overlays (the config's [[overlays]]: files the game loads at a fixed address and runs, read from the disc image) are
recompiled beside the executable, each function named <overlay>_XXXXXXXX. Several overlays share addresses, so a call into
one goes through ps2_call, which runs the overlay whose code is in memory there (the runtime's cpu.cpp): table.c lists each
overlay function with its first words, and each overlay with words that tell it from the others over the same addresses.
"""

import argparse
import hashlib
import os
import struct
import sys
import time
import tomllib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from analysis import Program  # noqa: E402
from codegen import FunctionEmitter  # noqa: E402
from elf import Elf  # noqa: E402
from image import CodeImage  # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
from disc import Disc  # noqa: E402

# How many of an overlay function's first words, and of an overlay's own distinguishing words, the runtime checks
HEAD_WORDS = 4
CHECK_WORDS = 8


def parse_int(v):
    return int(v, 0) if isinstance(v, str) else int(v)


def load_config(path):
    with open(path, "rb") as f:
        raw = tomllib.load(f)
    fn = raw.get("functions", {})
    text = raw.get("executable", {}).get("text")
    vutext = raw.get("executable", {}).get("vutext")
    overlays = []
    for o in raw.get("overlays", []):
        if not o["name"].isidentifier():
            sys.exit(f"overlay name {o['name']!r}: it names C functions, so it must be an identifier")
        overlays.append({
            "name": o["name"],
            "file": o["file"],
            "base": parse_int(o["base"]),
            "text": tuple(parse_int(x) for x in o["text"]),
            "extra_functions": [parse_int(x) for x in o.get("extra", [])],
            "not_functions": [parse_int(x) for x in o.get("not", [])],
            "overrides": {parse_int(k): v for k, v in o.get("overrides", {}).items()},
            "encrypted": bool(o.get("encrypted", False)),
        })
    return {
        "sha1": raw.get("executable", {}).get("sha1"),
        "text": [tuple(parse_int(x) for x in r) for r in text] if text else None,
        "vutext": tuple(parse_int(x) for x in vutext) if vutext else None,
        "extra_functions": [parse_int(x) for x in fn.get("extra", [])],
        "not_functions": [parse_int(x) for x in fn.get("not", [])],
        "setjmp": {parse_int(x) for x in fn.get("setjmp", [])},
        "longjmp": {parse_int(x) for x in fn.get("longjmp", [])},
        "overrides": {parse_int(k): v for k, v in raw.get("overrides", {}).items()},
        "overlays": overlays,
    }


def word_at(code, address):
    """The word an image holds at an address, or None outside it"""
    off = address - code.image_base
    if 0 <= off <= len(code.image) - 4:
        return struct.unpack_from("<I", code.image, off)[0]
    return None


def check_words(index, codes):
    """Up to CHECK_WORDS (address, word) pairs, spread over overlay `index`'s text, where it holds a non-zero word that no
    other overlay over the same address holds: what tells it from them in memory."""
    code = codes[index]
    others = [c for k, c in enumerate(codes) if k != index and
              c.image_base < code.image_base + len(code.image) and code.image_base < c.image_base + len(c.image)]
    found = []
    for a in range(code.text_start, code.text_end, 4):
        w = word_at(code, a)
        if w and all(word_at(c, a) != w for c in others):
            found.append((a, w))
    if not found:
        sys.exit(f"overlay {index}: no word of its text tells it from the overlays over the same addresses")
    step = max(1, len(found) // CHECK_WORDS)
    return found[::step][:CHECK_WORDS]


def write_if_changed(path, text):
    if os.path.exists(path):
        with open(path) as f:
            if f.read() == text:
                return False
    with open(path, "w") as f:
        f.write(text)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("config")
    ap.add_argument("elf")
    ap.add_argument("outdir")
    ap.add_argument("--files", type=int, default=48)
    ap.add_argument("--disc", help="the disc image the config's overlays are read from")
    args = ap.parse_args()

    config = load_config(args.config)
    data = open(args.elf, "rb").read()
    sha1 = hashlib.sha1(data).hexdigest()
    if config["sha1"] and config["sha1"] != sha1:
        sys.exit(f"{args.elf}: SHA-1 {sha1}, the config is for {config['sha1']}")
    t0 = time.time()
    program = Program(CodeImage.from_elf(Elf(args.elf), config["text"], config["vutext"]), config)
    functions = program.analyse()
    print(f"{len(functions)} functions, {len(program.jump_tables)} jump tables ({time.time() - t0:.1f} s)")

    overrides = config["overrides"]
    names = {s: f"f_{s:08x}" for s, _ in functions}
    for a, name in overrides.items():
        names[a] = name
    # longjmp goes back to its setjmp's place on the host too, after the game's own code has put the registers back
    for a in config["longjmp"]:
        names[a] = f"longjmp_{a:08x}"

    # Each overlay: its functions, named by the overlay; its own calls are direct, as are its calls into the executable
    overlays = []
    if config["overlays"] and not args.disc:
        sys.exit("the config has overlays: give the disc image they're read from (--disc)")
    disc = Disc(args.disc) if config["overlays"] else None
    for o in config["overlays"]:
        code = CodeImage.from_overlay(disc.read_file(o["file"]), o["base"], o["text"])
        op = Program(code, o)
        ofuncs = op.analyse()
        onames = dict(names)
        for s, _ in ofuncs:
            onames[s] = f"{o['name']}_{s:08x}"
        for a, name in o["overrides"].items():
            onames[a] = name
        overlays.append({"config": o, "code": code, "program": op, "functions": ofuncs, "names": onames})
        print(f"overlay {o['name']} ({o['file']} at {o['base']:#x}): {len(ofuncs)} functions, "
              f"{len(op.jump_tables)} jump tables")

    os.makedirs(args.outdir, exist_ok=True)
    header = ["// Generated by recomp.py: do not edit", "#pragma once", '#include "ps2/recomp.h"', ""]
    header += [f"void f_{s:08x}(Ps2Cpu* c);" for s, _ in functions]
    header += [f"void {name}(Ps2Cpu* c);" for name in sorted(set(overrides.values()))]
    header += [f"void longjmp_{a:08x}(Ps2Cpu* c);" for a in sorted(config["longjmp"])]
    for ov in overlays:
        o = ov["config"]
        header += [f"void {o['name']}_{s:08x}(Ps2Cpu* c);" for s, _ in ov["functions"]]
        header += [f"void {name}(Ps2Cpu* c);" for name in sorted(set(o["overrides"].values()))]
    write_if_changed(os.path.join(args.outdir, "funcs.h"), "\n".join(header) + "\n")

    table = ["// Generated by recomp.py: do not edit", '#include "funcs.h"', "",
             "const Ps2FunctionEntry ps2_function_table[] = {"]
    table += [f"    {{0x{s:08x}u, 0x{e:08x}u, {names[s]}}}," for s, e in functions]
    table += ["};", f"const u32 ps2_function_count = {len(functions)};", ""]
    for a in sorted(config["longjmp"]):
        table += [f"void longjmp_{a:08x}(Ps2Cpu* c)", "{", "    u32 buffer = c->r[4].uw[0];", f"    f_{a:08x}(c);",
                  "    ps2_longjmp(c, buffer);", "}", ""]
    table += overlay_tables(overlays)
    write_if_changed(os.path.join(args.outdir, "table.c"), "\n".join(table))

    # Every function to emit: (start, end, program, names, overrides, setjmps, encrypted, prefix)
    jobs = [(s, e, program, names, overrides, config["setjmp"], False, "f") for s, e in functions]
    for ov in overlays:
        o = ov["config"]
        jobs += [(s, e, ov["program"], ov["names"], o["overrides"], frozenset(), o["encrypted"], o["name"])
                 for s, e in ov["functions"]]
    per_file = (len(jobs) + args.files - 1) // args.files
    changed = 0
    for n in range(args.files):
        body = ["// Generated by recomp.py: do not edit", '#include "funcs.h"', ""]
        for s, e, p, nm, ovr, sj, enc, prefix in jobs[n * per_file:(n + 1) * per_file]:
            body.append(FunctionEmitter(p, s, e, nm, ovr, sj, enc, prefix).emit())
            body.append("")
        changed += write_if_changed(os.path.join(args.outdir, f"code_{n:03d}.c"), "\n".join(body))
    print(f"{args.files} files ({changed} changed) in {time.time() - t0:.1f} s")


def overlay_tables(overlays):
    """table.c's overlay part: every overlay function (sorted by address) with its first words, and every overlay with the
    words that tell it from the others over the same addresses. Empty tables for a game without overlays."""
    codes = [ov["code"] for ov in overlays]
    entries = []
    for k, ov in enumerate(overlays):
        for s, e in ov["functions"]:
            head = [word_at(ov["code"], s + 4 * n) for n in range(HEAD_WORDS)]
            if None in head:
                sys.exit(f"overlay {ov['config']['name']}: function {s:#x} ends within {HEAD_WORDS} words of the file's end")
            entries.append((s, k, e, ov["names"][s], head))
    entries.sort()
    out = ["const Ps2OverlayFunction ps2_overlay_function_table[] = {"]
    out += [f"    {{0x{s:08x}u, 0x{e:08x}u, {name}, {k}u, {{{', '.join(f'0x{w:08x}u' for w in head)}}}}},"
            for s, k, e, name, head in entries]
    if not entries:
        out.append("    {0, 0, 0, 0, {0}},")
    out += ["};", f"const u32 ps2_overlay_function_count = {len(entries)};", "",
            "const Ps2Overlay ps2_overlays[] = {"]
    for k, ov in enumerate(overlays):
        o = ov["config"]
        checks = check_words(k, codes)
        pairs = ", ".join(f"{{0x{a:08x}u, 0x{w:08x}u}}" for a, w in checks)
        out.append(f'    {{"{o["file"]}", 0x{o["base"]:08x}u, 0x{o["base"] + len(ov["code"].image):08x}u, '
                   f"{len(checks)}u, {{{pairs}}}}},")
    if not overlays:
        out.append('    {"", 0, 0, 0, {{0, 0}}},')
    out += ["};", f"const u32 ps2_overlay_count = {len(overlays)};", ""]
    return out


if __name__ == "__main__":
    main()
