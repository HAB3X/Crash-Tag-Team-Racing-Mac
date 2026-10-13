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
} Ps2GameInfo;

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
