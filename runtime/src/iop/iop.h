// The IOP's high level stand-in: the SIF (sif.cpp) and the RPC servers of the modules (servers.cpp)
#pragma once

#include "../runtime.h"
#include "ps2/iop.h"

namespace Ps2
{
// Where things live in the IOP's 2 MB
constexpr u32 IopCmdBuffer = 0x00001000;
constexpr u32 IopServerBase = 0x00002000;
constexpr u32 IopHeapBase = 0x00100000;

u32 IopAlloc(u32 size);
void IopReset();
// The IOP replacement image the reset loads (its name gives the modules' version)
void IopSetImage(const std::string& path);
void IopServersInit();
void IopServe(RpcCall& call);
const char* IopServerName(u32 sid);
void SifReplyTaken();
}
