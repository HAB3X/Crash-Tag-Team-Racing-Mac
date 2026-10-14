// Radical's audio IOP module (AUDIO.IRX: runtime/code/audio/src/hal/device/ps2/iop/), high level: its two RPC servers, read from
// the module's code. Both check the EE's version (0x145) first.
//
// The load server (0x11):
//   0 init: { version, fill the SPU with garbage, the DVD transfer buffer's bytes, the memory transfer buffer's, the
//     uninterleave buffer's, the clear buffer's, the debug channel, an SPU address, a size }. The four buffers are allocated
//     in the IOP's memory; a silent looping block of the size is put at the SPU address (ADPCM lines of zeros, each header
//     0xC0, the first's flags 6 (start, repeat), the last's 3 (end, repeat)). Answers { 1, the DVD buffer, the memory buffer }
//     in the same 0x40 bytes
//   0x11, 0x12 (0x2040 bytes: UpdateAudioPageNexts), 0x13: not done yet
// The voice server (0x12):
//   0 init: { version, the voice thread's period in microseconds, its priority, ... }: answers 1
//   0x10: not done yet
#include "ps2/iop.h"
#include "ps2/sound.h"

#include <cstring>
#include <vector>

namespace
{
using namespace Ps2;

constexpr u32 Version = 0x145;

void Unhandled(const RpcCall& c)
{
    if (Once("audio " + std::to_string(c.sid) + " " + std::to_string(c.fno)))
    {
        Log("audio module: server %x function %#x not done (%u bytes in, %u out)", c.sid, c.fno, c.inSize, c.recvSize);
    }
}

bool VersionOk(const RpcCall& c)
{
    if (c.In32(0) != Version)
    {
        Log("audio module: server %x's version %#x, not %#x", c.sid, c.In32(0), Version);
        return false;
    }
    return true;
}

// --- the load server
void LoadInit(RpcCall& c)
{
    if (!VersionOk(c))
    {
        c.Out32(0, 0);
        return;
    }
    u32 dvdBytes = c.In32(8), memoryBytes = c.In32(12), uninterleaveBytes = c.In32(16), clearBytes = c.In32(20);
    u32 silenceAddress = c.In32(28), silenceBytes = c.In32(32);
    u32 dvd = IopAlloc(dvdBytes), memory = IopAlloc(memoryBytes);
    IopAlloc(uninterleaveBytes);
    u32 clear = IopAlloc(clearBytes);
    // The silent loop, built in the clear buffer and sent to the sound memory
    u8* block = IopMemory(clear);
    std::memset(block, 0, clearBytes);
    for (u32 at = 0; at < clearBytes; at += 16)
    {
        block[at] = 0xC0;
    }
    block[1] = 6;
    block[silenceBytes - 15] = 3;
    {
        auto lock = SoundLock();
        SdVoiceTransWrite(silenceAddress, block, silenceBytes);
    }
    block[1] = 0;
    block[silenceBytes - 15] = 0;
    Log("audio module: load server: buffers at %05x (DVD, %u bytes) and %05x (memory, %u bytes); silence at %05x", dvd,
        dvdBytes, memory, memoryBytes, silenceAddress);
    c.Out32(0, 1);
    c.Out32(4, dvd);
    c.Out32(8, memory);
}

void LoadServer(RpcCall& c)
{
    switch (c.fno)
    {
    case 0:
        LoadInit(c);
        break;
    default:
        Unhandled(c);
        break;
    }
}

// --- the voice server
void VoiceServer(RpcCall& c)
{
    switch (c.fno)
    {
    case 0:
        c.Out32(0, VersionOk(c) ? 1 : 0);
        break;
    default:
        Unhandled(c);
        break;
    }
}
}

void CttrRegisterAudio()
{
    Ps2::IopRegisterServer(0x11, "audio load", LoadServer);
    Ps2::IopRegisterServer(0x12, "audio voice", VoiceServer);
}
