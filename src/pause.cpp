// The game's own pause menu kept, as the Wrath of Cortex port keeps its (Ps2GameInfo::ownPause): there's no pause menu of the
// runtime's, and Escape is the game's to bind in each mode (src/controls.cpp): its Start on foot and driving, so it pauses in the
// park and in races; the frontend menus' back there (they take Start as a choice, as cross is: the Single Player menu's ENTER
// THE PARK chosen); and Start again while the pause menu itself is up (a mode of its own), where triangle does nothing.
//
// The game's screens are Radical's presentation screens: each frame the screen up is updated by Screen::Update (0x2680E8, a0
// the screen), which reads the menus' inputs and leaves through Screen::Exit (0x267F38). A screen's class is its vtable (+0):
// the pause menu's, PauseScreen's, at 0x55A158 (its type_info's name "11PauseScreen" at 0x55A2A0), and its PREFERENCES',
// PausePreferencesScreen's, at 0x559F68 (seen updated as the pause menu's PREFERENCES is chosen).
#include "ps2/iop.h"
#include "ps2/recomp.h"
#include "ps2/runtime.h"

#include <atomic>
#include <cstring>

extern "C" void f_002680e8(Ps2Cpu* c);


namespace
{
constexpr u32 PauseScreen = 0x55A158;
constexpr u32 PausePreferencesScreen = 0x559F68;

// The game's vblanks, and the last one the pause menu was updated at (it's up while that's recent)
std::atomic<u32> g_vblanks{0};
std::atomic<u32> g_pauseAt{0};
std::atomic<bool> g_pauseSeen{false};
std::atomic<u32> g_preferencesAt{0};
std::atomic<bool> g_preferencesSeen{false};

u32 Read32(u32 address)
{
    u32 v;
    std::memcpy(&v, Ps2::EeMemory(address), 4);
    return v;
}

bool PauseUp() { return g_pauseSeen && g_vblanks.load() - g_pauseAt.load() < 15; }
bool PreferencesUp() { return g_preferencesSeen && g_vblanks.load() - g_preferencesAt.load() < 15; }
}

// Screen::Update (0x2680E8): the pause menu noted, then updated as before
extern "C" void CttrScreenUpdate(Ps2Cpu* c)
{
    u32 screen = Read32(c->r[4].uw[0]);
    if (screen == PauseScreen)
    {
        g_pauseAt = g_vblanks.load();
        g_pauseSeen = true;
    }
    else if (screen == PausePreferencesScreen)
    {
        g_preferencesAt = g_vblanks.load();
        g_preferencesSeen = true;
    }
    f_002680e8(c);
}

void CttrPauseVblank() { g_vblanks++; }

bool CttrPauseUp() { return PauseUp(); }

bool CttrPreferencesUp() { return PreferencesUp(); }
