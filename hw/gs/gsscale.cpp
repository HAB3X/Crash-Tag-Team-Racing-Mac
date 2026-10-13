// Drawing at a multiple of the PS2's resolution: the shadows of the buffers the GS draws into (gs.h's SetScale)

#include "gs.h"

#include <algorithm>

namespace Gs
{
namespace
{
// A shadow's lines (in the PS2's pixels): the frame's height. Rows past it aren't shadowed
constexpr u32 ShadowLines = 512;
constexpr u32 Pages = LocalMemoryBytes / 8192;

enum ShadowKind : u32
{
    KindColour32 = 0,
    KindColour16 = 1,
    KindColour16S = 2,
    KindDepth32 = 3,
    KindDepth16 = 4,
    KindDepth16S = 5,
    KindNone = 6,
};

u32 KindOf(u32 psm)
{
    switch (psm)
    {
    case PSMCT32:
    case PSMCT24:
        return KindColour32;
    case PSMCT16:
        return KindColour16;
    case PSMCT16S:
        return KindColour16S;
    case PSMZ32:
    case PSMZ24:
        return KindDepth32;
    case PSMZ16:
        return KindDepth16;
    case PSMZ16S:
        return KindDepth16S;
    default:
        return KindNone;
    }
}

// The format a shadow keeps its kind's words in (24 bit buffers keep the whole word)
u32 StoredFormat(u32 kind)
{
    static constexpr u32 Formats[] = {PSMCT32, PSMCT16, PSMCT16S, PSMZ32, PSMZ16, PSMZ16S};
    return Formats[kind];
}
}

void Gs::SetScale(u32 scale)
{
    scale = std::clamp<u32>(scale, 1, 4);
    if (scale == scale_)
    {
        return;
    }

    scale_ = scale;
    for (Shadow* shadow : shadows_)
    {
        delete shadow;
    }

    shadows_.clear();
}

Gs::Shadow* Gs::ShadowFor(u32 psm, u32 bp, u32 bw, bool make)
{
    u32 kind = KindOf(psm);
    if (kind == KindNone || bw == 0)
    {
        return nullptr;
    }

    for (Shadow* shadow : shadows_)
    {
        if (shadow->kind == kind && shadow->bp == bp && shadow->bw == bw)
        {
            return shadow;
        }
    }

    if (!make)
    {
        return nullptr;
    }

    auto* shadow = new Shadow();
    shadow->bp = bp;
    shadow->bw = bw;
    shadow->kind = kind;
    shadow->psm = StoredFormat(kind);
    shadow->width = bw * 64;
    shadow->height = ShadowLines;
    shadow->scale = scale_;
    shadow->samples.assign(static_cast<size_t>(shadow->width) * scale_ * shadow->height * scale_, 0);
    shadow->synced.assign(Pages, 0);
    shadow->pagePixels.resize(Pages);
    for (u32 y = 0; y < shadow->height; y++)
    {
        for (u32 x = 0; x < shadow->width; x++)
        {
            u32 page = PixelPage(shadow->psm, x, y, bp, bw) & (Pages - 1);
            shadow->pagePixels[page].push_back(y << 11 | x);
        }
    }

    // A fresh shadow takes every page from local memory
    shadows_.push_back(shadow);
    std::vector<u32> saved(Pages);
    for (u32 page = 0; page < Pages; page++)
    {
        saved[page] = pageWritten_[page];
        pageWritten_[page] = std::max<u32>(pageWritten_[page], 1);
    }

    SyncShadow(*shadow);
    for (u32 page = 0; page < Pages; page++)
    {
        pageWritten_[page] = saved[page];
    }

    return shadow;
}

void Gs::SyncShadow(Shadow& shadow)
{
    const u32 n = shadow.scale;
    const u32 rowSamples = shadow.width * n;
    bool changed = false;
    for (u32 page = 0; page < Pages; page++)
    {
        const std::vector<u32>& pixels = shadow.pagePixels[page];
        if (pixels.empty() || pageWritten_[page] <= shadow.synced[page])
        {
            continue;
        }

        for (u32 packed : pixels)
        {
            u32 x = packed & 2047;
            u32 y = packed >> 11;
            u32 value = ReadPixel(shadow.psm, x, y, shadow.bp, shadow.bw);
            u32* row = shadow.samples.data() + static_cast<size_t>(y * n) * rowSamples + x * n;
            for (u32 sy = 0; sy < n; sy++)
            {
                std::fill(row + static_cast<size_t>(sy) * rowSamples, row + static_cast<size_t>(sy) * rowSamples + n, value);
            }
        }

        shadow.synced[page] = generation_;
        changed = true;
    }

    if (changed)
    {
        shadow.version++;
    }
}
}
