// Crash Tag Team Racing for Mac: the recompiled executable on the PS2 runtime
#include "ps2/runtime.h"

void CttrRegisterRadIop();
void CttrRegisterAudio();

// Before the boot: the game's own IOP modules (src/radiop.cpp: Bink's; src/audio.cpp: Radical's audio)
static void CttrInit()
{
    CttrRegisterRadIop();
    CttrRegisterAudio();
    // The network: at the boot the game loads only NETCNF.IRX and EZNETCNF.IRX (the network configuration's modules) and
    // binds none of their servers, so the loader's "loaded" is all they need for now. Online play goes here, as a virtual
    // network adapter: the IOP side of ENT_SMAP and DEV9 (loaded when the game goes online) high level, passing the EE
    // stack's Ethernet frames (libeenet's ent_smap on the EE) to the Mac and back. Not a "no adapter" answer.
}

int main(int argc, char** argv)
{
    static const Ps2GameInfo game = {"Crash Tag Team Racing (USA)",
                                     "SLUS_211.91",
                                     4928124,
                                     CttrInit,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     0,
                                     nullptr,
                                     0,
                                     "Crash Tag Team Racing",
                                     1};
    return ps2_main(argc, argv, &game);
}
