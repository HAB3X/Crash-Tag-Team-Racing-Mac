#pragma once

// The SPU2: the PS2's sound processor, emulated at its registers. Two cores of 24 voices playing PS-ADPCM out of 2 MB of memory
// (Memory()) with the gaussian interpolation, ADSR envelopes, volume sweeps, noise and pitch modulation, a reverb per
// core working in its area of the memory, and each core's input from memory (AutoDMA). Every register is the hardware's: the
// I/O processor's code (libsd) drives it through Read and Write at its addresses (0x1F900000 on), and transfers to and from its
// memory through the I/O processor's DMA channels 4 and 7 (the IOP side calls the Dma functions). Tick makes one 48 kHz
// sample. What it does follows the hardware as PCSX2's SPU2 (pcsx2/SPU2) has it.
//
// From the Crash Twinsanity native port (src/platform/native/audio/: spu2.h, spu2.cpp and gaussian.h, branch features, commit
// 8268ddc2df70592e2fe3f0bf5d2b0a0c1ec7f8fa of 2026-10-11), where it was written for that port; its native/AUDIO.md lists the
// details. Copied unchanged except for the memory, which is the SPU2's own here (Memory()) instead of the port's g_SpuMemory,
// and the log, the runtime's. Fixes made in either place should be copied to the other
#include "common.h"

namespace Spu2
{
constexpr u32 RegisterBase = 0x1F900000;
constexpr u32 RegisterSize = 0x800;
// Memory in halfwords
constexpr u32 MemoryHalfwords = 0x100000;
constexpr u32 AddressMask = MemoryHalfwords - 1;
// The I/O processor's interrupt lines the SPU2 raises: its own (IRQA reached) and the two cores' DMA channels
constexpr s32 SpuInterrupt = 9;
constexpr s32 Dma4Interrupt = 36;
constexpr s32 Dma7Interrupt = 40;
// IOP cycles a 16 bit word of DMA takes (PCSX2's), and per sample (36.864 MHz / 48 kHz)
constexpr u32 CyclesPerDmaWord = 24;
constexpr u32 CyclesPerSample = 768;

struct Stereo
{
    s32 left;
    s32 right;
};

// What the SPU2 tells the I/O processor's side: an interrupt line raised
using InterruptFunction = void (*)(s32 line);
void SetInterruptFunction(InterruptFunction function);

void Reset();
// The sound memory: 2 MB, in halfwords
u16* Memory();
u16 Read(u32 address);
void Write(u32 address, u16 value);

// A transfer of the core's DMA channel (4 for core 0, 7 for core 1) as the IOP's DMA controller starts it: halfwords to (write)
// or from (read) the memory at the core's TSA. Returns the IOP cycles it takes: the IOP side calls DmaFinished once they've
// passed (it raises the channel's interrupt). With the core's AutoDMA on, a write is the input's data instead: it's played from
// data (which must stay valid) a block at a time, and the core raises the interrupt itself once it has taken all of it (the
// return is 0 then)
u32 DmaWrite(s32 core, const u16* data, u32 halfwords);
u32 DmaRead(s32 core, u16* data, u32 halfwords);
// What an AutoDMA write hasn't given the core yet (halfwords)
u32 AdmaRemaining(s32 core);
void DmaFinished(s32 core);

// The player's volume of a voice (0 to 0x8000, 0x8000 the game's own), applied after the voice's volume, and silence of the
// output; neither is the hardware's (volumes.h)
void SetVoiceGain(s32 voice, s32 gain);
void SetInputGain(s32 gain);
void SetMuted(bool muted);

// The voice's registers and state, for the log
void LogVoice(s32 voice);

// One sample of output (both cores mixed, the master volume applied)
Stereo Tick();
}
