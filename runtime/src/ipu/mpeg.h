// The MPEG-1/2 video decoding the IPU does in hardware (ISO/IEC 13818-2, from the standard): the VLCs its VDEC command
// decodes, a macroblock's blocks for BDEC and IDEC (DC prediction, the coefficients' VLCs, inverse quantisation, the inverse
// DCT). Motion compensation is the EE's (the IPU has none)
#pragma once

#include "bitstream.h"

namespace Ps2::Ipu
{
// What IPU_CTRL sets for the decoding
struct DecodeSettings
{
    bool mpeg1 = false;            // MP1: MPEG-1's escapes and oddification, no mismatch control or intra VLC table one
    bool intraVlcTableOne = false; // IVF: intra blocks' AC coefficients by table B.15
    bool alternateScan = false;    // AS
    u32 intraDcPrecision = 0;      // IDP: 8 + n bits
    bool nonLinearQuantiser = false; // QST
    u32 pictureType = 1;           // PCT: 1 I, 2 P, 3 B, 4 D (VDEC's macroblock_type)
};

// The quantiser matrices as SETIQ loads them: 64 bytes in the bitstream's order (zigzag scan order, as the sequence header and
// the quant matrix extension carry them)
struct Matrices
{
    u8 intra[64];
    u8 nonIntra[64];
};

// The quantiser scale a 5 bit quantiser_scale_code gives
int QuantiserScale(u32 code, bool nonLinear);

// VDEC: one VLC decoded at the position by its table (0 macroblock_address_increment, 1 macroblock_type, 2 motion_code,
// 3 dmvector), consumed. Returns IPU_CMD's word: the value in the low half (signed for motion_code and dmvector; for
// macroblock_address_increment 1-33, 0x22 stuffing, 0x23 escape), the code's length in bits in the high half; 0 for a code
// that isn't in the table
u32 DecodeVlc(Bitstream& bits, u32 table, const DecodeSettings& s);

// A macroblock decoded as BDEC returns it (RAW16): 256 Y (16 x 16), 64 Cb, 64 Cr (8 x 8), row by row. Intra: the pixels
// (0-255); not intra: the prediction errors (-256 to 255), the blocks the coded block pattern leaves out zero. `dcPredictor`
// is carried from one intra macroblock to the next. Returns the coded block pattern (bit 5 the first Y block ... bit 0 Cr);
// `error` is set by a code no table has
struct MacroblockParams
{
    bool intra;
    bool fieldDct;   // the Y blocks hold alternate lines (DT)
    int quantiserScale;
};
u32 DecodeMacroblock(Bitstream& bits, const DecodeSettings& s, const Matrices& m, const MacroblockParams& p, s32 dcPredictor[3],
                     s16 out[384], bool& error);
// The DC predictors' reset value (128 at 8 bits of precision)
inline s32 DcReset(const DecodeSettings& s) { return 128 << s.intraDcPrecision; }

// The inverse DCT of one block in place (the coefficients in raster order in, the samples rounded to integers out, not
// saturated), and the reference it's checked against (double precision, the transform's definition, as IEEE 1180 measures)
void InverseDct(s32 block[64]);
void InverseDctReference(const s32 in[64], double out[64]);
}
