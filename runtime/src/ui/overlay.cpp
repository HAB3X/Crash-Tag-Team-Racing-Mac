// The overlay's frames and fonts: the frame recorded on the presenter's thread (overlayrender.cpp draws it), the bundled fonts
// (runtime/fonts: Luckiest Guy under the Apache License 2.0, Fredoka under the SIL Open Font License) built into the program
// and read with stb_truetype, and the window's points mapped into the 1280 x 720 reference space. From the Crash Twinsanity
// native port's ui/overlay.cpp (same author)
#include "overlayinternal.h"

#include "../runtime.h"

#include <atomic>
#include <mutex>

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb/stb_truetype.h"

// The fonts' files in the program (PS2_FONTS_DIR is the runtime's fonts folder, set by the build)
#define PS2_FONT(symbol, file)                                                                                                 \
    __asm__(".section __DATA,__const\n"                                                                                        \
            ".globl _" #symbol "\n"                                                                                            \
            "_" #symbol ":\n"                                                                                                  \
            ".incbin \"" PS2_FONTS_DIR "/" file "\"\n"                                                                         \
            ".globl _" #symbol "_end\n"                                                                                        \
            "_" #symbol "_end:\n"                                                                                              \
            ".byte 0\n"                                                                                                        \
            ".text\n");                                                                                                        \
    extern "C" const unsigned char symbol[];                                                                                   \
    extern "C" const unsigned char symbol##_end[];

PS2_FONT(ps2_font_luckiest_guy, "LuckiestGuy-Regular.ttf")
PS2_FONT(ps2_font_fredoka_semibold, "Fredoka-SemiBold.ttf")
PS2_FONT(ps2_font_fredoka_bold, "Fredoka-Bold.ttf")

namespace Ps2::Ui
{
// ------------------------------------------------------------------------------------------------------------------ the fonts
namespace
{
struct FontFile
{
    stbtt_fontinfo info{};
    bool loaded = false;
    f32 ascent = 0.0f;
    f32 descent = 0.0f;
    f32 lineGap = 0.0f;
};

FontFile g_fonts[static_cast<u32>(OverlayFont::Count)];
std::once_flag g_fontsLoaded;

void LoadFonts()
{
    const unsigned char* data[static_cast<u32>(OverlayFont::Count)] = {ps2_font_luckiest_guy, ps2_font_fredoka_semibold,
                                                                        ps2_font_fredoka_bold};
    for (u32 face = 0; face < static_cast<u32>(OverlayFont::Count); face++)
    {
        FontFile& font = g_fonts[face];
        if (!stbtt_InitFont(&font.info, data[face], stbtt_GetFontOffsetForIndex(data[face], 0)))
        {
            Log("overlay: font %u doesn't read", face);
            continue;
        }
        int ascent, descent, lineGap;
        stbtt_GetFontVMetrics(&font.info, &ascent, &descent, &lineGap);
        font.ascent = static_cast<f32>(ascent);
        font.descent = static_cast<f32>(descent);
        font.lineGap = static_cast<f32>(lineGap);
        font.loaded = true;
    }
}
}

const stbtt_fontinfo* OverlayFontInfo(OverlayFont face)
{
    std::call_once(g_fontsLoaded, LoadFonts);
    const FontFile& font = g_fonts[static_cast<u32>(face)];
    return font.loaded ? &font.info : nullptr;
}

f32 OverlayFontScale(OverlayFont face, f32 size)
{
    const stbtt_fontinfo* info = OverlayFontInfo(face);
    return info != nullptr ? stbtt_ScaleForMappingEmToPixels(info, size) : 0.0f;
}

void OverlayFontMetrics(OverlayFont face, f32 size, f32* ascent, f32* descent, f32* lineGap)
{
    const FontFile& font = g_fonts[static_cast<u32>(face)];
    f32 scale = OverlayFontScale(face, size);
    *ascent = font.ascent * scale;
    *descent = font.descent * scale;
    *lineGap = font.lineGap * scale;
}

std::u32string Codepoints(const std::string& text)
{
    // UTF-8 (a broken sequence's bytes taken as Latin-1)
    std::u32string out;
    for (size_t i = 0; i < text.size();)
    {
        unsigned char c = static_cast<unsigned char>(text[i]);
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        char32_t code = extra == 0 ? c : extra == 1 ? (c & 0x1F) : extra == 2 ? (c & 0x0F) : (c & 0x07);
        bool good = i + static_cast<size_t>(extra) < text.size();
        for (int k = 1; good && k <= extra; k++)
        {
            unsigned char next = static_cast<unsigned char>(text[i + static_cast<size_t>(k)]);
            good = (next & 0xC0) == 0x80;
            code = (code << 6) | (next & 0x3F);
        }
        if (!good)
        {
            out.push_back(c);
            i++;
            continue;
        }
        out.push_back(code);
        i += static_cast<size_t>(extra) + 1;
    }
    return out;
}

f32 OverlayTextWidth(const std::string& text, const TextStyle& style)
{
    const stbtt_fontinfo* info = OverlayFontInfo(style.font);
    if (info == nullptr)
    {
        return 0.0f;
    }
    f32 scale = OverlayFontScale(style.font, style.size);
    std::u32string codes = Codepoints(text);
    f32 width = 0.0f;
    for (size_t i = 0; i < codes.size(); i++)
    {
        int advance, bearing;
        stbtt_GetCodepointHMetrics(info, static_cast<int>(codes[i]), &advance, &bearing);
        width += static_cast<f32>(advance) * scale + style.letterSpacing;
        if (i + 1 < codes.size())
        {
            width += static_cast<f32>(stbtt_GetCodepointKernAdvance(info, static_cast<int>(codes[i]), static_cast<int>(codes[i + 1]))) *
                     scale;
        }
    }
    return width - (codes.empty() ? 0.0f : style.letterSpacing);
}

f32 OverlayLineHeight(const TextStyle& style)
{
    f32 ascent, descent, lineGap;
    OverlayFontMetrics(style.font, style.size, &ascent, &descent, &lineGap);
    return ascent - descent + lineGap;
}

// ----------------------------------------------------------------------------------------------------------------- the frames
namespace
{
std::vector<OverlayCommand> g_recording;

// The window as the overlay saw it last
std::atomic<int> g_windowPixelWidth{0};
std::atomic<int> g_windowPixelHeight{0};
std::atomic<float> g_pixelScale{1.0f};
}

std::vector<OverlayCommand>& OverlayFrame::Commands() { return g_recording; }

void OverlayFrame::Clear() { g_recording.clear(); }

bool OverlayFrame::Empty() const { return g_recording.empty(); }

void OverlayFrame::Backdrop(f32 seconds)
{
    OverlayCommand command{};
    command.kind = CommandKind::Backdrop;
    command.seconds = seconds;
    g_recording.push_back(command);
}

void OverlayFrame::Box(f32 x, f32 y, f32 width, f32 height, const BoxStyle& style)
{
    OverlayCommand command{};
    command.kind = CommandKind::Box;
    command.x = x;
    command.y = y;
    command.width = width;
    command.height = height;
    command.box = style;
    g_recording.push_back(command);
}

void OverlayFrame::Text(const std::string& text, f32 x, f32 y, const TextStyle& style, OverlayAlign align)
{
    if (text.empty())
    {
        return;
    }
    OverlayCommand command{};
    command.kind = CommandKind::Text;
    f32 width = OverlayTextWidth(text, style);
    command.x = align == OverlayAlign::Left ? x : align == OverlayAlign::Right ? x - width : x - width * 0.5f;
    command.y = y;
    command.width = width;
    command.text = Codepoints(text);
    command.textStyle = style;
    g_recording.push_back(command);
}

void OverlayFrame::Image(OverlayImage image, f32 x, f32 y, f32 width, f32 height, f32 alpha)
{
    OverlayCommand command{};
    command.kind = CommandKind::Image;
    command.image = image;
    command.x = x;
    command.y = y;
    command.width = width;
    command.height = height;
    command.alpha = alpha;
    g_recording.push_back(command);
}

void OverlayFrame::Clip(f32 x, f32 y, f32 width, f32 height)
{
    OverlayCommand command{};
    command.kind = CommandKind::Clip;
    command.x = x;
    command.y = y;
    command.width = width;
    command.height = height;
    g_recording.push_back(command);
}

void OverlayFrame::NoClip()
{
    OverlayCommand command{};
    command.kind = CommandKind::NoClip;
    g_recording.push_back(command);
}

void NoteWindow(int pixelWidth, int pixelHeight, float pixelScale)
{
    g_windowPixelWidth = pixelWidth;
    g_windowPixelHeight = pixelHeight;
    g_pixelScale = pixelScale;
}

void ReferenceMapping(f32 pixelWidth, f32 pixelHeight, f32* scale, f32* offsetX, f32* offsetY)
{
    *scale = std::min(pixelWidth / ReferenceWidth, pixelHeight / ReferenceHeight);
    *offsetX = (pixelWidth - ReferenceWidth * *scale) * 0.5f;
    *offsetY = (pixelHeight - ReferenceHeight * *scale) * 0.5f;
}

void PixelsToReference(f32 x, f32 y, f32* referenceX, f32* referenceY)
{
    int width = g_windowPixelWidth;
    int height = g_windowPixelHeight;
    if (width <= 0 || height <= 0)
    {
        *referenceX = -1.0f;
        *referenceY = -1.0f;
        return;
    }
    f32 scale, offsetX, offsetY;
    ReferenceMapping(static_cast<f32>(width), static_cast<f32>(height), &scale, &offsetX, &offsetY);
    *referenceX = (x - offsetX) / scale;
    *referenceY = (y - offsetY) / scale;
}

void WindowToReference(f32 x, f32 y, f32* referenceX, f32* referenceY)
{
    f32 pixelScale = g_pixelScale;
    PixelsToReference(x * pixelScale, y * pixelScale, referenceX, referenceY);
}
}
