// The GS's local memory layouts (the GS User's Manual's "page", "block" and "column" arrangements of each pixel format)

#include "gs.h"
#include "backend.h"

#include <vector>

namespace Gs
{
namespace
{
// The blocks of a page: 32 blocks of 256 bytes. A 32 bit page is 8 by 4 blocks of 8 by 8 pixels, a 16 bit one 4 by 8 of 16 by 8,
// an 8 bit one 8 by 4 of 16 by 16, a 4 bit one 4 by 8 of 32 by 16. The blocks are numbered in a Z order of their x and y bits
constexpr u32 Block32(u32 bx, u32 by)
{
    return (bx & 1) | (by & 1) << 1 | (bx & 2) << 1 | (by & 2) << 2 | (bx & 4) << 2;
}

constexpr u32 Block16(u32 bx, u32 by)
{
    return (by & 1) | (bx & 1) << 1 | (by & 2) << 1 | (bx & 2) << 2 | (by & 4) << 2;
}

// PSMCT16S: the 16 bit page's blocks with the x bit 1 and y bit 2 swapped
constexpr u32 Block16S(u32 bx, u32 by)
{
    return (by & 1) | (bx & 1) << 1 | (by & 4) | (by & 2) << 2 | (bx & 2) << 3;
}

// The pixels of a block: 4 columns (each 64 bytes), a 32 bit column 8 by 2 pixels, a 16 bit one 16 by 2
constexpr u32 Column32(u32 x, u32 y)
{
    return (x & 1) | (y & 1) << 1 | (x & 6) << 1 | (y & 6) << 3;
}

constexpr u32 Column16(u32 x, u32 y)
{
    return (x & 8) >> 3 | (x & 1) << 1 | (y & 1) << 2 | (x & 2) << 2 | (x & 4) << 2 | (y & 6) << 4;
}

// The 8 and 4 bit columns (16 by 4 and 32 by 4 pixels) interleave their pixels across the column's words in a pattern that
// changes every other column: the manual's tables
constexpr u8 Column8Table[16][16] = {
    {0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54},
    {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
    {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23},
    {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31},
    {96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86},
    {104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94},
    {65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119},
    {73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127},
    {128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182},
    {136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190},
    {161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151},
    {169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159},
    {224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214},
    {232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222},
    {193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247},
    {201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255},
};

// The 4 bit column table (a column 32 by 4 pixels, 128 nibbles): pixel i of each group of 8 across goes to nibble
// (i & 1) * 8 + (i >> 1) * 32 plus the group's 2, the second row 16 further; the third and fourth rows take the odd nibbles, and
// the halves of each group trade places in every other row pair (starting with the second pair in even columns, the first in odd
// ones)
struct Column4
{
    u16 value[16][32];
};

constexpr Column4 MakeColumn4()
{
    Column4 table = {};
    for (u32 y = 0; y < 16; y++)
    {
        u32 column = y >> 2;
        u32 row = y & 3;
        for (u32 x = 0; x < 32; x++)
        {
            u32 group = x >> 3;
            u32 i = x & 7;
            bool swapped = ((row >> 1) ^ (column & 1)) != 0;
            u32 j = swapped ? i ^ 4 : i;
            u32 nibble = (row & 1) * 16 + group * 2 + (j & 1) * 8 + (j >> 1) * 32 + (row >> 1);
            table.value[y][x] = static_cast<u16>(column * 128 + nibble);
        }
    }

    return table;
}

constexpr Column4 Column4Table = MakeColumn4();

// The 8 bit columns follow the same scheme over bytes
constexpr u32 Column8(u32 x, u32 y)
{
    return Column8Table[y & 15][x & 15];
}

constexpr u32 BlockMask = BlockCount - 1;
}

u32 PixelAddress32(u32 x, u32 y, u32 bp, u32 bw)
{
    u32 page = (y >> 5) * bw + (x >> 6);
    u32 block = (bp + page * 32 + Block32((x >> 3) & 7, (y >> 3) & 3)) & BlockMask;
    return block << 6 | Column32(x & 7, y & 7);
}

u32 PixelAddress32Z(u32 x, u32 y, u32 bp, u32 bw)
{
    u32 page = (y >> 5) * bw + (x >> 6);
    u32 block = ((bp + page * 32 + Block32((x >> 3) & 7, (y >> 3) & 3)) ^ 0x18) & BlockMask;
    return block << 6 | Column32(x & 7, y & 7);
}

u32 PixelAddress16(u32 x, u32 y, u32 bp, u32 bw)
{
    u32 page = (y >> 6) * bw + (x >> 6);
    u32 block = (bp + page * 32 + Block16((x >> 4) & 3, (y >> 3) & 7)) & BlockMask;
    return block << 7 | Column16(x & 15, y & 7);
}

u32 PixelAddress16S(u32 x, u32 y, u32 bp, u32 bw)
{
    u32 page = (y >> 6) * bw + (x >> 6);
    u32 block = (bp + page * 32 + Block16S((x >> 4) & 3, (y >> 3) & 7)) & BlockMask;
    return block << 7 | Column16(x & 15, y & 7);
}

u32 PixelAddress16Z(u32 x, u32 y, u32 bp, u32 bw)
{
    u32 page = (y >> 6) * bw + (x >> 6);
    u32 block = ((bp + page * 32 + Block16((x >> 4) & 3, (y >> 3) & 7)) ^ 0x18) & BlockMask;
    return block << 7 | Column16(x & 15, y & 7);
}

u32 PixelAddress16SZ(u32 x, u32 y, u32 bp, u32 bw)
{
    u32 page = (y >> 6) * bw + (x >> 6);
    u32 block = ((bp + page * 32 + Block16S((x >> 4) & 3, (y >> 3) & 7)) ^ 0x18) & BlockMask;
    return block << 7 | Column16(x & 15, y & 7);
}

// 8 and 4 bit pages are 128 pixels wide: the buffer width (in 64 pixels) is halved for them
u32 PixelAddress8(u32 x, u32 y, u32 bp, u32 bw)
{
    u32 page = (y >> 6) * (bw >> 1) + (x >> 7);
    u32 block = (bp + page * 32 + Block32((x >> 4) & 7, (y >> 4) & 3)) & BlockMask;
    return block << 8 | Column8(x, y);
}

u32 PixelAddress4(u32 x, u32 y, u32 bp, u32 bw)
{
    u32 page = (y >> 7) * (bw >> 1) + (x >> 7);
    u32 block = (bp + page * 32 + Block16((x >> 5) & 3, (y >> 4) & 7)) & BlockMask;
    return block << 9 | Column4Table.value[y & 15][x & 31];
}

u32 BitsPerPixel(u32 psm)
{
    switch (psm)
    {
    case PSMCT32:
    case PSMCT24:
    case PSMT8H:
    case PSMT4HL:
    case PSMT4HH:
    case PSMZ32:
    case PSMZ24:
        return 32;
    case PSMCT16:
    case PSMCT16S:
    case PSMZ16:
    case PSMZ16S:
        return 16;
    case PSMT8:
        return 8;
    case PSMT4:
        return 4;
    default:
        return 0;
    }
}

u32 Gs::ReadPixel(u32 psm, u32 x, u32 y, u32 bp, u32 bw) const
{
    const u32* vm32 = reinterpret_cast<const u32*>(vm_);
    const u16* vm16 = reinterpret_cast<const u16*>(vm_);
    switch (psm)
    {
    case PSMCT32:
        return vm32[PixelAddress32(x, y, bp, bw)];
    case PSMCT24:
        return vm32[PixelAddress32(x, y, bp, bw)] & 0xFFFFFF;
    case PSMCT16:
        return vm16[PixelAddress16(x, y, bp, bw)];
    case PSMCT16S:
        return vm16[PixelAddress16S(x, y, bp, bw)];
    case PSMT8:
        return vm_[PixelAddress8(x, y, bp, bw)];
    case PSMT4:
    {
        u32 address = PixelAddress4(x, y, bp, bw);
        return (vm_[address >> 1] >> ((address & 1) << 2)) & 0xF;
    }
    case PSMT8H:
        return vm32[PixelAddress32(x, y, bp, bw)] >> 24;
    case PSMT4HL:
        return (vm32[PixelAddress32(x, y, bp, bw)] >> 24) & 0xF;
    case PSMT4HH:
        return vm32[PixelAddress32(x, y, bp, bw)] >> 28;
    case PSMZ32:
        return vm32[PixelAddress32Z(x, y, bp, bw)];
    case PSMZ24:
        return vm32[PixelAddress32Z(x, y, bp, bw)] & 0xFFFFFF;
    case PSMZ16:
        return vm16[PixelAddress16Z(x, y, bp, bw)];
    case PSMZ16S:
        return vm16[PixelAddress16SZ(x, y, bp, bw)];
    default:
        return 0;
    }
}

void Gs::WritePixel(u32 psm, u32 x, u32 y, u32 bp, u32 bw, u32 value)
{
    u32* vm32 = reinterpret_cast<u32*>(vm_);
    u16* vm16 = reinterpret_cast<u16*>(vm_);
    switch (psm)
    {
    case PSMCT32:
        {
        u32 a = PixelAddress32(x, y, bp, bw);
        vm32[a] = value;
        MarkWritten(a >> 11);
    }
        break;
    case PSMCT24:
    {
        u32 a = PixelAddress32(x, y, bp, bw);
        MarkWritten(a >> 11);
        u32& word = vm32[a];
        word = (word & 0xFF000000) | (value & 0xFFFFFF);
        break;
    }
    case PSMCT16:
        {
        u32 a = PixelAddress16(x, y, bp, bw);
        vm16[a] = static_cast<u16>(value);
        MarkWritten(a >> 12);
    }
        break;
    case PSMCT16S:
        {
        u32 a = PixelAddress16S(x, y, bp, bw);
        vm16[a] = static_cast<u16>(value);
        MarkWritten(a >> 12);
    }
        break;
    case PSMT8:
        {
        u32 a = PixelAddress8(x, y, bp, bw);
        vm_[a] = static_cast<u8>(value);
        MarkWritten(a >> 13);
    }
        break;
    case PSMT4:
    {
        u32 address = PixelAddress4(x, y, bp, bw);
        MarkWritten(address >> 14);
        u8& byte = vm_[address >> 1];
        u32 shift = (address & 1) << 2;
        byte = static_cast<u8>((byte & ~(0xF << shift)) | (value & 0xF) << shift);
        break;
    }
    case PSMT8H:
    {
        u32 a = PixelAddress32(x, y, bp, bw);
        MarkWritten(a >> 11);
        u32& word = vm32[a];
        word = (word & 0x00FFFFFF) | value << 24;
        break;
    }
    case PSMT4HL:
    {
        u32 a = PixelAddress32(x, y, bp, bw);
        MarkWritten(a >> 11);
        u32& word = vm32[a];
        word = (word & 0xF0FFFFFF) | (value & 0xF) << 24;
        break;
    }
    case PSMT4HH:
    {
        u32 a = PixelAddress32(x, y, bp, bw);
        MarkWritten(a >> 11);
        u32& word = vm32[a];
        word = (word & 0x0FFFFFFF) | (value & 0xF) << 28;
        break;
    }
    case PSMZ32:
        {
        u32 a = PixelAddress32Z(x, y, bp, bw);
        vm32[a] = value;
        MarkWritten(a >> 11);
    }
        break;
    case PSMZ24:
    {
        u32 a = PixelAddress32Z(x, y, bp, bw);
        MarkWritten(a >> 11);
        u32& word = vm32[a];
        word = (word & 0xFF000000) | (value & 0xFFFFFF);
        break;
    }
    case PSMZ16:
        {
        u32 a = PixelAddress16Z(x, y, bp, bw);
        vm16[a] = static_cast<u16>(value);
        MarkWritten(a >> 12);
    }
        break;
    case PSMZ16S:
        {
        u32 a = PixelAddress16SZ(x, y, bp, bw);
        vm16[a] = static_cast<u16>(value);
        MarkWritten(a >> 12);
    }
        break;
    default:
        break;
    }
}

// The 8 KB page a pixel of a format is in
u32 PixelPage(u32 psm, u32 x, u32 y, u32 bp, u32 bw)
{
    switch (psm)
    {
    case PSMCT16:
        return PixelAddress16(x, y, bp, bw) >> 12;
    case PSMCT16S:
        return PixelAddress16S(x, y, bp, bw) >> 12;
    case PSMZ16:
        return PixelAddress16Z(x, y, bp, bw) >> 12;
    case PSMZ16S:
        return PixelAddress16SZ(x, y, bp, bw) >> 12;
    case PSMT8:
        return PixelAddress8(x, y, bp, bw) >> 13;
    case PSMT4:
        return PixelAddress4(x, y, bp, bw) >> 14;
    case PSMZ32:
    case PSMZ24:
        return PixelAddress32Z(x, y, bp, bw) >> 11;
    default:
        return PixelAddress32(x, y, bp, bw) >> 11;
    }
}

void PagesOfRect(u32 psm, u32 bp, u32 bw, s32 left, s32 top, s32 right, s32 bottom, PageSet& pages)
{
    if (right < left || bottom < top)
    {
        return;
    }

    // The pages of a rectangle are a function of its arguments alone and a frame's draws read the same textures again and
    // again: a direct-mapped cache (one per thread) of the answers, so a texture's rectangle is walked once
    struct Entry
    {
        u32 psm = ~0u, bp = 0, bw = 0;
        s32 left = 0, top = 0, right = 0, bottom = 0;
        PageSet pages;
    };

    constexpr u32 CacheSize = 512;
    thread_local std::vector<Entry> cache(CacheSize);
    u32 hash = psm * 0x9E3779B1u;
    hash = (hash ^ bp) * 0x85EBCA6Bu;
    hash = (hash ^ bw) * 0xC2B2AE35u;
    hash = (hash ^ static_cast<u32>(left)) * 0x27D4EB2Fu;
    hash = (hash ^ static_cast<u32>(top)) * 0x165667B1u;
    hash = (hash ^ static_cast<u32>(right)) * 0x9E3779B1u;
    hash = (hash ^ static_cast<u32>(bottom)) * 0x85EBCA6Bu;
    Entry& entry = cache[(hash ^ (hash >> 15)) & (CacheSize - 1)];
    if (entry.psm == psm && entry.bp == bp && entry.bw == bw && entry.left == left && entry.top == top && entry.right == right &&
        entry.bottom == bottom)
    {
        pages |= entry.pages;
        return;
    }

    PageSet found;

    // Every block of every format is at least 8 pixels each way: a pixel every 8 (and the last row and column) meets them all
    auto columns = [&](s32 y) {
        for (s32 x = left;; x += 8)
        {
            s32 at = std::min(x, right);
            found.set(PixelPage(psm, static_cast<u32>(at) & 2047, static_cast<u32>(y) & 2047, bp, bw) & (PageCount - 1));
            if (at == right)
            {
                break;
            }
        }
    };
    for (s32 y = top;; y += 8)
    {
        s32 at = std::min(y, bottom);
        columns(at);
        if (at == bottom)
        {
            break;
        }
    }

    entry.psm = psm;
    entry.bp = bp;
    entry.bw = bw;
    entry.left = left;
    entry.top = top;
    entry.right = right;
    entry.bottom = bottom;
    entry.pages = found;
    pages |= found;
}

void FullPagesOfRect(u32 psm, u32 bp, u32 bw, s32 left, s32 top, s32 right, s32 bottom, PageSet& pages)
{
    // The rectangle's whole 8x8 cells (every format's block is a whole number of them, so a cell is in one page), counted by
    // page: a page is covered when all its cells are
    u32 bits = BitsPerPixel(psm == PSMCT24 || psm == PSMZ24 ? PSMCT32 : psm);
    if (bits == 0 || right < left || bottom < top)
    {
        return;
    }

    const u32 cellsInPage = 8192 * 8 / bits / 64;
    s32 x0 = (left + 7) & ~7;
    s32 y0 = (top + 7) & ~7;
    s32 x1 = (right + 1) & ~7;
    s32 y1 = (bottom + 1) & ~7;
    if (x1 <= x0 || y1 <= y0)
    {
        return;
    }

    std::vector<u16> counts(PageCount, 0);
    for (s32 y = y0; y < y1; y += 8)
    {
        for (s32 x = x0; x < x1; x += 8)
        {
            counts[PixelPage(psm, static_cast<u32>(x) & 2047, static_cast<u32>(y) & 2047, bp, bw) & (PageCount - 1)]++;
        }
    }

    for (u32 page = 0; page < PageCount; page++)
    {
        if (counts[page] >= cellsInPage)
        {
            pages.set(page);
        }
    }
}
}
