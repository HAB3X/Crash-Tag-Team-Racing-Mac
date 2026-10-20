// The mouse's images for the prompts (a keycap's "[LMB] Spin"): a mouse in the keycaps' ink on white, the button bound lit in the
// design's orange (the left, the right, the wheel), the wheel's up and down with an arrow. Drawn once at 64 x 88, 4 x 4 samples a
// pixel. From the Crash Twinsanity native port's ui/mouseart.cpp (same author), its namespace aside
#include "overlayinternal.h"

#include <algorithm>
#include <cmath>

namespace Ps2::Ui
{
namespace
{
constexpr s32 Width = 64;
constexpr s32 Height = 88;
// The design's keycap ink (#1b2340), white, the orange (#ff9a1f)
constexpr u32 Ink = 0xFF40231Bu;
constexpr u32 Paper = 0xFFFFFFFFu;
constexpr u32 Lit = 0xFF1F9AFFu;

// Inside a rounded rectangle (centre, half sizes, radius)
bool InRounded(f32 x, f32 y, f32 cx, f32 cy, f32 hw, f32 hh, f32 radius)
{
    f32 qx = std::fabs(x - cx) - (hw - radius);
    f32 qy = std::fabs(y - cy) - (hh - radius);
    f32 outside = std::sqrt(std::max(qx, 0.0f) * std::max(qx, 0.0f) + std::max(qy, 0.0f) * std::max(qy, 0.0f));
    return outside + std::min(std::max(qx, qy), 0.0f) - radius <= 0.0f;
}

bool InTriangle(f32 x, f32 y, f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy)
{
    auto side = [](f32 px, f32 py, f32 x1, f32 y1, f32 x2, f32 y2) { return (px - x2) * (y1 - y2) - (x1 - x2) * (py - y2); };
    f32 d1 = side(x, y, ax, ay, bx, by);
    f32 d2 = side(x, y, bx, by, cx, cy);
    f32 d3 = side(x, y, cx, cy, ax, ay);
    bool negative = d1 < 0 || d2 < 0 || d3 < 0;
    bool positive = d1 > 0 || d2 > 0 || d3 > 0;
    return !(negative && positive);
}

// A sample's colour (0: nothing there)
u32 Sample(OverlayImage image, f32 x, f32 y)
{
    // The body below the arrows' room: 52 x 72 round, the buttons above its line at 32
    constexpr f32 CentreX = 32.0f;
    constexpr f32 CentreY = 50.0f;
    constexpr f32 HalfWidth = 25.0f;
    constexpr f32 HalfHeight = 36.0f;
    constexpr f32 Outline = 4.5f;
    constexpr f32 ButtonLine = 46.0f;
    bool wheelUp = image == OverlayImage::MouseWheelUp;
    bool wheelDown = image == OverlayImage::MouseWheelDown;
    // The wheel's arrow above or below the mouse (the mouse moved down or up to make room)
    f32 shift = wheelUp ? 4.0f : wheelDown ? -8.0f : 0.0f;
    f32 by = y - shift;
    if (wheelUp && InTriangle(x, y, 32.0f, 0.5f, 22.0f, 11.0f, 42.0f, 11.0f))
    {
        return Lit;
    }

    if (wheelDown && InTriangle(x, y, 32.0f, 87.5f, 22.0f, 77.0f, 42.0f, 77.0f))
    {
        return Lit;
    }

    f32 scale = wheelUp || wheelDown ? 0.86f : 1.0f;
    f32 sx = CentreX + (x - CentreX) / scale;
    f32 sy = CentreY + (by - CentreY) / scale;
    if (!InRounded(sx, sy, CentreX, CentreY, HalfWidth, HalfHeight, 24.0f))
    {
        return 0;
    }

    if (!InRounded(sx, sy, CentreX, CentreY, HalfWidth - Outline, HalfHeight - Outline, 24.0f - Outline))
    {
        return Ink;
    }

    // The wheel: a rounded slot in the middle of the buttons
    bool wheel = InRounded(sx, sy, CentreX, 30.0f, 5.5f, 10.0f, 5.5f);
    bool wheelLit = image == OverlayImage::MouseMiddle || wheelUp || wheelDown;
    if (wheel)
    {
        return wheelLit ? Lit : Ink;
    }

    // The lines between the buttons and under them
    if ((std::fabs(sx - CentreX) <= 1.6f && sy <= ButtonLine) || std::fabs(sy - ButtonLine) <= 1.6f)
    {
        return Ink;
    }

    if (sy < ButtonLine && ((image == OverlayImage::MouseLeft && sx < CentreX) || (image == OverlayImage::MouseRight && sx > CentreX)))
    {
        return Lit;
    }

    return Paper;
}
}

bool MouseArt(OverlayImage image, std::vector<u32>* pixels, s32* width, s32* height)
{
    if (image < OverlayImage::MouseLeft || image >= OverlayImage::Count)
    {
        return false;
    }

    pixels->assign(static_cast<size_t>(Width) * Height, 0);
    constexpr s32 Samples = 4;
    for (s32 py = 0; py < Height; py++)
    {
        for (s32 px = 0; px < Width; px++)
        {
            f32 r = 0.0f;
            f32 g = 0.0f;
            f32 b = 0.0f;
            f32 a = 0.0f;
            for (s32 sy = 0; sy < Samples; sy++)
            {
                for (s32 sx = 0; sx < Samples; sx++)
                {
                    u32 colour = Sample(image, static_cast<f32>(px) + (static_cast<f32>(sx) + 0.5f) / Samples,
                                        static_cast<f32>(py) + (static_cast<f32>(sy) + 0.5f) / Samples);
                    if (colour != 0)
                    {
                        r += static_cast<f32>(colour & 0xFF);
                        g += static_cast<f32>((colour >> 8) & 0xFF);
                        b += static_cast<f32>((colour >> 16) & 0xFF);
                        a += 1.0f;
                    }
                }
            }

            if (a == 0.0f)
            {
                continue;
            }

            // Straight alpha: the colour the covered samples' average
            u32 alpha = static_cast<u32>(a / (Samples * Samples) * 255.0f + 0.5f);
            u32 red = static_cast<u32>(r / a + 0.5f);
            u32 green = static_cast<u32>(g / a + 0.5f);
            u32 blue = static_cast<u32>(b / a + 0.5f);
            (*pixels)[static_cast<size_t>(py) * Width + px] = red | (green << 8) | (blue << 16) | (alpha << 24);
        }
    }

    *width = Width;
    *height = Height;
    return true;
}
}
