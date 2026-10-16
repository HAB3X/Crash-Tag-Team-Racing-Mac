// The IOP's RPC servers, for a game's own IOP modules: a server registered here answers sceSifCallRpc calls to its ID the
// way the module would (the runtime's own: runtime/src/iop/servers.cpp)
#pragma once

#include "ps2/recomp.h"

#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace Ps2
{
struct RpcCall
{
    u32 sid;
    u32 fno;
    const u8* in;        // what the EE sent (in the IOP's memory)
    u32 inSize;
    u32 receive;         // the EE's receive buffer
    u32 recvSize;
    std::vector<u8> out; // copied to the receive buffer unless the server wrote it itself
    bool wroteReceive = false;

    u32 In32(u32 off) const
    {
        u32 v = 0;
        std::memcpy(&v, in + off, 4);
        return v;
    }
    u16 In16(u32 off) const
    {
        u16 v = 0;
        std::memcpy(&v, in + off, 2);
        return v;
    }
    std::string InString(u32 off, u32 max = 256) const
    {
        std::string s;
        for (u32 i = 0; i < max && in[off + i] != 0; i++)
        {
            s += static_cast<char>(in[off + i]);
        }
        return s;
    }
    void Out32(u32 off, u32 value)
    {
        if (out.size() < off + 4)
        {
            out.resize(off + 4);
        }
        std::memcpy(&out[off], &value, 4);
    }
};

using RpcServer = std::function<void(RpcCall&)>;
void IopRegisterServer(u32 sid, const char* name, RpcServer server);

// A module's own SIF commands (the EE's sceSifSendCmd to a command ID the module added a handler for): the packet as it landed
// in the IOP's memory (its 16 byte header, then the rest the EE sent) and its size. Called with the SIF's lock held
using IopCommandHandler = std::function<void(const u8* packet, u32 size)>;
void IopRegisterCommand(u32 cid, IopCommandHandler handler);
// A module's command to the EE (its isceSifSendCmd): the packet (the first 16 bytes its header, whose sizes, address and ID are
// filled here; the module's own fields from 12 on), with data sent ahead of it into the EE's memory at eeAddress. Any thread:
// the commands reach the EE's handlers in order, after the replies already waiting
void IopSendCommand(u32 cid, std::vector<u8> packet, u32 eeAddress = 0, const void* data = nullptr, u32 dataSize = 0);

// A module of the game's own that the EE loads (sceSifLoadModule from the disc or sceSifLoadModuleBuffer from the IOP's memory),
// known by its name (the module's own, in its .iopmod section: "RAD_IOP_SIDE"): start runs with its arguments as it's loaded
using IopModuleStart = std::function<void(const std::vector<std::string>& args)>;
void IopRegisterModule(const char* name, IopModuleStart start);
// A module of the game's own with no name of its own (an empty .iopmod name), known instead by a string in its image (its
// version banner, say)
void IopRegisterUnnamedModule(const char* text, IopModuleStart start);

// A module's thread that runs once a vertical blank (its loop round WaitVblankStart or WaitVblankEnd): the handler runs on the
// game's side as each blank begins, between the game's instructions, so it may read and write the EE's memory as the IOP's DMA
// would. Added as the module starts
void IopOnVblank(std::function<void()> handler);

// The IOP's memory (2 MB) at an address, and its heap (a module's AllocSysMemory and FreeSysMemory; the iop heap server's):
// an allocation's address, and false from a free of something not allocated
u8* IopMemory(u32 address);
u32 IopAlloc(u32 size);
bool IopFree(u32 at);

// A semaphore signalled from the IOP's side (a module's completion), and the EE's memory
void KernelSignalSema(int id);
u8* EeMemory(u32 address);
void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void Fatal(const char* format, ...) __attribute__((format(printf, 1, 2)));
bool Once(const std::string& key);
// Whether --log asked for the IOP's detail (a module's own, logged only then)
bool IopLogging();
}
