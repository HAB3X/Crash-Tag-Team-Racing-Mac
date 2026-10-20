// CRI Middleware's ADX IOP driver ("CRI ADX Driver Ver.8.82", module CRI_ADX_Driver), stood in for natively: the EE's ADX
// library decodes the streams to 16 bit PCM and sends it to the IOP, whose driver mixes it and plays it through an SPU2 core's
// input. The pieces are the driver's own, read from its module (symbols in its .symtab; addresses below are offsets in that
// IRX): DTX (a mailbox between the EE and the IOP), SJ (stream joints: chunk queues), SJRMT and SJX (SJs made and fed from
// the EE), IOPRNA (the EE's streams' commands) and PSM (the mixer and the output). docs/cri_adx.md has the protocol.
//
// Everything here belongs to whoever holds the sound lock (ps2/sound.h): the RPC server's calls, the module's thread at each
// vertical blank, and the output's interrupt on the sound's thread
#pragma once

#include "iop.h"

#include <array>
#include <deque>
#include <memory>

namespace Ps2::CriAdx
{
// The driver known to the module loader by its name (servers.cpp, as the IOP's servers start)
void Register();

// A chunk: an address in the IOP's memory and its size in bytes
struct Chunk
{
    u32 data = 0;
    s32 size = 0;
};

// A stream joint: chunks in four lanes (0 holds free room, 1 data, by the library's convention), taken from a lane's front
// and put at its back or back at its front. The functions are the SJ interface's (its vtable from offset 0x0C on: destroy,
// uuid (not done), reset, get, unget, put, data, is-get); a misuse is reported as the driver's error function does and otherwise
// ignored, as there
class Sj
{
public:
    virtual ~Sj() = default;
    virtual void Reset() = 0;
    virtual Chunk Get(u32 lane, s32 size) = 0;
    virtual void Unget(u32 lane, Chunk chunk) = 0;
    virtual void Put(u32 lane, Chunk chunk) = 0;
    virtual s32 Data(u32 lane) const = 0;
    // Whether a chunk of the size can be got, and the room there is
    virtual bool IsGet(u32 lane, s32 size, s32& room) const = 0;
};

// SJUNI (0x5AB4): chunks kept as they were put, in a chain of 16 byte links from a work area of the IOP's memory (its size /
// 16 links). Mode 1 joins a chunk put right after the lane's last (or ungot right before its first) to it, and gives part of
// a chunk bigger than asked for; mode 0 neither
std::unique_ptr<Sj> MakeSjUni(u32 mode, u32 work, s32 workSize);
// SJRBF (0x511C): a ring buffer in the IOP's memory, its size and the extra room after it that data put there is copied
// from (and to, at the start); lane 0 is its room, lane 1 its data, each read round from its own position
std::unique_ptr<Sj> MakeSjRbf(u32 buffer, s32 size, s32 extra);

// --- cri_psm.cpp: PSM, the mixer (PSM_*, PSMSLT_*, PSMMIX_*, PSMOUT_*, RCV_*)
// A stream: its channels each read from an SJ of 16 bit PCM at its rate, converted to 48 kHz, mixed with a volume and pan
// into a core's output
struct Psm;
// PSM_SetupSnd: the core whose input plays (0 or 1)
void PsmSetupCore(s32 core);
// PSM_ReinitSj: the channels' buffers (0x400 bytes each) in a work area of the IOP's memory
void PsmReinitBuffers(s32 channels, u32 work, s32 workSize);
// PSM_Init and PSM_Finish (counted): the two cores' mixers and outputs, the chosen core's output started; out is the module's
// own buffer for the outputs (0x2000 bytes of the IOP's memory)
void PsmInit(u32 out);
void PsmFinish();
// PSM_Create (nullptr when there's no room); the SJs are the channels' inputs
Psm* PsmCreate(s32 core, s32 channels, const std::vector<Sj*>& inputs);
void PsmDestroy(Psm* psm);
void PsmStart(Psm* psm);
void PsmStop(Psm* psm);
void PsmSetRate(Psm* psm, s32 rate);
void PsmSetVolume(Psm* psm, s32 channel, s32 volume);
void PsmSetPan(Psm* psm, s32 channel, s32 pan);
void PsmSetMix(Psm* psm, s32 mix);
void PsmSetChannels(Psm* psm, s32 channels);
}
