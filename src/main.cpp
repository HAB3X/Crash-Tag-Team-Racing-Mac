// Crash Tag Team Racing for Mac: the recompiled executable on the PS2 runtime
#include "ps2/runtime.h"

void CttrRegisterRadIop();
void CttrRegisterAudio();
void CttrPcText();
void CttrControlsVblank();
void CttrTypingVblank();
void CttrMenusVblank(int menu);
void CttrPauseVblank();
int CttrTyping();
int CttrMode();
int CttrMenu();
const Ps2GameControl* CttrControls();
int CttrControlCount();
const char* const* CttrModes();
int CttrModeCount();

// Before the boot: the game's own IOP modules (src/radiop.cpp: Bink's; src/audio.cpp: Radical's audio), and its texts said as
// a computer's (src/pctext.cpp)
static void CttrInit()
{
    CttrRegisterRadIop();
    CttrRegisterAudio();
    CttrPcText();
    // The network: at the boot the game loads only NETCNF.IRX and EZNETCNF.IRX (the network configuration's modules) and
    // binds none of their servers, so the loader's "loaded" is all they need for now. Online play goes here, as a virtual
    // network adapter: the IOP side of ENT_SMAP and DEV9 (loaded when the game goes online) high level, passing the EE
    // stack's Ethernet frames (libeenet's ent_smap on the EE) to the Mac and back. Not a "no adapter" answer.
}

// At each vertical blank, on the game's side
static void CttrVblank()
{
    CttrControlsVblank();
    CttrTypingVblank();
    CttrMenusVblank(CttrMenu());
    CttrPauseVblank();
}

int main(int argc, char** argv)
{
    // The PC controls in three modes, the menus, on foot and driving (src/controls.cpp), names typed (src/typing.cpp) and the
    // mouse on the menus (src/menus.cpp). The game's own pause menu is kept (src/pause.cpp): Escape is its Start (the
    // frontend menus' back), a gamepad's Back its Select
    static const Ps2GameInfo game = {"Crash Tag Team Racing (USA)",
                                     "SLUS_211.91",
                                     4928124,
                                     CttrInit,
                                     CttrTyping,
                                     CttrVblank,
                                     CttrMenu,
                                     nullptr,
                                     0,
                                     nullptr,
                                     0,
                                     "Crash Tag Team Racing",
                                     1,
                                     CttrControls(),
                                     CttrControlCount(),
                                     CttrModes(),
                                     CttrModeCount(),
                                     CttrMode,
                                     nullptr,
                                     0,
                                     nullptr};
    return ps2_main(argc, argv, &game);
}
