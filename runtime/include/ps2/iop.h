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

// A semaphore signalled from the IOP's side (a module's completion), and the EE's memory
void KernelSignalSema(int id);
u8* EeMemory(u32 address);
void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
bool Once(const std::string& key);
}
