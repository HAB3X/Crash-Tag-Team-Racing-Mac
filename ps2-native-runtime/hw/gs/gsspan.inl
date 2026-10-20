// The rasteriser's fast path (included by gsdraw.cpp): a row's run of pixels through the same pipeline as ShadePixel, with the
// frame's and depth buffer's addresses taken from a row's base and a column's offset (the GS's layouts add the parts x and y
// give), the attributes stepped across the row, and the pipeline's main choices made template parameters

namespace
{
enum SpanFeature : u32
{
    SpanTextured = 1,
    SpanBlend = 2,
    SpanDepthTest = 4,
    SpanFrame16 = 8,
    SpanGouraud = 16,
    SpanAlphaTest = 32,
    // Drawing at a scale into the shadows (their samples a word each, a row after another)
    SpanShadowed = 64,
};

// Where a buffer's pixels are: an address is (row base + column offset) masked to local memory, in the format's units (words
// for 32 and 24 bit, half words for 16 bit)
struct BufferLayout
{
    bool usable = false;
    bool sixteen = false;
    bool z = false;
    u32 bp = 0;
    u32 bw = 0;
    u32 psm = 0;
    u32 mask = 0;
    std::vector<u32> columns;
};

// The layout of a buffer, when its format adds up that way (Z formats need a base on a page: their blocks' XOR)
const BufferLayout& Layout(u32 psm, u32 bp, u32 bw)
{
    static BufferLayout layouts[8];
    static u32 next = 0;
    for (const BufferLayout& layout : layouts)
    {
        if (layout.usable && layout.psm == psm && layout.bp == bp && layout.bw == bw)
        {
            return layout;
        }
    }

    BufferLayout& layout = layouts[next];
    next = (next + 1) % 8;
    layout = BufferLayout{};
    layout.psm = psm;
    layout.bp = bp;
    layout.bw = bw;
    bool zFormat = psm == PSMZ32 || psm == PSMZ24 || psm == PSMZ16 || psm == PSMZ16S;
    bool supported = psm == PSMCT32 || psm == PSMCT24 || psm == PSMCT16 || psm == PSMCT16S || zFormat;
    if (!supported || (zFormat && (bp & 31) != 0))
    {
        return layout;
    }

    layout.sixteen = psm == PSMCT16 || psm == PSMCT16S || psm == PSMZ16 || psm == PSMZ16S;
    layout.z = zFormat;
    layout.mask = layout.sixteen ? 0x1FFFFF : 0xFFFFF;
    // The column offsets: the address of (x, 0) less the address of (0, 0), with the base left out
    layout.columns.resize(2048);
    auto address = [&](u32 x, u32 y) -> u32 {
        switch (psm)
        {
        case PSMCT16:
            return PixelAddress16(x, y, 0, bw);
        case PSMCT16S:
            return PixelAddress16S(x, y, 0, bw);
        case PSMZ16:
            return PixelAddress16Z(x, y, 0, bw);
        case PSMZ16S:
            return PixelAddress16SZ(x, y, 0, bw);
        case PSMZ32:
        case PSMZ24:
            return PixelAddress32Z(x, y, 0, bw);
        default:
            return PixelAddress32(x, y, 0, bw);
        }
    };
    u32 origin = address(0, 0);
    for (u32 x = 0; x < 2048; x++)
    {
        layout.columns[x] = address(x, 0) - origin;
    }

    layout.usable = true;
    return layout;
}

// A row's base address: (0, y)'s with the buffer's base
u32 RowBase(const BufferLayout& layout, u32 y)
{
    u32 unit = layout.sixteen ? 7 : 6;
    switch (layout.psm)
    {
    case PSMCT16:
        return PixelAddress16(0, y, 0, layout.bw) + (layout.bp << unit);
    case PSMCT16S:
        return PixelAddress16S(0, y, 0, layout.bw) + (layout.bp << unit);
    case PSMZ16:
        return PixelAddress16Z(0, y, 0, layout.bw) + (layout.bp << unit);
    case PSMZ16S:
        return PixelAddress16SZ(0, y, 0, layout.bw) + (layout.bp << unit);
    case PSMZ32:
    case PSMZ24:
        return PixelAddress32Z(0, y, 0, layout.bw) + (layout.bp << unit);
    default:
        return PixelAddress32(0, y, 0, layout.bw) + (layout.bp << unit);
    }
}

// A row's run: its pixels from x0 to x1 (inclusive) and the attributes at x0 and their steps a pixel to the right
struct Span
{
    s32 y;
    s32 x0;
    s32 x1;
    double z;
    double dz;
    f32 colour[4];
    f32 dcolour[4];
    f32 f;
    f32 df;
    f32 s;
    f32 t;
    f32 q;
    f32 ds;
    f32 dt;
    f32 dq;
    // Whether S and T are divided by Q at each pixel (STQ coordinates of a triangle or line; a sprite's come divided)
    bool divide;
};

template <u32 Features>
void ShadeSpan(Gs& gs, const Gs::DrawState& state, const Pipeline& p, const Span& span, const RegTex0& tex0,
               const BufferLayout& frameLayout, const BufferLayout& depthLayout)
{
    constexpr bool Textured = (Features & SpanTextured) != 0;
    constexpr bool Blend = (Features & SpanBlend) != 0;
    constexpr bool DepthTest = (Features & SpanDepthTest) != 0;
    constexpr bool Frame16 = (Features & SpanFrame16) != 0;
    constexpr bool Gouraud = (Features & SpanGouraud) != 0;
    constexpr bool AlphaTest = (Features & SpanAlphaTest) != 0;
    constexpr bool Shadowed = (Features & SpanShadowed) != 0;

    u8* vm = gs.memory();
    auto* vm32 = reinterpret_cast<u32*>(vm);
    auto* vm16 = reinterpret_cast<u16*>(vm);
    u32 frameRow;
    u32 depthRow = 0;
    Gs::Shadow* frameShadow = state.frameShadow;
    Gs::Shadow* depthShadow = state.depthShadow;
    if constexpr (Shadowed)
    {
        // Rows past the shadows' aren't drawn
        if (static_cast<u32>(span.y) >= frameShadow->height * frameShadow->scale ||
            (depthShadow != nullptr && static_cast<u32>(span.y) >= depthShadow->height * depthShadow->scale))
        {
            return;
        }

        frameRow = static_cast<u32>(span.y) * frameShadow->width * frameShadow->scale;
        depthRow = depthShadow != nullptr ? static_cast<u32>(span.y) * depthShadow->width * depthShadow->scale : 0;
    }
    else
    {
        frameRow = RowBase(frameLayout, static_cast<u32>(span.y));
        depthRow = depthLayout.usable ? RowBase(depthLayout, static_cast<u32>(span.y)) : 0;
    }
    const u32 fpsm = state.fpsm;
    const bool frame24 = fpsm == PSMCT24;
    const bool depth16 = state.zpsm == PSMZ16 || state.zpsm == PSMZ16S;
    const bool depth24 = state.zpsm == PSMZ24;
    const u32 frameShift = Frame16 ? 12 : 11;
    const u32 depthShift = depth16 ? 12 : 11;
    const u32 vr0 = 0;
    (void)vr0;

    // The flat colour (the primitive's last vertex's) with 7 fraction bits
    s32 flat[4];
    for (u32 c = 0; c < 4; c++)
    {
        flat[c] = static_cast<s32>(span.colour[c]) << 7;
    }

    for (s32 x = span.x0; x <= span.x1; x++)
    {
        f32 i = static_cast<f32>(x - span.x0);
        u32 frameAddress;
        u32 depthAddress;
        if constexpr (Shadowed)
        {
            if (static_cast<u32>(x) >= frameShadow->width * frameShadow->scale ||
                (depthShadow != nullptr && static_cast<u32>(x) >= depthShadow->width * depthShadow->scale))
            {
                continue;
            }

            frameAddress = frameRow + static_cast<u32>(x);
            depthAddress = depthRow + static_cast<u32>(x);
        }
        else
        {
            frameAddress = (frameRow + frameLayout.columns[static_cast<u32>(x) & 2047]) & frameLayout.mask;
            depthAddress = depthLayout.usable ? (depthRow + depthLayout.columns[static_cast<u32>(x) & 2047]) & depthLayout.mask : 0;
        }

        // The depth test
        double zv = span.z + span.dz * static_cast<double>(x - span.x0);
        u32 z = zv <= 0.0 ? 0u : zv >= 4294967295.0 ? 0xFFFFFFFFu : static_cast<u32>(zv);
        z = std::min(z, state.zmax);
        if constexpr (DepthTest)
        {
            u32 zd;
            if constexpr (Shadowed)
            {
                zd = depthShadow->samples[depthAddress];
            }
            else
            {
                zd = depth16 ? vm16[depthAddress] : vm32[depthAddress];
            }
            if (depth24)
            {
                zd &= 0xFFFFFF;
            }

            if (state.ztst == ZtstGequal ? z < zd : z <= zd)
            {
                continue;
            }
        }

        s32 cr;
        s32 cg;
        s32 cb;
        s32 ca;
        if constexpr (Gouraud)
        {
            cr = std::clamp(static_cast<s32>((span.colour[0] + span.dcolour[0] * i) * 128.0f), 0, 255 << 7);
            cg = std::clamp(static_cast<s32>((span.colour[1] + span.dcolour[1] * i) * 128.0f), 0, 255 << 7);
            cb = std::clamp(static_cast<s32>((span.colour[2] + span.dcolour[2] * i) * 128.0f), 0, 255 << 7);
            ca = std::clamp(static_cast<s32>((span.colour[3] + span.dcolour[3] * i) * 128.0f), 0, 255 << 7);
        }
        else
        {
            cr = flat[0];
            cg = flat[1];
            cb = flat[2];
            ca = flat[3];
        }

        u32 va = static_cast<u32>(ca >> 7);
        u32 r = static_cast<u32>(cr >> 7);
        u32 g = static_cast<u32>(cg >> 7);
        u32 b = static_cast<u32>(cb >> 7);
        u32 a = va;
        if constexpr (Textured)
        {
            f32 s = span.s + span.ds * i;
            f32 t = span.t + span.dt * i;
            f32 q = span.q + span.dq * i;
            s32 u;
            s32 v;
            if (!span.divide)
            {
                u = ToFixed(s);
                v = ToFixed(t);
            }
            else
            {
                u = ToFixed(s / q);
                v = ToFixed(t / q);
            }

            u32 texel = SampleTexture(gs, state, u, v, q, p, tex0);
            u32 tr = texel & 0xFF;
            u32 tg = (texel >> 8) & 0xFF;
            u32 tb = (texel >> 16) & 0xFF;
            u32 ta = texel >> 24;
            switch (state.tfx)
            {
            case TfxModulate:
                r = std::min<u32>((tr * static_cast<u32>(cr)) >> 14, 255);
                g = std::min<u32>((tg * static_cast<u32>(cg)) >> 14, 255);
                b = std::min<u32>((tb * static_cast<u32>(cb)) >> 14, 255);
                a = state.tcc ? std::min<u32>((ta * static_cast<u32>(ca)) >> 14, 255) : va;
                break;
            case TfxDecal:
                r = tr;
                g = tg;
                b = tb;
                a = state.tcc ? ta : va;
                break;
            case TfxHighlight:
                r = std::min<u32>(((tr * static_cast<u32>(cr)) >> 14) + va, 255);
                g = std::min<u32>(((tg * static_cast<u32>(cg)) >> 14) + va, 255);
                b = std::min<u32>(((tb * static_cast<u32>(cb)) >> 14) + va, 255);
                a = state.tcc ? std::min<u32>(ta + va, 255) : va;
                break;
            default:
                r = std::min<u32>(((tr * static_cast<u32>(cr)) >> 14) + va, 255);
                g = std::min<u32>(((tg * static_cast<u32>(cg)) >> 14) + va, 255);
                b = std::min<u32>(((tb * static_cast<u32>(cb)) >> 14) + va, 255);
                a = state.tcc ? ta : va;
                break;
            }
        }

        bool writeFrame = state.frameWritten;
        bool writeDepth = state.zWritten;
        u32 alphaMask = 0;
        if constexpr (AlphaTest)
        {
            bool pass;
            switch (state.atst)
            {
            case AtstNever:
                pass = false;
                break;
            case AtstAlways:
                pass = true;
                break;
            case AtstLess:
                pass = a < state.aref;
                break;
            case AtstLequal:
                pass = a <= state.aref;
                break;
            case AtstEqual:
                pass = a == state.aref;
                break;
            case AtstGequal:
                pass = a >= state.aref;
                break;
            case AtstGreater:
                pass = a > state.aref;
                break;
            default:
                pass = a != state.aref;
                break;
            }

            if (!pass)
            {
                switch (state.afail)
                {
                case AfailKeep:
                    continue;
                case AfailFbOnly:
                    writeDepth = false;
                    break;
                case AfailZbOnly:
                    writeFrame = false;
                    break;
                default:
                    writeDepth = false;
                    alphaMask = 0xFF000000u;
                    break;
                }
            }
        }

        if (state.fog)
        {
            u32 f = static_cast<u32>(std::clamp(span.f + span.df * i, 0.0f, 255.0f));
            r = (f * r + (255 - f) * state.fogR) >> 8;
            g = (f * g + (255 - f) * state.fogG) >> 8;
            b = (f * b + (255 - f) * state.fogB) >> 8;
        }

        u32 mask = state.fbmsk | alphaMask;
        bool readsFrame = Blend || state.date || mask != 0;
        u32 fd = 0;
        if (readsFrame)
        {
            if constexpr (Shadowed)
            {
                fd = frameShadow->samples[frameAddress];
                if (frame24)
                {
                    fd &= 0xFFFFFF;
                }
            }
            else
            {
                fd = Frame16 ? vm16[frameAddress] : vm32[frameAddress];
            }
        }

        if (state.date)
        {
            u32 bit = Frame16 ? (fd >> 15) & 1 : fd >> 31;
            if (bit != state.datm)
            {
                continue;
            }
        }

        if constexpr (Shadowed)
        {
            if (writeDepth && depthShadow != nullptr)
            {
                u32& sample = depthShadow->samples[depthAddress];
                sample = depth16 ? (z & 0xFFFF) : depth24 ? (sample & 0xFF000000) | (z & 0xFFFFFF) : z;
            }
        }
        else if (writeDepth && depthLayout.usable)
        {
            if (depth16)
            {
                vm16[depthAddress] = static_cast<u16>(z);
            }
            else if (depth24)
            {
                vm32[depthAddress] = (vm32[depthAddress] & 0xFF000000) | (z & 0xFFFFFF);
            }
            else
            {
                vm32[depthAddress] = z;
            }

            gs.MarkWritten(depthAddress >> depthShift);
        }

        if (!writeFrame)
        {
            continue;
        }

        s32 sr = static_cast<s32>(r);
        s32 sg = static_cast<s32>(g);
        s32 sb = static_cast<s32>(b);
        if constexpr (Blend)
        {
            if (!(state.pabe && (a & 0x80) == 0))
            {
                s32 dr;
                s32 dg;
                s32 db;
                s32 da;
                if constexpr (Frame16)
                {
                    dr = static_cast<s32>((fd & 0x1F) << 3);
                    dg = static_cast<s32>(((fd >> 5) & 0x1F) << 3);
                    db = static_cast<s32>(((fd >> 10) & 0x1F) << 3);
                    da = (fd & 0x8000) ? 0x80 : 0;
                }
                else
                {
                    dr = static_cast<s32>(fd & 0xFF);
                    dg = static_cast<s32>((fd >> 8) & 0xFF);
                    db = static_cast<s32>((fd >> 16) & 0xFF);
                    da = frame24 ? 0x80 : static_cast<s32>(fd >> 24);
                }

                auto pick = [](u32 select, s32 source, s32 destination) {
                    return select == 0 ? source : select == 1 ? destination : 0;
                };
                s32 c = state.blendC == 0 ? static_cast<s32>(a) : state.blendC == 1 ? da : static_cast<s32>(state.blendFix);
                sr = (((pick(state.blendA, sr, dr) - pick(state.blendB, sr, dr)) * c) >> 7) + pick(state.blendD, sr, dr);
                sg = (((pick(state.blendA, sg, dg) - pick(state.blendB, sg, dg)) * c) >> 7) + pick(state.blendD, sg, dg);
                sb = (((pick(state.blendA, sb, db) - pick(state.blendB, sb, db)) * c) >> 7) + pick(state.blendD, sb, db);
            }
        }

        if (state.dither)
        {
            u32 shift = (((static_cast<u32>(span.y) / state.scale) & 3) * 4 + ((static_cast<u32>(x) / state.scale) & 3)) * 4;
            s32 dm = static_cast<s32>((p.dimx >> shift) & 7);
            if (dm & 4)
            {
                dm -= 8;
            }

            sr += dm;
            sg += dm;
            sb += dm;
        }

        u32 fr;
        u32 fg;
        u32 fb;
        if (state.colclamp)
        {
            fr = Clamp255(sr);
            fg = Clamp255(sg);
            fb = Clamp255(sb);
        }
        else
        {
            fr = static_cast<u32>(sr) & 0xFF;
            fg = static_cast<u32>(sg) & 0xFF;
            fb = static_cast<u32>(sb) & 0xFF;
        }

        u32 fa = a | (state.fba ? 0x80u : 0u);
        if constexpr (Frame16)
        {
            u32 c16 = (fr >> 3) | (fg >> 3) << 5 | (fb >> 3) << 10 | (fa & 0x80) << 8;
            u32 m16 = ((mask >> 3) & 0x1F) | ((mask >> 6) & 0x3E0) | ((mask >> 9) & 0x7C00) | ((mask >> 16) & 0x8000);
            if constexpr (Shadowed)
            {
                frameShadow->samples[frameAddress] = (c16 & ~m16) | (fd & m16);
            }
            else
            {
                vm16[frameAddress] = static_cast<u16>((c16 & ~m16) | (fd & m16));
            }
        }
        else
        {
            u32 colour = fr | fg << 8 | fb << 16 | fa << 24;
            if constexpr (Shadowed)
            {
                u32& sample = frameShadow->samples[frameAddress];
                u32 kept = frame24 ? (mask | 0xFF000000u) : mask;
                sample = (colour & ~kept) | (sample & kept);
            }
            else
            {
                vm32[frameAddress] = (colour & ~mask) | (fd & mask);
            }
        }

        if constexpr (!Shadowed)
        {
            gs.MarkWritten(frameAddress >> frameShift);
        }
    }
}

using SpanShader = void (*)(Gs&, const Gs::DrawState&, const Pipeline&, const Span&, const RegTex0&, const BufferLayout&,
                            const BufferLayout&);

template <u32... Features>
constexpr std::array<SpanShader, sizeof...(Features)> MakeSpanShaders(std::integer_sequence<u32, Features...>)
{
    return {&ShadeSpan<Features>...};
}

const std::array<SpanShader, 128> g_SpanShaders = MakeSpanShaders(std::make_integer_sequence<u32, 128>{});

// The span shader a primitive's state takes, or none when the frame or depth buffer's layout doesn't add up (the pixels then go
// through ShadePixel)
SpanShader SpanShaderFor(const Gs::DrawState& state, const BufferLayout& frame, const BufferLayout& depth)
{
    bool depthTest = state.ztst == ZtstGequal || state.ztst == ZtstGreater;
    bool shadowed = state.frameShadow != nullptr;
    if (state.ztst == ZtstNever)
    {
        return nullptr;
    }

    if (shadowed)
    {
        // The frame's and depth's formats as the shadows keep them
        bool frameColour = state.fpsm == PSMCT32 || state.fpsm == PSMCT24 || state.fpsm == PSMCT16 || state.fpsm == PSMCT16S;
        if (!frameColour || ((depthTest || state.zWritten) && state.depthShadow == nullptr))
        {
            return nullptr;
        }
    }
    else
    {
        if (!frame.usable || frame.z)
        {
            return nullptr;
        }

        if ((depthTest || state.zWritten) && !depth.usable)
        {
            return nullptr;
        }
    }

    u32 features = 0;
    features |= state.textured ? SpanTextured : 0;
    features |= state.blend ? SpanBlend : 0;
    features |= depthTest ? SpanDepthTest : 0;
    features |= frame.sixteen ? SpanFrame16 : 0;
    features |= state.gouraud ? SpanGouraud : 0;
    features |= state.alphaTest ? SpanAlphaTest : 0;
    features |= shadowed ? SpanShadowed : 0;
    if (shadowed)
    {
        features &= ~static_cast<u32>(SpanFrame16);
        features |= (state.fpsm == PSMCT16 || state.fpsm == PSMCT16S) ? SpanFrame16 : 0;
    }
    return g_SpanShaders[features];
}
}
