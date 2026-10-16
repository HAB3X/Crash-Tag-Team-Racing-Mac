// The graphics hardware's runtime side: the hardware layer's GS, GIF, VIFs and VUs made and wired together, VU0's macro mode
// registers handed to recompiled code, the GS's privileged registers (CSR's vblank and FINISH bits, the IMR), and the picture
// the PCRTC shows at each vertical blank given to the presenter (a window, or PNGs when headless).
#include "runtime.h"

#include "../../hw/ee/gif.h"
#include "../../hw/ee/vif.h"
#include "../../hw/ee/vu.h"
#include "../../hw/ee/vu1translate.h"
#include "../../hw/gs/gs.h"
#include "../../hw/png.h"
#include "ui/menus.h"
#include "sound/spu2.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>

Ps2Vu0Regs* ps2_vu0 = nullptr;
u8* ps2_vu0_mem = nullptr;
u32 ps2_vu0_cmsar0 = 0;

namespace Ps2
{
Options g_options;

namespace
{
Hardware g_hw;
// CSR's SIGNAL, FINISH and VSINT bits: set from the render thread (render.cpp) as well as the game's
std::atomic<u64> g_csr{0};
u64 g_imr = 0xFF00;
u64 g_smode1 = 0;
u64 g_busdir = 0;
std::atomic<u64> g_siglblid{0};
// The privileged registers the PCRTC reads, as the game last wrote them: the GS's own copies are set in order with the drawing
// (on the render thread), these answer the game's reads at once
u64 g_pmode = 0, g_smode2 = 0, g_dispfb[2] = {}, g_display[2] = {}, g_bgcolor = 0;
u64 g_other[0x20] = {};
u32 g_field = 0;
std::atomic<u64> g_vblanks{0};
// The game's frames: the vblank periods in which it presented a picture, by showing another buffer (an enabled circuit's
// DISPFB changed) or by writing into the one shown (a game that copies each finished picture into a display buffer it never
// moves: CTTR). Counted on the GS's side at each vblank, in order with what the game sent before it
std::atomic<u64> g_frames{0};

void CountFrame(const Gs::Gs& gs)
{
    static u64 shown[2] = {};
    static u64 writes = 0;
    bool moved = (gs.pmode.en1 != 0 && gs.dispfb[0].value != shown[0]) || (gs.pmode.en2 != 0 && gs.dispfb[1].value != shown[1]);
    bool written = gs.shownBufferWrites != writes;
    shown[0] = gs.dispfb[0].value;
    shown[1] = gs.dispfb[1].value;
    writes = gs.shownBufferWrites;
    if (moved || written)
    {
        g_frames++;
    }
}

std::mutex g_pictureLock;
std::condition_variable g_pictureCv;
Gs::DisplayImage g_picture;
// The snapshots made, for the presenter to save (their names and pictures)
std::vector<std::pair<std::string, Gs::DisplayImage>> g_snapshots;
// $PS2_DUMP_BUFFER: the shown buffer read whole (its width and the display's full height), saved beside each snapshot
bool g_dumpBuffer = getenv("PS2_DUMP_BUFFER") != nullptr;
u64 g_pictureSerial = 0;
bool g_stop = false;
SDL_Window* g_window = nullptr;

void GsEvent(u32 address, u64 value)
{
    switch (address)
    {
    case Gs::SIGNAL:
        g_siglblid = (g_siglblid.load() & ~0xFFFFFFFFull) | (value & 0xFFFFFFFF);
        g_csr |= 1;
        if (!(g_imr & 0x100))
        {
            RaiseIntc(0);
        }
        break;
    case Gs::FINISH:
        g_csr |= 2;
        if (!(g_imr & 0x200))
        {
            RaiseIntc(0);
        }
        break;
    case Gs::LABEL:
        g_siglblid = (g_siglblid.load() & 0xFFFFFFFFull) | (value << 32);
        break;
    }
}
}

Hardware& Hw() { return g_hw; }

void GsInit()
{
    g_hw.gs = new Gs::Gs();
    g_hw.gif = new Ee::Gif(*g_hw.gs);
    g_hw.vu0 = new Ee::Vu(0);
    g_hw.vu1 = new Ee::Vu(1);
    g_hw.vif0 = new Ee::Vif(0, *g_hw.vu0, nullptr);
    g_hw.vif1 = new Ee::Vif(1, *g_hw.vu1, g_hw.gif);
    // A code's interrupt bit: the INTC's VIF0 or VIF1 cause, the VIF stalled until the game cancels it
    g_hw.vif0->interrupt = [](u32) { RaiseIntc(4); };
    g_hw.vif1->interrupt = [](u32) { RaiseIntc(5); };
    g_hw.gs->eventHook = GsEvent;
    // Field rendering's half line dropped, so the picture doesn't shake (hw/gs/gs.h; $PS2_STEADY_FIELDS=0 keeps it, as on a TV)
    const char* steady = getenv("PS2_STEADY_FIELDS");
    g_hw.gs->steadyFields = steady == nullptr || strcmp(steady, "0") != 0;
    Ee::Vu* vu1 = g_hw.vu1;
    Ee::Gif* gif = g_hw.gif;
    // $PS2_VU_STEP=ADDR: the first run of the VU1 program at that byte address traced instruction by instruction
    static u32 stepAt = getenv("PS2_VU_STEP") != nullptr ? static_cast<u32>(strtoul(getenv("PS2_VU_STEP"), nullptr, 0)) : ~0u;
    if (stepAt != ~0u)
    {
        vu1->runner = [vu1](u32 start) {
            if (start * 8 != stepAt)
            {
                vu1->RunInterpreted(start);
                return;
            }
            stepAt = ~0u;
            Ee::VuMicroState state;
            vu1->BeginMicro(state, start);
            int n = 0;
            do
            {
                const Ee::Vf& f11 = vu1->vf[11];
                Log("vu1 step %d pc %x: vi2 %x vi3 %x vi13 %x vi14 %x vi12 %x | vf11 %08x %08x %08x %08x", n, state.pc * 8,
                    vu1->vi[2], vu1->vi[3], vu1->vi[13], vu1->vi[14], vu1->vi[12], f11.u[0], f11.u[1], f11.u[2], f11.u[3]);
                n++;
            } while (vu1->StepMicro(state) && n < 400);
            vu1->EndMicro(state);
            const u32* q = reinterpret_cast<const u32*>(vu1->data() + ((vu1->top + 0xd2) & 0x3FF) * 16);
            Log("vu1 top %x: [top+d2] %08x %08x %08x %08x [top+d3] %08x %08x %08x %08x", vu1->top, q[0], q[1], q[2], q[3], q[4],
                q[5], q[6], q[7]);
        };
    }
    else
    {
        // The game's VU1 programs run translated (the build made them from its executable; $PS2_VU1=interpret or verify)
        Ee::AttachVu1Translation(*vu1);
    }
    static int kickTrace = getenv("PS2_KICK_TRACE") != nullptr ? atoi(getenv("PS2_KICK_TRACE")) : 0;
    vu1->xgkick = [vu1, gif](u32 address) {
        if (kickTrace > 0)
        {
            static bool dumped = false;
            if (!dumped && getenv("PS2_DUMP_VU1") != nullptr)
            {
                dumped = true;
                FILE* f = fopen(getenv("PS2_DUMP_VU1"), "wb");
                fwrite(vu1->code(), 8, vu1->codeSize(), f);
                fwrite(vu1->data(), 1, vu1->dataSize(), f);
                fclose(f);
            }
            kickTrace--;
            const u32* q = reinterpret_cast<const u32*>(vu1->data() + (address & 0x3FF) * 16);
            Log("xgkick from program %x at %x: qw %x tag %08x %08x %08x %08x | %08x %08x %08x %08x | vi %x %x %x %x %x %x %x %x",
                vu1->runStart * 8, vu1->kickPc * 8, address, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], vu1->vi[1],
                vu1->vi[2], vu1->vi[3], vu1->vi[4], vu1->vi[5], vu1->vi[6], vu1->vi[7], vu1->vi[8]);
        }
        gif->Kick(vu1->data(), address, vu1->kickPc * 8);
    };
    // VU0's registers as recompiled code's macro mode sees them: the same members in the same order
    static_assert(offsetof(Ee::Vu, vi) - offsetof(Ee::Vu, vf) == offsetof(Ps2Vu0Regs, vi), "VU0 layout");
    static_assert(offsetof(Ee::Vu, acc) - offsetof(Ee::Vu, vf) == offsetof(Ps2Vu0Regs, acc), "VU0 layout");
    static_assert(offsetof(Ee::Vu, clipFlag) - offsetof(Ee::Vu, vf) == offsetof(Ps2Vu0Regs, clip), "VU0 layout");
    ps2_vu0 = reinterpret_cast<Ps2Vu0Regs*>(&g_hw.vu0->vf[0]);
    ps2_vu0_mem = g_hw.vu0->data();
    ps2_vu0->vf[0].f[3] = 1.0f;
    // The window and the GPU's contexts first (on the main thread): the render thread starts the hardware renderer with its own
    g_window = DisplayOpen();
    RenderStart();
}

u64 GsCsr()
{
    // FIELD (bit 13), the FIFO empty (bits 14-15 = 01), REV 0x1B and ID 0x55
    return g_csr.load() | (static_cast<u64>(g_field) << 13) | (1ull << 14) | (0x551Bull << 16);
}

u64 GsPrivRead(u32 a)
{
    switch (a & 0x1FFFFFF8)
    {
    case 0x12000000: return g_pmode;
    case 0x12000010: return g_smode1;
    case 0x12000020: return g_smode2;
    case 0x12000070: return g_dispfb[0];
    case 0x12000080: return g_display[0];
    case 0x12000090: return g_dispfb[1];
    case 0x120000A0: return g_display[1];
    case 0x120000E0: return g_bgcolor;
    case 0x12001000:
        // The game may be waiting for a FINISH or SIGNAL in data the render thread hasn't been handed yet
        RenderFlush();
        if (ps2_irq_pending && t_cpu != nullptr)
        {
            ps2_irq_service(t_cpu);
        }
        return GsCsr();
    case 0x12001010: return g_imr;
    case 0x12001040: return g_busdir;
    case 0x12001080: return g_siglblid;
    default: return g_other[((a - 0x12000000) >> 4) & 0x1F];
    }
}

void GsPrivWrite(u32 a, u64 v)
{
    Gs::Gs* gs = g_hw.gs;
    PS2_LOG(LogGs, "GS %08x = %016llx", a, static_cast<unsigned long long>(v));
    switch (a & 0x1FFFFFF8)
    {
    case 0x12000000:
        g_pmode = v;
        RenderCall([gs, v] { gs->pmode.value = v; });
        break;
    case 0x12000010: g_smode1 = v; break;
    case 0x12000020:
        g_smode2 = v;
        RenderCall([gs, v] { gs->smode2.value = v; });
        break;
    case 0x12000070:
        g_dispfb[0] = v;
        RenderCall([gs, v] { gs->dispfb[0].value = v; });
        break;
    case 0x12000080:
        g_display[0] = v;
        RenderCall([gs, v] { gs->display[0].value = v; });
        break;
    case 0x12000090:
        g_dispfb[1] = v;
        RenderCall([gs, v] { gs->dispfb[1].value = v; });
        break;
    case 0x120000A0:
        g_display[1] = v;
        RenderCall([gs, v] { gs->display[1].value = v; });
        break;
    case 0x120000E0:
        g_bgcolor = v;
        RenderCall([gs, v] { gs->bgcolor = v; });
        break;
    case 0x12001000:
        // SIGNAL, FINISH, HSINT, VSINT and EDWINT are cleared by ones
        g_csr &= ~(v & 0x1F);
        if (v & 0x200)
        {
            g_csr = 0;
        }
        break;
    case 0x12001010: g_imr = v & 0x7F00; break;
    case 0x12001040: g_busdir = v; break;
    case 0x12001080: g_siglblid = v; break;
    default: g_other[((a - 0x12000000) >> 4) & 0x1F] = v; break;
    }
}

void GsSetCrt(u32 interlaced, u32 mode, u32 field)
{
    PS2_LOG(LogGs, "SetGsCrt interlaced %u mode %u field %u", interlaced, mode, field);
    // SMODE2: INT (bit 0), FFMD (bit 1)
    g_smode2 = (g_smode2 & ~3ull) | (interlaced & 1) | ((field & 1) << 1);
    Gs::Gs* gs = g_hw.gs;
    u64 v = g_smode2;
    RenderCall([gs, v] { gs->smode2.value = v; });
}

bool GsVblank(bool newest, bool gameThread)
{
    u64 vblanks = ++g_vblanks;
    Gs::Gs* gs = g_hw.gs;
    Ee::Gif* gif = g_hw.gif;
    RenderCall([gs, gif, vblanks] {
        CountFrame(*gs);
        GsRecordVblank(*gs, *gif, vblanks);
    });
    g_field ^= 1;
    g_csr |= 8;
    if (!(g_imr & 0x800))
    {
        RaiseIntc(0);
    }
    // The snapshots due now, by the game's time (its vblanks): a script can count on it, the wall clock's runs differ with how
    // fast the host draws and loads
    static double nextEvery = g_options.snapshotEvery;
    static size_t nextAt = 0;
    // Due and not yet given a picture (they wait for the newest blank's)
    static std::vector<std::string> names;
    if (!g_options.snapshotDir.empty())
    {
        double game = static_cast<double>(vblanks) / 59.94;
        char name[64];
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
    }
    // The picture: every frame for a window or snapshots (the game's thread kept at most two pictures ahead), its pixels read
    // for a snapshot when one is due
    bool show = !g_options.headless || !g_options.ui.empty();
    if ((!show && g_options.snapshotDir.empty()) || !newest)
    {
        return false;
    }
    // Made on the GS's side once it has drawn what came before (the game's thread goes on, at most two pictures ahead): the
    // GPU's picture copied for the window, the pixels read when there's no copy or a snapshot wants them
    bool queued = RenderFrame([show, names = names] {
        Gs::DisplayImage image;
        bool copied = false;
        if (show && DisplayOnGpu())
        {
            copied = DisplayCopyShown() >= 0;
            if (!copied)
            {
                DisplayDropShown();
            }
        }
        if (!names.empty() || (show && !copied))
        {
            DisplayReadShown(image);
        }
        Gs::DisplayImage buffer;
        if (g_dumpBuffer && !names.empty())
        {
            Gs::Gs& gs = *g_hw.gs;
            int i = gs.pmode.en2 ? 1 : 0;
            buffer.width = static_cast<u32>(gs.dispfb[i].fbw) * 64;
            buffer.height = static_cast<u32>(gs.display[i].dh + 1);
            gs.PrepareCpuRect(false, static_cast<u32>(gs.dispfb[i].psm), static_cast<u32>(gs.dispfb[i].fbp) * 32,
                              static_cast<u32>(gs.dispfb[i].fbw), 0, 0, static_cast<s32>(buffer.width) - 1,
                              static_cast<s32>(buffer.height) - 1, "dump");
            buffer.pixels.resize(static_cast<size_t>(buffer.width) * buffer.height);
            for (u32 y = 0; y < buffer.height; y++)
            {
                for (u32 x = 0; x < buffer.width; x++)
                {
                    u32 v = gs.ReadPixel(static_cast<u32>(gs.dispfb[i].psm), x, y, static_cast<u32>(gs.dispfb[i].fbp) * 32,
                                         static_cast<u32>(gs.dispfb[i].fbw));
                    buffer.pixels[static_cast<size_t>(y) * buffer.width + x] = v | 0xFF000000u;
                }
            }
        }
        std::lock_guard<std::mutex> lock(g_pictureLock);
        for (const std::string& name : names)
        {
            g_snapshots.push_back({name + ".png", image});
            if (buffer.width != 0)
            {
                g_snapshots.push_back({name + "-buffer.png", buffer});
            }
        }
        g_picture = std::move(image);
        g_pictureSerial++;
        g_pictureCv.notify_all();
    }, gameThread);
    if (queued)
    {
        names.clear();
    }
    return gameThread;
}

void PresenterStop()
{
    std::lock_guard<std::mutex> lock(g_pictureLock);
    g_stop = true;
    g_pictureCv.notify_all();
}

static void SavePicture(const Gs::DisplayImage& image, const std::string& name)
{
    if (image.width == 0 || image.height == 0)
    {
        return;
    }
    std::string path = g_options.snapshotDir + "/" + name;
    if (WritePng(path, image.pixels.data(), image.width, image.height))
    {
        Log("saved %s (%ux%u)", path.c_str(), image.width, image.height);
    }
}

void PresenterRun()
{
    using clock = std::chrono::steady_clock;
    auto start = clock::now();
    double nextWatch = 0;
    double nextFps = 1;
    u64 lastFrames = 0, lastVblanks = 0;
    InputInit();
    u64 shown = 0;
    // The last picture of the GS's memory (the software GS's), for the menus' scripted pictures when headless
    Gs::DisplayImage lastImage;
    for (;;)
    {
        Gs::DisplayImage image;
        std::vector<std::pair<std::string, Gs::DisplayImage>> snapshots;
        bool fresh = false;
        // The PC menus up: drawn again and again over the paused game's last picture
        bool overlay = Ui::MenusVisible();
        {
            std::unique_lock<std::mutex> lock(g_pictureLock);
            g_pictureCv.wait_for(lock, std::chrono::milliseconds(overlay ? 4 : 20),
                                 [&] { return g_stop || g_pictureSerial != shown; });
            if (g_stop)
            {
                break;
            }
            if (g_pictureSerial != shown)
            {
                shown = g_pictureSerial;
                image = std::move(g_picture);
                snapshots.swap(g_snapshots);
                fresh = true;
            }
        }
        double t = std::chrono::duration<double>(clock::now() - start).count();
        double game = static_cast<double>(g_vblanks.load()) / 59.94;
        InputUpdate(game, g_window);
        for (const auto& [name, picture] : snapshots)
        {
            SavePicture(picture, name);
        }
        if (t >= nextFps)
        {
            // Each second: the game's frames and the vblanks (60 a second is the PS2's own speed), in the window's title and
            // (--fps) the log
            u64 counted = g_frames.load(), vblanks = g_vblanks;
            u64 frames = counted - lastFrames, blanks = vblanks - lastVblanks;
            if (g_options.fps)
            {
                Log("%.0f s: %llu frames, %llu vblanks", t, static_cast<unsigned long long>(frames),
                    static_cast<unsigned long long>(blanks));
            }
            if (g_window != nullptr)
            {
                char title[160];
                if (blanks >= 57)
                {
                    snprintf(title, sizeof(title), "%s - %llu fps", WindowTitle().c_str(), static_cast<unsigned long long>(frames));
                }
                else
                {
                    snprintf(title, sizeof(title), "%s - %llu fps, %llu%% speed", WindowTitle().c_str(),
                             static_cast<unsigned long long>(frames), static_cast<unsigned long long>(blanks * 100 / 60));
                }
                SDL_SetWindowTitle(g_window, title);
            }
            lastFrames = counted;
            lastVblanks = vblanks;
            nextFps = t + 1;
        }
#ifdef PS2_TRACE_ENTER
        {
            // $PS2_TRACE_POKE=SECONDS:ADDRESS=VALUE (hex address and value): one guest word set at that time, for experiments
            static int pokeState = 0;
            static double pokeAt = 0;
            static u32 pokeAddress = 0, pokeValue = 0;
            if (pokeState == 0)
            {
                pokeState = 1;
                const char* r = getenv("PS2_TRACE_POKE");
                if (r != nullptr && sscanf(r, "%lf:%x=%x", &pokeAt, &pokeAddress, &pokeValue) == 3)
                {
                    pokeState = 2;
                }
            }
            if (pokeState == 2 && t >= pokeAt)
            {
                Write32(pokeAddress, pokeValue);
                Log("poke %08x = %08x at %.1f s", pokeAddress, pokeValue, t);
                pokeState = 3;
            }
        }
#endif
        if (!g_options.watch.empty() && t >= nextWatch)
        {
            std::string line;
            for (const auto& [address, count] : g_options.watch)
            {
                char item[32];
                snprintf(item, sizeof(item), " %08x:", address);
                line += item;
                for (u32 k = 0; k < count; k++)
                {
                    snprintf(item, sizeof(item), " %08x", Read32(address + k * 4));
                    line += item;
                }
            }
            Log("watch %.1f s:%s", t, line.c_str());
            nextWatch += g_options.watchEvery;
        }
        if (g_options.runSeconds > 0 && t >= g_options.runSeconds)
        {
            Log("ran %.0f s: stopping", t);
#ifdef PS2_TRACE_ENTER
            TraceEnterDump();
            // The SPU2's two cores' state (core 0 voice 0, core 1 voice 0): attributes, mixes, master volumes
            Spu2::LogVoice(0);
            Spu2::LogVoice(24);
#endif
            const Ee::Vu1TranslationStats& vs = Ee::GetVu1TranslationStats();
            Log("VU1: %llu instructions translated, %llu interpreted%s", static_cast<unsigned long long>(vs.translatedInstructions),
                static_cast<unsigned long long>(vs.interpretedInstructions),
                vs.verifiedRuns != 0 ? (", " + std::to_string(vs.verifiedRuns) + " runs verified, " +
                                        std::to_string(vs.differences) + " differences").c_str()
                                     : "");
            Ee::ReportVu1();
            KernelDumpThreads();
            break;
        }
        if (g_window != nullptr)
        {
            SDL_Event e;
            while (SDL_PollEvent(&e))
            {
                if (e.type == SDL_EVENT_QUIT)
                {
                    g_stop = true;
                }
                // The PC menus take what's theirs (all of it while they're up)
                if (!Ui::MenusEvent(e))
                {
                    InputEvent(e);
                }
            }
            Ui::MenusFrame(game);
            if (g_stop || Ui::MenusQuitting())
            {
                break;
            }
            if (fresh && image.width != 0 && g_options.headless)
            {
                lastImage = image;
            }
            overlay = Ui::MenusVisible();
            if (fresh || overlay)
            {
                SDL_FRect rect = {};
                DisplayPresent(fresh && image.width != 0 ? &image : nullptr, overlay, &rect);
                if (rect.w > 0)
                {
                    InputPicture(rect.x, rect.y, rect.w, rect.h);
                }
            }
            std::string shot = Ui::MenusShotDue();
            if (!shot.empty())
            {
                DisplayShot(shot, lastImage.width != 0 ? &lastImage : nullptr);
            }
        }
    }
    DisplayClose();
}
}

using namespace Ps2;

extern "C" u32 vu0_cfc2(int reg)
{
    if (reg < 16)
    {
        return ps2_vu0->vi[reg];
    }
    switch (reg)
    {
    case 16: return ps2_vu0->status;
    case 17: return ps2_vu0->mac;
    case 18: return ps2_vu0->clip;
    case 20: return ps2_vu0->r;
    case 21: return ps2_vu0->i;
    case 22: return ps2_vu0->q;
    case 26: return g_hw.vu0->pc();
    case 27: return ps2_vu0_cmsar0;
    case 29: return 0; // VPU-STAT: both units idle
    default: return 0;
    }
}

extern "C" void vu0_ctc2(int reg, u32 value)
{
    if (reg < 16)
    {
        if (reg != 0)
        {
            ps2_vu0->vi[reg] = static_cast<u16>(value);
        }
        return;
    }
    switch (reg)
    {
    case 16: ps2_vu0->status = value; break;
    case 18: ps2_vu0->clip = value; break;
    case 20: ps2_vu0->r = value; break;
    case 21: ps2_vu0->i = value; break;
    case 22: ps2_vu0->q = value; break;
    case 27: ps2_vu0_cmsar0 = value & 0xFFFF; break;
    case 28: break; // FBRST
    case 31: // CMSAR1: starts VU1's program
        g_hw.vu1->Run(value & 0xFFFF);
        break;
    default:
        break;
    }
}

extern "C" void vu0_callms(u32 byteAddress) { g_hw.vu0->CallMicro(byteAddress); }
