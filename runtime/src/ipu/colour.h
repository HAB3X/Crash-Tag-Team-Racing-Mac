// The IPU's colour conversions on a 16 x 16 macroblock: YCbCr (RAW8) to RGB32 (CSC, IDEC), RGB32 to RGB16 with the 4 x 4
// dither (CSC's, IDEC's and PACK's RGB16), and RGB16 to 4 bit indexes into the VQ CLUT (PACK's INDX4)
#pragma once

#include "ps2/recomp.h"

namespace Ps2::Ipu
{
// RAW8: 256 Y bytes (16 x 16), 64 Cb, 64 Cr (8 x 8), row by row: CSC's input, 24 quadwords
struct Raw8
{
    u8 y[256];
    u8 cb[64];
    u8 cr[64];
};

// SETTH's thresholds: a pixel whose R, G and B are all under TH0 is all zero (transparent black), else under TH1 its alpha is
// 0x40, else 0x80
struct Thresholds
{
    u32 th0 = 0;
    u32 th1 = 0;
};

// 256 pixels R, G, B, A (64 quadwords). `sign` (IDEC's SGN) makes R, G and B signed: 128 taken off, wrapping
void YcbcrToRgb32(const Raw8& in, const Thresholds& th, bool sign, u8 out[1024]);
// 256 pixels of 5 bits each R (bits 0-4), G (5-9), B (10-14), and A (15) set for an alpha of 0x40 (32 quadwords)
void Rgb32ToRgb16(const u8 in[1024], bool dither, u16 out[256]);
// Each pixel the nearest of the CLUT's 16 colours (5 bit RGB, the squared distance; the lowest index on a tie), two to a
// byte, the left one in the low nibble (8 quadwords)
void Rgb16ToIndx4(const u16 in[256], const u16 clut[16], u8 out[128]);
}
