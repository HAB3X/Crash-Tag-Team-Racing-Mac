#pragma once

// VU1's programs run as C++ translated from the game's microcode at build time (recomp/vutranslate.py, from the executable's
// .vutext), the interpreter (vu.cpp) running whatever has no translation. The results are the interpreter's, bit for bit:
// $PS2_VU1 chooses
//
//     (unset)      translated
//     interpret    the interpreter alone
//     verify       each program run twice, by the interpreter on a copy and translated, and compared: every XGKICK's packet
//                  (VU1's data memory) and registers, and at the program's end the registers, flags and memory
//
// A program's translation is used where micro memory holds its code (checked whenever the code changes), wherever it was
// uploaded to.

#include "common.h"

namespace Ee
{
class Vu;

// Makes VU1 run its programs translated (or as $PS2_VU1 says)
void AttachVu1Translation(Vu& vu1);
// Makes VU0 run its microprograms translated, the same way ($PS2_VU0: the same choices; none are translated yet)
void AttachVu0Translation(Vu& vu0);

// Statistics: the instructions run translated, and (verify) the runs and kicks compared and the differences found
struct Vu1TranslationStats
{
    u64 translatedInstructions = 0;
    u64 interpretedInstructions = 0;
    u64 entries = 0;
    u64 verifiedRuns = 0;
    u64 verifiedKicks = 0;
    u64 differences = 0;
};
const Vu1TranslationStats& GetVu1TranslationStats();
}
