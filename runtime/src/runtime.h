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

#include "../../hw/hardware.h"

namespace Gs
{
struct DisplayImage;
}

union SDL_Event;
struct SDL_Window;
struct SDL_FRect;
struct SDL_Gamepad;

namespace Ps2
{
namespace Ui
{
enum class PadControl : int;
}

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
// COP0 Status as the game reads it: kernel mode, IE, INT0 and INT1 on, EIE as EI and DI set it, EXL and ERL as the game set them
u32 KernelStatus();
// Status written (MTC0): its EIE as EI or DI would set it, its EXL and ERL kept (interrupts wait while either is on); the rest
// is the kernel's
void KernelSetStatus(Ps2Cpu* c, u32 status);
// The PC menus' pause: the game stops at its next poll point (and the timer makes no vblanks or alarms; its time stands still)
// until it's let go
void KernelPause(bool paused);
// Work done on the game's side between its instructions (by the thread holding the processor, or the timer thread when none
// does), paused or not: a setting of the game's changed, an action of the game's, the GS's settings (RenderCall) changed
void KernelOnGameSide(std::function<void()> work);

// --- io.cpp
void IoInit();
// The timers' events since the last call (COMP reached, overflow): their MODE flags set, and where MODE asks, the INTC's timer
// cause (9-12) raised. Returns the bus cycle of the next event (~0: none), for the kernel's timer thread to wake at. Several
// events of one timer between two calls raise one interrupt
u64 IoTimerEvents();

// --- dmac.cpp
void DmacInit();
// A VIF's stall cancelled (VIFn_FBRST's STC): what it held, then its DMA transfer, go on
void DmacVifUnstall(int vif);
u32 DmacRead(u32 address);
void DmacWrite(u32 address, u32 value);
// The channels' data from the SIF (the IOP side): SIF0 (IOP to EE) moved by the IOP's HLE
bool Sif0Receive(const u8* data, u32 bytes);

// --- gs.cpp
// The hardware layer's set (hw/hardware.h): the hardware renderer finds the GS through NativeGraphics::GetHardware
using Hardware = NativeGraphics::Hardware;
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
    // The memory cards' folders (port0, port1 in it), and the settings' (bindings.txt, graphics.txt, options.txt; --settings)
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
    // Keys and mouse buttons held at given times (seconds of game time), by bindings.txt's names, as if the player held them
    struct KeyPress
    {
        double at;
        double hold;
        std::vector<std::string> keys;
    };
    std::vector<KeyPress> keys;
    // Text typed at given times (seconds of game time), as the keyboard types it while the game takes text
    std::vector<std::pair<double, std::string>> typed;
    // Mouse clicks at given times (seconds of game time), at places in the picture (0-1 across and down)
    struct Click
    {
        double at;
        float x;
        float y;
    };
    std::vector<Click> clicks;
    // What draws the GS's primitives: 1 the GPU (the hardware layer's OpenGL renderer), 0 the software GS; the GPU's internal
    // resolution (1-8 times the PS2's) and its textures' upscale (0, 2 or 4 times). -1: $PS2_GS, $PS2_GS_SCALE,
    // $PS2_TEXTURE_UPSCALE, or the defaults (the GPU at 3x, no upscale)
    int gs = -1;
    int gsScale = -1;
    int textureUpscale = -1;
    // The sound: everything the SPU2 makes written to a WAV file; no audio device (the sound is made all the same)
    std::string audioDump;
    bool mute = false;
    // The PC menus driven by a script (--ui): from a time of the game's, keys pressed one after another and the window's
    // pictures with the menus saved in the snapshots' folder (ui/menus.cpp)
    std::vector<std::pair<double, std::string>> ui;
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

// --- display.cpp: the window (OpenGL) and the picture's way from the GS to it
// The window and the GPU's contexts made (the main thread, before RenderStart): the window, or null when headless
SDL_Window* DisplayOpen();
// The render thread's start: its context made current and the hardware renderer started (unless the software GS is asked for)
void DisplayRenderThreadStart();
// No render thread: the software GS draws
void DisplayNoRenderThread();
// On the GS's side: whether the GPU draws; the shown picture copied for the window (its slot, -1 when the shown buffer isn't the
// GPU's), the last copy dropped, and the picture's pixels read (at the internal resolution when the GPU has it)
bool DisplayOnGpu();
int DisplayCopyShown();
void DisplayDropShown();
bool DisplayReadShown(Gs::DisplayImage& image);
// On the main thread: the newest copy (or the pixels given) drawn over the window with the PC menus' overlay over it, or (redraw)
// the last one drawn again; where the picture is, in the window's points
void DisplayPresent(const Gs::DisplayImage* image, bool redraw, ::SDL_FRect* pictureRect);
// The window's picture with the PC menus over it saved as a PNG (headless: drawn in the hidden window's context)
bool DisplayShot(const std::string& path, const Gs::DisplayImage* image);
void DisplayClose();
// The display settings of the PC menus' Display tab (graphics.txt): set applies (the GS's on its side) and saves them. The
// window's size is a choice of DisplayWindowSizeCount (-1: a size of its own), the texture upscale 0-2 (off, 2x, 4x)
enum class DisplaySetting
{
    Fullscreen,
    WindowSize,
    Scale,
    TextureUpscale,
    Filtering,
    AntiAliasing,
    Crt,
    VSync,
    OnGpu,
};
int DisplayGet(DisplaySetting setting);
void DisplaySet(DisplaySetting setting, int value);
std::string DisplayWindowSizeText(int choice);
int DisplayWindowSizeCount();
// The window's size in points as it is, and put back (the menus' countdown going back)
void DisplayWindowPoints(int* width, int* height);
void DisplaySetWindowPoints(int width, int height);

// --- input.cpp: port 0's controller from the keyboard, a gamepad and --press
u32 ButtonsFromNames(const std::string& names);
// The window's title: the game's (its title less a bracketed edition)
std::string WindowTitle();
void InputInit();
// Where the presenter shows the picture (render coordinates, the same as the mouse's events after conversion)
void InputPicture(float x, float y, float w, float h);
void InputEvent(const ::SDL_Event& e);
void InputUpdate(double seconds, ::SDL_Window* window);
// For the PC menus: what's held now counted only once it's let go; the device used last (for the prompts); the gamepad
void InputIgnoreHeld();
void InputNoteDevice(const ::SDL_Event& e);
bool InputLastKeyboard();
::SDL_Gamepad* InputGamepad();
// The actions of bindings.txt (the Controls table's rows): a label, the gamepad's control, the keys in their slots (-1 none)
int InputActionCount();
std::string InputActionLabel(int action);
Ui::PadControl InputActionPad(int action);
// The heading an action is under: its mode's name (a game's own controls), or "" (the runtime's)
std::string InputActionGroup(int action);
int InputActionKey(int action, int slot);
std::string InputKeyName(int key);
// A mouse button as a key (-1: none bindable)
int InputMouseKey(int button);
// A key put in an action's slot, taken from where it was (that slot given the old key: the action it swapped with, or -1);
// a slot cleared; the defaults; each saved to bindings.txt
int InputBindKey(int action, int slot, int key);
void InputClearKey(int action, int slot);
void InputResetKeys();
bool InputIsMenuKey(int key);
// The game's prompt (ps2_show_prompt; empty: none)
std::string InputPrompt();

// --- disc.cpp
bool DiscOpen(const std::string& path);
// Whether the disc is a DVD (UDF's volume recognition on it, as a PlayStation 2 DVD has), not a CD
bool DiscIsDvd();
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
// The modules' commands to the EE that waited for the SIF's lock, sent (the kernel's timer thread, every vblank)
void IopUpdate();
// The EE took the IOP's last reply (its SifSetDChain, or the end of its SIF0 interrupt)
void SifReplyTaken();
u8* IopMem();

// --- sound/output.cpp: the SPU2's output (ps2/sound.h)
void SoundInit();
// The WAV file finished
void SoundStop();
// The PC menus' pause: nothing made meanwhile (the driver's timer stands still with the SPU2); the output's gain (0-1: the
// master volume, 0 when muted; the WAV file keeps what's made)
void SoundPause(bool paused);
void SoundSetGain(float gain);

// --- sound/libsd.cpp
// The game's LIBSD.IRX loaded (its file, or its image in the IOP's memory): its tables of the reverb's presets are kept for
// SdSetEffectAttr
void SdModuleLoaded(const u8* elf, size_t size);

// --- iop/padman.cpp
// The pads' frames, rewritten at each vertical blank (the IOP's PADMAN does it then)
void PadVblank();
// A port's buttons (PS2 bits, 1 = pressed: select, L3, R3, start, up, right, down, left, L2, R2, L1, R1, triangle, circle,
// cross, square) and sticks (right x, right y, left x, left y; 0x80 is the centre)
void PadSetInput(u32 port, u32 pressed, const u8 sticks[4]);
constexpr u32 IopRamSize = 2 * 1024 * 1024;
}
