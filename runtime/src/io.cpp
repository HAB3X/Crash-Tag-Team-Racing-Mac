// The EE's hardware registers (0x10000000-0x1FFFFFFF): the timers, the IPU, GIF and VIF registers and FIFOs, the DMAC, the
// INTC, the SIF's mailbox registers, the GS's privileged registers, and the boot ROM.
#include "runtime.h"

#include "../../hw/ee/gif.h"
#include "../../hw/ee/vif.h"

namespace Ps2
{
namespace
{
struct Timer
{
    u32 mode = 0;
    u32 comp = 0;
    u32 hold = 0;
    u64 base = 0;    // bus cycle the count started from
    u32 start = 0;   // the count written
};
Timer g_timers[4];

u64 TimerDivider(u32 mode)
{
    switch (mode & 3)
    {
    case 0: return 1;    // bus clock
    case 1: return 16;
    case 2: return 256;
    default: return 9372; // hblank (NTSC)
    }
}

u32 TimerCount(int n)
{
    Timer& t = g_timers[n];
    if (!(t.mode & 0x80))
    {
        return t.start;
    }
    return static_cast<u32>((t.start + (BusCycles() - t.base) / TimerDivider(t.mode)) & 0xFFFF);
}

std::string g_sioLine;

u32 Read32Io(u32 a)
{
    if (a < 0x10002000)
    {
        int n = (a >> 11) & 3;
        switch (a & 0x7F0)
        {
        case 0x00: return TimerCount(n);
        case 0x10: return g_timers[n].mode;
        case 0x20: return g_timers[n].comp;
        case 0x30: return g_timers[n].hold;
        }
        return 0;
    }
    if (a < 0x10003000)
    {
        // IPU: idle
        return 0;
    }
    if (a < 0x10003800)
    {
        // GIF_STAT and the rest: idle
        return 0;
    }
    if (a < 0x10004000)
    {
        // VIF0 (0x10003800) and VIF1 (0x10003C00) registers: idle, empty FIFOs
        return 0;
    }
    if (a >= 0x10008000 && a < 0x1000F000)
    {
        return DmacRead(a);
    }
    switch (a)
    {
    case 0x1000F000:
        if (ps2_irq_pending && t_cpu != nullptr)
        {
            ps2_irq_service(t_cpu);
        }
        return g_intcStat;
    case 0x1000F010:
        return g_intcMask;
    case 0x1000F200:
    case 0x1000F210:
    case 0x1000F220:
    case 0x1000F230:
    case 0x1000F240:
    case 0x1000F260:
        return SifGetReg(((a - 0x1000F200) >> 4) + 1);
    case 0x1000F520: // D_ENABLER
        return 0x1201;
    case 0x1000F130:
    case 0x1000F430: // the memory controller's init values: done
        return 0;
    case 0x1000F440:
        return 0;
    default:
        if (a >= 0x1FC00000)
        {
            // The boot ROM: its version string at 0x1FC7FF52 isn't there
            return 0;
        }
        if (Once("ioread" + std::to_string(a)))
        {
            PS2_LOG(LogIo, "read of unknown register %08x", a);
        }
        return 0;
    }
}

void Write32Io(u32 a, u32 v)
{
    if (a < 0x10002000)
    {
        int n = (a >> 11) & 3;
        Timer& t = g_timers[n];
        switch (a & 0x7F0)
        {
        case 0x00:
            t.start = v & 0xFFFF;
            t.base = BusCycles();
            return;
        case 0x10:
            t.start = TimerCount(n);
            t.base = BusCycles();
            t.mode = v & 0x3FF;
            return;
        case 0x20:
            t.comp = v & 0xFFFF;
            return;
        case 0x30:
            t.hold = v & 0xFFFF;
            return;
        }
        return;
    }
    if (a < 0x10004000)
    {
        if (Once("iowrite" + std::to_string(a)))
        {
            PS2_LOG(LogIo, "write %08x to %08x (GIF/VIF/IPU register)", v, a);
        }
        return;
    }
    if (a >= 0x10008000 && a < 0x1000F000)
    {
        DmacWrite(a, v);
        return;
    }
    switch (a)
    {
    case 0x1000F000:
        g_intcStat &= ~v;
        return;
    case 0x1000F010:
        g_intcMask ^= v & 0x7FFF;
        return;
    case 0x1000F180: // the SIO's transmit: kernel prints
        if (v == '\n')
        {
            Log("sio: %s", g_sioLine.c_str());
            g_sioLine.clear();
        }
        else if (v != '\r')
        {
            g_sioLine += static_cast<char>(v);
        }
        return;
    case 0x1000F200:
    case 0x1000F210:
    case 0x1000F220:
    case 0x1000F230:
    case 0x1000F240:
    case 0x1000F260:
        SifSetReg(((a - 0x1000F200) >> 4) + 1, v);
        return;
    case 0x1000F590: // D_ENABLEW
        return;
    default:
        if (Once("iowrite" + std::to_string(a)))
        {
            PS2_LOG(LogIo, "write %08x to unknown register %08x", v, a);
        }
        return;
    }
}

// The FIFOs take quadwords
void WriteFifo(u32 a, u128 v)
{
    u32 words[4];
    std::memcpy(words, &v, 16);
    Hardware& hw = Hw();
    if (a >= 0x10004000 && a < 0x10005000)
    {
        hw.vif0->Process(words, 4);
    }
    else if (a >= 0x10005000 && a < 0x10006000)
    {
        hw.vif1->Process(words, 4);
    }
    else if (a >= 0x10006000 && a < 0x10007000)
    {
        hw.gif->Transfer(2, reinterpret_cast<const u8*>(words), 1);
    }
    else if (Once("fifo" + std::to_string(a)))
    {
        PS2_LOG(LogIo, "write to FIFO %08x", a);
    }
}
}

void IoInit() {}
}

using namespace Ps2;

static bool IsGsPriv(u32 a) { return a >= 0x12000000 && a < 0x12002000; }

extern "C" u8 ps2_io_read8(u32 a)
{
    return static_cast<u8>(ps2_io_read32(a & ~3u) >> ((a & 3) * 8));
}
extern "C" u16 ps2_io_read16(u32 a)
{
    return static_cast<u16>(ps2_io_read32(a & ~3u) >> ((a & 2) * 8));
}
extern "C" u32 ps2_io_read32(u32 a)
{
    a &= 0x1FFFFFFF;
    if (IsGsPriv(a))
    {
        return static_cast<u32>(GsPrivRead(a & ~7u) >> ((a & 4) * 8));
    }
    return Read32Io(a);
}
extern "C" u64 ps2_io_read64(u32 a)
{
    a &= 0x1FFFFFFF;
    if (IsGsPriv(a))
    {
        return GsPrivRead(a);
    }
    return Read32Io(a) | (static_cast<u64>(Read32Io(a + 4)) << 32);
}
extern "C" u128 ps2_io_read128(u32 a)
{
    return ps2_io_read64(a) | (static_cast<u128>(ps2_io_read64(a + 8)) << 64);
}
extern "C" void ps2_io_write8(u32 a, u8 v)
{
    a &= 0x1FFFFFFF;
    if (a == 0x1000F180)
    {
        Write32Io(a, v);
        return;
    }
    u32 shift = (a & 3) * 8;
    u32 old = ps2_io_read32(a & ~3u);
    ps2_io_write32(a & ~3u, (old & ~(0xFFu << shift)) | (static_cast<u32>(v) << shift));
}
extern "C" void ps2_io_write16(u32 a, u16 v)
{
    a &= 0x1FFFFFFF;
    u32 shift = (a & 2) * 8;
    u32 old = ps2_io_read32(a & ~3u);
    ps2_io_write32(a & ~3u, (old & ~(0xFFFFu << shift)) | (static_cast<u32>(v) << shift));
}
extern "C" void ps2_io_write32(u32 a, u32 v)
{
    a &= 0x1FFFFFFF;
    if (IsGsPriv(a))
    {
        u64 old = GsPrivRead(a & ~7u);
        u32 shift = (a & 4) * 8;
        GsPrivWrite(a & ~7u, (old & ~(0xFFFFFFFFull << shift)) | (static_cast<u64>(v) << shift));
        return;
    }
    Write32Io(a, v);
}
extern "C" void ps2_io_write64(u32 a, u64 v)
{
    a &= 0x1FFFFFFF;
    if (IsGsPriv(a))
    {
        GsPrivWrite(a, v);
        return;
    }
    if (a >= 0x10004000 && a < 0x10008000)
    {
        Fatal("64 bit FIFO write at %08x", a);
    }
    Write32Io(a, static_cast<u32>(v));
    Write32Io(a + 4, static_cast<u32>(v >> 32));
}
extern "C" void ps2_io_write128(u32 a, u128 v)
{
    a &= 0x1FFFFFFF;
    if (a >= 0x10004000 && a < 0x10008000)
    {
        WriteFifo(a, v);
        return;
    }
    ps2_io_write64(a, static_cast<u64>(v));
    ps2_io_write64(a + 8, static_cast<u64>(v >> 64));
}
