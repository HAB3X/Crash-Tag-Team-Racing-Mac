#pragma once

// The GS's drawing state shared by the software rasteriser (gsdraw.cpp) and the hardware renderer (hw/): a primitive's
// registers worked out once (Gs::SetUpState), its texture levels

#include "gs.h"

namespace Gs
{
enum PrimitiveType : u32
{
    PrimPoint = 0,
    PrimLine = 1,
    PrimLineStrip = 2,
    PrimTriangle = 3,
    PrimTriangleStrip = 4,
    PrimTriangleFan = 5,
    PrimSprite = 6,
};

enum TextureFunction : u32
{
    TfxModulate = 0,
    TfxDecal = 1,
    TfxHighlight = 2,
    TfxHighlight2 = 3,
};

enum AlphaTest : u32
{
    AtstNever = 0,
    AtstAlways = 1,
    AtstLess = 2,
    AtstLequal = 3,
    AtstEqual = 4,
    AtstGequal = 5,
    AtstGreater = 6,
    AtstNotequal = 7,
};

enum AlphaFail : u32
{
    AfailKeep = 0,
    AfailFbOnly = 1,
    AfailZbOnly = 2,
    AfailRgbOnly = 3,
};

enum DepthTest : u32
{
    ZtstNever = 0,
    ZtstAlways = 1,
    ZtstGequal = 2,
    ZtstGreater = 3,
};

enum WrapMode : u32
{
    WrapRepeat = 0,
    WrapClamp = 1,
    WrapRegionClamp = 2,
    WrapRegionRepeat = 3,
};

// A texture level: its base, buffer width, format, size, and its CLAMP region (shifted for the level)
struct TextureLevel
{
    u32 bp;
    u32 bw;
    u32 psm;
    u32 width;
    u32 height;
    u32 minu;
    u32 maxu;
    u32 minv;
    u32 maxv;
    // The level's texels decoded (RGBA, alpha 0x80 = 1.0: the TEXA and CLUT expansion done), a row after another
    const u32* texels;
    // Drawing at a scale, a texture read from a shadow: its samples a texel's width (the sizes and CLAMP's region then count
    // samples), how many there are a row and how many rows (the shadow's; past them the texel is local memory's)
    u32 scale;
    u32 stride;
    u32 rows;
    // The decoded texture the texels belong to and its version (changed whenever its texels do), for the hardware renderer's
    // texture cache; null for texels that aren't a cached decode
    const void* cacheId;
    u32 version;
    // Changed with every tile decoded too (the texels a draw hasn't read yet)
    u32 tilesVersion;
};

// What a primitive draws with: its context's registers and the attributes, worked out once
struct Gs::DrawState
{
    const Context* context;
    u32 type;
    bool gouraud;
    bool textured;
    bool fog;
    bool blend;
    bool uv;
    // The frame: its base (blocks), width, format and mask, and whether it's written at all
    u32 fbp;
    u32 fbw;
    u32 fpsm;
    u32 fbmsk;
    bool frameWritten;
    // The depth buffer: its base, format and its largest value, whether it's tested and written
    u32 zbp;
    u32 zpsm;
    u32 zmax;
    bool zWritten;
    u32 ztst;
    // The tests
    bool alphaTest;
    u32 atst;
    u32 aref;
    u32 afail;
    bool date;
    u32 datm;
    // The blend
    u32 blendA;
    u32 blendB;
    u32 blendC;
    u32 blendD;
    u32 blendFix;
    bool pabe;
    bool colclamp;
    bool fba;
    bool dither;
    // The scissor rectangle (pixels, inclusive)
    s32 scissorLeft;
    s32 scissorRight;
    s32 scissorTop;
    s32 scissorBottom;
    s32 offsetX;
    s32 offsetY;
    // The texture: its levels, the function, colour component, TEXA, filters and the level of detail
    TextureLevel levels[7];
    u32 levelCount;
    u32 tfx;
    bool tcc;
    u32 wms;
    u32 wmt;
    bool magLinear;
    u32 mmin;
    bool mipmapped;
    bool lodConstant;
    f32 k;
    u32 l;
    u32 mxl;
    // The fog colour
    u32 fogR;
    u32 fogG;
    u32 fogB;
    // Drawing at a scale: the shadows drawn into (none: local memory) and the scale
    Gs::Shadow* frameShadow;
    Gs::Shadow* depthShadow;
    u32 scale;
};
}
