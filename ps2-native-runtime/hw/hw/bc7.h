#pragma once

// BC7 (BPTC) blocks decoded: 4x4 texels of RGBA (R in the low byte), for the replacement textures' DDS files
#include "common.h"

namespace NativeGraphics
{
void DecodeBc7Block(const u8* block, u32* out);
}
