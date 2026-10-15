// The menus with the mouse: pointing at a menu's line moves its cursor there, clicking it chooses it (the cursor put on it and
// cross pressed), as Wrath of Cortex's src/menus.cpp does. Return and Backspace are the menus' cross and triangle by the
// controls (src/controls.cpp).
//
// The menus are pure3d's frontend menus (pure3d::frontend::Menu, its constructor at 0x304028 and destructor at 0x304080): +112
// and +116 the items (MenuItem*, begin and end), +132 its first byte the selected item, set with Menu::SetSelection (0x3044E0:
// a0 the menu, a1 the item, the old one unlit and the new one lit, the cursor moved). An item's +136 bit 3 is whether it can be
// chosen, +144 its text (pure3d::frontend::Text). Texts are drawn by Text::Draw (0x30C488), overridden to note where each
// menu's line is: a text is a box from (+92, +96), its left and bottom, +88 wide and +90 high (halfwords, in 640ths), in the
// page's units (a page is 1 wide and 0.75 high, y up), drawn through its own matrix (+16, the selected line's larger) and the
// matrix stack's top (the page centred: x - 0.5, y - 0.375), which is the picture's middle. With several menus drawn, the
// last is on top (a box asking a question, NO and YES, over the menu it asks about): only its lines are reached.
// CTTR_MENU_TRACE logs each click and the lines shown then.
#include "ps2/iop.h"
#include "ps2/recomp.h"
#include "ps2/runtime.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" void f_00304028(Ps2Cpu* c);
extern "C" void f_00304080(Ps2Cpu* c);
extern "C" void f_003044e0(Ps2Cpu* c);
extern "C" void f_0030c488(Ps2Cpu* c);

namespace
{
constexpr u32 Cross = 1u << 14;
// The graphics context (pddi), whose matrix stacks (+32, a pointer each: +0 the matrices, 64 bytes each, +8 the top's index)
// the frontend draws through
constexpr u32 Context = 0x5D5F08;

struct Line
{
    u32 menu;
    int item;
    float left, top, right, bottom; // in the picture: 0-1 across and down
};

// On the game's side: the menus there are, the lines drawn since the last vblank, the last complete set, and what the mouse
// asked for (a line to point at, and whether to choose it), done at the next text drawn
std::unordered_set<u32> g_menus;
std::vector<Line> g_drawing;
std::vector<Line> g_shown;
u32 g_pointMenu = 0;
int g_pointItem = -1;
bool g_choose = false;

u32 Read32(u32 address)
{
    u32 v;
    std::memcpy(&v, Ps2::EeMemory(address), 4);
    return v;
}

float ReadFloat(u32 address)
{
    float v;
    std::memcpy(&v, Ps2::EeMemory(address), 4);
    return v;
}

s16 Read16(u32 address)
{
    s16 v;
    std::memcpy(&v, Ps2::EeMemory(address), 2);
    return v;
}

int Selected(u32 menu) { return static_cast<s8>(*Ps2::EeMemory(menu + 132)); }

bool Choosable(u32 menu, int item) { return (Read32(Read32(Read32(menu + 112) + 4 * item) + 136) >> 3 & 1) != 0; }

// The menu and item a text is the line of, if any
bool LineOf(u32 text, u32* menu, int* item)
{
    for (u32 m : g_menus)
    {
        u32 begin = Read32(m + 112), end = Read32(m + 116);
        for (u32 at = begin; at < end && at < begin + 4 * 64; at += 4)
        {
            if (Read32(Read32(at) + 144) == text)
            {
                *menu = m;
                *item = static_cast<int>((at - begin) / 4);
                return true;
            }
        }
    }
    return false;
}

// A point of the page through a 4x4 matrix (rows, the translation last)
void Transform(u32 matrix, float& x, float& y)
{
    float m[16];
    std::memcpy(m, Ps2::EeMemory(matrix), sizeof(m));
    float tx = x * m[0] + y * m[4] + m[12];
    float ty = x * m[1] + y * m[5] + m[13];
    x = tx;
    y = ty;
}

// A text's box in the picture (menu and item left 0 and -1)
Line Box(u32 text)
{
    u32 stack = Read32(Read32(Read32(Context)) + 32);
    u32 top = Read32(stack) + Read32(stack + 8) * 64;
    float corners[2][2] = {{ReadFloat(text + 92), ReadFloat(text + 96)},
                           {ReadFloat(text + 92) + Read16(text + 88) / 640.0f, ReadFloat(text + 96) + Read16(text + 90) / 640.0f}};
    Line line{0, -1, 1, 1, 0, 0};
    for (auto& corner : corners)
    {
        float x = corner[0], y = corner[1];
        Transform(text + 16, x, y);
        Transform(top, x, y);
        float px = x + 0.5f, py = 0.5f - y / 0.75f;
        line.left = std::min(line.left, px);
        line.right = std::max(line.right, px);
        line.top = std::min(line.top, py);
        line.bottom = std::max(line.bottom, py);
    }
    return line;
}

// A text drawn: a menu's line noted
void Note(u32 text)
{
    u32 menu;
    int item;
    if (LineOf(text, &menu, &item))
    {
        Line line = Box(text);
        line.menu = menu;
        line.item = item;
        g_drawing.push_back(line);
    }
}

const Line* LineAt(float x, float y)
{
    for (const Line& line : g_shown)
    {
        if (x >= line.left && x <= line.right && y >= line.top && y <= line.bottom)
        {
            return &line;
        }
    }
    return nullptr;
}

// The mouse's wish done, on the game's side where the menu can be called: the cursor put on the line, and cross pressed for
// a click
void Point(Ps2Cpu* c)
{
    if (g_pointItem < 0 || !g_menus.count(g_pointMenu))
    {
        g_pointItem = -1;
        return;
    }
    if (Choosable(g_pointMenu, g_pointItem))
    {
        if (Selected(g_pointMenu) != g_pointItem)
        {
            u64 a0 = c->r[4].ud[0], a1 = c->r[5].ud[0];
            c->r[4].ud[0] = g_pointMenu;
            c->r[5].ud[0] = static_cast<u32>(g_pointItem);
            f_003044e0(c);
            c->r[4].ud[0] = a0;
            c->r[5].ud[0] = a1;
        }
        if (g_choose)
        {
            ps2_press_buttons(Cross, 0.1);
        }
    }
    g_pointItem = -1;
    g_choose = false;
}
}

extern "C" void CttrMenuCreate(Ps2Cpu* c)
{
    u32 menu = c->r[4].uw[0];
    f_00304028(c);
    g_menus.insert(menu);
}

extern "C" void CttrMenuDestroy(Ps2Cpu* c)
{
    g_menus.erase(c->r[4].uw[0]);
    f_00304080(c);
}

extern "C" void CttrTextDraw(Ps2Cpu* c)
{
    Point(c);
    Note(c->r[4].uw[0]);
    f_0030c488(c);
}

void CttrMenusVblank(int menu)
{
    if (!g_drawing.empty())
    {
        // With several menus drawn, the last is on top (a box asking a question over a menu): only its lines are reached
        u32 last = g_drawing.back().menu;
        g_drawing.erase(std::remove_if(g_drawing.begin(), g_drawing.end(), [last](const Line& l) { return l.menu != last; }),
                        g_drawing.end());
        g_shown.swap(g_drawing);
        g_drawing.clear();
    }
    float x, y;
    if (!menu)
    {
        // Clicks with no menu up go nowhere
        while (ps2_take_click(&x, &y))
        {
        }
        g_shown.clear();
        return;
    }
    if (ps2_mouse_moved(&x, &y))
    {
        if (const Line* line = LineAt(x, y))
        {
            g_pointMenu = line->menu;
            g_pointItem = line->item;
        }
    }
    while (ps2_take_click(&x, &y))
    {
        // CTTR_MENU_TRACE: each click and the lines shown then
        static const bool trace = std::getenv("CTTR_MENU_TRACE") != nullptr;
        if (trace)
        {
            Ps2::Log("menus: a click at %.3f, %.3f", x, y);
            for (const Line& line : g_shown)
            {
                Ps2::Log("menus:   menu %08x line %d (chosen %d): %.3f-%.3f across, %.3f-%.3f down", line.menu, line.item,
                         Selected(line.menu), line.left, line.right, line.top, line.bottom);
            }
        }
        if (const Line* line = LineAt(x, y))
        {
            g_pointMenu = line->menu;
            g_pointItem = line->item;
            g_choose = true;
        }
    }
}
