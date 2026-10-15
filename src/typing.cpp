// The name's entry (Name Selection) typed from the keyboard. The screen is a grid of three rows of thirteen: A-M, N-Z, then
// BACK, -, 1-9, 0 and DONE (grid places 0-38), and six letter boxes; a typed letter goes in the way the grid puts one, by the
// screen's own input handler: the grid's cursor put next to the letter and moved onto it (which shows it in the current box),
// then chosen. The grid's cursor is then put on BACK, which shows a space in the next box: the box the next letter goes in is
// empty, as a typist expects, where the grid would show the last letter again. Backspace takes the current letter off, as the
// screen's own button for it does, and Return chooses DONE.
//
// NameScreen (USA): its update at 0x20F458, run every frame it's up (overridden: the typed text taken there, on the game's side,
// where the handler can be called), and its input handler at 0x20E7A0 (a0 the screen, a1 an input: 0-3 up, down, left and
// right on the grid, 4 Start, 5 cross (choose the grid's place: DONE finishes once there's a letter), 6 take a letter off). The
// screen: +328 the grid's row, +332 its column, +340 the box the next letter goes in (6 when all six are full), +344 the boxes'
// letters (UTF-16, two characters each). Finishing makes the name of the boxes before that one, or all six when it's the last
// (five letters typed: the five and the sixth box's space, as the grid makes them).
#include "ps2/iop.h"
#include "ps2/recomp.h"
#include "ps2/runtime.h"

#include <atomic>
#include <cctype>
#include <cstring>

extern "C" void f_0020f458(Ps2Cpu* c);
extern "C" void f_0020e7a0(Ps2Cpu* c);

namespace
{
constexpr u32 Columns = 13;
constexpr u32 Boxes = 6;
constexpr u32 PlaceBack = 26;
constexpr u32 PlaceDone = 38;
enum Input : u32
{
    Right = 3,
    Choose = 5,
    TakeOff = 6,
};

// The game's vblanks, and the last one the screen was updated at (it's up while that's recent)
std::atomic<u32> g_vblanks{0};
std::atomic<u32> g_nameScreenAt{0};
std::atomic<bool> g_nameScreenSeen{false};

u32 Read32(u32 address)
{
    u32 v;
    std::memcpy(&v, Ps2::EeMemory(address), 4);
    return v;
}

void Write32(u32 address, u32 v) { std::memcpy(Ps2::EeMemory(address), &v, 4); }

void Handle(Ps2Cpu* c, u32 screen, Input input)
{
    c->r[4].ud[0] = screen;
    c->r[5].ud[0] = input;
    f_0020e7a0(c);
}

// The grid's cursor put on a place: next to it on its row, then moved right onto it
void PointAt(Ps2Cpu* c, u32 screen, u32 place)
{
    Write32(screen + 328, place / Columns);
    Write32(screen + 332, (place % Columns + Columns - 1) % Columns);
    Handle(c, screen, Right);
}

// A character's grid place (A-Z, -, 1-9, 0), or -1
int Place(char ch)
{
    if (ch >= 'A' && ch <= 'Z')
    {
        return ch - 'A';
    }
    if (ch == '-' || ch == ' ' || ch == '_')
    {
        return 27;
    }
    if (ch >= '1' && ch <= '9')
    {
        return 28 + (ch - '1');
    }
    if (ch == '0')
    {
        return 37;
    }
    return -1;
}

void Type(Ps2Cpu* c, u32 screen, char ch)
{
    int place = Place(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    if (place < 0 || Read32(screen + 340) >= Boxes)
    {
        return;
    }
    PointAt(c, screen, static_cast<u32>(place));
    Handle(c, screen, Choose);
    if (Read32(screen + 340) < Boxes)
    {
        PointAt(c, screen, PlaceBack);
    }
}
}

// NameScreen's update (0x20F458): as before, then what's been typed put in
extern "C" void CttrNameScreenUpdate(Ps2Cpu* c)
{
    u32 screen = c->r[4].uw[0];
    f_0020f458(c);
    g_nameScreenAt = g_vblanks.load();
    g_nameScreenSeen = true;
    char typed[32];
    int n = ps2_take_typed(typed, sizeof(typed));
    for (int i = 0; i < n; i++)
    {
        if (typed[i] == '\b')
        {
            Handle(c, screen, TakeOff);
        }
        else if (typed[i] == '\r')
        {
            // DONE chosen (the grid's cursor put on it): with all six boxes full that's the screen's only way to finish
            Write32(screen + 328, PlaceDone / Columns);
            Write32(screen + 332, PlaceDone % Columns);
            Handle(c, screen, Choose);
        }
        else
        {
            Type(c, screen, typed[i]);
        }
    }
}

void CttrTypingVblank() { g_vblanks++; }

// Taking typed text while the name's screen has been updated in the last quarter of a second
int CttrTyping() { return g_nameScreenSeen && g_vblanks.load() - g_nameScreenAt.load() < 15 ? 1 : 0; }
