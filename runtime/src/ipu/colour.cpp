// The conversion's arithmetic is the IPU's own integer method (as the EE User's Manual gives it, ITU-R BT.601 in 1/128
// steps, each product shifted down 6 bits before the sum's rounding shift): the same pictures as the console's, not a
// floating point approximation
#include "colour.h"

#include <algorithm>
#include <climits>
#include <cstring>

namespace Ps2::Ipu
{
namespace
{
inline u8 Clamp255(s32 v) { return static_cast<u8>(std::clamp(v, 0, 255)); }

// The 4 x 4 dither added before RGB16's truncation to 5 bits
constexpr s32 kDither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
}

void YcbcrToRgb32(const Raw8& in, const Thresholds& th, bool sign, u8 out[1024])
{
    // Each chroma sample's terms once for its 2 x 2 pixels, then each row's pixels in one loop the compiler vectorises
    for (int cy = 0; cy < 8; cy++)
    {
        s32 rc[8], gc[8], bc[8];
        for (int cx = 0; cx < 8; cx++)
        {
            s32 cb = in.cb[cy * 8 + cx] - 128;
            s32 cr = in.cr[cy * 8 + cx] - 128;
            rc[cx] = (0xCC * cr) >> 6;
            gc[cx] = ((-0x68 * cr) >> 6) + ((-0x32 * cb) >> 6);
            bc[cx] = (0x102 * cb) >> 6;
        }
        for (int y = cy * 2; y < cy * 2 + 2; y++)
        {
            u32 row[16];
            for (int x = 0; x < 16; x++)
            {
                s32 luma = (0x95 * std::max(in.y[y * 16 + x] - 16, 0)) >> 6;
                u32 r = Clamp255((luma + rc[x >> 1] + 1) >> 1);
                u32 g = Clamp255((luma + gc[x >> 1] + 1) >> 1);
                u32 b = Clamp255((luma + bc[x >> 1] + 1) >> 1);
                row[x] = r | (g << 8) | (b << 16) | 0x80000000u;
            }
            std::memcpy(out + y * 64, row, sizeof(row)); // R, G, B, A in memory order
        }
    }
    if (th.th0 != 0 || th.th1 != 0)
    {
        for (int i = 0; i < 256; i++)
        {
            u8* p = out + i * 4;
            if (p[0] < th.th0 && p[1] < th.th0 && p[2] < th.th0)
            {
                p[0] = p[1] = p[2] = p[3] = 0;
            }
            else if (p[0] < th.th1 && p[1] < th.th1 && p[2] < th.th1)
            {
                p[3] = 0x40;
            }
        }
    }
    if (sign)
    {
        for (int i = 0; i < 256; i++)
        {
            out[i * 4] ^= 0x80;
            out[i * 4 + 1] ^= 0x80;
            out[i * 4 + 2] ^= 0x80;
        }
    }
}

void Rgb32ToRgb16(const u8 in[1024], bool dither, u16 out[256])
{
    for (int y = 0; y < 16; y++)
    {
        for (int x = 0; x < 16; x++)
        {
            const u8* p = in + (y * 16 + x) * 4;
            s32 d = dither ? kDither[y & 3][x & 3] : 0;
            u32 r = Clamp255(p[0] + d) >> 3;
            u32 g = Clamp255(p[1] + d) >> 3;
            u32 b = Clamp255(p[2] + d) >> 3;
            u32 a = p[3] == 0x40 ? 1 : 0;
            out[y * 16 + x] = static_cast<u16>(r | (g << 5) | (b << 10) | (a << 15));
        }
    }
}

void Rgb16ToIndx4(const u16 in[256], const u16 clut[16], u8 out[128])
{
    for (int i = 0; i < 256; i++)
    {
        s32 r = in[i] & 31, g = (in[i] >> 5) & 31, b = (in[i] >> 10) & 31;
        u32 best = 0;
        s32 bestDistance = INT_MAX;
        for (u32 k = 0; k < 16; k++)
        {
            s32 dr = r - (clut[k] & 31), dg = g - ((clut[k] >> 5) & 31), db = b - ((clut[k] >> 10) & 31);
            s32 distance = dr * dr + dg * dg + db * db;
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = k;
            }
        }
        if (i & 1)
        {
            out[i >> 1] |= static_cast<u8>(best << 4);
        }
        else
        {
            out[i >> 1] = static_cast<u8>(best);
        }
    }
}
}
