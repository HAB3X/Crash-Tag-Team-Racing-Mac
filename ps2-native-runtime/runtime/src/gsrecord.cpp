// The GS's side recorded and played back (--gs-record, --gs-replay): a stretch of what the game sent the GIF (its paths' data
// and VU1's XGKICK packets, in order) with the GS's and the GIF's state at its start and the PCRTC's registers at each vblank,
// played back later through the GS alone. The same recording drawn by two builds of the renderer gives the same pictures when
// nothing that shows has changed, whatever the host's speed (a game's own pictures differ from run to run with the frames it
// skips), and the render thread's work measured without the game's.
//
// The file: "PS2GSREC", a version, the state (Gs::Serialize, Gif::Serialize) with its size, then records of a 16 byte header
// (kind, value, quadwords, 0) and their quadwords: a path's transfer (value: the path), a kick (value: the kicking code's
// address) or a vblank (value: the vblank's number; PMODE, SMODE2, DISPFB1-2, DISPLAY1-2 and BGCOLOR in its 4 quadwords).
#include "runtime.h"

#include "../../hw/ee/gif.h"
#include "../../hw/gs/backend.h"
#include "../../hw/gs/gs.h"
#include "../../hw/png.h"
#include "../../hw/state.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <ctime>
#include <vector>

namespace Ps2
{
namespace
{
constexpr char Magic[8] = {'P', 'S', '2', 'G', 'S', 'R', 'E', 'C'};
constexpr u32 Version = 1;

enum RecordKind : u32
{
    KindTransfer,
    KindKick,
    KindVblank,
};

struct Header
{
    u32 kind;
    u32 value;
    u32 quadwords;
    u32 unused;
};
static_assert(sizeof(Header) == 16);

struct Pcrtc
{
    u64 pmode, smode2, dispfb[2], display[2], bgcolor, unused;
};
static_assert(sizeof(Pcrtc) == 64);

// The recording being made (on the GS's side: the render thread, or the game's thread without one)
FILE* g_file = nullptr;
u64 g_vblanksLeft = 0;
bool g_done = false;
// --control's record request (taken by the GS's side at its next vblank)
std::mutex g_requestLock;
std::string g_requestFile;
u64 g_requestVblanks = 0;

void Write(u32 kind, u32 value, const void* data, u32 quadwords)
{
    Header header = {kind, value, quadwords, 0};
    if (fwrite(&header, sizeof(header), 1, g_file) != 1 ||
        (quadwords != 0 && fwrite(data, 16, quadwords, g_file) != quadwords))
    {
        Log("gs record: writing %s failed; the recording stops", g_options.gsRecordFile.c_str());
        fclose(g_file);
        g_file = nullptr;
        g_done = true;
    }
}

bool Begin(Gs::Gs& gs, Ee::Gif& gif)
{
    if (!gif.forwarding())
    {
        Log("gs record: only with the render thread (not with $PS2_RENDER_THREAD=0)");
        return false;
    }
    g_file = fopen(g_options.gsRecordFile.c_str(), "wb");
    if (g_file == nullptr)
    {
        Log("gs record: can't write %s", g_options.gsRecordFile.c_str());
        return false;
    }
    // Local memory made current first: the GPU's renderer keeps newer pages in its own buffers
    if (gs.backend() != nullptr)
    {
        Gs::PageSet all;
        all.set();
        gs.backend()->Flush();
        gs.backend()->BeforeCpuRead(all);
    }
    std::vector<u8> state;
    NativeGraphics::StateWriter writer(state);
    NativeGraphics::Saver saver{writer};
    gs.Serialize(saver);
    gif.Serialize(saver);
    u64 size = state.size();
    fwrite(Magic, sizeof(Magic), 1, g_file);
    fwrite(&Version, sizeof(Version), 1, g_file);
    fwrite(&size, sizeof(size), 1, g_file);
    fwrite(state.data(), 1, state.size(), g_file);
    g_vblanksLeft = g_options.gsRecordVblanks;
    Log("gs record: %s from vblank %.1f s, %llu vblanks", g_options.gsRecordFile.c_str(), g_options.gsRecordAt,
        static_cast<unsigned long long>(g_vblanksLeft));
    return true;
}

Pcrtc PcrtcOf(const Gs::Gs& gs)
{
    Pcrtc p = {};
    p.pmode = gs.pmode.value;
    p.smode2 = gs.smode2.value;
    p.dispfb[0] = gs.dispfb[0].value;
    p.dispfb[1] = gs.dispfb[1].value;
    p.display[0] = gs.display[0].value;
    p.display[1] = gs.display[1].value;
    p.bgcolor = gs.bgcolor;
    return p;
}

double ThreadCpuSeconds()
{
    timespec t = {};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_nsec) / 1e9;
}

u64 Fnv(const std::vector<u32>& pixels)
{
    u64 h = 1469598103934665603ull;
    for (u32 p : pixels)
    {
        h = (h ^ p) * 1099511628211ull;
    }
    return h;
}
}

void GsRecordData(u32 path, const u8* data, u32 quadwords, bool kick)
{
    if (g_file != nullptr)
    {
        Write(kick ? KindKick : KindTransfer, path, data, quadwords);
    }
}

void GsLiveRecord(const std::string& file, u64 vblanks)
{
    std::lock_guard<std::mutex> lock(g_requestLock);
    g_requestFile = file;
    g_requestVblanks = vblanks;
}

void GsRecordVblank(Gs::Gs& gs, Ee::Gif& gif, u64 vblank)
{
    {
        std::lock_guard<std::mutex> lock(g_requestLock);
        if (!g_requestFile.empty() && g_file == nullptr)
        {
            g_options.gsRecordFile = g_requestFile;
            g_options.gsRecordVblanks = g_requestVblanks;
            g_options.gsRecordAt = 0;
            g_requestFile.clear();
            g_done = false;
        }
    }
    if (g_options.gsRecordFile.empty() || g_done)
    {
        return;
    }
    if (g_file == nullptr)
    {
        if (static_cast<double>(vblank) / 59.94 < g_options.gsRecordAt || !Begin(gs, gif))
        {
            g_done = g_file == nullptr && static_cast<double>(vblank) / 59.94 >= g_options.gsRecordAt;
            return;
        }
    }
    Pcrtc p = PcrtcOf(gs);
    Write(KindVblank, static_cast<u32>(vblank), &p, sizeof(p) / 16);
    if (g_file != nullptr && --g_vblanksLeft == 0)
    {
        fclose(g_file);
        g_file = nullptr;
        g_done = true;
        Log("gs record: %s written", g_options.gsRecordFile.c_str());
    }
}

// On the GS's side: the recording's state loaded, its records played into the GIF; at each vblank the PCRTC's registers set and,
// with --snapshots, the picture's hash written to replay.txt there and the pictures --snapshot-at/--snapshot-every ask for saved
bool GsReplay(Gs::Gs& gs, Ee::Gif& gif, const std::vector<u8>& file, u32 pass)
{
    if (file.size() < sizeof(Magic) + 12 || std::memcmp(file.data(), Magic, sizeof(Magic)) != 0)
    {
        Log("gs replay: %s isn't a recording", g_options.gsReplayFile.c_str());
        return false;
    }
    u32 version = 0;
    u64 size = 0;
    std::memcpy(&version, file.data() + 8, 4);
    std::memcpy(&size, file.data() + 12, 8);
    if (version != Version || size > file.size() - 20)
    {
        Log("gs replay: %s is version %u (this build reads %u) or cut short", g_options.gsReplayFile.c_str(), version, Version);
        return false;
    }
    // Nothing runs to take the GS's interrupts
    gs.eventHook = nullptr;
    NativeGraphics::StateReader reader(file.data() + 20, size);
    NativeGraphics::Loader loader{reader};
    gs.Serialize(loader);
    gif.Serialize(loader);
    if (reader.failed())
    {
        Log("gs replay: %s's state is cut short", g_options.gsReplayFile.c_str());
        return false;
    }
    FILE* hashes = nullptr;
    if (!g_options.snapshotDir.empty() && pass == 0)
    {
        hashes = fopen((g_options.snapshotDir + "/replay.txt").c_str(), "w");
    }
    // --snapshot-every counts from the game's start, as in a run
    double nextEvery = g_options.snapshotEvery;
    size_t nextAt = 0;
    u64 vblanks = 0;
    auto start = std::chrono::steady_clock::now();
    const double cpuStart = ThreadCpuSeconds();
    double picturesTime = 0;
    for (size_t at = 20 + size; at + sizeof(Header) <= file.size();)
    {
        Header header;
        std::memcpy(&header, file.data() + at, sizeof(header));
        at += sizeof(header);
        const u8* data = file.data() + at;
        at += static_cast<size_t>(header.quadwords) * 16;
        if (at > file.size())
        {
            Log("gs replay: %s is cut short", g_options.gsReplayFile.c_str());
            break;
        }
        switch (header.kind)
        {
        case KindTransfer:
            gif.TransferNow(header.value, data, header.quadwords);
            break;
        case KindKick:
            gif.KickRow(data, header.quadwords, header.value);
            break;
        case KindVblank:
        {
            Pcrtc p;
            std::memcpy(&p, data, sizeof(p));
            gs.pmode.value = p.pmode;
            gs.smode2.value = p.smode2;
            gs.dispfb[0].value = p.dispfb[0];
            gs.dispfb[1].value = p.dispfb[1];
            gs.display[0].value = p.display[0];
            gs.display[1].value = p.display[1];
            gs.bgcolor = p.bgcolor;
            vblanks++;
            if (hashes == nullptr)
            {
                break;
            }
            auto before = std::chrono::steady_clock::now();
            Gs::DisplayImage image;
            DisplayReadShown(image);
            fprintf(hashes, "%u %ux%u %016llx\n", header.value, image.width, image.height,
                    static_cast<unsigned long long>(Fnv(image.pixels)));
            double game = static_cast<double>(header.value) / 59.94;
            std::vector<std::string> names;
            char name[64];
            if (g_options.snapshotEvery > 0 && nextEvery < game - g_options.snapshotEvery)
            {
                nextEvery = std::ceil(game / g_options.snapshotEvery) * g_options.snapshotEvery;
            }
            if (g_options.snapshotEvery > 0 && game >= nextEvery)
            {
                snprintf(name, sizeof(name), "t%06.1f", game);
                names.push_back(name);
                nextEvery += g_options.snapshotEvery;
            }
            while (nextAt < g_options.snapshotAt.size() && game >= g_options.snapshotAt[nextAt])
            {
                snprintf(name, sizeof(name), "at%06.1f", g_options.snapshotAt[nextAt]);
                names.push_back(name);
                nextAt++;
            }
            for (const std::string& n : names)
            {
                WritePng(g_options.snapshotDir + "/" + n + ".png", image.pixels.data(), image.width, image.height);
            }
            picturesTime += std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count();
            break;
        }
        default:
            Log("gs replay: an unknown record (%u) in %s", header.kind, g_options.gsReplayFile.c_str());
            at = file.size();
            break;
        }
    }
    if (gs.backend() != nullptr)
    {
        gs.backend()->Flush();
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() - picturesTime;
    // (the thread's own CPU time too: on a busy host the wall clock's varies with what else runs)
    Log("gs replay: pass %u: %llu vblanks drawn in %.3f s (%.1f vblanks a second), %.3f s of the thread's CPU%s", pass + 1,
        static_cast<unsigned long long>(vblanks), seconds, static_cast<double>(vblanks) / seconds, ThreadCpuSeconds() - cpuStart,
        hashes != nullptr ? ", the pictures' reads not counted" : "");
    if (hashes != nullptr)
    {
        fclose(hashes);
    }
    return true;
}
}
