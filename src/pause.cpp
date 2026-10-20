// The game's own pause menu kept, as the Wrath of Cortex port keeps its (Ps2GameInfo::ownPause): Escape, the PC menus' key, is
// the game's Start, so it pauses in the park and in races, starts on the title and goes on from the pause menu (CONTINUE).
// The frontend's menus take Start as a choice, as cross is (the Single Player menu's ENTER THE PARK chosen), where a PC
// player's Escape goes back: in the menus mode (src/controls.cpp) Escape is triangle, the menus' back, except while the pause
// menu itself is up, where triangle does nothing and Start goes back to the game.
//
// The game's screens are Radical's presentation screens: each frame the screen up is updated by Screen::Update (0x2680E8, a0
// the screen), which reads the menus' inputs and leaves through Screen::Exit (0x267F38). A screen's class is its vtable (+0):
// the pause menu's, PauseScreen's, at 0x55A158 (its type_info's name "11PauseScreen" at 0x55A2A0).
#include "ps2/iop.h"
#include "ps2/recomp.h"
#include "ps2/runtime.h"

#include <atomic>
#include <cstring>

extern "C" void f_002680e8(Ps2Cpu* c);

int CttrMenu();

namespace
{
constexpr u32 PauseScreen = 0x55A158;

// The game's vblanks, and the last one the pause menu was updated at (it's up while that's recent)
std::atomic<u32> g_vblanks{0};
std::atomic<u32> g_pauseAt{0};
std::atomic<bool> g_pauseSeen{false};

u32 Read32(u32 address)
{
    u32 v;
    std::memcpy(&v, Ps2::EeMemory(address), 4);
    return v;
}

bool PauseUp() { return g_pauseSeen && g_vblanks.load() - g_pauseAt.load() < 15; }
}

// Screen::Update (0x2680E8): the pause menu noted, then updated as before
extern "C" void CttrScreenUpdate(Ps2Cpu* c)
{
    if (Read32(c->r[4].uw[0]) == PauseScreen)
    {
        g_pauseAt = g_vblanks.load();
        g_pauseSeen = true;
    }
    f_002680e8(c);
}

void CttrPauseVblank() { g_vblanks++; }

// What Escape presses: triangle in the frontend's menus, the game's Start everywhere else
const char* CttrMenuKeyPress() { return CttrMenu() && !PauseUp() ? "triangle" : nullptr; }
