#include "mpeg.h"

#include "vlc.h"

#include <algorithm>
#include <cmath>

namespace Ps2::Ipu
{
namespace
{
// Scan position to raster position: the zigzag scan and the alternate scan (13818-2 7.3)
constexpr u8 kZigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};
constexpr u8 kAlternate[64] = {
    0,  8,  16, 24, 1,  9,  2,  10, 17, 25, 32, 40, 48, 56, 57, 49, 41, 33, 26, 18, 3,  11,
    4,  12, 19, 27, 34, 42, 50, 58, 35, 43, 51, 59, 20, 28, 5,  13, 6,  14, 21, 29, 36, 44,
    52, 60, 37, 45, 53, 61, 22, 30, 7,  15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63,
};

// For each scan, a scan position's raster position and its weight's place in a matrix loaded in zigzag order. With the
// alternate scan the weight is the raster position's, as the standard has it (whether the IPU does the same isn't known: no
// game seen uses the alternate scan)
struct Scan
{
    u8 raster[64];
    u8 weight[64];
};
struct Scans
{
    Scan zigzag;
    Scan alternate;
    Scans()
    {
        u8 zigzagOf[64];
        for (int i = 0; i < 64; i++)
        {
            zigzagOf[kZigzag[i]] = static_cast<u8>(i);
        }
        for (int i = 0; i < 64; i++)
        {
            zigzag.raster[i] = kZigzag[i];
            zigzag.weight[i] = static_cast<u8>(i);
            alternate.raster[i] = kAlternate[i];
            alternate.weight[i] = zigzagOf[kAlternate[i]];
        }
    }
};
const Scans g_scans;

constexpr int kNonLinearScale[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12, 14, 16, 18,  20,  22,
                                     24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112};

// The inverse DCT's matrix, T[n][k] = C(k)/2 cos((2n+1)k pi/16) (C(0) = 1/sqrt 2, else 1) in kIdctBits bits of fraction. The
// rows' pass keeps kIdctPassBits bits of fraction for the columns' pass. The sums are 64 bit: with 12 bit coefficients the
// constants' own rounding has to be under 2^-16 for IEEE 1180's mean square error (13 bits gave 0.027 overall, over its 0.02)
constexpr int kIdctBits = 17;
constexpr int kIdctPassBits = 6;
struct IdctMatrix
{
    s32 t[8][8];
    IdctMatrix()
    {
        for (int n = 0; n < 8; n++)
        {
            for (int k = 0; k < 8; k++)
            {
                double c = (k == 0 ? std::sqrt(0.5) : 1.0) / 2 * std::cos((2 * n + 1) * k * M_PI / 16);
                t[n][k] = static_cast<s32>(std::lround(c * (1 << kIdctBits)));
            }
        }
    }
};
const IdctMatrix g_idct;

// One 8 point inverse transform: in[k * stride] to out[n * stride], shifted down by `shift` with rounding. The even and odd
// coefficients' halves: out[n] = E + O, out[7 - n] = E - O
inline void Idct8(const s32* in, s32* out, int stride, int shift)
{
    const auto& t = g_idct.t;
    s64 round = s64(1) << (shift - 1);
    s64 x0 = in[0], x1 = in[stride], x2 = in[2 * stride], x3 = in[3 * stride];
    s64 x4 = in[4 * stride], x5 = in[5 * stride], x6 = in[6 * stride], x7 = in[7 * stride];
    for (int n = 0; n < 4; n++)
    {
        s64 e = t[n][0] * x0 + t[n][2] * x2 + t[n][4] * x4 + t[n][6] * x6 + round;
        s64 o = t[n][1] * x1 + t[n][3] * x3 + t[n][5] * x5 + t[n][7] * x7;
        out[n * stride] = static_cast<s32>((e + o) >> shift);
        out[(7 - n) * stride] = static_cast<s32>((e - o) >> shift);
    }
}

inline s32 Saturate(s32 v, s32 low, s32 high) { return v < low ? low : (v > high ? high : v); }

struct BlockContext
{
    Bitstream& bits;
    const DecodeSettings& s;
    const Scan& scan;
    int quantiserScale;
    bool& error;
};

// A coefficient's reconstruction (7.4.2): its magnitude from the level, the weight and the scale; MPEG-1's made odd toward
// zero (its own mismatch control); saturated to 12 bits with the sign
inline s32 Reconstruct(u32 level, bool negative, u32 weight, int scale, bool intra, bool mpeg1)
{
    u32 magnitude = intra ? (level * weight * static_cast<u32>(scale)) >> 4
                          : ((2 * level + 1) * weight * static_cast<u32>(scale)) >> 5;
    if (mpeg1 && magnitude != 0 && (magnitude & 1) == 0)
    {
        magnitude--;
    }
    return negative ? -static_cast<s32>(std::min<u32>(magnitude, 2048)) : static_cast<s32>(std::min<u32>(magnitude, 2047));
}

// The coefficients of one block from scan position `first` to the end of block, reconstructed into `coef` (raster order,
// zeroed by the caller). Returns whether any but the DC coefficient was set
bool DecodeCoefficients(BlockContext& b, const DctTable& table, const u8* weights, int first, bool intra, s32* coef, s32& sum)
{
    bool any = false;
    for (int i = first;; i++)
    {
        u32 next = b.bits.Peek(16);
        u32 run;
        u32 level;
        bool negative;
        if (!intra && i == 0 && (next & 0x8000) != 0)
        {
            // A non-intra block's first coefficient: '1' is (0, 1)
            b.bits.Skip(1);
            run = 0;
            level = 1;
            negative = b.bits.Get(1) != 0;
        }
        else
        {
            const DctEntry& e = table.Find(next);
            if (e.len == 0)
            {
                b.error = true;
                return any;
            }
            b.bits.Skip(e.len);
            if (e.run == RunEndOfBlock)
            {
                return any;
            }
            if (e.run == RunEscape)
            {
                run = b.bits.Get(6);
                s32 signedLevel;
                if (!b.s.mpeg1)
                {
                    signedLevel = b.bits.PeekSigned(12);
                    b.bits.Skip(12);
                }
                else
                {
                    // MPEG-1: 8 bits, or 0x00 / 0x80 then 8 more for the levels past 127
                    signedLevel = b.bits.PeekSigned(8);
                    b.bits.Skip(8);
                    if (signedLevel == 0)
                    {
                        signedLevel = static_cast<s32>(b.bits.Get(8));
                    }
                    else if (signedLevel == -128)
                    {
                        signedLevel = static_cast<s32>(b.bits.Get(8)) - 256;
                    }
                }
                negative = signedLevel < 0;
                level = static_cast<u32>(negative ? -signedLevel : signedLevel);
            }
            else
            {
                run = e.run;
                level = e.level;
                negative = b.bits.Get(1) != 0;
            }
        }
        i += static_cast<int>(run);
        if (i > 63)
        {
            b.error = true;
            return any;
        }
        u32 weight = weights[b.scan.weight[i]];
        s32 v = Reconstruct(level, negative, weight, b.quantiserScale, intra, b.s.mpeg1);
        coef[b.scan.raster[i]] = v;
        sum += v;
        any = true;
    }
}

// One block decoded and transformed into `out` (16 bit samples, `stride` apart): intra saturated to 0-255, else -256 to 255
void DecodeBlock(BlockContext& b, const Matrices& m, bool intra, int cc, s32& dcPredictor, s16* out, int stride)
{
    alignas(16) s32 coef[64] = {};
    s32 sum = 0;
    bool ac;
    if (intra)
    {
        const VlcTable& sizes = DcSizeTable(cc != 0);
        const VlcEntry& e = sizes.entries[b.bits.Peek(sizes.bits)];
        if (e.len == 0)
        {
            b.error = true;
            return;
        }
        b.bits.Skip(e.len);
        u32 size = static_cast<u32>(e.value);
        if (size != 0)
        {
            s32 diff = static_cast<s32>(b.bits.Get(size));
            if (diff < (1 << (size - 1)))
            {
                diff -= (1 << size) - 1;
            }
            dcPredictor += diff;
        }
        coef[0] = dcPredictor * (8 >> b.s.intraDcPrecision);
        sum = coef[0];
        const DctTable& table = b.s.intraVlcTableOne && !b.s.mpeg1 ? DctTableOne() : DctTableZero();
        ac = DecodeCoefficients(b, table, m.intra, 1, true, coef, sum);
    }
    else
    {
        ac = DecodeCoefficients(b, DctTableZero(), m.nonIntra, 0, false, coef, sum);
    }
    if (!b.s.mpeg1 && (sum & 1) == 0)
    {
        // Mismatch control (7.4.4): an even sum makes the last coefficient's lowest bit flip
        coef[63] ^= 1;
        ac = true;
    }
    s32 low = intra ? 0 : -256;
    s32 high = 255;
    if (!ac)
    {
        // Only a DC coefficient: every sample is DC / 8
        s16 v = static_cast<s16>(Saturate((coef[0] + 4) >> 3, low, high));
        for (int y = 0; y < 8; y++)
        {
            for (int x = 0; x < 8; x++)
            {
                out[y * stride + x] = v;
            }
        }
        return;
    }
    InverseDct(coef);
    for (int y = 0; y < 8; y++)
    {
        for (int x = 0; x < 8; x++)
        {
            out[y * stride + x] = static_cast<s16>(Saturate(coef[y * 8 + x], low, high));
        }
    }
}
}

int QuantiserScale(u32 code, bool nonLinear) { return nonLinear ? kNonLinearScale[code & 31] : static_cast<int>(code & 31) * 2; }

u32 DecodeVlc(Bitstream& bits, u32 table, const DecodeSettings& s)
{
    const VlcTable* t;
    switch (table & 3)
    {
    case 0: t = &MbAddressIncrementTable(); break;
    case 1: t = &MbTypeTable(s.pictureType); break;
    case 2: t = &MotionCodeTable(); break;
    default: t = &DmVectorTable(); break;
    }
    const VlcEntry& e = t->entries[bits.Peek(t->bits)];
    if (e.len == 0 || (table == 0 && e.value == MbaStuffing && !s.mpeg1))
    {
        // No such code (stuffing is MPEG-1's only)
        return 0;
    }
    bits.Skip(e.len);
    return (static_cast<u32>(e.value) & 0xFFFF) | (static_cast<u32>(e.len) << 16);
}

u32 DecodeMacroblock(Bitstream& bits, const DecodeSettings& s, const Matrices& m, const MacroblockParams& p, s32 dcPredictor[3],
                     s16 out[384], bool& error)
{
    u32 cbp = 0x3F;
    if (!p.intra)
    {
        const VlcTable& t = CodedBlockPatternTable();
        const VlcEntry& e = t.entries[bits.Peek(t.bits)];
        if (e.len == 0)
        {
            error = true;
            std::memset(out, 0, 384 * sizeof(s16));
            return 0;
        }
        bits.Skip(e.len);
        cbp = static_cast<u32>(e.value);
    }
    BlockContext b{bits, s, s.alternateScan ? g_scans.alternate : g_scans.zigzag, p.quantiserScale, error};
    for (int block = 0; block < 6; block++)
    {
        s16* at;
        int stride;
        if (block < 4)
        {
            // Frame DCT: the four 8 x 8 quarters; field DCT: blocks 0 and 1 the even lines, 2 and 3 the odd
            int x = (block & 1) * 8;
            at = p.fieldDct ? out + (block >> 1) * 16 + x : out + (block >> 1) * 128 + x;
            stride = p.fieldDct ? 32 : 16;
        }
        else
        {
            at = out + 256 + (block - 4) * 64;
            stride = 8;
        }
        if ((cbp & (0x20 >> block)) == 0)
        {
            for (int y = 0; y < 8; y++)
            {
                std::memset(at + y * stride, 0, 8 * sizeof(s16));
            }
            continue;
        }
        int cc = block < 4 ? 0 : block - 3;
        DecodeBlock(b, m, p.intra, cc, dcPredictor[cc], at, stride);
    }
    return cbp;
}

void InverseDct(s32 block[64])
{
    alignas(16) s32 rows[64];
    for (int r = 0; r < 8; r++)
    {
        const s32* x = block + r * 8;
        s32* out = rows + r * 8;
        if ((x[1] | x[2] | x[3] | x[4] | x[5] | x[6] | x[7]) == 0)
        {
            s32 v = static_cast<s32>((s64(g_idct.t[0][0]) * x[0] + (s64(1) << (kIdctBits - kIdctPassBits - 1))) >>
                                     (kIdctBits - kIdctPassBits));
            for (int n = 0; n < 8; n++)
            {
                out[n] = v;
            }
            continue;
        }
        Idct8(x, out, 1, kIdctBits - kIdctPassBits);
    }
    for (int c = 0; c < 8; c++)
    {
        Idct8(rows + c, block + c, 8, kIdctBits + kIdctPassBits);
    }
}

void InverseDctReference(const s32 in[64], double out[64])
{
    for (int y = 0; y < 8; y++)
    {
        for (int x = 0; x < 8; x++)
        {
            double sum = 0;
            for (int v = 0; v < 8; v++)
            {
                for (int u = 0; u < 8; u++)
                {
                    double cu = u == 0 ? std::sqrt(0.5) : 1.0;
                    double cv = v == 0 ? std::sqrt(0.5) : 1.0;
                    sum += cu * cv / 4 * in[v * 8 + u] * std::cos((2 * x + 1) * u * M_PI / 16) *
                           std::cos((2 * y + 1) * v * M_PI / 16);
                }
            }
            out[y * 8 + x] = sum;
        }
    }
}
}
