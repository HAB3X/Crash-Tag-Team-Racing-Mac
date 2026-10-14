// The PC menus' view, the Crash Twinsanity native port's options screen (its ui/settingsview.cpp, same author: the approved
// design's values) drawn on the overlay in its 1280 x 720 reference space at the window's own resolution. Options and Extras:
// the blue backdrop with the Twinsanity spiral, the title in Luckiest Guy over the game's name, the tabs down the left, the
// white panel of rows (Fredoka; the selected row orange, its value's pill between chevrons), the description under it, the bar
// of keycaps (or the gamepad's buttons) at the bottom. The pause menu is the same tabs' buttons over the game's picture, dimmed.
// Its parts are recorded as hit regions for the mouse. The game's prompt and the scripted pictures' cursor are drawn here too
#include "menusinternal.h"

#include "../runtime.h"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace Ps2::Ui
{
namespace
{
// ------------------------------------------------------------------------------------------------------------- the design's values
constexpr Rgba32 Orange = Hex(0xff9a1f);
constexpr Rgba32 DeepOrange = Hex(0xff8a00);
constexpr Rgba32 Lip = Hex(0xffb43a);
constexpr Rgba32 Brown = Hex(0x3a1200);
constexpr Rgba32 TabBrown = Hex(0x4a1800);
constexpr Rgba32 Ink = Hex(0x173a6b);
constexpr Rgba32 PillBlue = Hex(0x1b6fd0);
constexpr Rgba32 PillFill = Hex(0xeaf2ff);
constexpr Rgba32 Separator = Hex(0xeef3fa);
constexpr Rgba32 White = Hex(0xffffff);
constexpr Rgba32 KeyInk = Hex(0x1b2340);
constexpr Rgba32 Muted = Hex(0x6b86a8);

constexpr f32 PanelX = 360.0f;
constexpr f32 PanelY = 170.0f;
constexpr f32 PanelWidth = 860.0f;
constexpr f32 PanelHeight = 420.0f;
constexpr f32 PanelPadX = 18.0f;
constexpr f32 PanelPadY = 16.0f;
constexpr f32 RowHeight = 48.0f;
// Rows are 48 apart: the design's border-box rows, a 2px divider inside the top of each after the first
constexpr f32 RowPitch = 48.0f;
constexpr f32 RowPad = 22.0f;
constexpr f32 ValueWidth = 280.0f;
constexpr f32 ChevronGap = 14.0f;
// The controls table's columns' middles and their pills' width
constexpr f32 ColumnMiddle[BindingColumns] = {800.0f, 945.0f, 1095.0f};
constexpr f32 CellWidth = 132.0f;

TextStyle Body(f32 size, const Rgba32& colour, bool bold = false)
{
    TextStyle style;
    style.font = bold ? OverlayFont::BodyBold : OverlayFont::Body;
    style.size = size;
    style.colour = colour;
    return style;
}

TextStyle Display(f32 size, const Rgba32& colour)
{
    TextStyle style;
    style.font = OverlayFont::Display;
    style.size = size;
    style.colour = colour;
    style.letterSpacing = 1.0f;
    return style;
}

Rgba32 Faded(Rgba32 colour, f32 alpha)
{
    colour.a *= alpha;
    return colour;
}

std::string Capitals(std::string text)
{
    for (char& c : text)
    {
        if (c >= 'a' && c <= 'z')
        {
            c = static_cast<char>(c - 0x20);
        }
    }
    return text;
}

f32 Seconds()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<f32>(std::chrono::steady_clock::now() - start).count();
}

f32 EaseOut(f32 t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

// ------------------------------------------------------------------------------------------------------------------ the prompts
// A prompt: a key of the keyboard's, a mouse button, or a gamepad's button
struct Prompt
{
    // A keycap's text (a key's name, or a gamepad's shoulder's: "L1", "LB"), or a mouse image, or a face button
    std::string text;
    OverlayImage image = OverlayImage::Count;
    // A gamepad's face button by its place (South, East, West, North), drawn in the style's way
    PadControl face = PadControl::None;
};

// A mouse key's image (the left, right and middle buttons), none for the others
OverlayImage MouseImage(const std::string& keyName)
{
    if (keyName == "Mouse Left")
    {
        return OverlayImage::MouseLeft;
    }
    if (keyName == "Mouse Right")
    {
        return OverlayImage::MouseRight;
    }
    if (keyName == "Mouse Middle")
    {
        return OverlayImage::MouseMiddle;
    }
    return OverlayImage::Count;
}

// A keycap (common.css's .key): rounded 8, a white to grey gradient, a 2px border and a 4px lip of the dark ink, Fredoka 700 15
f32 KeycapWidth(const Prompt& prompt)
{
    if (prompt.image != OverlayImage::Count)
    {
        return 44.0f;
    }
    return std::max(44.0f, OverlayTextWidth(Capitals(prompt.text), Body(15.0f, KeyInk, true)) + 24.0f);
}

f32 Keycap(OverlayFrame& frame, const Prompt& prompt, f32 x, f32 middle)
{
    f32 width = KeycapWidth(prompt);
    f32 height = 34.0f;
    BoxStyle box;
    box.fill = Hex(0xfdfdfd);
    box.fillBottom = Hex(0xcfd6e3);
    box.radius = 8.0f;
    box.border = 2.0f;
    box.borderColour = KeyInk;
    box.lip = 4.0f;
    box.lipColour = KeyInk;
    f32 top = middle - height * 0.5f - 2.0f;
    frame.Box(x, top, width, height, box);
    if (prompt.image != OverlayImage::Count)
    {
        // The mouse 64 x 88 in the keycap
        f32 iconHeight = 26.0f;
        f32 iconWidth = iconHeight * 64.0f / 88.0f;
        frame.Image(prompt.image, x + (width - iconWidth) * 0.5f, top + (height - iconHeight) * 0.5f, iconWidth, iconHeight);
        return width;
    }
    frame.Text(Capitals(prompt.text), x + width * 0.5f, top + height * 0.5f, Body(15.0f, KeyInk, true), OverlayAlign::Centre);
    return width;
}

constexpr f32 ButtonSize = 38.0f;

// A face button: the PlayStation's shapes (drawn), an Xbox pad's letters in their colours on a dark button, a Nintendo pad's
// letters on a grey one
void FaceButton(OverlayFrame& frame, PadControl face, PromptStyle style, f32 x, f32 y, f32 size, f32 alpha = 1.0f)
{
    int place = face == PadControl::South ? 0 : face == PadControl::East ? 1 : face == PadControl::West ? 2 : 3;
    if (style == PromptStyle::PlayStation || style == PromptStyle::Keyboard)
    {
        static const OverlayImage Shapes[4] = {OverlayImage::PlayStationCross, OverlayImage::PlayStationCircle,
                                               OverlayImage::PlayStationSquare, OverlayImage::PlayStationTriangle};
        frame.Image(Shapes[place], x, y, size, size, alpha);
        return;
    }
    // Xbox: A B X Y at the bottom, right, left and top; Nintendo: B A Y X there
    static const char* const XboxLetters[4] = {"A", "B", "X", "Y"};
    static const char* const SwitchLetters[4] = {"B", "A", "Y", "X"};
    static const Rgba32 XboxColours[4] = {Hex(0x6cc04a), Hex(0xf0533f), Hex(0x3f8fe8), Hex(0xf5c518)};
    bool xbox = style == PromptStyle::Xbox;
    BoxStyle button;
    button.radius = size * 0.5f;
    button.fill = Faded(xbox ? Hex(0x30343e) : Hex(0x5b5f69), alpha);
    button.fillBottom = Faded(xbox ? Hex(0x15171d) : Hex(0x3b3e45), alpha);
    button.shadowOffset = 1.5f;
    button.shadowBlur = 3.0f;
    button.shadowColour = Hex(0x000000, 0.35f * alpha);
    frame.Box(x, y, size, size, button);
    TextStyle letter = Body(size * 0.55f, Faded(xbox ? XboxColours[place] : White, alpha), true);
    frame.Text(xbox ? XboxLetters[place] : SwitchLetters[place], x + size * 0.5f, y + size * 0.5f, letter, OverlayAlign::Centre);
}

bool IsFace(PadControl control)
{
    return control == PadControl::South || control == PadControl::East || control == PadControl::West || control == PadControl::North;
}

f32 PromptWidth(const Prompt& prompt) { return IsFace(prompt.face) ? ButtonSize : KeycapWidth(prompt); }

f32 DrawPrompt(OverlayFrame& frame, const Prompt& prompt, PromptStyle style, f32 x, f32 middle)
{
    if (IsFace(prompt.face))
    {
        FaceButton(frame, prompt.face, style, x, middle - ButtonSize * 0.5f, ButtonSize);
        return ButtonSize;
    }
    return Keycap(frame, prompt, x, middle);
}

Prompt KeyPrompt(const std::string& name) { return {name, MouseImage(name), PadControl::None}; }

Prompt PadPrompt(PadControl control, PromptStyle style)
{
    if (IsFace(control))
    {
        return {"", OverlayImage::Count, control};
    }
    return {PadControlText(control, style), OverlayImage::Count, PadControl::None};
}

// The menus' prompts in the style in use: choose, go back, the tabs, clear
Prompt SelectPrompt(PromptStyle style) { return style == PromptStyle::Keyboard ? KeyPrompt("Enter") : PadPrompt(PadControl::South, style); }
Prompt BackPrompt(PromptStyle style) { return style == PromptStyle::Keyboard ? KeyPrompt("Esc") : PadPrompt(PadControl::East, style); }
Prompt TabPrompt(PromptStyle style, bool next)
{
    if (style == PromptStyle::Keyboard)
    {
        return KeyPrompt(next ? "E" : "Q");
    }
    return PadPrompt(next ? PadControl::RightShoulder : PadControl::LeftShoulder, style);
}
Prompt ClearPrompt(PromptStyle style) { return style == PromptStyle::Keyboard ? KeyPrompt("Del") : PadPrompt(PadControl::West, style); }

struct FooterItem
{
    Prompt prompt;
    Prompt prompt2;
    bool two;
    std::string word;
    HitKind kind;
};

// The bar (common.css's .foot and .prompt): black 45%, 58px, the prompts right-aligned 60px from the edge, 34px apart, each its
// keys (8px apart) and its word (Fredoka 600 19 white, a 2px black drop)
void Footer(OverlayFrame& frame, const std::vector<FooterItem>& items, PromptStyle style, std::vector<HitRegion>& regions)
{
    f32 top = ReferenceHeight - 58.0f;
    f32 middle = top + 29.0f;
    BoxStyle bar;
    bar.fill = Hex(0x000000, 0.45f);
    frame.Box(-1000.0f, top, ReferenceWidth + 2000.0f, 58.0f + 1000.0f, bar);
    TextStyle word = Body(19.0f, White);
    word.dropY = 2.0f;
    word.dropColour = Hex(0x000000);
    f32 width = 0.0f;
    for (size_t i = 0; i < items.size(); i++)
    {
        const FooterItem& item = items[i];
        width += (i == 0 ? 0.0f : 34.0f) + PromptWidth(item.prompt) + 8.0f;
        if (item.two)
        {
            width += PromptWidth(item.prompt2) + 8.0f;
        }
        width += OverlayTextWidth(item.word, word);
    }
    f32 x = ReferenceWidth - 60.0f - width;
    for (size_t i = 0; i < items.size(); i++)
    {
        const FooterItem& item = items[i];
        x += i == 0 ? 0.0f : 34.0f;
        f32 start = x;
        x += DrawPrompt(frame, item.prompt, style, x, middle) + 8.0f;
        if (item.two)
        {
            x += DrawPrompt(frame, item.prompt2, style, x, middle) + 8.0f;
        }
        frame.Text(item.word, x, middle, word);
        x += OverlayTextWidth(item.word, word);
        regions.push_back({item.kind, start, top, x - start, 58.0f, -1, -1, -1});
    }
}

// The title (Luckiest Guy, orange, a brown outline and drop) and the game's name under it
void Title(OverlayFrame& frame, const std::string& text)
{
    TextStyle title = Display(66.0f, Orange);
    title.outline = 2.0f;
    title.outlineColour = Brown;
    title.dropY = 4.0f;
    title.dropColour = Brown;
    frame.Text(text, 64.0f, 40.0f + OverlayLineHeight(title) * 0.5f, title);
    TextStyle sub = Body(18.0f, Hex(0xbcd9ff));
    sub.letterSpacing = 3.0f;
    sub.dropY = 1.0f;
    sub.dropColour = Hex(0x000000, 0.35f);
    frame.Text(GameTitle(), 68.0f, 118.0f + OverlayLineHeight(sub) * 0.5f, sub);
}

// A button down the left (the tabs' look): white with an orange lip when it's the one selected
void SideButton(OverlayFrame& frame, const std::string& label, f32 y, f32 width, f32 height, bool selected, bool enabled,
                f32 textSize)
{
    BoxStyle box;
    box.radius = 16.0f;
    box.border = 2.0f;
    if (selected)
    {
        box.fill = White;
        box.borderColour = White;
        box.lip = 6.0f;
        box.lipColour = Lip;
    }
    else
    {
        box.fill = Hex(0xffffff, 0.08f);
        box.borderColour = Hex(0xffffff, 0.15f);
    }
    frame.Box(60.0f, y, width, height, box);
    TextStyle text = Display(textSize, Hex(0xdbe9ff, enabled ? 1.0f : 0.4f));
    if (selected)
    {
        text.colour = Faded(DeepOrange, enabled ? 1.0f : 0.5f);
        text.outline = 1.2f;
        text.outlineColour = TabBrown;
    }
    frame.Text(Capitals(label), 86.0f, y + height * 0.5f + 2.0f, text);
}

// The white box over a screen asking a question: its text and its two buttons (the chosen one orange)
void QuestionBox(OverlayFrame& frame, const std::string& question, const std::string& detail, const std::string& yes,
                 const std::string& no, int chosen, std::vector<HitRegion>& regions)
{
    BoxStyle panel;
    panel.fill = White;
    panel.radius = 22.0f;
    panel.lip = 10.0f;
    panel.lipColour = Lip;
    panel.shadowOffset = 26.0f;
    panel.shadowBlur = 50.0f;
    panel.shadowColour = Hex(0x000000, 0.4f);
    frame.Box(390.0f, 255.0f, 500.0f, 190.0f, panel);
    frame.Text(question, 640.0f, detail.empty() ? 315.0f : 300.0f, Body(23.0f, Ink, true), OverlayAlign::Centre);
    if (!detail.empty())
    {
        frame.Text(detail, 640.0f, 334.0f, Body(17.0f, Muted), OverlayAlign::Centre);
    }
    const f32 xs[2] = {470.0f, 670.0f};
    const std::string labels[2] = {yes, no};
    for (int i = 0; i < 2; i++)
    {
        BoxStyle button;
        button.fill = i == chosen ? Orange : PillFill;
        button.radius = 20.0f;
        if (i == chosen)
        {
            button.lip = 3.0f;
            button.lipColour = Hex(0xd86f00);
        }
        frame.Box(xs[i], 375.0f, 140.0f, 42.0f, button);
        frame.Text(labels[i], xs[i] + 70.0f, 396.0f, Body(21.0f, i == chosen ? White : PillBlue), OverlayAlign::Centre);
        regions.push_back({i == 0 ? HitKind::BoxYes : HitKind::BoxNo, xs[i], 375.0f, 140.0f, 42.0f, -1, -1, -1});
    }
}

// --------------------------------------------------------------------------------------------------------------- the animations
struct Animation
{
    f32 barY = -1.0f;
    f32 barFrom = -1.0f;
    f32 barTarget = -1.0f;
    f32 barStart = 0.0f;
    int page = -1;
    unsigned pageChanges = 0;
    f32 pageStart = -10.0f;
    f32 scroll = 0.0f;
    f32 lastTime = 0.0f;
};
Animation g_animation;

// ------------------------------------------------------------------------------------------------------------------- a row
struct RowColours
{
    Rgba32 label;
    Rgba32 pillFill;
    Rgba32 pillText;
    Rgba32 chevron;
};

// A value between chevrons (common.css's .v: centred in its 280px on the row's right)
void ChoiceValue(OverlayFrame& frame, const std::string& value, f32 centre, f32 middle, const RowColours& colours, int index,
                 f32 top, std::vector<HitRegion>& regions)
{
    TextStyle text = Body(21.0f, colours.pillText);
    TextStyle chevron = Body(21.0f, colours.chevron, true);
    const std::string left = "‹", right = "›";
    f32 chevronWidth = OverlayTextWidth(left, chevron);
    f32 textWidth = OverlayTextWidth(value, text);
    // A long value made smaller to fit (the chevrons and the pill's ends beside it)
    f32 fits = ValueWidth - 36.0f - 2.0f * (chevronWidth + ChevronGap);
    if (textWidth > fits && fits > 0.0f)
    {
        text.size *= fits / textWidth;
        textWidth = fits;
    }
    f32 pillWidth = textWidth + 36.0f;
    f32 pillHeight = OverlayLineHeight(Body(21.0f, colours.pillText)) + 8.0f;
    f32 total = chevronWidth * 2.0f + ChevronGap * 2.0f + pillWidth;
    f32 x = centre - total * 0.5f;
    frame.Text(left, x, middle, chevron);
    regions.push_back({HitKind::ArrowLeft, x - 12.0f, top, chevronWidth + 24.0f, RowHeight, index, -1, -1});
    x += chevronWidth + ChevronGap;
    BoxStyle pill;
    pill.fill = colours.pillFill;
    pill.radius = 20.0f;
    frame.Box(x, middle - pillHeight * 0.5f, pillWidth, pillHeight, pill);
    frame.Text(value, x + pillWidth * 0.5f, middle, text, OverlayAlign::Centre);
    x += pillWidth + ChevronGap;
    frame.Text(right, x, middle, chevron);
    regions.push_back({HitKind::ArrowRight, x - 12.0f, top, chevronWidth + 24.0f, RowHeight, index, -1, -1});
}

// A slider: a chunky track, its orange fill, the Wumpa-orange knob, the value next to it
void SliderValue(OverlayFrame& frame, const Row& row, f32 left, f32 middle, const RowColours& colours, int index, f32 top,
                 std::vector<HitRegion>& regions)
{
    int value = row.get();
    f32 fraction = static_cast<f32>(value - row.minimum) / static_cast<f32>(row.maximum - row.minimum);
    f32 width = 220.0f;
    BoxStyle track;
    track.fill = Hex(0xdbe7f7);
    track.radius = 8.0f;
    frame.Box(left, middle - 6.0f, width, 12.0f, track);
    BoxStyle fill;
    fill.fill = Hex(0xffc04d);
    fill.fillBottom = DeepOrange;
    fill.radius = 8.0f;
    if (fraction > 0.0f)
    {
        frame.Box(left, middle - 6.0f, std::max(12.0f, width * fraction), 12.0f, fill);
    }
    BoxStyle knob;
    knob.fill = Hex(0xffb347);
    knob.fillBottom = Hex(0xe8650a);
    knob.radius = 13.0f;
    knob.border = 2.0f;
    knob.borderColour = Brown;
    knob.lip = 2.0f;
    knob.lipColour = Hex(0x000000, 0.25f);
    frame.Box(left + width * fraction - 13.0f, middle - 13.0f, 26.0f, 26.0f, knob);
    char number[8];
    std::snprintf(number, sizeof(number), "%d", value);
    frame.Text(number, left + width + 34.0f, middle, Body(21.0f, colours.label), OverlayAlign::Centre);
    regions.push_back({HitKind::Slider, left, top, width, RowHeight, index, -1, -1});
}

// A binding row's cells: the keys' names (a mouse button's with the mouse), the gamepad's control
void BindingCells(OverlayFrame& frame, const MenuView& view, const Row& row, int index, f32 middle, f32 fade,
                  std::vector<HitRegion>& regions)
{
    for (int column = 0; column < BindingColumns; column++)
    {
        bool cell = index == view.row && column == view.column;
        f32 cellHeight = 32.0f;
        BoxStyle pill;
        pill.fill = Faded(cell ? Orange : PillFill, fade);
        pill.radius = 16.0f;
        f32 left = ColumnMiddle[column] - CellWidth * 0.5f;
        frame.Box(left, middle - cellHeight * 0.5f, CellWidth, cellHeight, pill);
        regions.push_back({HitKind::Cell, left, middle - cellHeight * 0.5f, CellWidth, cellHeight, index, column, -1});
        TextStyle style = Body(17.0f, Faded(cell ? White : PillBlue, fade));
        std::string text;
        OverlayImage image = OverlayImage::Count;
        if (column == ColumnPad)
        {
            PadControl control = InputActionPad(row.action);
            if (IsFace(control))
            {
                constexpr f32 ImageSize = 28.0f;
                FaceButton(frame, control, view.padStyle, ColumnMiddle[column] - ImageSize * 0.5f, middle - ImageSize * 0.5f,
                           ImageSize, fade);
                continue;
            }
            text = PadControlText(control, view.padStyle);
        }
        else if (cell && view.capturing)
        {
            text = "…";
        }
        else
        {
            text = InputKeyName(InputActionKey(row.action, column));
            image = MouseImage(text);
            if (image != OverlayImage::Count)
            {
                text = text.substr(6);
            }
            text = text.empty() && image == OverlayImage::Count ? "-" : text;
        }
        // The mouse beside its button's name
        f32 iconHeight = 24.0f;
        f32 iconWidth = image != OverlayImage::Count ? iconHeight * 64.0f / 88.0f : 0.0f;
        f32 gap = image != OverlayImage::Count ? 5.0f : 0.0f;
        f32 room = CellWidth - 16.0f - iconWidth - gap;
        f32 textWidth = OverlayTextWidth(text, style);
        if (textWidth > room)
        {
            style.size *= room / textWidth;
            textWidth = room;
        }
        if (image != OverlayImage::Count)
        {
            f32 x = ColumnMiddle[column] - (iconWidth + gap + textWidth) * 0.5f;
            frame.Image(image, x, middle - iconHeight * 0.5f, iconWidth, iconHeight, fade);
            frame.Text(text, x + iconWidth + gap, middle, style);
            continue;
        }
        frame.Text(text, ColumnMiddle[column], middle, style, OverlayAlign::Centre);
    }
}

// ---------------------------------------------------------------------------------------------------------------- the screens
void DrawPages(OverlayFrame& frame, const MenuView& view, std::vector<HitRegion>& regions)
{
    const std::vector<Page>& pages = *view.pages;
    f32 now = Seconds();
    frame.Backdrop(now);
    Title(frame, view.screen == Screen::Extras ? "EXTRAS" : "OPTIONS");

    // The tabs (closer together when there are many)
    int tabs = static_cast<int>(pages.size());
    f32 pitch = std::min(64.0f, 448.0f / static_cast<f32>(std::max(tabs, 1)));
    f32 tabHeight = std::min(52.0f, pitch - 10.0f);
    for (int tab = 0; tab < tabs; tab++)
    {
        f32 y = 170.0f + static_cast<f32>(tab) * pitch;
        SideButton(frame, pages[static_cast<size_t>(tab)].title, y, 250.0f, tabHeight, tab == view.page, true,
                   std::min(26.0f, tabHeight * 0.5f));
        regions.push_back({HitKind::Tab, 60.0f, y, 250.0f, tabHeight, -1, -1, tab});
    }

    // The panel: white, rounded, its orange lip and its soft shadow
    BoxStyle panel;
    panel.fill = White;
    panel.radius = 22.0f;
    panel.lip = 10.0f;
    panel.lipColour = Lip;
    panel.shadowOffset = 26.0f;
    panel.shadowBlur = 50.0f;
    panel.shadowColour = Hex(0x000000, 0.4f);
    frame.Box(PanelX, PanelY, PanelWidth, PanelHeight, panel);

    // The page's cross-fade, the list's scroll (smooth) and the selection bar's slide
    Animation& a = g_animation;
    f32 delta = std::clamp(now - a.lastTime, 0.0f, 0.1f);
    a.lastTime = now;
    if (view.pageChanges != a.pageChanges || view.page != a.page)
    {
        a.pageChanges = view.pageChanges;
        a.page = view.page;
        a.pageStart = now;
        a.barY = -1.0f;
        a.scroll = 0.0f;
    }
    // Headless, only the scripted pictures are drawn: nothing animates, everything's where it settles
    bool settled = g_options.headless;
    f32 fade = settled ? 1.0f : EaseOut((now - a.pageStart) / 0.12f);
    const std::vector<Row>& rows = pages[static_cast<size_t>(view.page)].rows;
    std::vector<int> visible;
    for (int i = 0; i < static_cast<int>(rows.size()); i++)
    {
        if (!rows[static_cast<size_t>(i)].shown || rows[static_cast<size_t>(i)].shown())
        {
            visible.push_back(i);
        }
    }
    int selectedLine = 0;
    // The controls table's header line (-1 none): it sticks to the top of the panel when the table scrolls under it
    int headerLine = -1;
    for (int i = 0; i < static_cast<int>(visible.size()); i++)
    {
        if (visible[static_cast<size_t>(i)] == view.row)
        {
            selectedLine = i;
        }
        if (rows[static_cast<size_t>(visible[static_cast<size_t>(i)])].kind == RowHeader)
        {
            headerLine = i;
        }
    }
    f32 rowsTop = PanelY + PanelPadY;
    f32 rowsHeight = PanelHeight - PanelPadY * 2.0f;
    f32 contentHeight = static_cast<f32>(visible.size()) * RowPitch;
    f32 maxScroll = std::max(0.0f, contentHeight - rowsHeight);
    f32 selectedTop = static_cast<f32>(selectedLine) * RowPitch;
    f32 wantScroll = a.scroll;
    // A row under the stuck header scrolls into view below it
    f32 headerAllowance = headerLine >= 0 && selectedLine > headerLine ? RowPitch : 0.0f;
    // A heading just above the selection comes into view with it
    if (selectedLine > 0 && rows[static_cast<size_t>(visible[static_cast<size_t>(selectedLine - 1)])].kind == RowHeading)
    {
        headerAllowance = std::max(headerAllowance, RowPitch);
    }
    if (selectedTop - headerAllowance < a.scroll)
    {
        wantScroll = selectedTop - headerAllowance;
    }
    else if (selectedTop + RowHeight > a.scroll + rowsHeight)
    {
        wantScroll = selectedTop + RowHeight - rowsHeight;
    }
    wantScroll = std::clamp(wantScroll, 0.0f, maxScroll);
    a.scroll += (wantScroll - a.scroll) * std::min(1.0f, delta * 18.0f);
    if (std::fabs(wantScroll - a.scroll) < 0.5f || settled)
    {
        a.scroll = wantScroll;
    }
    MenuScroll() = static_cast<int>(a.scroll / RowPitch);
    frame.Clip(PanelX, rowsTop, PanelWidth, rowsHeight);
    f32 rowLeft = PanelX + PanelPadX;
    f32 rowWidth = PanelWidth - PanelPadX * 2.0f;
    f32 valueCentre = rowLeft + rowWidth - RowPad - ValueWidth * 0.5f;

    if (rows.empty())
    {
        frame.Text("Nothing here yet", PanelX + PanelWidth * 0.5f, PanelY + PanelHeight * 0.5f, Body(21.0f, Muted),
                   OverlayAlign::Centre);
    }

    // The selection bar sliding to its row (in the rows' own space: the scroll moves it with the rows)
    f32 targetY = selectedTop;
    if (a.barY < 0.0f || a.barTarget != targetY)
    {
        a.barFrom = a.barY < 0.0f ? targetY : a.barY;
        a.barTarget = targetY;
        a.barStart = now;
    }
    a.barY = settled ? a.barTarget : a.barFrom + (a.barTarget - a.barFrom) * EaseOut((now - a.barStart) / 0.08f);
    f32 barTop = rowsTop + a.barY - a.scroll;
    const Row* selectedRow = view.row >= 0 && view.row < static_cast<int>(rows.size()) ? &rows[static_cast<size_t>(view.row)] : nullptr;
    if (selectedRow != nullptr && selectedRow->kind != RowHeader && selectedRow->kind != RowHeading)
    {
        BoxStyle bar;
        bar.fill = Faded(selectedRow->kind == RowBinding ? Hex(0xfff0dc) : Orange, fade);
        bar.radius = 12.0f;
        frame.Box(rowLeft, barTop, rowWidth, RowHeight, bar);
    }

    for (int line = 0; line < static_cast<int>(visible.size()); line++)
    {
        f32 top = rowsTop + static_cast<f32>(line) * RowPitch - a.scroll;
        if (top + RowHeight < rowsTop || top > rowsTop + rowsHeight)
        {
            continue;
        }
        int index = visible[static_cast<size_t>(line)];
        const Row& row = rows[static_cast<size_t>(index)];
        // The text in the middle of the row below its divider (the selected row's divider is clear)
        f32 middle = top + (line > 0 ? 1.0f : 0.0f) + RowHeight * 0.5f;
        bool selected = index == view.row && row.kind != RowBinding;
        bool enabled = !row.enabled || row.enabled();
        if (line > 0 && index != view.row)
        {
            BoxStyle separator;
            separator.fill = Faded(Separator, fade);
            frame.Box(rowLeft, top, rowWidth, 2.0f, separator);
        }
        if ((index == view.flashRows[0] || index == view.flashRows[1]) && view.flashOn)
        {
            BoxStyle flash;
            flash.fill = Faded(Lip, fade * 0.8f);
            flash.radius = 12.0f;
            frame.Box(rowLeft, top, rowWidth, RowHeight, flash);
        }
        if (row.kind == RowHeader)
        {
            // Drawn after the rows (it sticks)
            continue;
        }
        if (row.kind == RowHeading)
        {
            // A group's heading: small capitals in the muted blue, on the panel's white
            TextStyle heading = Body(15.0f, Faded(Muted, fade), true);
            heading.letterSpacing = 1.5f;
            frame.Text(Capitals(row.label), rowLeft + RowPad, middle + 4.0f, heading);
            continue;
        }
        RowColours colours;
        f32 alpha = fade * (enabled ? 1.0f : 0.45f);
        colours.label = Faded(selected ? White : Ink, alpha);
        colours.pillFill = Faded(selected ? Hex(0xffffff, 0.25f) : PillFill, alpha);
        colours.pillText = Faded(selected ? White : PillBlue, alpha);
        colours.chevron = Faded(selected ? White : Orange, alpha);
        regions.push_back({HitKind::Row, rowLeft, std::max(top, rowsTop), rowWidth, RowHeight, index, -1, -1});
        TextStyle label = Body(21.0f, colours.label);
        f32 labelRoom = (row.kind == RowBinding ? ColumnMiddle[0] - CellWidth * 0.5f - 12.0f
                         : row.kind == RowAction ? rowLeft + rowWidth - RowPad
                                                 : valueCentre - ValueWidth * 0.5f - 8.0f) -
                        (rowLeft + RowPad);
        f32 labelWidth = OverlayTextWidth(row.label, label);
        if (labelWidth > labelRoom)
        {
            label.size *= labelRoom / labelWidth;
        }
        frame.Text(row.label, rowLeft + RowPad, middle, label);
        switch (row.kind)
        {
        case RowChoice:
            ChoiceValue(frame, row.valueText(row.get()), valueCentre, middle, colours, index, top, regions);
            break;
        case RowSlider:
            SliderValue(frame, row, valueCentre - ValueWidth * 0.5f, middle, colours, index, top, regions);
            break;
        case RowBinding:
            BindingCells(frame, view, row, index, middle, fade, regions);
            break;
        default:
            break;
        }
    }

    // The controls table's header: Action, Key, Alt key, Controller over the columns, stuck to the panel's top once the table
    // scrolls under it
    if (headerLine >= 0)
    {
        f32 headerTop = std::max(rowsTop, rowsTop + static_cast<f32>(headerLine) * RowPitch - a.scroll);
        if (headerTop < rowsTop + rowsHeight)
        {
            BoxStyle backing;
            backing.fill = Faded(White, fade);
            frame.Box(rowLeft, headerTop, rowWidth, RowHeight, backing);
            BoxStyle divider;
            divider.fill = Faded(Separator, fade);
            frame.Box(rowLeft, headerTop + RowHeight - 2.0f, rowWidth, 2.0f, divider);
            f32 middle = headerTop + RowHeight * 0.5f;
            TextStyle header = Body(15.0f, Faded(Muted, fade), true);
            frame.Text("Action", rowLeft + RowPad, middle, header);
            const char* const Headers[BindingColumns] = {"Key", "Alt key", "Controller"};
            for (int column = 0; column < BindingColumns; column++)
            {
                frame.Text(Headers[column], ColumnMiddle[column], middle, header, OverlayAlign::Centre);
            }
        }
    }
    frame.NoClip();

    // The scrollbar: thin and orange, when the rows overflow
    if (maxScroll > 0.0f)
    {
        f32 trackHeight = rowsHeight - 8.0f;
        f32 thumbHeight = std::max(30.0f, trackHeight * rowsHeight / contentHeight);
        f32 thumbTop = rowsTop + 4.0f + (trackHeight - thumbHeight) * (a.scroll / maxScroll);
        BoxStyle thumb;
        thumb.fill = Orange;
        thumb.radius = 3.0f;
        frame.Box(PanelX + PanelWidth - 9.0f, thumbTop, 5.0f, thumbHeight, thumb);
    }

    // The description of the highlighted row (or what's waited for)
    std::string description = view.capturing ? "Press a key or a mouse button for it (Esc cancels)"
                              : selectedRow == nullptr ? ""
                              : selectedRow->enabled && !selectedRow->enabled() ? "Not now"
                                                                                : selectedRow->description;
    TextStyle describe = Body(19.0f, Hex(0xd6e8ff));
    describe.dropY = 1.0f;
    describe.dropColour = Hex(0x000000, 0.3f);
    frame.Text(description, 360.0f, 612.0f + OverlayLineHeight(describe) * 0.5f, describe);

    // The countdown: keep these display settings?
    if (view.keeping)
    {
        char seconds[32];
        std::snprintf(seconds, sizeof(seconds), "Going back in %d s", std::max(view.keepSecondsLeft, 0));
        QuestionBox(frame, "Keep these display settings?", seconds, "Keep", "Go back", 0, regions);
    }

    // The bar of prompts for the device in use
    std::vector<FooterItem> items;
    if (selectedRow != nullptr && selectedRow->kind == RowBinding && view.column != ColumnPad)
    {
        items.push_back({ClearPrompt(view.prompts), {}, false, "Clear", HitKind::FooterClear});
    }
    if (pages.size() > 1)
    {
        items.push_back({TabPrompt(view.prompts, false), TabPrompt(view.prompts, true), true, "Tabs", HitKind::FooterTabs});
    }
    items.push_back({SelectPrompt(view.prompts), {}, false, "Select", HitKind::FooterSelect});
    items.push_back({BackPrompt(view.prompts), {}, false, "Back", HitKind::FooterBack});
    Footer(frame, items, view.prompts, regions);
}

void DrawPause(OverlayFrame& frame, const MenuView& view, std::vector<HitRegion>& regions)
{
    // The game's picture dimmed, a blue wash from the left under the buttons
    BoxStyle dim;
    dim.fill = Hex(0x04122a, 0.55f);
    frame.Box(-1000.0f, -1000.0f, ReferenceWidth + 2000.0f, ReferenceHeight + 2000.0f, dim);
    BoxStyle wash;
    wash.fill = Hex(0x0a3b7a, 0.55f);
    wash.fillBottom = Hex(0x071f45, 0.55f);
    frame.Box(-1000.0f, -1000.0f, 1000.0f + 440.0f, ReferenceHeight + 2000.0f, wash);
    Title(frame, "PAUSED");
    f32 y = 170.0f;
    for (size_t i = 0; i < view.pauseItems.size(); i++)
    {
        const PauseItem& item = view.pauseItems[i];
        SideButton(frame, item.label, y, 320.0f, 52.0f, static_cast<int>(i) == view.pauseSelected && !view.confirmingQuit,
                   item.enabled, 26.0f);
        regions.push_back({HitKind::PauseItem, 60.0f, y, 320.0f, 52.0f, static_cast<int>(i), -1, -1});
        y += 64.0f;
    }
    if (view.confirmingQuit)
    {
        QuestionBox(frame, "Quit the game?", "What isn't saved is lost", "Quit", "Cancel", view.boxChoice, regions);
    }
    std::vector<FooterItem> items;
    items.push_back({SelectPrompt(view.prompts), {}, false, "Select", HitKind::FooterSelect});
    items.push_back({BackPrompt(view.prompts), {}, false, view.confirmingQuit ? "Cancel" : "Resume", HitKind::FooterBack});
    Footer(frame, items, view.prompts, regions);
}
}

void DrawMenus(OverlayFrame& frame, const MenuView& view, std::vector<HitRegion>& regions)
{
    if (view.screen == Screen::Pause)
    {
        DrawPause(frame, view, regions);
    }
    else if (view.screen == Screen::Options || view.screen == Screen::Extras)
    {
        DrawPages(frame, view, regions);
    }
}

void DrawGamePrompt(OverlayFrame& frame, const std::string& text, f32 left, f32 top, f32 width, f32 height)
{
    // Near the picture's bottom, in the footer's words' look on a dark pill
    TextStyle style = Body(22.0f, White, true);
    style.outline = 2.0f;
    style.outlineColour = Hex(0x000000, 0.6f);
    style.dropY = 2.0f;
    style.dropColour = Hex(0x000000);
    f32 textWidth = OverlayTextWidth(text, style);
    f32 middle = top + height * 0.9f;
    f32 pillHeight = OverlayLineHeight(style) + 10.0f;
    BoxStyle pill;
    pill.fill = Hex(0x000000, 0.45f);
    pill.radius = pillHeight * 0.5f;
    frame.Box(left + width * 0.5f - textWidth * 0.5f - 22.0f, middle - pillHeight * 0.5f, textWidth + 44.0f, pillHeight, pill);
    frame.Text(text, left + width * 0.5f, middle, style, OverlayAlign::Centre);
}

void DrawCursor(OverlayFrame& frame, f32 x, f32 y, bool overClickable)
{
    // The Wumpa fruit, its top the hot spot, swelling a little over what can be clicked
    f32 size = 30.0f * (overClickable ? 1.12f : 1.0f);
    frame.Image(OverlayImage::Cursor, x - size * 0.3f, y - size * 0.1f, size, size);
}
}
