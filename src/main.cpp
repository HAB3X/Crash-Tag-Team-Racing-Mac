// Crash Tag Team Racing for Mac: the recompiled executable on the PS2 runtime
#include "ps2/runtime.h"

int main(int argc, char** argv)
{
    static const Ps2GameInfo game = {"Crash Tag Team Racing (USA)",
                                     "SLUS_211.91",
                                     4928124,
                                     nullptr,
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
