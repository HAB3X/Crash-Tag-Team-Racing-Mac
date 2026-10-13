// What a game's program gives the runtime
#pragma once

#include <stdint.h>

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
} Ps2GameInfo;

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
