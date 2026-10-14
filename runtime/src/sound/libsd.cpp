// libsd, the IOP's sound library, high level: its calls (ps2/sound.h) at the SPU2's registers as LIBSD.IRX makes them. Its
// table of each entry's register, the cores' strides (0x400 for a core's registers, 0x28 for the volumes from 0x760 on), the
// address registers' halves (bits 17-20, then bits 1-16 with the low three cleared), and sceSdInit's writes were read from the
// module's code (SDK 2.5's LIBSD.IRX, as Wrath of Cortex has it)
#include "../runtime.h"
#include "ps2/iop.h"
#include "ps2/sound.h"
#include "spu2.h"

namespace Ps2
{
namespace
{
constexpr u32 Base = Spu2::RegisterBase;

// The register of each entry (entry >> 8), from the SPU2's base, for core 0 and voice 0
constexpr u16 Registers[0x29] = {
    0x000, 0x002, 0x004, 0x006, 0x008, 0x00A, 0x00C, 0x00E, // VOLL, VOLR, PITCH, ADSR1, ADSR2, ENVX, VOLXL, VOLXR
    0x198, 0x760, 0x762, 0x764, 0x766, 0x768, 0x76A, 0x76C, // MMIX, MVOL, EVOL, AVOL, BVOLL
    0x76E, 0x770, 0x772, 0x180, 0x184, 0x1A0, 0x1A4, 0x340, // BVOLR, MVOLX, PMON, NON, KON, KOFF, ENDX
    0x188, 0x18C, 0x190, 0x194, 0x2E0, 0x33C, 0x1A8, 0x19C, // VMIXL, VMIXEL, VMIXR, VMIXER, ESA, EEA, TSA, IRQA
    0x1C0, 0x1C4, 0x1C8, 0x19A, 0x1A8, 0x1AC, 0x1AE, 0x1B0, // SSA, LSAX, NAX, ATTR, TSA, the data port, ADMAS
    0x344,                                                  // STATX
};

u32 Register(u16 entry)
{
    u32 index = entry >> 8;
    return index < 0x29 ? Registers[index] : 0x7FE;
}

void Write(u32 offset, u16 value) { Spu2::Write(Base + offset, value); }
u16 Read(u32 offset) { return Spu2::Read(Base + offset); }

// A parameter's register: a voice's, or (bit 7) a core's volume
u32 ParamRegister(u16 entry)
{
    u32 core = entry & 1;
    return Register(entry) + core * ((entry & 0x80) != 0 ? 0x28 : 0x400) + (entry & 0x3E) * 8;
}

u32 AddrRegister(u16 entry) { return Register(entry) + (entry & 1) * 0x400 + (entry & 0x3E) * 6; }
}

void SdSetParam(u16 entry, u16 value) { Write(ParamRegister(entry), value); }

u16 SdGetParam(u16 entry) { return Read(ParamRegister(entry)); }

void SdSetSwitch(u16 entry, u32 value)
{
    u32 reg = Register(entry) + (entry & 1) * 0x400;
    Write(reg, static_cast<u16>(value));
    Write(reg + 2, static_cast<u16>((value >> 16) & 0xFF));
}

u32 SdGetSwitch(u16 entry)
{
    u32 reg = Register(entry) + (entry & 1) * 0x400;
    return Read(reg) | (static_cast<u32>(Read(reg + 2)) << 16);
}

void SdSetAddr(u16 entry, u32 address)
{
    u32 reg = AddrRegister(entry);
    Write(reg, static_cast<u16>(address >> 17));
    // EEA has only its high half
    if ((entry & 0xFF00) != SD_A_EEA)
    {
        Write(reg + 2, static_cast<u16>((address >> 1) & 0xFFF8));
    }
}

u32 SdGetAddr(u16 entry)
{
    u32 reg = AddrRegister(entry);
    u32 high = static_cast<u32>(Read(reg)) << 17;
    if ((entry & 0xFF00) == SD_A_EEA)
    {
        return high | 0x1FFFF;
    }
    return high | (static_cast<u32>(Read(reg + 2)) << 1);
}

void SdSetCoreAttr(u16 entry, u16 value)
{
    u32 attr = 0x19A + (entry & 1) * 0x400;
    switch (entry & 0xE)
    {
    case SD_C_SPDIF_MODE:
        // The digital output's mode: nothing to hear here
        break;
    case SD_C_NOISE_CLK:
        Write(attr, static_cast<u16>((Read(attr) & 0xC0FF) | ((value & 0x3F) << 8)));
        break;
    default:
    {
        // Effects (bit 7), IRQ (bit 6), mute (bit 14)
        u32 bit = (entry & 0xE) == SD_C_EFFECT_ENABLE ? 7 : (entry & 0xE) == SD_C_IRQ_ENABLE ? 6 : 14;
        Write(attr, static_cast<u16>((Read(attr) & ~(1u << bit)) | ((value & 1u) << bit)));
        break;
    }
    }
}

void SdVoiceTransWrite(u32 address, const void* data, u32 size)
{
    u16* memory = Spu2::Memory();
    const u8* bytes = static_cast<const u8*>(data);
    u32 at = (address >> 1) & Spu2::AddressMask;
    for (u32 i = 0; i + 1 < size; i += 2)
    {
        memory[(at + i / 2) & Spu2::AddressMask] = static_cast<u16>(bytes[i] | (bytes[i + 1] << 8));
    }
}

// sceSdInit(0): the cores reset and on, every voice to the dry mixes, the effect areas at the top of the memory, core 0's
// output into core 1's (AVOL), every voice silent (volume 0, pitch 0x3FFF, at a block of silence that loops on itself at
// 0x5000) keyed on then off, ENDX cleared
void SdInit()
{
    Spu2::Reset();
    for (u32 core = 0; core < 2; core++)
    {
        u32 c = core * 0x400;
        Write(0x19A + c, static_cast<u16>(0xC000 | core));
        for (u32 reg : {0x188u, 0x18Cu, 0x190u, 0x194u})
        {
            Write(reg + c, 0xFFFF);
            Write(reg + 2 + c, 0xFF);
        }
        Write(0x198 + c, core == 0 ? 0xFF0 : 0xFFC);
        Write(0x2E0 + c, static_cast<u16>(0xE + core));
        Write(0x2E2 + c, 0xFFF8);
        Write(0x33C + c, static_cast<u16>(0xE + core));
        for (u32 reg : {0x180u, 0x182u, 0x184u, 0x186u})
        {
            Write(reg + c, 0);
        }
    }
    for (u32 reg = 0x760; reg < 0x770; reg += 2)
    {
        Write(reg, 0);
        Write(reg + 0x28, 0);
    }
    Write(0x790, 0x7FFF);
    Write(0x792, 0x7FFF);
    // The block of silence at 0x5000 (halfword 0x2800)
    Write(0x1A8, 0);
    Write(0x1AA, 0x2800);
    for (u32 i = 0; i < 8; i++)
    {
        Write(0x1AC, 0x0707);
    }
    for (u32 core = 0; core < 2; core++)
    {
        for (u32 voice = 0; voice < 24; voice++)
        {
            u32 v = core * 0x400 + voice * 0x10;
            Write(v + 0, 0);
            Write(v + 2, 0);
            Write(v + 4, 0x3FFF);
            Write(v + 6, 0);
            Write(v + 8, 0);
            u32 a = core * 0x400 + 0x1C0 + voice * 12;
            Write(a, 0);
            Write(a + 2, 0x2800);
        }
        Write(0x1A0 + core * 0x400, 0xFFFF);
        Write(0x1A2 + core * 0x400, 0xFF);
    }
    Spu2::Tick();
    for (u32 core = 0; core < 2; core++)
    {
        Write(0x1A4 + core * 0x400, 0xFFFF);
        Write(0x1A6 + core * 0x400, 0xFF);
    }
    Spu2::Tick();
    Write(0x340, 0);
    Write(0x342, 0);
    Write(0x740, 0);
    Write(0x742, 0);
}

namespace
{
// A core's looping block transfer: the buffer, its size, the half being taken, the handler
struct BlockTrans
{
    bool on = false;
    u32 address = 0;
    u32 size = 0;
    u32 half = 0;
    std::function<void(s32)> interrupt;
};
BlockTrans g_blockTrans[2];

// The SPU2's DMA interrupts (its own interrupt line is nobody's yet): a looping transfer goes on with its other half, then its
// handler runs
void SpuInterrupt(s32 line)
{
    s32 core = line == Spu2::Dma4Interrupt ? 0 : line == Spu2::Dma7Interrupt ? 1 : -1;
    if (core < 0 || !g_blockTrans[core].on)
    {
        return;
    }
    BlockTrans& t = g_blockTrans[core];
    t.half ^= 1;
    Spu2::DmaWrite(core, reinterpret_cast<const u16*>(IopMemory(t.address + t.half * (t.size / 2))), t.size / 4);
    if (t.interrupt)
    {
        t.interrupt(core);
    }
}
}

void SdBlockTransLoop(s32 core, u32 iopAddress, u32 size, std::function<void(s32 core)> interrupt)
{
    BlockTrans& t = g_blockTrans[core & 1];
    t = {true, iopAddress, size, 0, std::move(interrupt)};
    Spu2::SetInterruptFunction(SpuInterrupt);
    // AutoDMA on for the core (ADMAS), then the first half
    Write(0x1B0 + (core & 1) * 0x400, static_cast<u16>(1 << (core & 1)));
    Spu2::DmaWrite(core & 1, reinterpret_cast<const u16*>(IopMemory(iopAddress)), size / 4);
}

void SdBlockTransStop(s32 core)
{
    g_blockTrans[core & 1] = {};
    Write(0x1B0 + (core & 1) * 0x400, 0);
}

u32 SdBlockTransStatus(s32 core)
{
    const BlockTrans& t = g_blockTrans[core & 1];
    return t.on ? (t.half << 24) | ((t.address + t.half * (t.size / 2)) & 0xFFFFFF) : 0;
}
}
