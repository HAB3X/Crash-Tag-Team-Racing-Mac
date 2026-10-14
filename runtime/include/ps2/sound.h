// The sound side of the IOP, for a game's own sound driver (an RPC server registered with ps2/iop.h): the SPU2
// (runtime/src/sound/spu2.cpp) driven the way Sony's libsd drives it, the IOP's hardware timer in the SPU2's own time, and the
// disc for the driver's reads. The SPU2's output is played through SDL3 at 48 kHz (and written to a WAV file with
// --audio-dump).
//
// Threads: the SPU2 and whatever a driver keeps belong to whoever holds the sound lock. The runtime holds it while it makes
// samples (on SDL's audio thread, or a thread of its own when nothing plays) and while it runs the timer's function; a server
// takes it (SoundLock) for each call it answers. Every function here but SoundLock and the disc's is called with it held
#pragma once

#include "ps2/recomp.h"

#include <functional>
#include <mutex>
#include <string>

namespace Ps2
{
std::unique_lock<std::recursive_mutex> SoundLock();

// The IOP's hardware timer for the driver's thread: the function runs every so many of the SPU2's samples (48,000 a second)
void SoundSetTimer(u32 samples, std::function<void()> tick);
// More timers of the same kind (another module's alarm): an ID to remove it by
int SoundAddTimer(u32 samples, std::function<void()> tick);
void SoundRemoveTimer(int id);

// libsd: its calls with their arguments (libsd.h's entries: a voice's parameter is (register << 8) | (voice << 1) | core, a
// core's is (register << 8) | core; addresses are bytes in the sound memory), done as LIBSD.IRX does them at the SPU2's
// registers. SdInit is sceSdInit(0)
void SdInit();
void SdSetParam(u16 entry, u16 value);
u16 SdGetParam(u16 entry);
void SdSetSwitch(u16 entry, u32 value);
u32 SdGetSwitch(u16 entry);
void SdSetAddr(u16 entry, u32 address);
u32 SdGetAddr(u16 entry);
void SdSetCoreAttr(u16 entry, u16 value);
// sceSdVoiceTrans's write (channel 0 or 1, mode 0): bytes into the sound memory at a byte address, done at once
void SdVoiceTransWrite(u32 address, const void* data, u32 size);
// sceSdBlockTrans's write in loop mode into a core's input (AutoDMA): the buffer (in the IOP's memory) played round a half at a
// time, the transfer's interrupt (sceSdSetTransIntrHandler's handler) as each half has been taken and the other one starts.
// Stop ends it and clears the input
void SdBlockTransLoop(s32 core, u32 iopAddress, u32 size, std::function<void(s32 core)> interrupt);
void SdBlockTransStop(s32 core);
// sceSdBlockTransStatus: the half being taken (bit 24) and the address it's taken from
u32 SdBlockTransStatus(s32 core);
// sceSdSetEffectAttr: a core's reverb, from its sceSdEffectAttr's mode (a preset 0-9 in the low byte; 0x100 asks for the work
// area cleared, 0x200 to clear it through DMA channel 1), depths, and the echo and delay presets' delay and feedback. The
// presets are the game's LIBSD.IRX's own tables (none: only the depths are set, and it's logged). Returns libsd's result
s32 SdSetEffectAttr(s32 core, u32 mode, u16 depthLeft, u16 depthRight, u32 delay, u32 feedback);
// sceSdClearEffectWorkArea: a preset's work area zeroed below the core's end address (EEA)
s32 SdClearEffectWorkArea(s32 core, s32 channel, u32 mode);

// The disc (any thread): a file by its path ("\\SFX.DAT" or "/SFX.DAT"), its first sector and size; sectors of 2048 bytes
bool DiscFind(const std::string& path, u32& lsn, u32& size);
bool DiscReadSectors(u32 lsn, u32 count, u8* out);
}

// libsd.h's entries
enum : unsigned short
{
    SD_VP_VOLL = 0x0000,
    SD_VP_VOLR = 0x0100,
    SD_VP_PITCH = 0x0200,
    SD_VP_ADSR1 = 0x0300,
    SD_VP_ADSR2 = 0x0400,
    SD_VP_ENVX = 0x0500,
    SD_VP_VOLXL = 0x0600,
    SD_VP_VOLXR = 0x0700,
    SD_P_MMIX = 0x0800,
    SD_P_MVOLL = 0x0980,
    SD_P_MVOLR = 0x0A80,
    SD_P_EVOLL = 0x0B80,
    SD_P_EVOLR = 0x0C80,
    SD_P_AVOLL = 0x0D80,
    SD_P_AVOLR = 0x0E80,
    SD_P_BVOLL = 0x0F80,
    SD_P_BVOLR = 0x1080,
    SD_S_PMON = 0x1300,
    SD_S_NON = 0x1400,
    SD_S_KON = 0x1500,
    SD_S_KOFF = 0x1600,
    SD_S_ENDX = 0x1700,
    SD_S_VMIXL = 0x1800,
    SD_S_VMIXEL = 0x1900,
    SD_S_VMIXR = 0x1A00,
    SD_S_VMIXER = 0x1B00,
    SD_A_ESA = 0x1C00,
    SD_A_EEA = 0x1D00,
    SD_A_TSA = 0x1E00,
    SD_A_IRQA = 0x1F00,
    SD_VA_SSA = 0x2040,
    SD_VA_LSAX = 0x2140,
    SD_VA_NAX = 0x2240,
    SD_C_EFFECT_ENABLE = 2,
    SD_C_IRQ_ENABLE = 4,
    SD_C_MUTE_ENABLE = 6,
    SD_C_NOISE_CLK = 8,
    SD_C_SPDIF_MODE = 10,
};
