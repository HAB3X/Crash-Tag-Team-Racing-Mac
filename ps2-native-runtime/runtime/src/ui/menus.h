// The PC menus: the runtime's own menus over the game, as the Crash Twinsanity native port has them (its options screen's look,
// and its PC conveniences). The menu key (Escape, or a gamepad's Back button) opens the pause menu: the game is paused under it
// (its code stops, its time and sound with it, its last picture stays): Resume, Options, Extras (when the game has any), the
// game's own pause actions, Quit. Options has tabs: Gameplay, Display, Sound and Controls with the runtime's own settings and
// the game's own (ps2/runtime.h's Ps2GameSetting, Ps2GameAction) on the tabs they name, then the game's other tabs. Everything
// is drawn over the window at its own resolution (overlay.h), taken from the keyboard, the mouse and a gamepad. The game's
// prompt (ps2_show_prompt) is drawn in the same style while it plays.
//
// Threads: everything here is the presenter's (the host's main thread), except MenusOpen and PadBackIsSelect (any thread) and
// what the game's settings and actions are given to do on the game's side (KernelOnGameSide)
#pragma once

#include "ps2/recomp.h"

#include <string>

union SDL_Event;
struct SDL_FRect;

namespace Ps2::Ui
{
// A gamepad's control, as the Controls table shows it for an action (by the gamepad's style)
enum class PadControl : int
{
    None,
    South,
    East,
    West,
    North,
    LeftShoulder,
    RightShoulder,
    LeftTrigger,
    RightTrigger,
    Start,
    // The Back button: Select, or the PC menus' (the Controls tab's choice)
    BackAsSelect,
    BackAsMenu,
    LeftStick,
    RightStick,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    LeftStickUp,
    LeftStickDown,
    LeftStickLeft,
    LeftStickRight,
    RightStickUp,
    RightStickDown,
    RightStickLeft,
    RightStickRight,
};

// The settings (options.txt in the settings folder) read and applied (the master volume); the game's own saved choices are set
// at its first vertical blank (MenusGameVblank)
void MenusInit();
// On the game's side at each vertical blank
void MenusGameVblank();
// Whether the menus are up (the game paused under them). Any thread
bool MenusOpen();
// Whether a gamepad's Back button is the PS2's Select (else it opens the PC menus). Any thread
bool PadBackIsSelect();
// An event of the window's: true when it's the menus' (all of the keyboard's, the mouse's and the gamepad's while they're up;
// the menu key, Cmd+F and Alt+Enter while they aren't)
bool MenusEvent(const SDL_Event& e);
// Once a turn of the presenter's: the input taken, the script (--ui) played on (gameSeconds: the game's time)
void MenusFrame(double gameSeconds);
// Whether the menus are drawn over the picture now (it's drawn again and again while they are)
bool MenusVisible();
// Quit was chosen: the program ends
bool MenusQuitting();
// A picture of the window with the menus the script asked for (its path; empty: none)
std::string MenusShotDue();
// The overlay drawn over the window's framebuffer (bound, of these pixels; the picture's place in them)
void MenusDraw(int pixelWidth, int pixelHeight, float pixelScale, const SDL_FRect& picture);
}
