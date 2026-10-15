#!/usr/bin/env python3
"""Tests of the recompiler on small hand-assembled code: overlays, text with holes, data kept in the text.

    python3 recomp/test_recomp.py
"""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import recomp  # noqa: E402
from analysis import Program  # noqa: E402
from codegen import FunctionEmitter  # noqa: E402
from image import CodeImage  # noqa: E402

NOP = 0x00000000
JR_RA = 0x03E00008


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def addiu(rt, rs, imm):
    return 0x24000000 | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def words(*ws):
    return struct.pack(f"<{len(ws)}I", *ws)


HEADER = bytes(0x40)
BASE = 0x500000


def overlay(code):
    """An overlay file: a 0x40 byte header, then code at BASE + 0x40"""
    data = HEADER + code
    return CodeImage.from_overlay(data, BASE, (BASE + 0x40, BASE + len(data)))


class OverlayTest(unittest.TestCase):
    def setUp(self):
        # f at 0x500040 calls g at 0x500060 and a function of the executable at 0x100000
        self.code = overlay(words(jal(0x500060), NOP, jal(0x100000), NOP, JR_RA, NOP, NOP, NOP,
                                  addiu(2, 0, 7), JR_RA, NOP, NOP))
        self.program = Program(self.code, {})
        self.functions = self.program.analyse()

    def test_functions_found(self):
        self.assertEqual([s for s, _ in self.functions], [0x500040, 0x500060])

    def test_body_named_by_overlay_and_calls_direct(self):
        names = {0x100000: "f_00100000", 0x500040: "ov_00500040", 0x500060: "ov_00500060"}
        c = FunctionEmitter(self.program, 0x500040, 0x500060, names, {}, prefix="ov").emit()
        self.assertIn("void ov_00500040(Ps2Cpu* c)", c)
        self.assertIn("ov_00500060(c);", c)      # its own function, direct
        self.assertIn("f_00100000(c);", c)       # the executable's, direct
        self.assertNotIn("void f_00500040", c)   # the body isn't named as the executable's

    def test_calls_into_unknown_code_go_through_ps2_call(self):
        code = overlay(words(jal(0x600000), NOP, JR_RA, NOP))
        p = Program(code, {})
        p.analyse()
        c = FunctionEmitter(p, 0x500040, 0x500050, {0x500040: "ov_00500040"}, {}, prefix="ov").emit()
        self.assertIn("ps2_call(c, 0x00600000u);", c)


class PaddedOverlayTest(unittest.TestCase):
    def test_first_code_after_padding_is_a_function(self):
        # 0x40 zero bytes, then the overlay's entry, which nothing in the overlay calls or points at
        code = overlay(bytes(0x40) + words(addiu(2, 0, 1), JR_RA, NOP, NOP))
        functions = Program(code, {}).analyse()
        self.assertEqual([s for s, _ in functions], [0x500080])


class CheckWordsTest(unittest.TestCase):
    def test_words_tell_overlays_apart(self):
        a = overlay(words(addiu(2, 0, 1), JR_RA, NOP, NOP))
        b = overlay(words(addiu(2, 0, 2), JR_RA, NOP, NOP))
        checks = recomp.check_words(0, [a, b])
        self.assertTrue(checks)
        for address, word in checks:
            self.assertNotEqual(recomp.word_at(b, address), word)
            self.assertNotEqual(word, 0)

    def test_identical_overlays_fail_loudly(self):
        a = overlay(words(addiu(2, 0, 1), JR_RA, NOP, NOP))
        with self.assertRaises(SystemExit):
            recomp.check_words(0, [a, overlay(words(addiu(2, 0, 1), JR_RA, NOP, NOP))])


class TextTest(unittest.TestCase):
    def test_hole_is_not_code(self):
        # Code, a hole of words that would decode, code again
        data = words(JR_RA, NOP, 0x12345678, 0x9ABCDEF0, JR_RA, NOP)
        code = CodeImage(0x100000, data, [(0x100000, 0x100008), (0x100010, 0x100018)], 0x100000, [])
        self.assertTrue(code.in_hole(0x100008))
        self.assertEqual(code.text_words()[2:4], [0, 0])
        functions = Program(code, {}).analyse()
        self.assertEqual([s for s, _ in functions], [0x100000, 0x100010])

    def test_table_in_text_is_not_a_function(self):
        # A function, then a table of words that don't all decode (0x00000055 is no instruction), then a function
        data = words(JR_RA, NOP, 0x00000055, 0x80075038, 0x00000056, 0x800750C8, JR_RA, NOP)
        code = CodeImage(0x100000, data, [(0x100000, 0x100000 + len(data))], 0x100000, [])
        functions = Program(code, {"extra_functions": [0x100018]}).analyse()
        self.assertEqual([s for s, _ in functions], [0x100000, 0x100018])


if __name__ == "__main__":
    unittest.main()
