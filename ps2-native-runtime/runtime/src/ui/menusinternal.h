// The PC menus' model as their view sees it (menus.cpp keeps it and takes the input, menuview.cpp draws it and records where its
// parts are for the mouse). From the Crash Twinsanity native port's ui/settingsmodel.h and uicanvas.h (same author)
#pragma once

#include "menus.h"
#include "overlay.h"

#include <functional>
#include <string>
#include <vector>

namespace Ps2::Ui
{
enum RowKind
{
    RowChoice,
    RowSlider,
    RowAction,
    RowBinding,
    // The controls table's column names
    RowHeader,
    // A heading over a group of rows (a game's actions' group)
    RowHeading,
};

struct Row
{
    RowKind kind = RowAction;
    std::string label;
    std::string description;
    // A choice's or a slider's value (a choice's outside its range: a value of its own, valueText's), and its range
    std::function<int()> get;
    std::function<void(int)> set;
    int minimum = 0;
    int maximum = 1;
    std::function<std::string(int)> valueText;
    // An action's
    std::function<void()> activate;
    // A binding row's action (bindings.txt's)
    int action = -1;
    // Whether it's on the page (null: always), and whether it can be chosen now (null: always; greyed when not)
    std::function<bool()> shown;
    std::function<bool()> enabled;
    // A display row whose change asks to be kept (the countdown)
    bool asksToKeep = false;
};

struct Page
{
    std::string title;
    std::vector<Row> rows;
};

enum class Screen
{
    None,
    Pause,
    Options,
    Extras,
};

// How the prompts show the controls: the keyboard's keycaps, or a gamepad's buttons
enum class PromptStyle
{
    Keyboard,
    PlayStation,
    Xbox,
    Switch,
};

// The controls table's columns: the key, the alt key, the gamepad's control
enum BindingColumn : int
{
    ColumnKey,
    ColumnAltKey,
    ColumnPad,
    BindingColumns,
};

// What a part of the screen is, for the mouse (reference pixels)
enum class HitKind
{
    Row,
    Cell,
    Tab,
    ArrowLeft,
    ArrowRight,
    Slider,
    FooterSelect,
    FooterBack,
    FooterTabs,
    FooterClear,
    BoxYes,
    BoxNo,
    PauseItem,
};

struct HitRegion
{
    HitKind kind;
    f32 x;
    f32 y;
    f32 width;
    f32 height;
    int row;
    int column;
    int tab;

    bool Contains(f32 px, f32 py) const { return px >= x && px <= x + width && py >= y && py <= y + height; }
};

// The pause menu's items
struct PauseItem
{
    std::string label;
    bool enabled;
};

// What the view draws this frame
struct MenuView
{
    Screen screen;
    const std::vector<Page>* pages;
    int page;
    int row;
    int column;
    bool capturing;
    bool keeping;
    int keepSecondsLeft;
    // The rows swapped by a binding just made, while they flash (-1 none)
    int flashRows[2];
    bool flashOn;
    // When the page changed (the view's cross-fade)
    unsigned pageChanges;
    std::vector<PauseItem> pauseItems;
    int pauseSelected;
    // The box asking whether to quit, and its button chosen (0 yes, 1 no)
    bool confirmingQuit;
    int boxChoice;
    // The prompts' style (the device in use) and the gamepad's (the Controls table's gamepad column)
    PromptStyle prompts;
    PromptStyle padStyle;
};

// The game's title for the screens' subtitle
std::string GameTitle();
// The list's first row shown (the view keeps the selection in its window)
int& MenuScroll();
// A gamepad's control's name in a style, as text ("L1", "LB", "Left stick up"...; "" when the style draws it as a button)
std::string PadControlText(PadControl control, PromptStyle style);

// The view (menuview.cpp)
class OverlayFrame;
void DrawMenus(OverlayFrame& frame, const MenuView& view, std::vector<HitRegion>& regions);
// The game's prompt at the bottom of the picture (its place in reference pixels), at an opacity (0-1: it fades in and out)
void DrawGamePrompt(OverlayFrame& frame, const std::string& text, f32 opacity, f32 left, f32 top, f32 width, f32 height);
// The cursor (the scripted pictures' pointer: there's no system pointer in them)
void DrawCursor(OverlayFrame& frame, f32 x, f32 y, bool overClickable);
}
