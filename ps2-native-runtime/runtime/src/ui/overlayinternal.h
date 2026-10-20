// The overlay's parts' shared insides (overlay.cpp records and measures, overlayrender.cpp draws)
#pragma once

#include "overlay.h"

#include <algorithm>
#include <string>
#include <vector>

struct stbtt_fontinfo;

namespace Ps2::Ui
{
enum class CommandKind : u32
{
    Backdrop,
    Box,
    Text,
    Image,
    Clip,
    NoClip,
};

struct OverlayCommand
{
    CommandKind kind;
    // Reference pixels (a text's left and its line's middle)
    f32 x;
    f32 y;
    f32 width;
    f32 height;
    BoxStyle box;
    std::u32string text;
    TextStyle textStyle;
    OverlayImage image;
    f32 alpha;
    f32 seconds;
};

const stbtt_fontinfo* OverlayFontInfo(OverlayFont face);
f32 OverlayFontScale(OverlayFont face, f32 size);
void OverlayFontMetrics(OverlayFont face, f32 size, f32* ascent, f32* descent, f32* lineGap);
std::u32string Codepoints(const std::string& text);
void NoteWindow(int pixelWidth, int pixelHeight, float pixelScale);
// A mouse image's art (mouseart.cpp): drawn at a fixed size, RGBA (red the low byte, straight alpha)
bool MouseArt(OverlayImage image, std::vector<u32>* pixels, s32* width, s32* height);
}
