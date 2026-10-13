// Calls by address (the function table), COP0 and the FPU's control registers, and the instructions that stop the program
#include "runtime.h"

#include <algorithm>

namespace Ps2
{
const Ps2FunctionEntry* FindFunction(u32 address)
{
    const Ps2FunctionEntry* begin = ps2_function_table;
    const Ps2FunctionEntry* end = ps2_function_table + ps2_function_count;
    const Ps2FunctionEntry* it =
        std::upper_bound(begin, end, address, [](u32 a, const Ps2FunctionEntry& e) { return a < e.start; });
    if (it == begin)
    {
        return nullptr;
    }
    --it;
    return address < it->end ? it : nullptr;
}

void Call(Ps2Cpu* c, u32 address) { ps2_call(c, address); }
}

using namespace Ps2;

extern "C" void ps2_call(Ps2Cpu* c, u32 address)
{
    const Ps2FunctionEntry* f = FindFunction(address & 0x1FFFFFFF);
    if (f == nullptr || f->start != (address & 0x1FFFFFFF))
    {
        if (f != nullptr)
        {
            Fatal("call to %08x: inside the function at %08x, not a start (add it to the config's extra functions)", address,
                  f->start);
        }
        Fatal("call to %08x: no code there", address);
    }
    PS2_LOG(LogCall, "call %08x", address);
    c->depth++;
    f->function(c);
    c->depth--;
}

extern "C" void ps2_break(Ps2Cpu* c, u32 pc)
{
    (void)c;
    Log("break at %08x", pc);
}

extern "C" void ps2_eret(Ps2Cpu* c, u32 pc)
{
    (void)c;
    Fatal("eret at %08x", pc);
}

extern "C" u32 ps2_mfc0(Ps2Cpu* c, int reg)
{
    (void)c;
    switch (reg)
    {
    case 9: // Count: the core's clock, twice the bus's
        return static_cast<u32>(BusCycles() * 2);
    case 12: // Status: kernel mode (the games' libraries read it there), IE, INT0 and INT1 on, EIE as EI and DI set it
        return 0x70000C01 | (InterruptsEnabled() ? 0x10000u : 0);
    case 15: // PRId
        return 0x2E20;
    case 16: // Config
        return 0x440;
    default:
        return 0;
    }
}

extern "C" void ps2_mtc0(Ps2Cpu* c, int reg, u32 value)
{
    (void)c;
    if (Once("mtc0 " + std::to_string(reg)))
    {
        PS2_LOG(LogKernel, "mtc0 $%d = %08x", reg, value);
    }
}

extern "C" int ps2_cpcond0(Ps2Cpu* c)
{
    (void)c;
    // The DMAC's channels in D_PCR's CPC bits are all done: transfers finish when they're started
    return 1;
}

extern "C" u32 ps2_cfc1(Ps2Cpu* c, int reg)
{
    if (reg == 0)
    {
        return 0x2E30;
    }
    if (reg == 31)
    {
        return c->fcr31;
    }
    return 0;
}

extern "C" void ps2_ctc1(Ps2Cpu* c, int reg, u32 value)
{
    if (reg == 31)
    {
        c->fcr31 = value;
    }
}
