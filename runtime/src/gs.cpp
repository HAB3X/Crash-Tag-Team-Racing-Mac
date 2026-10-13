// The graphics hardware's runtime side: the hardware layer's GS, GIF, VIFs and VUs made and wired together, VU0's macro mode
// registers handed to recompiled code, the GS's privileged registers (CSR's vblank and FINISH bits, the IMR), and the picture
// the PCRTC shows at each vertical blank given to the presenter (a window, or PNGs when headless).
#include "runtime.h"

#include "../../hw/ee/gif.h"
#include "../../hw/ee/vif.h"
#include "../../hw/ee/vu.h"
#include "../../hw/gs/gs.h"
#include "../../hw/png.h"

#include <SDL3/SDL.h>

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
u64 g_csr = 0;
u64 g_imr = 0xFF00;
u64 g_smode1 = 0;
u64 g_busdir = 0;
u64 g_siglblid = 0;
u64 g_other[0x20] = {};
u32 g_field = 0;
u64 g_vblanks = 0;

std::mutex g_pictureLock;
std::condition_variable g_pictureCv;
Gs::DisplayImage g_picture;
// $PS2_DUMP_BUFFER: the shown buffer read whole (its width and the display's full height), saved beside each snapshot
Gs::DisplayImage g_buffer;
bool g_dumpBuffer = getenv("PS2_DUMP_BUFFER") != nullptr;
u64 g_pictureSerial = 0;
bool g_stop = false;

void GsEvent(u32 address, u64 value)
{
    switch (address)
    {
    case Gs::SIGNAL:
        g_siglblid = (g_siglblid & ~0xFFFFFFFFull) | (value & 0xFFFFFFFF);
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
        g_siglblid = (g_siglblid & 0xFFFFFFFFull) | (value << 32);
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
    g_hw.gs->eventHook = GsEvent;
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
}

u64 GsCsr()
{
    // FIELD (bit 13), the FIFO empty (bits 14-15 = 01), REV 0x1B and ID 0x55
    return g_csr | (static_cast<u64>(g_field) << 13) | (1ull << 14) | (0x551Bull << 16);
}

u64 GsPrivRead(u32 a)
{
    Gs::Gs& gs = *g_hw.gs;
    switch (a & 0x1FFFFFF8)
    {
    case 0x12000000: return gs.pmode.value;
    case 0x12000010: return g_smode1;
    case 0x12000020: return gs.smode2.value;
    case 0x12000070: return gs.dispfb[0].value;
    case 0x12000080: return gs.display[0].value;
    case 0x12000090: return gs.dispfb[1].value;
    case 0x120000A0: return gs.display[1].value;
    case 0x120000E0: return gs.bgcolor;
    case 0x12001000:
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
    Gs::Gs& gs = *g_hw.gs;
    PS2_LOG(LogGs, "GS %08x = %016llx", a, static_cast<unsigned long long>(v));
    switch (a & 0x1FFFFFF8)
    {
    case 0x12000000: gs.pmode.value = v; break;
    case 0x12000010: g_smode1 = v; break;
    case 0x12000020: gs.smode2.value = v; break;
    case 0x12000070: gs.dispfb[0].value = v; break;
    case 0x12000080: gs.display[0].value = v; break;
    case 0x12000090: gs.dispfb[1].value = v; break;
    case 0x120000A0: gs.display[1].value = v; break;
    case 0x120000E0: gs.bgcolor = v; break;
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
    g_hw.gs->smode2.interlaced = interlaced & 1;
    g_hw.gs->smode2.ffmd = field & 1;
}

void GsVblank(bool start)
{
    if (!start)
    {
        return;
    }
    g_vblanks++;
    g_field ^= 1;
    g_csr |= 8;
    if (!(g_imr & 0x800))
    {
        RaiseIntc(0);
    }
    // The picture: every frame for a window, at the asked times when headless
    bool want = !g_options.headless;
    if (g_options.headless && !g_options.snapshotDir.empty())
    {
        want = true;
    }
    if (want)
    {
        Gs::DisplayImage image;
        g_hw.gs->ReadDisplay(image);
        Gs::DisplayImage buffer;
        if (g_dumpBuffer)
        {
            Gs::Gs& gs = *g_hw.gs;
            int i = gs.pmode.en2 ? 1 : 0;
            buffer.width = static_cast<u32>(gs.dispfb[i].fbw) * 64;
            buffer.height = static_cast<u32>(gs.display[i].dh + 1);
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
        g_picture = std::move(image);
        g_buffer = std::move(buffer);
        g_pictureSerial++;
        g_pictureCv.notify_all();
    }
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
    double nextEvery = g_options.snapshotEvery;
    size_t nextAt = 0;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    u32 texW = 0, texH = 0;
    if (!g_options.headless)
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            Log("SDL_Init: %s (running headless)", SDL_GetError());
            g_options.headless = true;
        }
        else
        {
            window = SDL_CreateWindow("PS2 native runtime", 1280, 960, SDL_WINDOW_RESIZABLE);
            renderer = SDL_CreateRenderer(window, nullptr);
            SDL_SetRenderVSync(renderer, 1);
        }
    }
    u64 shown = 0;
    for (;;)
    {
        Gs::DisplayImage image;
        Gs::DisplayImage buffer;
        {
            std::unique_lock<std::mutex> lock(g_pictureLock);
            g_pictureCv.wait_for(lock, std::chrono::milliseconds(20), [&] { return g_stop || g_pictureSerial != shown; });
            if (g_stop)
            {
                break;
            }
            if (g_pictureSerial != shown)
            {
                shown = g_pictureSerial;
                image = g_picture;
                buffer = g_buffer;
            }
        }
        double t = std::chrono::duration<double>(clock::now() - start).count();
        if (image.width != 0 && !g_options.snapshotDir.empty())
        {
            char name[64];
            if (g_options.snapshotEvery > 0 && t >= nextEvery)
            {
                snprintf(name, sizeof(name), "t%06.1f.png", t);
                SavePicture(image, name);
                snprintf(name, sizeof(name), "t%06.1f-buffer.png", t);
                SavePicture(buffer, name);
                nextEvery += g_options.snapshotEvery;
            }
            while (nextAt < g_options.snapshotAt.size() && t >= g_options.snapshotAt[nextAt])
            {
                snprintf(name, sizeof(name), "at%06.1f.png", g_options.snapshotAt[nextAt]);
                SavePicture(image, name);
                nextAt++;
            }
        }
        if (g_options.runSeconds > 0 && t >= g_options.runSeconds)
        {
            Log("ran %.0f s: stopping", t);
            KernelDumpThreads();
            break;
        }
        if (window != nullptr)
        {
            SDL_Event e;
            while (SDL_PollEvent(&e))
            {
                if (e.type == SDL_EVENT_QUIT)
                {
                    g_stop = true;
                }
            }
            if (g_stop)
            {
                break;
            }
            if (image.width != 0)
            {
                if (texture == nullptr || texW != image.width || texH != image.height)
                {
                    if (texture != nullptr)
                    {
                        SDL_DestroyTexture(texture);
                    }
                    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                                static_cast<int>(image.width), static_cast<int>(image.height));
                    texW = image.width;
                    texH = image.height;
                }
                SDL_UpdateTexture(texture, nullptr, image.pixels.data(), static_cast<int>(image.width * 4));
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);
                int w, h;
                SDL_GetRenderOutputSize(renderer, &w, &h);
                float scale = std::min(w / 640.0f, h / 480.0f);
                SDL_FRect dst = {(w - 640 * scale) / 2, (h - 480 * scale) / 2, 640 * scale, 480 * scale};
                SDL_RenderTexture(renderer, texture, nullptr, &dst);
                SDL_RenderPresent(renderer);
            }
        }
    }
    if (window != nullptr)
    {
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
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
