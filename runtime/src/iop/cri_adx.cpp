// CRI_ADX_Driver, CRI Middleware's ADX IOP module, stood in for natively (cri_adx.h). Its RPC server (DTX, 0x90000200) makes
// the EE's mailboxes and runs the EE's remote calls; its thread, once a vertical blank, drains the streams that are being
// flushed and exchanges each mailbox with the EE: what the EE sent last is taken in (the SJX's chunks of data, the streams'
// commands) and the IOP's answer goes back (the SJX's chunks of room). Offsets are the module's (09.irx in File 2's
// DRIVE.DAT; the same module in File 1's): DTX_* 0x610-0xD40, IOPRNA_* 0xD40-0x1530, SJRMT 0x6510-0x6970, SJX 0x6970-0x6F70
#include "cri_adx.h"
#include "ps2/sound.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>

namespace Ps2::CriAdx
{
namespace
{
constexpr u32 DtxServer = 0x90000200; // dtx_rpc_id

// The module's own memory for what the EE holds handles to (its tables in its .bss: the IRX's strides kept) and the outputs'
// buffers: one block of the IOP's heap, taken as the module starts
constexpr u32 DtxTable = 0x0000;      // 8 x 0x44
constexpr u32 SjxTable = 0x0240;      // 32 x 0x14
constexpr u32 RnaTable = 0x04C0;      // 16 x 0x20
constexpr u32 SjTable = 0x0700;       // 192 x 0x40 (SJUNI's 64 and SJRBF's 128)
constexpr u32 OutMemory = 0x3800;     // the outputs' buffers and their SJs' links (cri_psm.cpp)
constexpr u32 ModuleBytes = 0x5900;
constexpr u32 SjCount = 192;

// A mailbox (DTX_Create 0x8F0): the EE's and the IOP's buffers of the same size, the last 64 bytes a trailer whose last word
// is the count of the exchanges; the IOP waits until the EE's count passes its own, takes the EE's buffer in, then sends its
// own back with its count one more
enum class Mailbox
{
    None,
    Sjx,  // DTX 0: sjx_rcvcbf_iop / sjx_sndcbf_iop (0x6B3C, 0x6BE8)
    Rna,  // DTX 1: ioprna_rcvcbf / ioprna_sndcbf (0xD40, 0xF38)
};

struct Dtx
{
    bool used = false;
    bool waiting = false; // sent, until the EE's count passes it
    s32 count = 0;
    u32 ee = 0;
    u32 iop = 0;
    u32 size = 0;
};

struct SjxObject
{
    bool used = false;
    u16 resets = 0;  // sjx_reset_rmt's count (+2), sent with each chunk
    Sj* sj = nullptr;
    u32 eeSj = 0;
    u32 lane = 0;    // the lane the IOP sends from: room (0) when the EE sends data
    u32 eeObject = 0;
};

struct RnaObject
{
    bool used = false;
    bool flushing = false; // command 8: the inputs' data thrown away at each vertical blank
    s32 channels = 0;      // made with (+4)
    s32 active = 0;        // played (+8, command 3)
    std::array<Sj*, 2> inputs = {};
    Psm* psm = nullptr;
    s32 mix = 0;
    s32 volume = 0;
};

struct Module
{
    bool started = false;
    u32 memory = 0;
    s32 core = 1;
    std::array<Dtx, 8> dtx;
    std::array<Mailbox, 8> bound = {};
    std::array<SjxObject, 32> sjx;
    std::array<RnaObject, 16> rna;
    std::array<std::unique_ptr<Sj>, SjCount> sjs;
};
Module g_module;

u32 Read32(u32 iopAddress)
{
    u32 v;
    std::memcpy(&v, IopMemory(iopAddress), 4);
    return v;
}

void Write32(u32 iopAddress, u32 value) { std::memcpy(IopMemory(iopAddress), &value, 4); }

u32 Handle(u32 table, u32 stride, size_t index) { return g_module.memory + table + static_cast<u32>(index) * stride; }

// A handle the EE gave back: its object's index, or the game's corruption (the module would write through it)
size_t IndexOf(u32 handle, u32 table, u32 stride, size_t count, const char* what)
{
    u32 offset = handle - g_module.memory - table;
    if (handle < g_module.memory + table || offset % stride != 0 || offset / stride >= count)
    {
        Fatal("CRI ADX: %08x isn't one of the module's %s", handle, what);
    }
    return offset / stride;
}

Sj* SjOf(u32 handle)
{
    size_t i = IndexOf(handle, SjTable, 0x40, SjCount, "SJs");
    if (!g_module.sjs[i])
    {
        Fatal("CRI ADX: SJ %08x was destroyed", handle);
    }
    return g_module.sjs[i].get();
}

u32 SjAdd(std::unique_ptr<Sj> sj)
{
    for (size_t i = 0; i < SjCount; i++)
    {
        if (!g_module.sjs[i])
        {
            g_module.sjs[i] = std::move(sj);
            return Handle(SjTable, 0x40, i);
        }
    }
    Log("CRI ADX: no SJ left (the module has %u)", SjCount);
    return 0;
}

// --- the EE's remote calls (dtx_rpc_func's functions 0x400 on: DTX_SetUrpcCbf's table), their arguments and answers in words
using Words = std::vector<u32>;

// SJRMT (0x6838): SJs made and used from the EE
void SjUniCreate(Words& w) { w[0] = SjAdd(MakeSjUni(w[0], w[1], static_cast<s32>(w[2]))); }
void SjRbfCreate(Words& w) { w[0] = SjAdd(MakeSjRbf(w[0], static_cast<s32>(w[1]), static_cast<s32>(w[2]))); }
void SjDestroy(Words& w)
{
    auto& sj = g_module.sjs[IndexOf(w[0], SjTable, 0x40, SjCount, "SJs")];
    // The module would call through the cleared object at the next exchange or mix
    for (const SjxObject& o : g_module.sjx)
    {
        if (o.used && o.sj == sj.get())
        {
            Fatal("CRI ADX: SJ %08x destroyed while an SJX joins it", w[0]);
        }
    }
    for (const RnaObject& o : g_module.rna)
    {
        if (o.used && (o.inputs[0] == sj.get() || o.inputs[1] == sj.get()))
        {
            Fatal("CRI ADX: SJ %08x destroyed while a stream reads it", w[0]);
        }
    }
    sj.reset();
}
void SjReset(Words& w) { SjOf(w[0])->Reset(); }
void SjGet(Words& w)
{
    Chunk c = SjOf(w[0])->Get(w[1], static_cast<s32>(w[2]));
    w[0] = c.data;
    w[1] = static_cast<u32>(c.size);
}
void SjUnget(Words& w) { SjOf(w[0])->Unget(w[1], {w[2], static_cast<s32>(w[3])}); }
void SjPut(Words& w) { SjOf(w[0])->Put(w[1], {w[2], static_cast<s32>(w[3])}); }
void SjData(Words& w) { w[0] = static_cast<u32>(SjOf(w[0])->Data(w[1])); }
void SjIsGet(Words& w)
{
    s32 room = 0;
    w[0] = SjOf(w[0])->IsGet(w[1], static_cast<s32>(w[2]), room) ? 1 : 0;
    w[1] = static_cast<u32>(room);
}

// SJX (0x6E78): an EE SJ joined to an IOP SJ through DTX 0. The EE's SJ, the IOP's, 1 when the EE sends data (the IOP then
// sends room), and the EE's own object; answers the IOP's object (0 when there's none left)
void SjxCreate(Words& w)
{
    for (size_t i = 0; i < g_module.sjx.size(); i++)
    {
        SjxObject& o = g_module.sjx[i];
        if (!o.used)
        {
            o = {true, 0, SjOf(w[1]), w[0], w[2] == 1 ? 0u : 1u, w[3]};
            w[0] = Handle(SjxTable, 0x14, i);
            return;
        }
    }
    w[0] = 0;
}
SjxObject& SjxOf(u32 handle) { return g_module.sjx[IndexOf(handle, SjxTable, 0x14, 32, "SJX objects")]; }
void SjxDestroy(Words& w) { SjxOf(w[0]).used = false; }
void SjxReset(Words& w) { SjxOf(w[0]).resets = static_cast<u16>(w[1]); }
void SjxRebind(Words&) { g_module.bound[0] = Mailbox::Sjx; }

// IOPRNA (0x13D8): the EE's streams. Create: channels, the core, the channels' SJs; answers the object
void RnaCreate(Words& w)
{
    for (size_t i = 0; i < g_module.rna.size(); i++)
    {
        RnaObject& o = g_module.rna[i];
        if (o.used)
        {
            continue;
        }
        s32 channels = static_cast<s32>(w[0]);
        if (channels < 0 || channels > 2 || w.size() < 2 + static_cast<size_t>(channels))
        {
            Fatal("CRI ADX: a stream of %d channels asked for (the module keeps two, %zu words sent)", channels, w.size());
        }
        o = {};
        o.channels = channels;
        o.active = channels;
        std::vector<Sj*> inputs;
        for (s32 ch = 0; ch < channels; ch++)
        {
            o.inputs[ch] = SjOf(w[2 + ch]);
            inputs.push_back(o.inputs[ch]);
        }
        o.psm = PsmCreate(static_cast<s32>(w[1]), channels, inputs);
        if (o.psm == nullptr)
        {
            // "E0110120: ioprna_create_rmt: can't create PSM", then the module stops for good
            Fatal("CRI ADX: E0110120: ioprna_create_rmt: can't create PSM (%d channels, core %u)", channels, w[1]);
        }
        PsmSetChannels(o.psm, channels);
        PsmSetRate(o.psm, 48000);
        o.used = true;
        w[0] = Handle(RnaTable, 0x20, i);
        PS2_LOG(LogIop, "CRI ADX: stream %zu made: %d channels", i, channels);
        return;
    }
    w[0] = 0;
}
RnaObject& RnaOf(u32 handle) { return g_module.rna[IndexOf(handle, RnaTable, 0x20, 16, "streams")]; }
void RnaDestroy(Words& w)
{
    RnaObject& o = RnaOf(w[0]);
    PsmDestroy(o.psm);
    o = {};
}
// The channels' buffers (channels, work area, its size), and DTX 1 bound again
void RnaReinit(Words& w)
{
    PsmReinitBuffers(static_cast<s32>(w[0]), w[1], static_cast<s32>(w[2]));
    g_module.bound[1] = Mailbox::Rna;
}
void SoundRestore(Words& w)
{
    PsmSetupCore(static_cast<s32>(w[0]));
    PsmFinish();
    PsmInit(g_module.memory + OutMemory);
}
void SoundInit(Words&) { PsmInit(g_module.memory + OutMemory); }
void SoundFinish(Words&) { PsmFinish(); }

struct RemoteCall
{
    const char* name;
    void (*call)(Words&);
};
const std::map<u32, RemoteCall> g_calls = {
    {0x00, {"sjx_create_rmt", SjxCreate}},
    {0x01, {"sjx_destroy_rmt", SjxDestroy}},
    {0x02, {"sjx_reset_rmt", SjxReset}},
    {0x03, {"sjx_reint_rmt", SjxRebind}},
    {0x08, {"ioprna_create_rmt", RnaCreate}},
    {0x09, {"ioprna_destroy_rmt", RnaDestroy}},
    {0x0A, {"ioprna_reinit_rmt", RnaReinit}},
    {0x0B, {"ioprna_restore_iopsnd_rmt", SoundRestore}},
    {0x0C, {"ioprna_init_iopsnd_rmt", SoundInit}},
    {0x0D, {"ioprna_finish_iopsnd_rmt", SoundFinish}},
    {0x20, {"sjrmt_rbf_create", SjRbfCreate}},
    {0x22, {"sjrmt_uni_create", SjUniCreate}},
    {0x23, {"sjrmt_destroy", SjDestroy}},
    {0x25, {"sjrmt_reset", SjReset}},
    {0x26, {"sjrmt_get_chunk", SjGet}},
    {0x27, {"sjrmt_unget_chunk", SjUnget}},
    {0x28, {"sjrmt_put_chunk", SjPut}},
    {0x29, {"sjrmt_get_num_data", SjData}},
    {0x2A, {"sjrmt_is_get_chunk", SjIsGet}},
};

// --- DTX's RPC server (dtx_rpc_func 0x610): 2 makes a mailbox (its ID, the EE's buffer, the IOP's, their size), 3 removes one,
// 0x400 on are the remote calls. The answer is the call's buffer as the function left it
void DtxCreate(Words& w)
{
    u32 id = w[0], ee = w[1], iop = w[2], size = w[3];
    w[0] = 0;
    if ((ee & 0x3F) != 0 || (iop & 0x1F) != 0 || (size & 0x3F) != 0 || id >= 8)
    {
        Log("CRI ADX: DTX_Create %u refused (EE %08x, IOP %06x, %u bytes: the module's alignment)", id, ee, iop, size);
        return;
    }
    Dtx& d = g_module.dtx[id];
    if (d.used)
    {
        return;
    }
    // The IOP waits for the EE's first
    d = {true, true, 0, ee, iop, size};
    std::memset(IopMemory(iop), 0, size);
    w[0] = Handle(DtxTable, 0x44, id);
    PS2_LOG(LogIop, "CRI ADX: DTX %u: EE %08x, IOP %06x, %u bytes", id, ee, iop, size);
}

void DtxDestroy(Words& w)
{
    size_t id = IndexOf(w[0], DtxTable, 0x44, 8, "DTXs");
    // DTX_Destroy clears the whole client, its callbacks with it
    g_module.dtx[id] = {};
    g_module.bound[id] = Mailbox::None;
}

void DtxServe(RpcCall& c)
{
    auto lock = SoundLock();
    Words w(std::max<u32>(c.inSize, c.recvSize) / 4 + 4, 0);
    std::memcpy(w.data(), c.in, c.inSize);
    if (c.fno == 2)
    {
        DtxCreate(w);
    }
    else if (c.fno == 3)
    {
        DtxDestroy(w);
    }
    else if (c.fno >= 0x400 && c.fno < 0x440)
    {
        auto it = g_calls.find(c.fno - 0x400);
        if (it == g_calls.end())
        {
            // The module's table has nothing there (or the call isn't done here): it answers the arguments unchanged
            if (Once("cri urpc " + std::to_string(c.fno)))
            {
                Log("CRI ADX: remote call %#x not done: answered with its arguments", c.fno - 0x400);
            }
        }
        else
        {
            PS2_LOG(LogIop, "CRI ADX: %s(%08x %08x %08x %08x)", it->second.name, w[0], w[1], w[2], w[3]);
            it->second.call(w);
        }
    }
    else if (Once("cri dtx " + std::to_string(c.fno)))
    {
        Log("CRI ADX: DTX RPC function %u: not one of the module's", c.fno);
    }
    c.out.assign(reinterpret_cast<const u8*>(w.data()), reinterpret_cast<const u8*>(w.data()) + c.recvSize);
}

// --- the mailboxes' callbacks. A buffer's first word is the count of its 16 byte entries, which start at 0x10
constexpr u32 MaxEntries = 0x80;

// sjx_rcvcbf_iop: the EE's chunks of data put into the IOP's SJs
void SjxReceive(u32 buffer)
{
    u32 n = Read32(buffer);
    for (u32 i = 0; i < n; i++)
    {
        u32 e = buffer + 0x10 + i * 0x10;
        if (*IopMemory(e) != 0)
        {
            continue;
        }
        SjxObject& o = SjxOf(Read32(e + 4));
        o.sj->Put(1, {Read32(e + 8), static_cast<s32>(Read32(e + 12))});
    }
}

// sjx_sndcbf_iop: each joined SJ's chunks from its lane (all of them, 128 at most) sent to the EE's object
void SjxSend(u32 buffer)
{
    u32 n = 0;
    for (const SjxObject& o : g_module.sjx)
    {
        if (!o.used)
        {
            continue;
        }
        while (n < MaxEntries)
        {
            Chunk c = o.sj->Get(o.lane, 0x7FFFFFFF);
            if (c.size == 0)
            {
                break;
            }
            u32 e = buffer + 0x10 + n * 0x10;
            IopMemory(e)[0] = 0;
            IopMemory(e)[1] = static_cast<u8>(o.lane);
            std::memcpy(IopMemory(e + 2), &o.resets, 2);
            Write32(e + 4, o.eeObject);
            Write32(e + 8, c.data);
            Write32(e + 12, static_cast<u32>(c.size));
            n++;
        }
        if (n == MaxEntries)
        {
            break;
        }
    }
    Write32(buffer, n);
}

// ioprna_rcvcbf: the EE's commands to its streams (a halfword command, the stream, two arguments)
void RnaReceive(u32 buffer)
{
    u32 n = Read32(buffer);
    for (u32 i = 0; i < n; i++)
    {
        u32 e = buffer + 0x10 + i * 0x10;
        s16 command;
        std::memcpy(&command, IopMemory(e), 2);
        RnaObject& o = RnaOf(Read32(e + 4));
        s32 a = static_cast<s32>(Read32(e + 8));
        s32 b = static_cast<s32>(Read32(e + 12));
        PS2_LOG(LogIop, "CRI ADX: stream %08x command %d (%d, %d)", Read32(e + 4), command, a, b);
        switch (command)
        {
        case 0:
            PsmStart(o.psm);
            break;
        case 1:
            PsmStop(o.psm);
            break;
        case 2:
            a == 1 ? PsmStart(o.psm) : PsmStop(o.psm);
            break;
        case 3:
            o.active = a;
            PsmSetChannels(o.psm, a);
            break;
        case 4:
            PsmSetRate(o.psm, a);
            break;
        case 5:
            o.volume = b;
            for (s32 ch = 0; ch < o.channels; ch++)
            {
                PsmSetVolume(o.psm, ch, o.volume);
            }
            break;
        case 8:
            o.flushing = static_cast<u8>(a) != 0;
            break;
        case 9:
            PsmSetPan(o.psm, a, b);
            break;
        case 10:
            // The mix: 1 the first core's mixer; otherwise the second's, the first channel's volume set again (once a
            // channel, as the module does)
            o.mix = a;
            if (a == 1)
            {
                PsmSetMix(o.psm, 0);
            }
            else
            {
                PsmSetMix(o.psm, 1);
                for (s32 ch = 0; ch < o.channels; ch++)
                {
                    PsmSetVolume(o.psm, 0, o.volume);
                }
            }
            break;
        case 6:
        case 7:
            break;
        default:
            if (Once("cri rna command " + std::to_string(command)))
            {
                Log("CRI ADX: stream command %d: not one of the module's (ignored, as there)", command);
            }
            break;
        }
    }
}

// DTX_ExecHndl (0xA44): the EE's buffer taken in once its count passes the IOP's, then the IOP's sent back (the IOP's SIF DMA
// into the EE's memory)
void DtxExchange(size_t id)
{
    Dtx& d = g_module.dtx[id];
    u32 data = d.iop;
    u32 trailer = d.iop + d.size - 0x40;
    if (d.waiting)
    {
        s32 theirs = static_cast<s32>(Read32(trailer + 0x3C));
        if (d.count >= theirs)
        {
            return;
        }
        if (g_module.bound[id] == Mailbox::Sjx)
        {
            SjxReceive(data);
        }
        else if (g_module.bound[id] == Mailbox::Rna)
        {
            RnaReceive(data);
        }
        d.waiting = false;
        d.count = theirs;
    }
    if (g_module.bound[id] == Mailbox::Sjx)
    {
        SjxSend(data);
    }
    else if (g_module.bound[id] == Mailbox::Rna)
    {
        // ioprna_sndcbf sends no entries
        Write32(data, 0);
    }
    d.count++;
    Write32(trailer + 0x3C, static_cast<u32>(d.count));
    std::memcpy(EeMemory(d.ee), IopMemory(d.iop), d.size);
    d.waiting = true;
}

// iop_main_proc's loop (ADXI_ExecServer, after WaitVblankEnd): the flushed streams' data thrown away (IOPRNA_ExecHndl), then
// every mailbox exchanged
void Vblank()
{
    auto lock = SoundLock();
    for (RnaObject& o : g_module.rna)
    {
        if (!o.used || !o.flushing)
        {
            continue;
        }
        for (s32 ch = 0; ch < std::min(o.active, 2); ch++)
        {
            Sj* sj = o.inputs[ch];
            for (Chunk c = sj->Get(1, 0x7FFFFFFF); c.size > 0; c = sj->Get(1, 0x7FFFFFFF))
            {
                sj->Put(0, c);
            }
        }
    }
    for (size_t id = 0; id < g_module.dtx.size(); id++)
    {
        if (g_module.dtx[id].used)
        {
            DtxExchange(id);
        }
    }
}

// The module's arguments: "spucore=N" (0 or 1) is the core whose input plays; "sdinit=1" has it call sceSdInit(0) itself; the
// threads' priorities and "debug=" don't matter here
void Start(const std::vector<std::string>& args)
{
    if (g_module.started)
    {
        Fatal("CRI ADX: the driver loaded a second time");
    }
    s32 core = 1;
    bool sdInit = false;
    for (const std::string& a : args)
    {
        auto number = [&](const char* key) -> int {
            size_t n = std::strlen(key);
            return a.compare(0, n, key) == 0 && a.size() > n && std::isdigit(static_cast<unsigned char>(a[n]))
                       ? std::atoi(a.c_str() + n)
                       : -1;
        };
        if (int v = number("spucore="); v == 0 || v == 1)
        {
            core = v;
        }
        if (int v = number("sdinit="); v == 0 || v == 1)
        {
            sdInit = v == 1;
        }
    }
    g_module.started = true;
    g_module.memory = IopAlloc(ModuleBytes);
    std::memset(IopMemory(g_module.memory), 0, ModuleBytes);
    g_module.core = core;
    {
        auto lock = SoundLock();
        if (sdInit)
        {
            SdInit();
        }
        PsmSetupCore(core);
        // ADXI_Init: SJX_Init and IOPRNA_Init bind DTX 0 and 1 (and PSM_ReinitSj with nothing)
        g_module.bound[0] = Mailbox::Sjx;
        g_module.bound[1] = Mailbox::Rna;
    }
    IopRegisterServer(DtxServer, "CRI ADX DTX", DtxServe);
    IopOnVblank(Vblank);
    Log("CRI ADX: driver started (spu core %d%s; module memory %06x)", core, sdInit ? ", sceSdInit" : "", g_module.memory);
}
}

void Register() { IopRegisterModule("CRI_ADX_Driver", Start); }
}
