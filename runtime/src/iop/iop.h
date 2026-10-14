// The IOP's high level stand-in: the SIF (sif.cpp) and the RPC servers of the modules (servers.cpp)
#pragma once

#include "../runtime.h"
#include "ps2/iop.h"

namespace Ps2
{
// Where things live in the IOP's 2 MB
constexpr u32 IopCmdBuffer = 0x00001000;
constexpr u32 IopServerBase = 0x00002000;
// The rest is the heap (the iop heap server's allocations and the servers' receive buffers): no module of the console's runs
// here, so a game finds at least the room it would on the console (CTTR asks for 264 KB after binding 14 servers)
constexpr u32 IopHeapBase = 0x00010000;

void IopReset();
// The IOP replacement image the reset loads (its name gives the modules' version)
void IopSetImage(const std::string& path);
void IopServersInit();
void IopServe(RpcCall& call);
// The memory card server (mcserv.cpp)
void McServ(RpcCall& call);
// The pad server (padman.cpp)
void PadMan(RpcCall& call);
const char* IopServerName(u32 sid);
void SifReplyTaken();
}
