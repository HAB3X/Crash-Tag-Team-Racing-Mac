// Crash Tag Team Racing for Mac: the recompiled executable on the PS2 runtime
#include "ps2/runtime.h"

void CttrRegisterRadIop();
void CttrRegisterAudio();
void CttrPcText();
void CttrControlsVblank();
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
static void CttrVblank() { CttrControlsVblank(); }

int main(int argc, char** argv)
{
    // The PC controls in three modes, the menus, on foot and driving (src/controls.cpp). The PC menus are the runtime's own:
    // Escape opens them (and a gamepad's Back, unless it's set to be Select), and Return is the game's Start, its pause
    static const Ps2GameInfo game = {"Crash Tag Team Racing (USA)",
                                     "SLUS_211.91",
                                     4928124,
                                     CttrInit,
                                     nullptr,
                                     CttrVblank,
                                     CttrMenu,
                                     nullptr,
                                     0,
                                     nullptr,
                                     0,
                                     "Crash Tag Team Racing",
                                     0,
                                     CttrControls(),
                                     CttrControlCount(),
                                     CttrModes(),
                                     CttrModeCount(),
                                     CttrMode};
    return ps2_main(argc, argv, &game);
}
