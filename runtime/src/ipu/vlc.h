// The variable length codes of ISO/IEC 13818-2's Annex B, as lookup tables built once from the standard's code lists
#pragma once

#include "ps2/recomp.h"

namespace Ps2::Ipu
{
// A table read by the next `bits` bits: each entry the code's value and length (0: no code starts so)
struct VlcEntry
{
    s16 value;
    u8 len;
};
struct VlcTable
{
    u32 bits;
    const VlcEntry* entries;
};

// macroblock_type's flags, in the bitstream's order (VDEC returns them so; Sony's libmpeg tests them by these bits)
constexpr u32 MbQuant = 0x10;
constexpr u32 MbMotionForward = 0x08;
constexpr u32 MbMotionBackward = 0x04;
constexpr u32 MbPattern = 0x02;
constexpr u32 MbIntra = 0x01;

// macroblock_address_increment's two codes that aren't increments: the values VDEC returns for them
constexpr s16 MbaStuffing = 0x22;
constexpr s16 MbaEscape = 0x23;

const VlcTable& MbAddressIncrementTable();    // B.1
const VlcTable& MbTypeTable(u32 pictureType); // B.2 (I), B.3 (P), B.4 (B), D pictures' one bit
const VlcTable& CodedBlockPatternTable();     // B.9
const VlcTable& MotionCodeTable();            // B.10, the sign bit included
const VlcTable& DmVectorTable();              // B.11
const VlcTable& DcSizeTable(bool chroma);     // B.12, B.13

// The DCT coefficients' codes (B.14, B.15), the sign bit not included. Looked up by the next 16 bits: codes that begin with
// six zeros are all in `low` by those bits' low 10 (they're at most 16 long), the rest in `high` by the top 10
constexpr u8 RunEndOfBlock = 64;
constexpr u8 RunEscape = 65;
struct DctEntry
{
    u8 run;    // 0-63, RunEndOfBlock or RunEscape
    u8 len;    // 0: no code
    u16 level;
};
struct DctTable
{
    DctEntry high[1024];
    DctEntry low[1024];
    const DctEntry& Find(u32 next16) const { return next16 >= 0x400 ? high[next16 >> 6] : low[next16 & 0x3FF]; }
};
const DctTable& DctTableZero();
const DctTable& DctTableOne();
}
