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
} Ps2GameInfo;

// Runs the game: parses the options, boots the disc, and returns only when the game ends
int ps2_main(int argc, char** argv, const Ps2GameInfo* game);
