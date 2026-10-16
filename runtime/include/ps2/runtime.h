// What a game's program gives the runtime
#pragma once

#include <stdint.h>

// A game's own setting in the runtime's PC menus (the pause menu's Options and Extras): a row with choices. Its tab: "Gameplay",
// "Display", "Sound" or "Controls" put it on the runtime's own tab of that name (after the runtime's rows), "Extras" on the
// Extras screen (the pause menu has Extras when the game has any), any other name a tab of its own after the runtime's. The
// choice made is saved in the settings folder's options.txt (by its tab, name and choice's label) and set again at the game's
// first vertical blank of each run
typedef struct Ps2GameSetting
{
    const char* tab;              // the Options tab it's on ("Gameplay", "Sound", "Extras"...)
    const char* name;             // its label
    const char* const* choices;   // the choices' labels, ending with NULL ("Off", "On", NULL)
    int (*get)(void);             // the choice now (asked on the presenter's thread: reading the game's memory is fine)
    void (*set)(int choice);      // a choice made (called on the game's side, between its instructions; saved by the runtime)
} Ps2GameSetting;

// A game's own action in the PC menus (a button: e.g. a level of the level select). Its tab as a setting's; "Pause" makes it an
// item of the pause menu itself, before Quit (e.g. "Quit to title"). A group's heading is shown over each run of actions in it.
// An action not available now is shown greyed
typedef struct Ps2GameAction
{
    const char* tab;              // the tab ("Extras")
    const char* group;            // a heading over a group of actions (NULL: none)
    const char* name;             // its label
    int (*available)(void);       // whether it can be chosen now (NULL: always)
    void (*run)(void);            // chosen: called on the game's side, between its instructions, with the menu closed
} Ps2GameAction;

// A game's own control on the keyboard and mouse (the PC menus' Controls and bindings.txt), for a game the runtime's defaults
// (the Crash Twinsanity port's) don't suit, or whose controls differ by what's being played (on foot, driving...): what it
// presses on port 0's controller, its keys by default, and its mode. Only the controls of the mode the game is in
// (Ps2GameInfo::mode) press anything, so a key can be a different control in each mode, and a key taken for a control is
// taken from another control of its mode only. The runtime adds the PC menus' key (Escape) to them
typedef struct Ps2GameControl
{
    const char* name;   // its name in bindings.txt ("drive_accelerate")
    const char* label;  // its row in Controls ("Accelerate")
    const char* press;  // what it presses: PS2 buttons as --press names them, joined by + ("cross", "l1", "cross+start"), a
                        // stick's direction ("left_up", "left_down", "left_left", "left_right", "right_up", "right_down",
                        // "right_left", "right_right"), or "walk" (the left stick at half, on and off)
    const char* keys;   // its keys by default as bindings.txt names them, joined by commas ("W, Up"; "" for none)
    int mode;           // its mode: an index into Ps2GameInfo::modes
} Ps2GameControl;

typedef struct Ps2GameInfo
{
    const char* title;
    // The executable SYSTEM.CNF boots, and its size: the recompiled code is for that one
    const char* executable;
    uint32_t executableSize;
    // Called before the boot: the game's own IOP servers (ps2/iop.h) are registered here
    void (*init)(void);
    // Optional. Whether the game is taking typed text now (a name's entry): the keyboard types instead of pressing buttons,
    // and what's typed waits in ps2_take_typed. Asked from the presenter's thread (reading the game's memory is fine)
    int (*typing)(void);
    // Optional. Called at every vertical blank on the game's side, between its instructions (the game's own memory can be
    // changed here: typed text put in)
    void (*vblank)(void);
    // Optional. Whether a menu is up: Return is then cross (confirm) and Backspace triangle (back), and the mouse's left
    // button clicks (ps2_take_click) instead of pressing its bound button
    int (*menu)(void);
    // Optional. The game's own settings and actions in the runtime's PC menus
    const Ps2GameSetting* settings;
    int settingCount;
    const Ps2GameAction* actions;
    int actionCount;
    // Optional. The settings folder's name in ~/Library/Application Support (the saves, the options, the disc's path);
    // NULL: "PS2 Native Runtime/<executable>"
    const char* folder;
    // Optional. Nonzero: the game's own pause menu is kept. The menu key (Escape) is then Start, a gamepad's Back is Select,
    // and the PC menus have no pause menu of their own: their Options (with an Extras tab for the game's Extras and Pause
    // actions) open when the game asks (ps2_open_settings, e.g. from a line of its own options menu), and going back from them
    // goes back to the game
    int ownPause;
    // Optional. The game's own keyboard and mouse controls (above) instead of the runtime's, and their modes: the modes' names
    // (Controls' headings, in this order) and the mode the game is in now (an index into modes; asked on the presenter's
    // thread at each input update, so reading the game's memory is fine). A game with controls of its own decides what Return
    // and Backspace are in its menus: the runtime's own (Return is cross and Backspace triangle while menu says a menu is up)
    // are for its defaults
    const Ps2GameControl* controls;
    int controlCount;
    const char* const* modes;
    int modeCount;
    int (*mode)(void);
} Ps2GameInfo;

// A prompt shown over the picture near its bottom (the PC menus' style: e.g. "Hold any button to skip"), NULL to take it down
// (it fades in and out); not shown while the PC menus are up. Any thread
#ifdef __cplusplus
extern "C"
#endif
    void
    ps2_show_prompt(const char* text);

// Whether the PC menus are up (the game is paused under them: its code stops at its next poll point, its time and sound stand
// still)
#ifdef __cplusplus
extern "C"
#endif
    int
    ps2_menus_open(void);

// Whether any button, key or mouse button of the player's is held now (for "hold any button to skip")
#ifdef __cplusplus
extern "C"
#endif
    int
    ps2_any_input_held(void);

// The mouse over the picture: where it is (0-1 across and down the PS2's picture; false when it's outside it or hasn't moved
// since the last ask), and its left button's clicks, taken one at a time (where, the same way)
#ifdef __cplusplus
extern "C"
#endif
    int
    ps2_mouse_moved(float* x, float* y);
#ifdef __cplusplus
extern "C"
#endif
    int
    ps2_take_click(float* x, float* y);

// While the game says a menu is up (Ps2GameInfo::menu): the mouse's other buttons' clicks (kind: SDL's button numbers, 2 middle, 3
// right, 4 and 5 the extra ones) and the wheel's turns (PS2_MOUSE_WHEEL_UP, PS2_MOUSE_WHEEL_DOWN), each with where the mouse was
// (0-1 over the picture), taken one at a time. The left button's clicks come from ps2_take_click
#define PS2_MOUSE_WHEEL_UP 100
#define PS2_MOUSE_WHEEL_DOWN 101
#ifdef __cplusplus
extern "C"
#endif
    int
    ps2_take_mouse_event(int* kind, float* x, float* y);

// What's been typed while the game was taking text (UTF-8; '\b' for backspace, '\r' for return), taken: up to max bytes into
// out, the count returned
#ifdef __cplusplus
extern "C"
#endif
    int
    ps2_take_typed(char* out, int max);

// The style the player's prompts are in now, as the PC menus' Button prompts setting decides (the device used last, or always
// one): 0 the keyboard and mouse, 1 PlayStation, 2 Xbox, 3 Nintendo Switch. Any thread
#ifdef __cplusplus
extern "C"
#endif
    int
    ps2_prompt_style(void);

// What the player presses for one of port 0's controls in the game's current mode, named for a prompt in a style (above): a
// control is a PS2 button or several as --press names them ("cross", "l1"), "left" or "right" (a stick, all its directions),
// or "dpad". On the keyboard, the first key bound to it ("Space"; a stick's or the d-pad's four keys "W/A/S/D", "WASD" and
// "Arrows" for those); on a gamepad, its button's name in the style ("A", "Cross", "B", "LB", "Left stick", "D-pad"). Written to
// out (with its 0, at most size bytes); its length, 0 when nothing presses it. The game's side, between its instructions
#ifdef __cplusplus
extern "C"
#endif
    int
    ps2_control_name(const char* control, int style, char* out, int size);

// Port 0's buttons (PS2 bits: cross is 1 << 14) pressed from the game's side for a while (seconds of game time)
#ifdef __cplusplus
extern "C"
#endif
    void
    ps2_press_buttons(uint32_t buttons, double seconds);

// The least time between the starts of two taps, in the game's frames (0: none, the default). A tap shorter than the
// game's frame is shown to it for whole frames, and taps close together one after the other; for a menu that ignores input for a
// while after each move, this keeps quick taps from falling in that time (each waits its turn). A key held isn't spaced. Any thread
#ifdef __cplusplus
extern "C"
#endif
    void
    ps2_set_tap_spacing(int vblanks);

// The PC menus' Options opened over the game (paused under them), for a game with its own pause menu (Ps2GameInfo::ownPause).
// Any thread
#ifdef __cplusplus
extern "C"
#endif
    void
    ps2_open_settings(void);

// Runs the game: parses the options, boots the disc, and returns only when the game ends
int ps2_main(int argc, char** argv, const Ps2GameInfo* game);
