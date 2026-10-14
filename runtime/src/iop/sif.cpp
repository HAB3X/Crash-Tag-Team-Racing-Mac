// The IOP, high level: the SIF's command protocol as the IOP's sifcmd and sifrpc modules speak it, and RPC servers standing in
// for the IOP modules games load (servers.cpp). The EE's own library code (sceSifInitCmd, sceSifBindRpc, sceSifCallRpc...)
// runs recompiled and unchanged: it sends commands with the kernel's SIF DMA (SifSetDma), which land in the IOP's memory here,
// and gets the replies the way it gets the real IOP's: written into its receive buffer by SIF0 DMA, then the DMAC's channel 5
// interrupt.
#include "iop.h"

#include <deque>
#include <map>
#include <mutex>

namespace Ps2
{
namespace
{
u8* g_iopMem = nullptr;
// Recursive: a module's command handler, called with it held, may send a command of its own
std::recursive_mutex g_sifLock;
// The modules' commands to the EE (IopSendCommand) wait here until the SIF's lock is free: a sender may hold the sound lock,
// which a server takes under the SIF's
std::mutex g_outLock;
std::deque<std::vector<u8>> g_outbox;
std::map<u32, IopCommandHandler>& CommandHandlers()
{
    static std::map<u32, IopCommandHandler> h;
    return h;
}
u32 g_sysregs[8] = {};
u32 g_msflag = 0;
u32 g_mscom = 0;
u32 g_eeRecvBuf = 0;
u32 g_iopSregs[32] = {};
std::deque<std::vector<u8>> g_replies;
bool g_inFlight = false;
// The heap: first fit over the free blocks (start to size), a freed block joined to its free neighbours
std::mutex g_heapLock;
std::map<u32, u32> g_heapFree = {{IopHeapBase, IopRamSize - IopHeapBase}};
std::map<u32, u32> g_heapUsed;

constexpr u32 SIF_CMD_CHANGE_SADDR = 0x80000000;
constexpr u32 SIF_CMD_SET_SREG = 0x80000001;
constexpr u32 SIF_CMD_INIT_CMD = 0x80000002;
constexpr u32 SIF_CMD_RESET_CMD = 0x80000003;
constexpr u32 SIF_CMD_RPC_END = 0x80000008;
constexpr u32 SIF_CMD_RPC_BIND = 0x80000009;
constexpr u32 SIF_CMD_RPC_CALL = 0x8000000A;
constexpr u32 SIF_CMD_RPC_RDATA = 0x8000000C;

struct BoundServer
{
    u32 sid;
    u32 address;  // the server's "SifRpcServerData" address in IOP memory (any unique value)
    u32 buffer;   // its receive buffer in IOP memory
};
std::map<u32, BoundServer> g_bySid;
std::map<u32, BoundServer> g_byAddress;

u32 R32(const u8* p, u32 off)
{
    u32 v;
    std::memcpy(&v, p + off, 4);
    return v;
}

void W32(std::vector<u8>& v, u32 off, u32 value)
{
    if (v.size() < off + 4)
    {
        v.resize(off + 4);
    }
    std::memcpy(&v[off], &value, 4);
}

void QueueReply(std::vector<u8> packet)
{
    g_replies.push_back(std::move(packet));
}

// The next reply into the EE's receive buffer, with channel 5's interrupt; the next waits until the EE took it
void DeliverNext()
{
    if (g_inFlight || g_replies.empty() || g_eeRecvBuf == 0)
    {
        return;
    }
    std::vector<u8> p = std::move(g_replies.front());
    g_replies.pop_front();
    std::memcpy(Host(g_eeRecvBuf & 0x1FFFFFF), p.data(), p.size());
    g_inFlight = true;
    PS2_LOG(LogSif, "IOP to EE: cid %08x (%zu bytes) at %08x", R32(p.data(), 8), p.size(), g_eeRecvBuf);
    RaiseDmac(5);
}

std::vector<u8> RendPacket(const u8* call, u32 cid, u32 server, u32 buff, u32 cbuff)
{
    std::vector<u8> r(48, 0);
    W32(r, 0, 48);         // psize 48, dsize 0
    W32(r, 4, 0);
    W32(r, 8, SIF_CMD_RPC_END);
    W32(r, 12, 0);
    W32(r, 16, R32(call, 16)); // rec_id
    W32(r, 20, R32(call, 20)); // pkt_addr
    W32(r, 24, R32(call, 24)); // rpc_id
    W32(r, 28, R32(call, 28)); // client
    W32(r, 32, cid);
    W32(r, 36, server);
    W32(r, 40, buff);
    W32(r, 44, cbuff);
    return r;
}

BoundServer& Bind(u32 sid)
{
    auto it = g_bySid.find(sid);
    if (it != g_bySid.end())
    {
        return it->second;
    }
    BoundServer s;
    s.sid = sid;
    s.address = IopServerBase + static_cast<u32>(g_bySid.size()) * 0x40;
    s.buffer = IopAlloc(0x10000);
    g_bySid[sid] = s;
    g_byAddress[s.address] = s;
    return g_bySid[sid];
}

void HandleCommand(const u8* p)
{
    u32 psize = R32(p, 0) & 0xFF;
    u32 cid = R32(p, 8);
    switch (cid)
    {
    case SIF_CMD_CHANGE_SADDR:
    case SIF_CMD_INIT_CMD:
        g_eeRecvBuf = R32(p, 16);
        PS2_LOG(LogSif, "EE's command buffer %08x (cid %08x)", g_eeRecvBuf, cid);
        if (cid == SIF_CMD_INIT_CMD && R32(p, 12) != 0)
        {
            // opt 1: the RPC layer's init, answered by setting the EE's software register 0 (SIF_SREG_RPCINIT)
            std::vector<u8> r(24, 0);
            W32(r, 0, 24);
            W32(r, 8, SIF_CMD_SET_SREG);
            W32(r, 16, 0);
            W32(r, 20, 1);
            QueueReply(r);
        }
        DeliverNext();
        break;
    case SIF_CMD_SET_SREG:
        g_iopSregs[R32(p, 16) & 31] = R32(p, 20);
        break;
    case SIF_CMD_RESET_CMD:
    {
        std::string arg(reinterpret_cast<const char*>(p + 24), strnlen(reinterpret_cast<const char*>(p + 24), 80));
        Log("IOP reset: \"%s\"", arg.c_str());
        IopSetImage(arg);
        IopReset();
        break;
    }
    case SIF_CMD_RPC_BIND:
    {
        u32 sid = R32(p, 32);
        BoundServer& s = Bind(sid);
        PS2_LOG(LogSif, "bind %08x (%s)", sid, IopServerName(sid));
        QueueReply(RendPacket(p, SIF_CMD_RPC_BIND, s.address, s.buffer, s.buffer + 0x8000));
        DeliverNext();
        break;
    }
    case SIF_CMD_RPC_CALL:
    {
        u32 fno = R32(p, 32);
        u32 sendSize = R32(p, 36);
        u32 receive = R32(p, 40);
        u32 recvSize = R32(p, 44);
        u32 server = R32(p, 52);
        auto it = g_byAddress.find(server);
        if (it == g_byAddress.end())
        {
            Log("RPC call to unknown server %08x", server);
            QueueReply(RendPacket(p, SIF_CMD_RPC_CALL, server, 0, 0));
            DeliverNext();
            break;
        }
        const BoundServer& s = it->second;
        RpcCall call;
        call.sid = s.sid;
        call.fno = fno;
        call.in = g_iopMem + s.buffer;
        call.inSize = sendSize;
        call.receive = receive & 0x1FFFFFFF;
        call.recvSize = recvSize;
        call.out.assign(recvSize, 0);
        IopServe(call);
        if (receive != 0 && recvSize > 0 && !call.wroteReceive)
        {
            std::memcpy(Host(call.receive), call.out.data(), std::min<size_t>(recvSize, call.out.size()));
        }
        QueueReply(RendPacket(p, SIF_CMD_RPC_CALL, server, s.buffer, s.buffer + 0x8000));
        DeliverNext();
        break;
    }
    case SIF_CMD_RPC_RDATA:
    {
        u32 src = R32(p, 32);
        u32 dest = R32(p, 36);
        u32 size = R32(p, 40);
        PS2_LOG(LogSif, "get other data %08x to %08x (%u bytes)", src, dest, size);
        std::memcpy(Host(dest & 0x1FFFFFF), g_iopMem + (src & (IopRamSize - 1)), size);
        std::vector<u8> r = RendPacket(p, SIF_CMD_RPC_RDATA, 0, 0, 0);
        QueueReply(r);
        DeliverNext();
        break;
    }
    default:
    {
        auto handler = CommandHandlers().find(cid);
        if (handler != CommandHandlers().end())
        {
            PS2_LOG(LogSif, "EE to IOP: command %08x (%u bytes)", cid, psize);
            handler->second(p, psize);
            break;
        }
        if (Once("sifcmd " + std::to_string(cid)))
        {
            Log("SIF command %08x (%u bytes) not done", cid, psize);
        }
        break;
    }
    }
}

// The modules' commands waiting in the outbox join the replies (with the SIF's lock held)
void TakeOutbox()
{
    std::lock_guard<std::mutex> lock(g_outLock);
    while (!g_outbox.empty())
    {
        g_replies.push_back(std::move(g_outbox.front()));
        g_outbox.pop_front();
    }
}
}

u8* IopMem() { return g_iopMem; }

u32 IopAlloc(u32 size)
{
    std::lock_guard<std::mutex> lock(g_heapLock);
    u32 need = std::max<u32>((size + 63) & ~63u, 64);
    for (auto it = g_heapFree.begin(); it != g_heapFree.end(); ++it)
    {
        if (it->second < need)
        {
            continue;
        }
        u32 at = it->first;
        u32 left = it->second - need;
        g_heapFree.erase(it);
        if (left > 0)
        {
            g_heapFree[at + need] = left;
        }
        g_heapUsed[at] = need;
        return at;
    }
    u32 free = 0;
    for (const auto& [at, n] : g_heapFree)
    {
        free += n;
    }
    Fatal("IOP memory full: %u bytes asked for, %u free in %zu blocks", size, free, g_heapFree.size());
}

bool IopFree(u32 at)
{
    std::lock_guard<std::mutex> lock(g_heapLock);
    auto used = g_heapUsed.find(at);
    if (used == g_heapUsed.end())
    {
        return false;
    }
    u32 start = at;
    u32 size = used->second;
    g_heapUsed.erase(used);
    auto next = g_heapFree.find(start + size);
    if (next != g_heapFree.end())
    {
        size += next->second;
        g_heapFree.erase(next);
    }
    auto prev = g_heapFree.lower_bound(start);
    if (prev != g_heapFree.begin() && (--prev)->first + prev->second == start)
    {
        prev->second += size;
    }
    else
    {
        g_heapFree[start] = size;
    }
    return true;
}

void IopReset()
{
    // The modules the reset loads (the replacement image's) are the servers' business: nothing to do
}

void IopInit()
{
    g_iopMem = static_cast<u8*>(std::calloc(1, IopRamSize));
    IopServersInit();
}

void SifSetDma(u32 src, u32 dest, u32 size, u32 attr)
{
    std::lock_guard<std::recursive_mutex> lock(g_sifLock);
    dest &= IopRamSize - 1;
    size = (size + 3) & ~3u;
    if (dest + size > IopRamSize)
    {
        Log("SIF DMA past the IOP's memory (%08x + %x)", dest, size);
        return;
    }
    std::memcpy(g_iopMem + dest, Host(src & 0x1FFFFFFF), size);
    PS2_LOG(LogSif, "EE to IOP: %08x to %08x, %u bytes, attr %x", src, dest, size, attr);
    if (dest == IopCmdBuffer)
    {
        HandleCommand(g_iopMem + dest);
        TakeOutbox();
        DeliverNext();
    }
}

void IopRegisterCommand(u32 cid, IopCommandHandler handler) { CommandHandlers()[cid] = std::move(handler); }

void IopSendCommand(u32 cid, std::vector<u8> packet, u32 eeAddress, const void* data, u32 dataSize)
{
    if (packet.size() < 16)
    {
        packet.resize(16);
    }
    if (data != nullptr && dataSize > 0)
    {
        // The data goes ahead of the command, as the IOP's SIF DMA sends it
        std::memcpy(Host(eeAddress & 0x1FFFFFF), data, dataSize);
    }
    W32(packet, 0, static_cast<u32>(packet.size()) | (dataSize << 8));
    W32(packet, 4, eeAddress);
    W32(packet, 8, cid);
    {
        std::lock_guard<std::mutex> lock(g_outLock);
        g_outbox.push_back(std::move(packet));
    }
    std::unique_lock<std::recursive_mutex> lock(g_sifLock, std::try_to_lock);
    if (lock.owns_lock())
    {
        TakeOutbox();
        DeliverNext();
    }
}

// The EE's command handler took the last reply: the next can go (its SifSetDChain, or the end of its interrupt)
void SifReplyTaken()
{
    std::lock_guard<std::recursive_mutex> lock(g_sifLock);
    TakeOutbox();
    if (g_inFlight)
    {
        g_inFlight = false;
        DeliverNext();
    }
}

u32 SifGetReg(u32 reg)
{
    if (reg & 0x80000000u)
    {
        return g_sysregs[reg & 7];
    }
    switch (reg)
    {
    case 1: // MAINADDR (MSCOM)
        return g_mscom;
    case 2: // SUBADDR (SMCOM): the IOP's command buffer
        return IopCmdBuffer;
    case 3: // MSFLAG
        return g_msflag;
    case 4: // SMFLAG: SIF initialised, commands initialised, boot ended
        return 0x00070000;
    default:
        return 0;
    }
}

void SifSetReg(u32 reg, u32 value)
{
    if (reg & 0x80000000u)
    {
        g_sysregs[reg & 7] = value;
        return;
    }
    switch (reg)
    {
    case 1:
        g_mscom = value;
        break;
    case 3:
        g_msflag = value;
        break;
    default:
        break;
    }
}

void IopUpdate()
{
    // The modules' commands sent while the SIF was busy
    std::lock_guard<std::recursive_mutex> lock(g_sifLock);
    TakeOutbox();
    DeliverNext();
}
}
