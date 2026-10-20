#pragma once

// The emulated graphics hardware as one set (the Twinsanity port's graphics/hardware.h without its DMA chains): the GS, the
// GIF, VU0 and VU1 with their VIFs. Whoever runs the hardware makes them and answers GetHardware (ps2-native-runtime: its
// gs.cpp); the hardware renderer finds the GS through it

#include "common.h"

namespace Gs
{
class Gs;
}

namespace Ee
{
class Gif;
class Vu;
class Vif;
}

namespace NativeGraphics
{
struct Hardware
{
    Gs::Gs* gs;
    Ee::Gif* gif;
    Ee::Vu* vu0;
    Ee::Vu* vu1;
    Ee::Vif* vif0;
    Ee::Vif* vif1;
};

Hardware& GetHardware();
}
