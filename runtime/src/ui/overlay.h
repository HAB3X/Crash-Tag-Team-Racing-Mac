// The PC menus' overlay: what's drawn over the window at the window's own resolution, in a 1280 x 720 reference space scaled
// uniformly to the window and centred. A frame of commands (rounded boxes with a gradient, a border, a lip and a soft shadow;
// texts in the bundled fonts with an outline and a drop; the options' backdrop; images) is recorded, then drawn over the
// picture (overlayrender.cpp). From the Crash Twinsanity native port's ui/overlay.h (same author), on the presenter's thread
// only here: the menus are run and drawn there while the game is paused under them
#pragma once

#include "ps2/recomp.h"

#include <string>
#include <vector>

namespace Ps2::Ui
{
constexpr f32 ReferenceWidth = 1280.0f;
constexpr f32 ReferenceHeight = 720.0f;

struct Rgba32
{
    f32 r;
    f32 g;
    f32 b;
    f32 a;
};

// #rrggbb with an alpha
constexpr Rgba32 Hex(u32 rgb, f32 alpha = 1.0f)
{
    return {static_cast<f32>((rgb >> 16) & 0xFF) / 255.0f, static_cast<f32>((rgb >> 8) & 0xFF) / 255.0f,
            static_cast<f32>(rgb & 0xFF) / 255.0f, alpha};
}

// The fonts: the display face (Luckiest Guy: titles, tabs) and the body face at its weights (Fredoka 600 and 700)
enum class OverlayFont : u32
{
    Display,
    Body,
    BodyBold,
    Count,
};

struct BoxStyle
{
    Rgba32 fill = {1, 1, 1, 1};
    // A vertical gradient's bottom colour (the alpha < 0: no gradient)
    Rgba32 fillBottom = {0, 0, 0, -1};
    f32 radius = 0.0f;
    f32 border = 0.0f;
    Rgba32 borderColour = {0, 0, 0, 0};
    // A solid copy below (CSS's "0 lip 0 colour" shadow)
    f32 lip = 0.0f;
    Rgba32 lipColour = {0, 0, 0, 0};
    // A soft shadow (CSS's "0 offset blur colour")
    f32 shadowOffset = 0.0f;
    f32 shadowBlur = 0.0f;
    Rgba32 shadowColour = {0, 0, 0, 0};
};

struct TextStyle
{
    OverlayFont font = OverlayFont::Body;
    f32 size = 20.0f;
    Rgba32 colour = {1, 1, 1, 1};
    f32 letterSpacing = 0.0f;
    // An outline round the letters (CSS's text-stroke painted under the fill: half of it shows)
    f32 outline = 0.0f;
    Rgba32 outlineColour = {0, 0, 0, 0};
    // A solid drop (CSS's "x y 0 colour" text-shadow)
    f32 dropX = 0.0f;
    f32 dropY = 0.0f;
    Rgba32 dropColour = {0, 0, 0, 0};
};

enum class OverlayAlign : u32
{
    Left,
    Centre,
    Right,
};

// The images: the cursor (a Wumpa fruit, drawn), the PlayStation's face buttons (drawn: the shapes in their colours on a dark
// button), the mouse with a button or its wheel lit (mouseart.cpp)
enum class OverlayImage : u32
{
    Cursor,
    PlayStationCross,
    PlayStationCircle,
    PlayStationSquare,
    PlayStationTriangle,
    MouseLeft,
    MouseRight,
    MouseMiddle,
    MouseWheelUp,
    MouseWheelDown,
    Count,
};

struct OverlayCommand;

// A frame being recorded
class OverlayFrame
{
public:
    void Clear();
    bool Empty() const;
    // The options' backdrop: the gradient, the ring target and the spiral (turned by the seconds)
    void Backdrop(f32 seconds);
    void Box(f32 x, f32 y, f32 width, f32 height, const BoxStyle& style);
    // A text (UTF-8) on one line: placed by its alignment across, its line's middle at y
    void Text(const std::string& text, f32 x, f32 y, const TextStyle& style, OverlayAlign align = OverlayAlign::Left);
    void Image(OverlayImage image, f32 x, f32 y, f32 width, f32 height, f32 alpha = 1.0f);
    // A rectangle the frame's later commands are clipped to (none: the whole window), as a list's scroll needs
    void Clip(f32 x, f32 y, f32 width, f32 height);
    void NoClip();

    std::vector<OverlayCommand>& Commands();
};

// A text's width (reference pixels) and its line's height in a style (the fonts' own metrics)
f32 OverlayTextWidth(const std::string& text, const TextStyle& style);
f32 OverlayLineHeight(const TextStyle& style);

// The frame drawn over the window's framebuffer (bound, of these pixels; pixelScale: the window's pixels a point)
void DrawOverlay(OverlayFrame& frame, int pixelWidth, int pixelHeight, float pixelScale);
// The reference space's place in a window of these pixels, and a window point (SDL's mouse) in reference space (by the window
// the overlay was drawn over last)
void ReferenceMapping(f32 pixelWidth, f32 pixelHeight, f32* scale, f32* offsetX, f32* offsetY);
void WindowToReference(f32 x, f32 y, f32* referenceX, f32* referenceY);
// A rectangle of the window's pixels in reference space
void PixelsToReference(f32 x, f32 y, f32* referenceX, f32* referenceY);
}
