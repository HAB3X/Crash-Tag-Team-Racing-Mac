// The code lists are ISO/IEC 13818-2's Annex B tables, written as the standard prints them (spaces only for reading). Each is
// checked as it's built: no code is the start of another
#include "vlc.h"

#include "../runtime.h"

#include <vector>

namespace Ps2::Ipu
{
namespace
{
struct Code
{
    const char* bits;
    s16 value;
};

u32 Parse(const char* bits, u32& len)
{
    u32 v = 0;
    len = 0;
    for (const char* p = bits; *p != 0; p++)
    {
        if (*p == '0' || *p == '1')
        {
            v = (v << 1) | static_cast<u32>(*p - '0');
            len++;
        }
    }
    return v;
}

struct BuiltTable
{
    std::vector<VlcEntry> entries;
    VlcTable table;
    BuiltTable(const Code* codes, size_t count, const char* name)
    {
        u32 bits = 0;
        for (size_t i = 0; i < count; i++)
        {
            u32 len;
            Parse(codes[i].bits, len);
            bits = std::max(bits, len);
        }
        entries.assign(size_t(1) << bits, VlcEntry{0, 0});
        for (size_t i = 0; i < count; i++)
        {
            u32 len;
            u32 v = Parse(codes[i].bits, len);
            u32 first = v << (bits - len);
            for (u32 j = 0; j < (1u << (bits - len)); j++)
            {
                if (entries[first + j].len != 0)
                {
                    Fatal("IPU: VLC table %s: code %s overlaps another", name, codes[i].bits);
                }
                entries[first + j] = VlcEntry{codes[i].value, static_cast<u8>(len)};
            }
        }
        table = VlcTable{bits, entries.data()};
    }
};

// B.1
const Code kMbAddressIncrement[] = {
    {"1", 1}, {"011", 2}, {"010", 3}, {"0011", 4}, {"0010", 5}, {"0001 1", 6}, {"0001 0", 7}, {"0000 111", 8},
    {"0000 110", 9}, {"0000 1011", 10}, {"0000 1010", 11}, {"0000 1001", 12}, {"0000 1000", 13}, {"0000 0111", 14},
    {"0000 0110", 15}, {"0000 0101 11", 16}, {"0000 0101 10", 17}, {"0000 0101 01", 18}, {"0000 0101 00", 19},
    {"0000 0100 11", 20}, {"0000 0100 10", 21}, {"0000 0100 011", 22}, {"0000 0100 010", 23}, {"0000 0100 001", 24},
    {"0000 0100 000", 25}, {"0000 0011 111", 26}, {"0000 0011 110", 27}, {"0000 0011 101", 28}, {"0000 0011 100", 29},
    {"0000 0011 011", 30}, {"0000 0011 010", 31}, {"0000 0011 001", 32}, {"0000 0011 000", 33},
    {"0000 0001 111", MbaStuffing}, {"0000 0001 000", MbaEscape},
};

// B.2, B.3, B.4; a D picture's macroblock_type is one bit, '1'
const Code kMbTypeI[] = {{"1", MbIntra}, {"01", MbQuant | MbIntra}};
const Code kMbTypeP[] = {
    {"1", MbMotionForward | MbPattern}, {"01", MbPattern}, {"001", MbMotionForward}, {"0001 1", MbIntra},
    {"0001 0", MbQuant | MbMotionForward | MbPattern}, {"0000 1", MbQuant | MbPattern}, {"0000 01", MbQuant | MbIntra},
};
const Code kMbTypeB[] = {
    {"10", MbMotionForward | MbMotionBackward}, {"11", MbMotionForward | MbMotionBackward | MbPattern},
    {"010", MbMotionBackward}, {"011", MbMotionBackward | MbPattern}, {"0010", MbMotionForward},
    {"0011", MbMotionForward | MbPattern}, {"0001 1", MbIntra},
    {"0001 0", MbQuant | MbMotionForward | MbMotionBackward | MbPattern}, {"0000 11", MbQuant | MbMotionForward | MbPattern},
    {"0000 10", MbQuant | MbMotionBackward | MbPattern}, {"0000 01", MbQuant | MbIntra},
};
const Code kMbTypeD[] = {{"1", MbIntra}};

// B.9 (bit 5 the first Y block ... bit 0 Cr)
const Code kCodedBlockPattern[] = {
    {"111", 60}, {"1101", 4}, {"1100", 8}, {"1011", 16}, {"1010", 32}, {"1001 1", 12}, {"1001 0", 48}, {"1000 1", 20},
    {"1000 0", 40}, {"0111 1", 28}, {"0111 0", 44}, {"0110 1", 52}, {"0110 0", 56}, {"0101 1", 1}, {"0101 0", 61},
    {"0100 1", 2}, {"0100 0", 62}, {"0011 11", 24}, {"0011 10", 36}, {"0011 01", 3}, {"0011 00", 63}, {"0010 111", 5},
    {"0010 110", 9}, {"0010 101", 17}, {"0010 100", 33}, {"0010 011", 6}, {"0010 010", 10}, {"0010 001", 18},
    {"0010 000", 34}, {"0001 1111", 7}, {"0001 1110", 11}, {"0001 1101", 19}, {"0001 1100", 35}, {"0001 1011", 13},
    {"0001 1010", 49}, {"0001 1001", 21}, {"0001 1000", 41}, {"0001 0111", 14}, {"0001 0110", 50}, {"0001 0101", 22},
    {"0001 0100", 42}, {"0001 0011", 15}, {"0001 0010", 51}, {"0001 0001", 23}, {"0001 0000", 43}, {"0000 1111", 25},
    {"0000 1110", 37}, {"0000 1101", 26}, {"0000 1100", 38}, {"0000 1011", 29}, {"0000 1010", 45}, {"0000 1001", 53},
    {"0000 1000", 57}, {"0000 0111", 30}, {"0000 0110", 46}, {"0000 0101", 54}, {"0000 0100", 58}, {"0000 0011 1", 31},
    {"0000 0011 0", 47}, {"0000 0010 1", 55}, {"0000 0010 0", 59}, {"0000 0001 1", 27}, {"0000 0001 0", 39},
    {"0000 0000 1", 0},
};

// B.10, each code with its sign bit (0 positive)
const Code kMotionCode[] = {
    {"1", 0}, {"010", 1}, {"011", -1}, {"0010", 2}, {"0011", -2}, {"0001 0", 3}, {"0001 1", -3}, {"0000 110", 4},
    {"0000 111", -4}, {"0000 1010", 5}, {"0000 1011", -5}, {"0000 1000", 6}, {"0000 1001", -6}, {"0000 0110", 7},
    {"0000 0111", -7}, {"0000 0101 10", 8}, {"0000 0101 11", -8}, {"0000 0101 00", 9}, {"0000 0101 01", -9},
    {"0000 0100 10", 10}, {"0000 0100 11", -10}, {"0000 0100 010", 11}, {"0000 0100 011", -11}, {"0000 0100 000", 12},
    {"0000 0100 001", -12}, {"0000 0011 110", 13}, {"0000 0011 111", -13}, {"0000 0011 100", 14}, {"0000 0011 101", -14},
    {"0000 0011 010", 15}, {"0000 0011 011", -15}, {"0000 0011 000", 16}, {"0000 0011 001", -16},
};

// B.11
const Code kDmVector[] = {{"0", 0}, {"10", 1}, {"11", -1}};

// B.12, B.13: dct_dc_size
const Code kDcSizeLuma[] = {
    {"100", 0}, {"00", 1}, {"01", 2}, {"101", 3}, {"110", 4}, {"1110", 5}, {"1111 0", 6}, {"1111 10", 7},
    {"1111 110", 8}, {"1111 1110", 9}, {"1111 1111 0", 10}, {"1111 1111 1", 11},
};
const Code kDcSizeChroma[] = {
    {"00", 0}, {"01", 1}, {"10", 2}, {"110", 3}, {"1110", 4}, {"1111 0", 5}, {"1111 10", 6}, {"1111 110", 7},
    {"1111 1110", 8}, {"1111 1111 0", 9}, {"1111 1111 10", 10}, {"1111 1111 11", 11},
};

struct DctCode
{
    const char* bits; // without the sign bit
    u8 run;
    u16 level;
};

// B.14 (table zero). "1" for (0, 1) as a non-intra block's first coefficient is the decoder's (it's not in this table)
const DctCode kDctZero[] = {
    {"10", RunEndOfBlock, 0}, {"11", 0, 1}, {"011", 1, 1}, {"0100", 0, 2}, {"0101", 2, 1}, {"0010 1", 0, 3},
    {"0011 1", 3, 1}, {"0011 0", 4, 1}, {"0001 10", 1, 2}, {"0001 11", 5, 1}, {"0001 01", 6, 1}, {"0001 00", 7, 1},
    {"0000 110", 0, 4}, {"0000 100", 2, 2}, {"0000 111", 8, 1}, {"0000 101", 9, 1}, {"0000 01", RunEscape, 0},
    {"0010 0110", 0, 5}, {"0010 0001", 0, 6}, {"0010 0101", 1, 3}, {"0010 0100", 3, 2}, {"0010 0111", 10, 1},
    {"0010 0011", 11, 1}, {"0010 0010", 12, 1}, {"0010 0000", 13, 1}, {"0000 0010 10", 0, 7}, {"0000 0011 00", 1, 4},
    {"0000 0010 11", 2, 3}, {"0000 0011 11", 4, 2}, {"0000 0010 01", 5, 2}, {"0000 0011 10", 14, 1},
    {"0000 0011 01", 15, 1}, {"0000 0010 00", 16, 1}, {"0000 0001 1101", 0, 8}, {"0000 0001 1000", 0, 9},
    {"0000 0001 0011", 0, 10}, {"0000 0001 0000", 0, 11}, {"0000 0001 1011", 1, 5}, {"0000 0001 0100", 2, 4},
    {"0000 0001 1100", 3, 3}, {"0000 0001 0010", 4, 3}, {"0000 0001 1110", 6, 2}, {"0000 0001 0101", 7, 2},
    {"0000 0001 0001", 8, 2}, {"0000 0001 1111", 17, 1}, {"0000 0001 1010", 18, 1}, {"0000 0001 1001", 19, 1},
    {"0000 0001 0111", 20, 1}, {"0000 0001 0110", 21, 1}, {"0000 0000 1101 0", 0, 12}, {"0000 0000 1100 1", 0, 13},
    {"0000 0000 1100 0", 0, 14}, {"0000 0000 1011 1", 0, 15}, {"0000 0000 1011 0", 1, 6}, {"0000 0000 1010 1", 1, 7},
    {"0000 0000 1010 0", 2, 5}, {"0000 0000 1001 1", 3, 4}, {"0000 0000 1001 0", 5, 3}, {"0000 0000 1000 1", 9, 2},
    {"0000 0000 1000 0", 10, 2}, {"0000 0000 1111 1", 22, 1}, {"0000 0000 1111 0", 23, 1}, {"0000 0000 1110 1", 24, 1},
    {"0000 0000 1110 0", 25, 1}, {"0000 0000 1101 1", 26, 1},
};
// B.14's and B.15's codes from 0000 0000 0111 11 on (the same in both)
const DctCode kDctCommonLong[] = {
    {"0000 0000 0111 11", 0, 16}, {"0000 0000 0111 10", 0, 17}, {"0000 0000 0111 01", 0, 18},
    {"0000 0000 0111 00", 0, 19}, {"0000 0000 0110 11", 0, 20}, {"0000 0000 0110 10", 0, 21},
    {"0000 0000 0110 01", 0, 22}, {"0000 0000 0110 00", 0, 23}, {"0000 0000 0101 11", 0, 24},
    {"0000 0000 0101 10", 0, 25}, {"0000 0000 0101 01", 0, 26}, {"0000 0000 0101 00", 0, 27},
    {"0000 0000 0100 11", 0, 28}, {"0000 0000 0100 10", 0, 29}, {"0000 0000 0100 01", 0, 30},
    {"0000 0000 0100 00", 0, 31}, {"0000 0000 0011 000", 0, 32}, {"0000 0000 0010 111", 0, 33},
    {"0000 0000 0010 110", 0, 34}, {"0000 0000 0010 101", 0, 35}, {"0000 0000 0010 100", 0, 36},
    {"0000 0000 0010 011", 0, 37}, {"0000 0000 0010 010", 0, 38}, {"0000 0000 0010 001", 0, 39},
    {"0000 0000 0010 000", 0, 40}, {"0000 0000 0011 111", 1, 8}, {"0000 0000 0011 110", 1, 9},
    {"0000 0000 0011 101", 1, 10}, {"0000 0000 0011 100", 1, 11}, {"0000 0000 0011 011", 1, 12},
    {"0000 0000 0011 010", 1, 13}, {"0000 0000 0011 001", 1, 14}, {"0000 0000 0001 0011", 1, 15},
    {"0000 0000 0001 0010", 1, 16}, {"0000 0000 0001 0001", 1, 17}, {"0000 0000 0001 0000", 1, 18},
    {"0000 0000 0001 0100", 6, 3}, {"0000 0000 0001 1010", 11, 2}, {"0000 0000 0001 1001", 12, 2},
    {"0000 0000 0001 1000", 13, 2}, {"0000 0000 0001 0111", 14, 2}, {"0000 0000 0001 0110", 15, 2},
    {"0000 0000 0001 0101", 16, 2}, {"0000 0000 0001 1111", 27, 1}, {"0000 0000 0001 1110", 28, 1},
    {"0000 0000 0001 1101", 29, 1}, {"0000 0000 0001 1100", 30, 1}, {"0000 0000 0001 1011", 31, 1},
};
// B.15 (table one: intra blocks with intra_vlc_format), its codes before the common ones
const DctCode kDctOne[] = {
    {"0110", RunEndOfBlock, 0}, {"10", 0, 1}, {"010", 1, 1}, {"110", 0, 2}, {"0010 1", 2, 1}, {"0111", 0, 3},
    {"0011 1", 3, 1}, {"0001 10", 4, 1}, {"0011 0", 1, 2}, {"0001 11", 5, 1}, {"0000 110", 6, 1}, {"0000 100", 7, 1},
    {"1110 0", 0, 4}, {"0000 111", 2, 2}, {"0000 101", 8, 1}, {"1111 000", 9, 1}, {"0000 01", RunEscape, 0},
    {"1110 1", 0, 5}, {"0001 01", 0, 6}, {"1111 001", 1, 3}, {"0010 0110", 3, 2}, {"1111 010", 10, 1},
    {"0010 0001", 11, 1}, {"0010 0101", 12, 1}, {"0010 0100", 13, 1}, {"0001 00", 0, 7}, {"0010 0111", 1, 4},
    {"1111 1100", 2, 3}, {"1111 1101", 4, 2}, {"0000 0010 0", 5, 2}, {"0000 0010 1", 14, 1}, {"0000 0011 1", 15, 1},
    {"0000 0011 01", 16, 1}, {"1111 011", 0, 8}, {"1111 100", 0, 9}, {"0010 0011", 0, 10}, {"0010 0010", 0, 11},
    {"0010 0000", 1, 5}, {"0000 0011 00", 2, 4}, {"0000 0001 1100", 3, 3}, {"0000 0001 0010", 4, 3},
    {"0000 0001 1110", 6, 2}, {"0000 0001 0101", 7, 2}, {"0000 0001 0001", 8, 2}, {"0000 0001 1111", 17, 1},
    {"0000 0001 1010", 18, 1}, {"0000 0001 1001", 19, 1}, {"0000 0001 0111", 20, 1}, {"0000 0001 0110", 21, 1},
    {"1111 1010", 0, 12}, {"1111 1011", 0, 13}, {"1111 1110", 0, 14}, {"1111 1111", 0, 15}, {"0000 0000 1011 0", 1, 6},
    {"0000 0000 1010 1", 1, 7}, {"0000 0000 1010 0", 2, 5}, {"0000 0000 1001 1", 3, 4}, {"0000 0000 1001 0", 5, 3},
    {"0000 0000 1000 1", 9, 2}, {"0000 0000 1000 0", 10, 2}, {"0000 0000 1111 1", 22, 1}, {"0000 0000 1111 0", 23, 1},
    {"0000 0000 1110 1", 24, 1}, {"0000 0000 1110 0", 25, 1}, {"0000 0000 1101 1", 26, 1},
};

void AddDct(DctTable& t, const DctCode* codes, size_t count, const char* name)
{
    for (size_t i = 0; i < count; i++)
    {
        u32 len;
        u32 v = Parse(codes[i].bits, len);
        u32 next16 = v << (16 - len);
        bool low = next16 < 0x400;
        if (!low && len > 10)
        {
            Fatal("IPU: DCT table %s: code %s is longer than 10 bits without six leading zeros", name, codes[i].bits);
        }
        DctEntry* entries = low ? t.low : t.high;
        u32 index = low ? (next16 & 0x3FF) : (next16 >> 6);
        u32 span = 1u << ((low ? 16 : 10) - len);
        for (u32 j = 0; j < span; j++)
        {
            if (entries[index + j].len != 0)
            {
                Fatal("IPU: DCT table %s: code %s overlaps another", name, codes[i].bits);
            }
            entries[index + j] = DctEntry{codes[i].run, static_cast<u8>(len), codes[i].level};
        }
    }
}

struct DctTables
{
    DctTable zero = {};
    DctTable one = {};
    DctTables()
    {
        AddDct(zero, kDctZero, std::size(kDctZero), "B.14");
        AddDct(zero, kDctCommonLong, std::size(kDctCommonLong), "B.14");
        AddDct(one, kDctOne, std::size(kDctOne), "B.15");
        AddDct(one, kDctCommonLong, std::size(kDctCommonLong), "B.15");
    }
};

const DctTables& Dct()
{
    static const DctTables tables;
    return tables;
}
}

#define PS2_IPU_TABLE(function, codes, name)                                                                                   \
    const VlcTable& function()                                                                                                 \
    {                                                                                                                          \
        static const BuiltTable built(codes, std::size(codes), name);                                                          \
        return built.table;                                                                                                    \
    }

PS2_IPU_TABLE(MbAddressIncrementTable, kMbAddressIncrement, "B.1")
PS2_IPU_TABLE(CodedBlockPatternTable, kCodedBlockPattern, "B.9")
PS2_IPU_TABLE(MotionCodeTable, kMotionCode, "B.10")
PS2_IPU_TABLE(DmVectorTable, kDmVector, "B.11")
#undef PS2_IPU_TABLE

const VlcTable& MbTypeTable(u32 pictureType)
{
    static const BuiltTable i(kMbTypeI, std::size(kMbTypeI), "B.2");
    static const BuiltTable p(kMbTypeP, std::size(kMbTypeP), "B.3");
    static const BuiltTable b(kMbTypeB, std::size(kMbTypeB), "B.4");
    static const BuiltTable d(kMbTypeD, std::size(kMbTypeD), "D");
    switch (pictureType)
    {
    case 2: return p.table;
    case 3: return b.table;
    case 4: return d.table;
    default: return i.table;
    }
}

const VlcTable& DcSizeTable(bool chroma)
{
    static const BuiltTable luma(kDcSizeLuma, std::size(kDcSizeLuma), "B.12");
    static const BuiltTable chromaTable(kDcSizeChroma, std::size(kDcSizeChroma), "B.13");
    return chroma ? chromaTable.table : luma.table;
}

const DctTable& DctTableZero() { return Dct().zero; }
const DctTable& DctTableOne() { return Dct().one; }
}
