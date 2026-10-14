// Radical's fibers (its radThread library's cooperative threads, several to a PS2 thread): they switch with the C library's
// setjmp and longjmp (config/cttr.toml's setjmp and longjmp, which the runtime follows on the host), and a new one starts on
// its own stack here
#include "ps2/recomp.h"

// 0x3B4EB0, called by the switch (0x3B4710) for a fiber's first run: SP = a0 (the fiber's stack), then on at a1 (0x3B4E30, which
// runs the fiber's function)
extern "C" void CttrFiberStart(Ps2Cpu* c) { ps2_switch_stack(c, c->r[4].uw[0], c->r[5].uw[0]); }
