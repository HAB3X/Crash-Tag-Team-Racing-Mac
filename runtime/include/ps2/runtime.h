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
} Ps2GameInfo;

// A prompt shown over the picture near its bottom (the PC menus' style: e.g. "Hold any button to skip"), NULL to take it down;
// not shown while the PC menus are up. Any thread
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

// What's been typed while the game was taking text (UTF-8; '\b' for backspace, '\r' for return), taken: up to max bytes into
// out, the count returned
#ifdef __cplusplus
extern "C"
#endif
    int
    ps2_take_typed(char* out, int max);

// Port 0's buttons (PS2 bits: cross is 1 << 14) pressed from the game's side for a while (seconds of game time)
#ifdef __cplusplus
extern "C"
#endif
    void
    ps2_press_buttons(uint32_t buttons, double seconds);

// Runs the game: parses the options, boots the disc, and returns only when the game ends
int ps2_main(int argc, char** argv, const Ps2GameInfo* game);
