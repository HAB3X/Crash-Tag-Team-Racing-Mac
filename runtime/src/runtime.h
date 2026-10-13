// The runtime's own parts, shared between its files
#pragma once

#include "ps2/recomp.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <pthread.h>
#endif

union SDL_Event;
struct SDL_Window;

namespace Gs
{
class Gs;
}
namespace Ee
{
class Vu;
class Vif;
class Gif;
}

namespace Ps2
{
// The calling thread kept on the performance cores (macOS puts busy threads of the default class on the efficiency cores too)
inline void ThreadOnPerformanceCores()
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

// --- log.cpp
enum LogChannel
{
    LogKernel = 1 << 0,
    LogSyscall = 1 << 1,
    LogIo = 1 << 2,
    LogDma = 1 << 3,
    LogSif = 1 << 4,
    LogIop = 1 << 5,
    LogGs = 1 << 6,
    LogCall = 1 << 7,
    LogThread = 1 << 8,
    LogIrq = 1 << 9,
};
extern u32 g_logMask;
void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));
#define PS2_LOG(channel, ...)                                                                                                  \
    do                                                                                                                         \
    {                                                                                                                          \
        if (::Ps2::g_logMask & (channel))                                                                                      \
            ::Ps2::Log(__VA_ARGS__);                                                                                           \
    } while (0)
[[noreturn]] void Fatal(const char* format, ...) __attribute__((format(printf, 1, 2)));
// The guest's call stack (from the host's) to the log
void Backtrace();
// Logs a message once per key
bool Once(const std::string& key);

// --- memory.cpp
constexpr u32 RamSize = 32 * 1024 * 1024;
constexpr u32 ScratchpadBase = 0x70000000;
constexpr u32 ScratchpadSize = 16 * 1024;
void MemoryInit();
inline u8* Ram() { return ps2_mem; }
inline u8* Host(u32 address) { return ps2_mem + address; }
// A DMA address: main memory (bits 0-24), or the scratchpad when bit 31 is set
u8* DmaHost(u32 address);
inline u32 Read32(u32 address)
{
    u32 v;
    std::memcpy(&v, ps2_mem + (address & 0x1FFFFFFF), 4);
    return v;
}
inline void Write32(u32 address, u32 value) { std::memcpy(ps2_mem + (address & 0x1FFFFFFF), &value, 4); }
std::string ReadString(u32 address, u32 max = 1024);

// --- cpu.cpp
const Ps2FunctionEntry* FindFunction(u32 address);
// Calls guest code at an address with a context (a thread's or a handler's)
void Call(Ps2Cpu* c, u32 address);
// The current host thread's guest context (for diagnostics)
extern thread_local Ps2Cpu* t_cpu;

// --- kernel.cpp
void KernelInit(u32 entry, u32 gp, const std::vector<std::string>& args);
// Runs the main thread from the executable's entry (doesn't return until the game exits)
void KernelRunMain();
// An interrupt raised by the hardware: an INTC cause (0-14) or a DMAC channel (0-9)
void RaiseIntc(u32 cause);
void RaiseDmac(u32 channel);
// A semaphore signalled from outside the guest (the IOP's completions); the scheduler picks it up at the next kernel call
void KernelSignalSema(int id);
// Every thread's state to the log
void KernelDumpThreads();
// The INTC's and DMAC's registers
extern u32 g_intcStat;
extern u32 g_intcMask;
extern u32 g_dmacStat;
// Wall-clock time in EE bus cycles (147.456 MHz)
u64 BusCycles();
// Whether the guest is in an interrupt handler
bool InInterrupt();
// COP0 Status's EIE: EI and DI
bool InterruptsEnabled();

// --- io.cpp
void IoInit();

// --- dmac.cpp
void DmacInit();
u32 DmacRead(u32 address);
void DmacWrite(u32 address, u32 value);
// The channels' data from the SIF (the IOP side): SIF0 (IOP to EE) moved by the IOP's HLE
bool Sif0Receive(const u8* data, u32 bytes);

// --- gs.cpp
struct Hardware
{
    Gs::Gs* gs;
    Ee::Gif* gif;
    Ee::Vu* vu0;
    Ee::Vu* vu1;
    Ee::Vif* vif0;
    Ee::Vif* vif1;
};
Hardware& Hw();
void GsInit();
u64 GsPrivRead(u32 address);
// CSR as the guest reads it (without servicing interrupts first)
u64 GsCsr();
void GsPrivWrite(u32 address, u64 value);
// The vertical blank: the GS's CSR, and the picture handed to the presenter
void GsVblank(bool start);
// SetGsCrt
void GsSetCrt(u32 interlaced, u32 mode, u32 field);
// The presenter (on the host's main thread): waits for pictures and shows or saves them; returns when the game ends
void PresenterRun();
void PresenterStop();
struct Options
{
    std::string disc;
    bool headless = false;
    std::string snapshotDir;
    double snapshotEvery = 0;   // seconds of game time (vblanks / 59.94) between saved pictures (0: none); --snapshot-at too
    std::vector<double> snapshotAt;
    double runSeconds = 0;      // stop after this long (0: never)
    u32 logMask = 0;
    // Guest words logged every watchEvery seconds: (address, count)
    std::vector<std::pair<u32, u32>> watch;
    double watchEvery = 1;
    // The game's frames and the vblanks of every second to the log
    bool fps = false;
    // The memory cards' folders (port0, port1 in it), and the settings' (bindings.txt)
    std::string memcardDir;
    std::string settingsDir;
    // Buttons pressed at given times (seconds of game time): port 0's PS2 button bits, held for a while
    struct Press
    {
        double at;
        double hold;
        u32 buttons;
    };
    std::vector<Press> presses;
    // Text typed at given times (seconds of game time), as the keyboard types it while the game takes text
    std::vector<std::pair<double, std::string>> typed;
    // The sound: everything the SPU2 makes written to a WAV file; no audio device (the sound is made all the same)
    std::string audioDump;
    bool mute = false;
};
extern Options g_options;

// --- render.cpp: the GS's side on a thread of its own
void RenderStart();
// Work on the GS's side in its place among the queued data (at once when there's no render thread)
void RenderCall(std::function<void()> work);
// What's queued handed over; everything queued done
void RenderFlush();
void RenderSync();
// A picture made on the GS's side; returns once the render thread is at most two pictures behind
void RenderFrame(std::function<void()> show);

// --- input.cpp: port 0's controller from the keyboard, a gamepad and --press
u32 ButtonsFromNames(const std::string& names);
void InputInit();
// Where the presenter shows the picture (render coordinates, the same as the mouse's events after conversion)
void InputPicture(float x, float y, float w, float h);
void InputEvent(const ::SDL_Event& e);
void InputUpdate(double seconds, ::SDL_Window* window);

// --- disc.cpp
bool DiscOpen(const std::string& path);
// A file on the disc by its ISO 9660 path ("\\SYS\\LIBSD.IRX;1" or "/SYS/LIBSD.IRX"): its first sector and size
bool DiscFind(const std::string& path, u32& lsn, u32& size);
bool DiscReadSectors(u32 lsn, u32 count, u8* out);
bool DiscReadFile(const std::string& path, std::vector<u8>& out);

// --- iop/sif.cpp
void IopInit();
// SIF DMA from the EE (sceSifSetDma's transfers): data written to IOP memory
void SifSetDma(u32 src, u32 dest, u32 size, u32 attr);
u32 SifGetReg(u32 reg);
void SifSetReg(u32 reg, u32 value);
// Work the IOP has queued for the EE (replies), done between guest instructions
void IopUpdate();
// The EE took the IOP's last reply (its SifSetDChain, or the end of its SIF0 interrupt)
void SifReplyTaken();
u8* IopMem();

// --- sound/output.cpp: the SPU2's output (ps2/sound.h)
void SoundInit();
// The WAV file finished
void SoundStop();

// --- iop/padman.cpp
// The pads' frames, rewritten at each vertical blank (the IOP's PADMAN does it then)
void PadVblank();
// A port's buttons (PS2 bits, 1 = pressed: select, L3, R3, start, up, right, down, left, L2, R2, L1, R1, triangle, circle,
// cross, square) and sticks (right x, right y, left x, left y; 0x80 is the centre)
void PadSetInput(u32 port, u32 pressed, const u8 sticks[4]);
constexpr u32 IopRamSize = 2 * 1024 * 1024;
}
