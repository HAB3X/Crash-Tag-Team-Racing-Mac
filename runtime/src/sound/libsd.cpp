// libsd, the IOP's sound library, high level: its calls (ps2/sound.h) at the SPU2's registers as LIBSD.IRX makes them. Its
// table of each entry's register, the cores' strides (0x400 for a core's registers, 0x28 for the volumes from 0x760 on), the
// address registers' halves (bits 17-20, then bits 1-16 with the low three cleared), and sceSdInit's writes were read from the
// module's code (SDK 2.5's LIBSD.IRX, as Wrath of Cortex has it)
#include "../runtime.h"
#include "ps2/iop.h"
#include "ps2/sound.h"
#include "spu2.h"

#include <algorithm>
#include <cstring>
#include <vector>

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

namespace
{
// libsd's reverb presets (sceSdSetEffectAttr, export 23): ten entries of 0x44 bytes, each a mask of the fields to write (0:
// all) and 32 halfwords: the two all-pass filters' sizes, the IIR's, four combs', the wall's and the two all-pass filters'
// volumes (0x774 on), the 20 offsets from 0x2EC on, the input's coefficients (0x784, 0x786). The sizes and offsets are in
// 8 byte units, written as halfword addresses (times 4) from the work area's start. Each preset's work area size (in the
// same units) is in a table of ten words 0x28 bytes before them. LIBSD.IRX's .data holds them: found by their shape (every
// mask 0, every preset but the first, which is the reverb off, ending with the input coefficients 0x8000), they're the
// game's own values (SDK 2.5's and 2.8's are the same; the room preset's first 16 are the PlayStation's documented ones)
constexpr u32 EffectPresets = 10;
constexpr u32 PresetBytes = 0x44;
constexpr u32 PresetFields = 32;
struct EffectTables
{
    bool found = false;
    u16 presets[EffectPresets][PresetFields] = {};
    u32 sizes[EffectPresets] = {};
};
EffectTables g_effects;

// sceSdNote2Pitch's tables (LIBSD.IRX's .data: 0x5170 and 0x5188 in SDK 2.8's): a semitone's step from 0x8000 for the twelve
// of an octave, and a fine tune's (1/128 semitone) for the 128 of a semitone, found by their first entries
constexpr u16 SemitoneStart[4] = {0x8000, 0x879C, 0x8FAC, 0x9837};
constexpr u16 FineStart[4] = {0x8000, 0x800E, 0x801D, 0x802C};
struct PitchTables
{
    bool found = false;
    u16 semitones[12] = {};
    u16 fines[128] = {};
};
PitchTables g_pitch;

// Each core's preset, delay and feedback, as libsd keeps them
struct EffectState
{
    u32 mode = 0;
    u32 delay = 0;
    u32 feedback = 0;
};
EffectState g_effectState[2];

u32 Get32(const u8* p) { return static_cast<u32>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)); }
u16 Get16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }

bool PresetsAt(const u8* data, size_t size, size_t at)
{
    if (at < 0x28 || at + EffectPresets * PresetBytes > size)
    {
        return false;
    }
    for (u32 k = 0; k < EffectPresets; k++)
    {
        const u8* preset = data + at + k * PresetBytes;
        if (Get32(preset) != 0 || Get32(data + at - 0x28 + k * 4) == 0)
        {
            return false;
        }
        if (k > 0 && (Get16(preset + 0x40) != 0x8000 || Get16(preset + 0x42) != 0x8000))
        {
            return false;
        }
    }
    return true;
}

// The work area's end: EEA's high bits, the rest ones
u32 EffectEnd(s32 core) { return SdGetAddr(static_cast<u16>(SD_A_EEA | core)); }

// A preset's field written to its register (sceSdSetEffectAttr's 0x13C)
void WriteEffectField(s32 core, u32 field, u16 value)
{
    if (field < 2 || (field >= 10 && field < 30))
    {
        u32 reg = static_cast<u32>(core) * 0x400 + (field < 2 ? 0x2E4 + field * 4 : 0x2EC + (field - 10) * 4);
        u32 halfwords = static_cast<u32>(value) * 4;
        Write(reg, static_cast<u16>(halfwords >> 16));
        Write(reg + 2, static_cast<u16>(halfwords));
        return;
    }
    Write(static_cast<u32>(core) * 0x28 + (field < 10 ? 0x774 + (field - 2) * 2 : 0x784 + (field - 30) * 2), value);
}
}

namespace
{
// The tables sceSdNote2Pitch reads: the twelve semitones' steps, then (right after them, as in the module) the 128 fine tunes'
void FindPitchTables(const u8* data, size_t size)
{
    auto at = [&](const u16* start) -> size_t {
        for (size_t o = 0; o + 8 <= size; o += 2)
        {
            if (Get16(data + o) == start[0] && Get16(data + o + 2) == start[1] && Get16(data + o + 4) == start[2] &&
                Get16(data + o + 6) == start[3])
            {
                return o;
            }
        }
        return size;
    };
    size_t semitones = at(SemitoneStart), fines = at(FineStart);
    if (semitones + 24 > size || fines + 256 > size)
    {
        Log("libsd: LIBSD.IRX's pitch tables not found: sceSdNote2Pitch can't be done");
        return;
    }
    auto lock = SoundLock();
    for (u32 i = 0; i < 12; i++)
    {
        g_pitch.semitones[i] = Get16(data + semitones + i * 2);
    }
    for (u32 i = 0; i < 128; i++)
    {
        g_pitch.fines[i] = Get16(data + fines + i * 2);
    }
    g_pitch.found = true;
}
}

void SdModuleLoaded(const u8* elf, size_t size)
{
    if (size < 52 || Get32(elf) != 0x464C457F)
    {
        return;
    }
    u32 phoff = Get32(elf + 28);
    u32 phentsize = Get16(elf + 42), phnum = Get16(elf + 44);
    std::vector<size_t> found;
    const u8* data = nullptr;
    size_t dataSize = 0;
    for (u32 i = 0; i < phnum && phoff + (i + 1) * phentsize <= size; i++)
    {
        const u8* ph = elf + phoff + i * phentsize;
        u32 offset = Get32(ph + 4), fileSize = Get32(ph + 16);
        if (Get32(ph) != 1 || offset > size || fileSize > size - offset)
        {
            continue;
        }
        data = elf + offset;
        dataSize = fileSize;
        for (size_t at = 0x28; at + EffectPresets * PresetBytes <= dataSize; at += 4)
        {
            if (PresetsAt(data, dataSize, at))
            {
                found.push_back(at);
            }
        }
        break;
    }
    FindPitchTables(data, dataSize);
    if (found.size() != 1)
    {
        Log("libsd: LIBSD.IRX's reverb presets %s: the reverb presets stay off", found.empty() ? "not found" : "found twice");
        return;
    }
    auto lock = SoundLock();
    for (u32 k = 0; k < EffectPresets; k++)
    {
        g_effects.sizes[k] = Get32(data + found[0] - 0x28 + k * 4);
        for (u32 f = 0; f < PresetFields; f++)
        {
            g_effects.presets[k][f] = Get16(data + found[0] + k * PresetBytes + 4 + f * 2);
        }
    }
    g_effects.found = true;
    PS2_LOG(LogIop, "libsd: the reverb presets at %zx in LIBSD.IRX's image", found[0]);
}

// sceSdClearEffectWorkArea (export 25): the preset's work area zeroed below the end address. libsd sends zeros through the
// channel's DMA; done at once here
s32 SdClearEffectWorkArea(s32 core, s32 channel, u32 mode)
{
    (void)channel;
    u32 preset = mode & 0xFF;
    if (preset >= EffectPresets)
    {
        return -100;
    }
    if (preset == 0)
    {
        return 0;
    }
    if (!g_effects.found)
    {
        if (Once("libsd clear effect"))
        {
            Log("libsd: a reverb work area's clearing asked for, without LIBSD.IRX's presets: not done");
        }
        return 0;
    }
    static const std::vector<u8> zeros(EffectPresets * 0x10000, 0);
    u32 bytes = g_effects.sizes[preset] * 8;
    SdVoiceTransWrite(EffectEnd(core & 1) - bytes + 1, zeros.data(), std::min<u32>(bytes, static_cast<u32>(zeros.size())));
    return 0;
}

// sceSdSetEffectAttr (export 23), as LIBSD.IRX does it: the core's effects turned off while its registers change, the old
// work area cleared first if asked and the effects were on, the depths, the preset's registers (the echo and delay presets'
// offsets moved by the delay and the wall's volume set by the feedback), the work area's start (ESA) below the end address
// by the preset's size, the new area cleared if asked, and the effects back on if they were
s32 SdSetEffectAttr(s32 core, u32 mode, u16 depthLeft, u16 depthRight, u32 delay, u32 feedback)
{
    core &= 1;
    u32 c = static_cast<u32>(core) * 0x400;
    u32 preset = mode & 0xFF;
    if (preset >= EffectPresets)
    {
        return -100;
    }
    if (!g_effects.found)
    {
        if (preset != 0 && Once("libsd effect presets"))
        {
            Log("libsd: reverb preset %u asked for, without LIBSD.IRX's presets: only its depths set", preset);
        }
        Write(0x764 + static_cast<u32>(core) * 0x28, depthLeft);
        Write(0x766 + static_cast<u32>(core) * 0x28, depthRight);
        return 0;
    }
    bool clear = (mode & 0x100) != 0;
    s32 channel = (mode & 0x200) != 0 ? 1 : 0;
    EffectState& state = g_effectState[core];
    u32 previous = state.mode;
    state.mode = preset;
    u16 fields[PresetFields];
    std::memcpy(fields, g_effects.presets[preset], sizeof(fields));
    state.delay = preset == 7 || preset == 8 ? 0x80 : 0;
    state.feedback = preset == 7 ? 0x80 : 0;
    if (preset == 7 || preset == 8)
    {
        state.delay = delay;
        state.feedback = feedback;
        u32 d = delay + 1;
        fields[10] = static_cast<u16>(d * 64 - fields[0]);
        fields[11] = static_cast<u16>(d * 32 - fields[1]);
        fields[16] = static_cast<u16>(fields[17] + d * 32);
        fields[12] = static_cast<u16>(fields[13] + d * 32);
        fields[27] = static_cast<u16>(fields[29] + d * 32);
        fields[26] = static_cast<u16>(fields[28] + d * 32);
        fields[7] = static_cast<u16>(feedback * 258);
    }
    u32 start = EffectEnd(core) - (g_effects.sizes[preset] * 8 - 1);
    bool enabled = (Read(0x19A + c) & 0x80) != 0;
    if (enabled)
    {
        Write(0x19A + c, static_cast<u16>(Read(0x19A + c) & ~0x80));
    }
    s32 result = enabled && clear ? SdClearEffectWorkArea(core, channel, previous) : 0;
    if (result >= 0)
    {
        Write(0x764 + static_cast<u32>(core) * 0x28, depthLeft);
        Write(0x766 + static_cast<u32>(core) * 0x28, depthRight);
        for (u32 f = 0; f < PresetFields; f++)
        {
            WriteEffectField(core, f, fields[f]);
        }
        Write(0x2E0 + c, static_cast<u16>(start >> 17));
        Write(0x2E2 + c, static_cast<u16>(start >> 1));
        result = clear ? SdClearEffectWorkArea(core, channel, preset) : 0;
    }
    if (enabled)
    {
        Write(0x19A + c, static_cast<u16>(Read(0x19A + c) | 0x80));
    }
    return result;
}

// sceSdGetEffectAttr (export 24, 0xBBC in SDK 2.8's LIBSD.IRX): the mode, delay and feedback kept by sceSdSetEffectAttr, the
// depths read back from EVOL
SdEffectAttr SdGetEffectAttr(s32 core)
{
    core &= 1;
    const EffectState& state = g_effectState[core];
    return {state.mode, Read(0x764 + static_cast<u32>(core) * 0x28), Read(0x766 + static_cast<u32>(core) * 0x28), state.delay,
            state.feedback};
}

// sceSdNote2Pitch (export 13, 0x3890 in SDK 2.8's LIBSD.IRX): the octaves from the centre note as a shift (four octaves up from
// the table's 0x8000 at the centre is 0x4000 << 2...), the semitone and the fine tune as the tables' product, as the module
// rounds them: a negative fine borrows from the semitone below
u16 SdNote2Pitch(u16 centreNote, u16 centreFine, u16 note, s16 fine)
{
    if (!g_pitch.found)
    {
        Fatal("libsd: sceSdNote2Pitch without LIBSD.IRX's pitch tables");
    }
    s32 f = fine + centreFine;
    s32 carry = f / 128;
    f -= carry * 128;
    s16 n = static_cast<s16>(note + carry - centreNote);
    s32 octave = n / 12;
    s32 shift = octave - 2;
    s32 semitone = n - octave * 12;
    if (semitone < 0 || (semitone == 0 && f < 0))
    {
        semitone += 12;
        shift = octave - 3;
    }
    if (f < 0)
    {
        semitone = semitone - 1 + carry;
        f += (carry + 1) * 128;
    }
    if (semitone < 0 || semitone >= 12 || f < 0 || f >= 128)
    {
        // The module would read outside its tables
        Fatal("libsd: sceSdNote2Pitch(%u, %u, %u, %d) out of its tables (semitone %d, fine %d)", centreNote, centreFine, note,
              fine, semitone, f);
    }
    u32 pitch = (static_cast<u32>(g_pitch.semitones[semitone]) * g_pitch.fines[f]) >> 16;
    s16 s = static_cast<s16>(shift);
    if (s < 0)
    {
        pitch = (pitch + (1u << (-s - 1))) >> -s;
    }
    return static_cast<u16>(pitch);
}
}
