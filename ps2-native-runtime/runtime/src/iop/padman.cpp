// The pad server (PADMAN on the IOP), high level, for the libpad of SDK 2.x: one RPC (0x80000100, function 1) whose 128 byte
// block starts with a command number, and a 256 byte area per open port in the EE's memory (two 128 byte frames) that PADMAN
// rewrites at every vertical blank, the older frame each time with its counter one past the newer's, so libpad reads the
// newest. Port 0 has a DualShock 2; its buttons come from PadSetInput (the presenter's keyboard and gamepads, or the scripted
// presses).
//
// A frame: the pad's answer (32 bytes: status, mode ID, buttons active low, the sticks, the pressures), the actuators (48),
// the mode table (80), the frame counter (88), the answer's length (96), the mode bytes (100-111), the state (112), the
// request's state (113), the pad's task (114), and the pressure buttons' mask (121-124).
#include "iop.h"

#include <algorithm>
#include <deque>
#include <mutex>

namespace Ps2
{
namespace
{
enum Command
{
    PortOpen = 0x01,
    SetMainMode = 0x06,
    SetActAlign = 0x08,
    PressMode = 0x0A,
    SetActDirect = 0x0B,
    GetPortMax = 0x0C,
    GetSlotMax = 0x0D,
    PortClose = 0x0E,
    End = 0x0F,
    Init = 0x10,
    ModuleVersion = 0x12,
};

constexpr u8 StateDisconnected = 0;
constexpr u8 StateStable = 6;
constexpr u8 RequestComplete = 0;
constexpr u8 RequestBusy = 2;

struct Port
{
    bool open = false;
    u32 area = 0;      // the EE's 256 bytes
    u32 iopArea = 0;   // where libpad sends the actuators' values
    u32 frame = 0;     // the counter of the last frame written
    bool analog = false;
    bool pressure = false;
    u8 lock = 0;
    int busy = 0;      // vertical blanks the last mode request still takes
};

Port g_ports[2];

bool Connected(u32 port) { return port == 0; }

void WriteFrame(u32 port)
{
    Port& p = g_ports[port];
    u8* area = Host(p.area & 0x1FFFFFF);
    u8* frames[2] = {area, area + 128};
    u32 counts[2];
    std::memcpy(&counts[0], frames[0] + 0x58, 4);
    std::memcpy(&counts[1], frames[1] + 0x58, 4);
    // The older frame is rewritten (libpad reads the one with the larger counter)
    u8* f = counts[0] <= counts[1] ? frames[0] : frames[1];
    u32 next = std::max(counts[0], counts[1]) + 1;
    u8 out[128] = {};
    if (Connected(port))
    {
        u8* data = out;
        data[0] = 0;
        data[1] = p.pressure ? 0x79 : p.analog ? 0x73 : 0x41;
        PadDualShock2Report(port, data + 2);
        // Two actuators (the small motor on/off, the big one 0-255), one combination of both
        const u8 act[8] = {1, 1, 0, 1, 1, 1, 1, 0x14};
        std::memcpy(out + 0x30, act, 8);
        const u8 comb[4] = {2, 0, 1, 0};
        std::memcpy(out + 0x40, comb, 4);
        const u16 modes[2] = {4, 7}; // digital, DualShock
        std::memcpy(out + 0x50, modes, 4);
        u32 length = 32;
        std::memcpy(out + 0x60, &length, 4);
        out[0x64] = 2;                 // the modes can be changed
        out[0x65] = data[1];           // the current mode's ID
        out[0x66] = 3;                 // a DualShock 2
        out[0x67] = 1;                 // the buttons are read
        out[0x68] = 2;                 // two modes
        out[0x69] = p.analog ? 1 : 0;  // the current mode's place in the table
        out[0x6A] = 2;
        out[0x6B] = 1;
        out[0x6E] = p.lock;
        out[0x6F] = 6;
        out[0x70] = StateStable;
        out[0x71] = p.busy > 0 ? RequestBusy : RequestComplete;
        out[0x72] = 1;
        out[0x79] = 0xFF;              // every button reports a pressure
        out[0x7A] = 0xFF;
        out[0x7B] = 0x03;
        out[0x7C] = 0x00;
        if (p.busy > 0)
        {
            p.busy--;
        }
    }
    else
    {
        out[0x70] = StateDisconnected;
        out[0x71] = RequestComplete;
    }
    std::memcpy(out + 0x58, &next, 4);
    // The counter last, so a frame is never newer before it's whole
    std::memcpy(f, out, 0x58);
    std::memcpy(f + 0x5C, out + 0x5C, 128 - 0x5C);
    std::memcpy(f + 0x58, &next, 4);
    p.frame = next;
}

void Reply32(RpcCall& c, u32 off, u32 value) { c.Out32(off, value); }

void Serve(RpcCall& c)
{
    // The answer is the block itself with the results written in
    c.out.assign(c.in, c.in + std::min<u32>(c.inSize, 128));
    c.out.resize(std::max<u32>(c.recvSize, 128));
    u32 command = c.In32(0);
    u32 port = c.In32(4) & 1;
    switch (command)
    {
    case Init:
    case End:
        Reply32(c, 12, 1);
        break;
    case ModuleVersion:
        Reply32(c, 12, 0x402);
        break;
    case GetPortMax:
        Reply32(c, 12, 2);
        break;
    case GetSlotMax:
        Reply32(c, 12, 1);
        break;
    case PortOpen:
    {
        Port& p = g_ports[port];
        p = Port();
        p.open = true;
        p.area = c.In32(16);
        p.iopArea = IopAlloc(64);
        std::memset(Host(p.area & 0x1FFFFFF), 0, 256);
        WriteFrame(port);
        WriteFrame(port);
        Log("pad: port %u opened (frames at %08x)%s", port, p.area, Connected(port) ? "" : ", nothing plugged in");
        Reply32(c, 12, 1);
        Reply32(c, 20, p.iopArea);
        break;
    }
    case PortClose:
        g_ports[port].open = false;
        Reply32(c, 12, 1);
        break;
    case SetMainMode: // mode 12 (0 digital, 1 analog), lock 16 (3 locked)
    {
        Port& p = g_ports[port];
        p.analog = c.In32(12) == 1;
        if (!p.analog)
        {
            p.pressure = false;
        }
        p.lock = static_cast<u8>(c.In32(16));
        p.busy = 2;
        Reply32(c, 20, 1);
        break;
    }
    case PressMode: // the buttons' mask at 12: 0xFFF enters the pressure mode, 0 leaves it
    {
        Port& p = g_ports[port];
        p.pressure = p.analog && c.In32(12) != 0;
        p.busy = 2;
        Reply32(c, 16, 1);
        break;
    }
    case SetActAlign:
        g_ports[port].busy = 2;
        Reply32(c, 20, 1);
        break;
    case SetActDirect:
        Reply32(c, 28, 1);
        break;
    default:
        if (Once("padman " + std::to_string(command)))
        {
            Log("pad: command %#x not done: answered 0", command);
        }
        break;
    }
}
}

void PadMan(RpcCall& c)
{
    if (c.sid == 0x80000100 && c.fno == 1)
    {
        Serve(c);
        return;
    }
    if (Once("padman sid " + std::to_string(c.sid) + " " + std::to_string(c.fno)))
    {
        Log("pad: RPC %08x function %u not done: answered with zeros", c.sid, c.fno);
    }
}

void PadVblank()
{
    for (u32 port = 0; port < 2; port++)
    {
        if (g_ports[port].open)
        {
            WriteFrame(port);
        }
    }
}

// A press shorter than the game's frame can fall between its reads (the newest frame PADMAN writes is what the game reads, once a
// frame of its own). So each press (a button, or a stick pushed off centre: its eight directions count as buttons 16-23) is
// shown to the game for whole game frames: until it has flipped twice and at least 3 vertical blanks passed (at most 30, for a
// game that isn't flipping: loading). Presses are shown in the order they came: one that comes while the last of its button is
// still shown waits, and the button is shown released for a game frame between them; ps2_set_tap_spacing makes every press
// wait for the one before it too (in game frames, or 3 vertical blanks to each when the game isn't flipping). A key held longer is shown as long as it's held
constexpr int PadBits = 24;
constexpr size_t PadQueueMax = 32;
const char* const PadBitNames[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                     "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};

struct Slot
{
    enum Phase : u8 { Idle, Shown, Gap } phase = Idle;
    u64 flipEnd = 0, vblankEnd = 0, vblankCap = 0;
};

struct Latch
{
    std::mutex lock;
    u32 down = 0;        // what the presenter said was down at its last update
    u32 delivered = 0;   // what the last frame showed
    u8 axes[4] = {0x80, 0x80, 0x80, 0x80}; // the sticks now (right x, right y, left x, left y)
    u8 lastAxes[4] = {0x80, 0x80, 0x80, 0x80}; // each one's last value off centre
    u64 vblank = 0;
    u64 spacing = 0;   // game frames between one press's start and the next's (ps2_set_tap_spacing)
    u64 nextFlip = 0, nextVblank = 0; // when the next press may start: at that flip, or that vertical blank if the game isn't flipping
    Slot slots[PadBits];
    std::deque<int> waiting; // the presses not yet shown, by button, in order
    u64 seen = 0, shown = 0; // presses taken from the presenter, and presses shown in frames (--log input)
} g_latch[2];

void Begin(Slot& s, Slot::Phase phase, u64 flips, u64 vblank, u64 minFlips, u64 minBlanks, u64 cap)
{
    s.phase = phase;
    s.flipEnd = flips + minFlips;
    s.vblankEnd = vblank + minBlanks;
    s.vblankCap = vblank + cap;
}

bool Over(const Slot& s, u64 flips, u64 vblank) { return vblank >= s.vblankCap || (flips >= s.flipEnd && vblank >= s.vblankEnd); }

// The waiting presses that can start now, in order: the first that can't (its button is still shown, or the spacing hasn't
// passed) holds the rest back
void StartWaiting(Latch& l, u64 flips)
{
    while (!l.waiting.empty() && (flips >= l.nextFlip || l.vblank >= l.nextVblank) &&
           l.slots[l.waiting.front()].phase == Slot::Idle)
    {
        Begin(l.slots[l.waiting.front()], Slot::Shown, flips, l.vblank, 2, 3, 30);
        l.waiting.pop_front();
        l.nextFlip = flips + l.spacing;
        l.nextVblank = l.vblank + 3 * l.spacing;
    }
}

const char* BitName(int bit)
{
    static const char* const sticks[8] = {"right x-", "right x+", "right y-", "right y+",
                                          "left x-",  "left x+",  "left y-",  "left y+"};
    return bit < 16 ? PadBitNames[bit] : sticks[bit - 16];
}

void PadDualShock2Report(u32 port, u8 report[18])
{
    Latch& l = g_latch[port];
    std::lock_guard<std::mutex> lock(l.lock);
    u64 vblank = ++l.vblank, flips = GameFrames();
    for (int b = 0; b < PadBits; b++)
    {
        Slot& s = l.slots[b];
        if (s.phase != Slot::Idle && Over(s, flips, vblank))
        {
            bool again = std::find(l.waiting.begin(), l.waiting.end(), b) != l.waiting.end();
            if (s.phase == Slot::Shown && again)
            {
                Begin(s, Slot::Gap, flips, vblank, 1, 2, 20);
            }
            else
            {
                s.phase = Slot::Idle;
            }
        }
    }
    StartWaiting(l, flips);
    u32 shown = 0, waiting = 0;
    for (int b : l.waiting)
    {
        waiting |= 1u << b;
    }
    for (int b = 0; b < PadBits; b++)
    {
        Slot& s = l.slots[b];
        // A key held on after its press was shown stays shown (a press still waiting isn't held yet)
        if (s.phase == Slot::Shown || (s.phase == Slot::Idle && ((l.down & ~waiting) >> b) & 1))
        {
            shown |= 1u << b;
        }
    }
    u32 rising = shown & ~l.delivered;
    for (int b = 0; b < PadBits; b++)
    {
        if ((rising >> b) & 1)
        {
            l.shown++;
            PS2_LOG(LogInput, "input: %s shown in frame %llu (press %llu seen, %llu shown)", BitName(b),
                    static_cast<unsigned long long>(vblank), static_cast<unsigned long long>(l.seen),
                    static_cast<unsigned long long>(l.shown));
        }
    }
    l.delivered = shown;
    u8 axes[4];
    for (int i = 0; i < 4; i++)
    {
        axes[i] = ((shown >> (16 + 2 * i)) & 3) == 0 ? 0x80 : l.axes[i] != 0x80 ? l.axes[i] : l.lastAxes[i];
    }
    u32 sticks;
    std::memcpy(&sticks, axes, 4);
    u32 pressed = shown & 0xFFFF;
    u16 buttons = static_cast<u16>(~pressed);
    std::memcpy(report, &buttons, 2);
    std::memcpy(report + 2, &sticks, 4);
    // The pressures: right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2
    static const int bits[12] = {5, 7, 4, 6, 12, 13, 14, 15, 10, 11, 8, 9};
    for (int i = 0; i < 12; i++)
    {
        report[6 + i] = (pressed >> bits[i]) & 1 ? 0xFF : 0;
    }
}

void PadSetInput(u32 port, u32 pressed, const u8 sticks[4])
{
    if (port >= 2)
    {
        return;
    }
    Latch& l = g_latch[port];
    std::lock_guard<std::mutex> lock(l.lock);
    u32 now = pressed & 0xFFFF;
    for (int i = 0; i < 4; i++)
    {
        l.axes[i] = sticks[i];
        if (sticks[i] != 0x80)
        {
            l.lastAxes[i] = sticks[i];
            now |= 1u << (16 + 2 * i + (sticks[i] > 0x80 ? 1 : 0));
        }
    }
    u32 rising = now & ~l.down;
    l.down = now;
    for (int b = 0; b < PadBits; b++)
    {
        if (!((rising >> b) & 1))
        {
            continue;
        }
        l.seen++;
        if (l.waiting.size() >= PadQueueMax)
        {
            Log("input: %zu presses waiting: %s's dropped", l.waiting.size(), BitName(b));
            continue;
        }
        l.waiting.push_back(b);
    }
    StartWaiting(l, GameFrames());
}

void PadSetTapSpacing(u32 vblanks)
{
    std::lock_guard<std::mutex> lock(g_latch[0].lock);
    g_latch[0].spacing = vblanks;
}

u64 PadPressesSeen() { return g_latch[0].seen; }
u64 PadPressesShown() { return g_latch[0].shown; }
}

extern "C" void ps2_set_tap_spacing(int vblanks) { Ps2::PadSetTapSpacing(static_cast<u32>(std::max(vblanks, 0))); }
