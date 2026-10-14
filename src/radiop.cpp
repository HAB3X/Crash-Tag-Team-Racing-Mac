// Bink's IOP side ("RAD_IOP_SIDE", RAD Game Tools' module for its movies), high level. The executable carries the module at
// 0x599E00 (7,096 bytes) and loads it from a buffer (0x4664D0, with the arguments "-1" and "0"); the EE talks to it with SIF
// commands of ID 0xB, both ways, whose word at 12 is the request's or the answer's type. Read from the module's code:
//
// The EE's requests (handled by the module's command handler; the file ones done on its own thread):
//   0 read: a handle (16), the EE's buffer (20), an IOP buffer (24), the 4 KB block (28). A disc file's 4 KB (its two sectors
//     from block * 2: the handle says which file only by being one) or a file's from block * 4096, into the IOP buffer, then
//     answer 0 { bytes (16): 0x1000 or what was read, 0 on an error; the error (20); the EE's buffer (24) } sent with the
//     data to the EE's buffer
//   1 open: the name's address in the IOP's memory (16). "\NAME", "?/..." or "cdrom0:NAME" is searched for on the disc
//     (sceCdSearchFile), anything else opened with ioman; answer 1 { found (16), the handle (20: the file's sector +
//     0x80000000 on the disc, else ioman's descriptor), the error (24), the size (28) }
//   2 close: the handle (16); answer 2
//   3 the timer: its period in milliseconds (16; 6,001 and over: 6 s); the module's alarm sends answer 4 at each period
//   4 the sound: a core (16), on (20), an IOP buffer of 4 KB (24). On: the buffer played round in the core's input
//     (sceSdBlockTrans in loop mode), at each half's end answer 5 { the core (16), the half to fill now (20) }, and the
//     input's volume last given set again at the first half's end (the on path ends in the volume request's tail, 0x4B8,
//     which marks the core's volume waiting). Off: the transfer stopped, the core's input volume (BVOLL, BVOLR) 0. The game
//     sends the volume once, at its boot (0x459608), so every movie after the first gets it back only this way
//   5 volumes: a core (16), MVOLL (20), MVOLR (24), BVOLL (28), BVOLR (32), each -1 for unchanged; the input's kept by the
//     module (0x10D0, 0x10D8: 0 until given) and set at the next half's end
// At its start the module sends answer 3 (ready), sets the timer to 6 s and, with "1" as its second argument, calls
// sceSdInit(0).
#include "ps2/iop.h"
#include "ps2/sound.h"

#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace
{
using namespace Ps2;

constexpr u32 CommandId = 0xB;
constexpr u32 BlockSize = 0x1000;
constexpr u32 SamplesPerMs = 48;

u32 Get32(const u8* p, u32 off)
{
    u32 v;
    std::memcpy(&v, p + off, 4);
    return v;
}

void Put32(std::vector<u8>& p, u32 off, u32 v)
{
    if (p.size() < off + 4)
    {
        p.resize(off + 4);
    }
    std::memcpy(&p[off], &v, 4);
}

// An answer of a type with its words from 16 on
void Answer(u32 type, std::initializer_list<u32> words, u32 eeAddress = 0, const void* data = nullptr, u32 dataSize = 0)
{
    std::vector<u8> p(16 + 4 * words.size(), 0);
    Put32(p, 12, type);
    u32 off = 16;
    for (u32 w : words)
    {
        Put32(p, off, w);
        off += 4;
    }
    IopSendCommand(CommandId, std::move(p), eeAddress, data, dataSize);
}

// --- the files, on the module's thread
struct File
{
    u32 lsn;
    u32 size;
};

struct Request
{
    u32 type;
    u32 words[4];
    std::string name;
};

std::mutex g_lock;
std::condition_variable g_wake;
std::deque<Request> g_requests;
std::map<u32, File> g_files; // by handle

void Open(const std::string& name)
{
    // The module's three ways to the disc; ioman's other devices aren't there
    std::string path = name;
    bool disc = !path.empty() && (path[0] == '\\' || (path.size() > 1 && path[1] == '/'));
    if (path.rfind("cdrom0:", 0) == 0)
    {
        path = path.substr(7);
        disc = true;
    }
    u32 lsn = 0, size = 0;
    if (disc && DiscFind(path, lsn, size))
    {
        u32 handle = lsn + 0x80000000u;
        g_files[handle] = {lsn, size};
        Answer(1, {1, handle, 0, size});
        return;
    }
    Log("Bink's IOP side: %s not found", name.c_str());
    Answer(1, {0, static_cast<u32>(-1), static_cast<u32>(-1), 0});
}

void Read(u32 handle, u32 eeBuffer, u32 iopBuffer, u32 block)
{
    auto it = g_files.find(handle);
    u8* buffer = IopMemory(iopBuffer);
    if (it == g_files.end())
    {
        Log("Bink's IOP side: a read of handle %08x, not open", handle);
        Answer(0, {0, static_cast<u32>(-1), eeBuffer});
        return;
    }
    // The two sectors from block * 2 of the file the handle names (the module reads from the disc's start: the EE's block
    // counts from there for a disc file, as the handle's sector does)
    DiscReadSectors(block * 2, 2, buffer);
    Answer(0, {BlockSize, 0, eeBuffer}, eeBuffer, buffer, BlockSize);
}

void Thread()
{
    for (;;)
    {
        Request r;
        {
            std::unique_lock<std::mutex> lock(g_lock);
            g_wake.wait(lock, [] { return !g_requests.empty(); });
            r = std::move(g_requests.front());
            g_requests.pop_front();
        }
        switch (r.type)
        {
        case 0:
            Read(r.words[0], r.words[1], r.words[2], r.words[3]);
            break;
        case 1:
            Open(r.name);
            break;
        case 2:
            g_files.erase(r.words[0]);
            Answer(2, {});
            break;
        }
    }
}

void Queue(Request r)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_requests.push_back(std::move(r));
    g_wake.notify_one();
}

// --- the timer and the sound, in the command handler
int g_timer = -1;
// Each core's input volume as the module keeps it (BVOLL, BVOLR), and whether it's to be set at the core's next half
u16 g_inputVolume[2][2] = {};
bool g_inputVolumeWaiting[2] = {};

void SetTimer(u32 ms)
{
    auto lock = SoundLock();
    if (g_timer >= 0)
    {
        SoundRemoveTimer(g_timer);
    }
    u32 samples = ms < 6001 ? ms * SamplesPerMs : 6000 * SamplesPerMs;
    g_timer = SoundAddTimer(samples, [] { Answer(4, {}); });
}

// The end of a half: the input's volume set if it's waiting, then the EE asked to fill the half just played (bit 24 of
// the status is the half now being taken)
void HalfDone(s32 core)
{
    if (g_inputVolumeWaiting[core])
    {
        g_inputVolumeWaiting[core] = false;
        SdSetParam(static_cast<u16>(SD_P_BVOLL | core), g_inputVolume[core][0]);
        SdSetParam(static_cast<u16>(SD_P_BVOLR | core), g_inputVolume[core][1]);
    }
    u32 status = SdBlockTransStatus(core);
    u32 base = (status & 0xFFFFFF) - ((status >> 24) & 1) * (BlockSize / 2);
    u32 free = (status & 0x1000000) == 0 ? base + BlockSize / 2 : base;
    Answer(5, {static_cast<u32>(core), free});
}

void Sound(s32 core, bool on, u32 buffer)
{
    auto lock = SoundLock();
    if (on)
    {
        std::memset(IopMemory(buffer), 0, BlockSize);
        g_inputVolumeWaiting[core] = true;
        SdBlockTransLoop(core, buffer, BlockSize, HalfDone);
        return;
    }
    SdSetParam(static_cast<u16>(SD_P_BVOLL | core), 0);
    SdSetParam(static_cast<u16>(SD_P_BVOLR | core), 0);
    SdBlockTransStop(core);
}

void Volumes(s32 core, const u8* p)
{
    auto lock = SoundLock();
    s32 mvolL = static_cast<s32>(Get32(p, 20)), mvolR = static_cast<s32>(Get32(p, 24));
    if (mvolL != -1)
    {
        SdSetParam(static_cast<u16>(SD_P_MVOLL | core), static_cast<u16>(mvolL));
    }
    if (mvolR != -1)
    {
        SdSetParam(static_cast<u16>(SD_P_MVOLR | core), static_cast<u16>(mvolR));
    }
    for (int side = 0; side < 2; side++)
    {
        s32 v = static_cast<s32>(Get32(p, 28 + side * 4));
        if (v != -1)
        {
            g_inputVolume[core][side] = static_cast<u16>(v);
            g_inputVolumeWaiting[core] = true;
        }
    }
}

void Command(const u8* p, u32 size)
{
    (void)size;
    u32 type = Get32(p, 12);
    if (Once("radiop request " + std::to_string(type)))
    {
        Log("Bink's IOP side: first request %u: %08x %08x %08x %08x %08x", type, Get32(p, 16), Get32(p, 20), Get32(p, 24),
            Get32(p, 28), Get32(p, 32));
    }
    switch (type)
    {
    case 0:
        Queue({0, {Get32(p, 16), Get32(p, 20), Get32(p, 24), Get32(p, 28)}, {}});
        break;
    case 1:
        Queue({1, {}, reinterpret_cast<const char*>(IopMemory(Get32(p, 16)))});
        break;
    case 2:
        Queue({2, {Get32(p, 16)}, {}});
        break;
    case 3:
        SetTimer(Get32(p, 16));
        break;
    case 4:
        Sound(static_cast<s32>(Get32(p, 16) & 1), Get32(p, 20) != 0, Get32(p, 24));
        break;
    case 5:
        Volumes(static_cast<s32>(Get32(p, 16) & 1), p);
        break;
    default:
        if (Once("radiop " + std::to_string(type)))
        {
            Log("Bink's IOP side: request %u not known", type);
        }
        break;
    }
}

void Start(const std::vector<std::string>& args)
{
    static bool started = false;
    if (!started)
    {
        started = true;
        std::thread(Thread).detach();
    }
    if (args.size() > 1 && args[1] == "1")
    {
        auto lock = SoundLock();
        SdInit();
    }
    SetTimer(6000);
    Answer(3, {});
}
}

void CttrRegisterRadIop()
{
    Ps2::IopRegisterCommand(CommandId, Command);
    Ps2::IopRegisterModule("RAD_IOP_SIDE", Start);
}
