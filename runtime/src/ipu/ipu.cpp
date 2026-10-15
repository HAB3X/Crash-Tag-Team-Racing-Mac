// The IPU: the EE's MPEG-2 macroblock decoder. Its commands (IPU_CMD), its control and status (IPU_CTRL), the bitstream's
// position (IPU_BP), the next 32 bits (IPU_TOP), and its input and output FIFOs, filled by toIPU's DMA (channel 4) or the EE
// and drained by fromIPU's (channel 3) or the EE.
//
// A command runs as soon as it's written, as far as its data lets it, in units (a macroblock, a VLC, a matrix): a unit that
// runs out of input is taken back and run again when more comes (bitstream.h); a unit whose output would overfill the output
// FIFO waits for fromIPU. So the game sees BUSY until the command could have ended on the console, and the FIFOs' counts and
// IPU_BP are the console's between commands (Sony's libipu stops and restarts its DMA from them). DESIGN.md has how the games
// seen use it
#include "../runtime.h"

#include "bitstream.h"
#include "colour.h"
#include "mpeg.h"
#include "vlc.h"

namespace Ps2
{
namespace
{
using namespace Ipu;

constexpr u32 IntcIpu = 8;
// The console's FIFOs: 8 quadwords each way, and 2 in the decoder's own buffer
constexpr u32 FifoQw = 8;
constexpr u32 InternalQw = 2;

enum Command : u32
{
    Bclr = 0,
    Idec = 1,
    Bdec = 2,
    Vdec = 3,
    Fdec = 4,
    Setiq = 5,
    Setvq = 6,
    Csc = 7,
    Pack = 8,
    Setth = 9,
};

u32 PullFromDma(u8* out, u32 max) { return DmacToIpu(out, max); }

// What a unit changes besides its output, put back with the position when the unit is taken back
struct UnitState
{
    s32 dc[3] = {128, 128, 128};
    int idecScale = 0;
    bool ecd = false;
    bool scd = false;
    u32 cbp = 0;
    u32 cmdData = 0;
};

struct Unit
{
    Bitstream bits{&PullFromDma};
    UnitState st;
    // IPU_CTRL's settings as written (IDP, AS, IVF, QST, MP1, PCT: bits 16-26)
    u32 settings = 0;
    bool busy = false;
    // The command's last unit is done; it ends when its output fits the FIFO
    bool ending = false;
    u32 command = 0;
    u32 progress = 0; // macroblocks done (IDEC, CSC, PACK), or whether the first unit ran
    // IPU_CMD's data: FDEC's and VDEC's results; after other commands it shows the next 32 bits
    bool dataResult = false;
    Matrices matrices = {};
    u16 clut[16] = {};
    Thresholds thresholds;
    // The output: a ring of quadwords, at most the FIFO's 8 and one unit's 64
    static constexpr u32 OutQw = 128;
    alignas(16) u8 out[OutQw * 16];
    u32 outHead = 0;
    u32 outCount = 0;
    // A unit's output, queued when the unit is done
    alignas(16) u8 unitOut[64 * 16];
    u32 unitOutQw = 0;
    bool running = false;
};
Unit g_ipu;

DecodeSettings Settings()
{
    u32 c = g_ipu.settings;
    DecodeSettings s;
    s.intraDcPrecision = (c >> 16) & 3;
    s.alternateScan = (c >> 20) & 1;
    s.intraVlcTableOne = (c >> 21) & 1;
    s.nonLinearQuantiser = (c >> 22) & 1;
    s.mpeg1 = (c >> 23) & 1;
    s.pictureType = (c >> 24) & 7;
    return s;
}

void QueueOutput(const void* data, u32 quadwords)
{
    const u8* p = static_cast<const u8*>(data);
    for (u32 i = 0; i < quadwords; i++)
    {
        u32 at = (g_ipu.outHead + g_ipu.outCount) % Unit::OutQw;
        std::memcpy(g_ipu.out + at * 16, p + i * 16, 16);
        g_ipu.outCount++;
    }
}

void DrainOutput()
{
    while (g_ipu.outCount > 0)
    {
        u32 run = std::min(g_ipu.outCount, Unit::OutQw - g_ipu.outHead);
        u32 taken = DmacFromIpu(g_ipu.out + g_ipu.outHead * 16, run);
        if (taken == 0)
        {
            return;
        }
        g_ipu.outHead = (g_ipu.outHead + taken) % Unit::OutQw;
        g_ipu.outCount -= taken;
    }
}

// After a macroblock: zero bits where the next macroblock would begin mean the slice is over. The position goes to the byte
// boundary and over zero bytes to what follows: a start code (SCD) or anything else (ECD)
void EndOfSliceCheck()
{
    Bitstream& bits = g_ipu.bits;
    if (bits.Peek(8) != 0)
    {
        return;
    }
    bits.AlignToByte();
    for (;;)
    {
        u32 next = bits.Peek(24);
        if (bits.Starved())
        {
            return;
        }
        if (next == 0)
        {
            bits.Skip(8);
            continue;
        }
        (next == 1 ? g_ipu.st.scd : g_ipu.st.ecd) = true;
        return;
    }
}

u32 VdecPictureType()
{
    u32 type = (g_ipu.settings >> 24) & 7;
    if (type < 1 || type > 4)
    {
        // A picture type the IPU doesn't name: decoded as an I picture's (a deliberate fallback, logged)
        if (Once("ipu-pct" + std::to_string(type)))
        {
            Log("IPU: VDEC of macroblock_type with IPU_CTRL's picture type %u: read as an I picture's", type);
        }
        return 1;
    }
    return type;
}

void Rgb32Out(const Raw8& mb, bool sign, bool rgb16, bool dither)
{
    alignas(16) u8 rgb32[1024];
    YcbcrToRgb32(mb, g_ipu.thresholds, sign, rgb32);
    if (rgb16)
    {
        Rgb32ToRgb16(rgb32, dither, reinterpret_cast<u16*>(g_ipu.unitOut));
        g_ipu.unitOutQw = 32;
    }
    else
    {
        std::memcpy(g_ipu.unitOut, rgb32, sizeof(rgb32));
        g_ipu.unitOutQw = 64;
    }
}

// IDEC's macroblock: its macroblock_type (an I picture's), quantiser_scale_code and dct_type, its blocks, colour converted;
// then the next macroblock_address_increment, or the slice's end. Returns whether the command is done
bool IdecMacroblock(u32 cmd)
{
    Bitstream& bits = g_ipu.bits;
    DecodeSettings s = Settings();
    DecodeSettings intra = s;
    intra.pictureType = 1;
    u32 type = DecodeVlc(bits, 1, intra);
    if (type == 0)
    {
        g_ipu.st.ecd = true;
        return true;
    }
    if (type & MbQuant)
    {
        g_ipu.st.idecScale = QuantiserScale(bits.Get(5), s.nonLinearQuantiser);
    }
    bool fieldDct = ((cmd >> 24) & 1) != 0 && bits.Get(1) != 0;
    alignas(16) s16 mb[384];
    bool error = false;
    g_ipu.st.cbp = DecodeMacroblock(bits, s, g_ipu.matrices, {true, fieldDct, g_ipu.st.idecScale}, g_ipu.st.dc, mb, error);
    if (error)
    {
        g_ipu.st.ecd = true;
    }
    Raw8 raw;
    for (int i = 0; i < 384; i++)
    {
        reinterpret_cast<u8*>(&raw)[i] = static_cast<u8>(mb[i]);
    }
    Rgb32Out(raw, (cmd >> 25) & 1, (cmd >> 27) & 1, (cmd >> 26) & 1);
    if (error)
    {
        return true;
    }
    u32 increment = 0;
    for (;;)
    {
        u32 v = DecodeVlc(bits, 0, s);
        if (v == 0)
        {
            // No increment: the slice is over
            EndOfSliceCheck();
            bits.Peek(32);
            return true;
        }
        s16 value = static_cast<s16>(v);
        if (value == MbaEscape)
        {
            increment += 33;
            continue;
        }
        if (value == MbaStuffing)
        {
            continue;
        }
        increment += static_cast<u32>(value);
        break;
    }
    if (increment > 1)
    {
        // Skipped macroblocks: the DC predictors start again
        for (s32& d : g_ipu.st.dc)
        {
            d = DcReset(s);
        }
    }
    return false;
}

// One unit of the current command; its output in unitOut. Returns whether it was the command's last
bool RunUnit()
{
    Bitstream& bits = g_ipu.bits;
    u32 cmd = g_ipu.command;
    g_ipu.unitOutQw = 0;
    bool first = g_ipu.progress == 0;
    switch (cmd >> 28)
    {
    case Bdec:
    {
        bits.Skip(cmd & 0x3F);
        DecodeSettings s = Settings();
        if ((cmd >> 26) & 1)
        {
            for (s32& d : g_ipu.st.dc)
            {
                d = DcReset(s);
            }
        }
        MacroblockParams p{((cmd >> 27) & 1) != 0, ((cmd >> 25) & 1) != 0, QuantiserScale((cmd >> 16) & 31, s.nonLinearQuantiser)};
        bool error = false;
        g_ipu.st.cbp = DecodeMacroblock(bits, s, g_ipu.matrices, p, g_ipu.st.dc, reinterpret_cast<s16*>(g_ipu.unitOut), error);
        g_ipu.unitOutQw = 48;
        if (error)
        {
            g_ipu.st.ecd = true;
        }
        EndOfSliceCheck();
        bits.Peek(32); // IPU_TOP's 32 bits come before the command ends
        return true;
    }
    case Idec:
        if (first)
        {
            bits.Skip(cmd & 0x3F);
            DecodeSettings s = Settings();
            for (s32& d : g_ipu.st.dc)
            {
                d = DcReset(s);
            }
            g_ipu.st.idecScale = QuantiserScale((cmd >> 16) & 31, s.nonLinearQuantiser);
        }
        return IdecMacroblock(cmd);
    case Vdec:
        bits.Skip(cmd & 0x3F);
        {
            DecodeSettings s = Settings();
            s.pictureType = ((cmd >> 26) & 3) == 1 ? VdecPictureType() : s.pictureType;
            g_ipu.st.cmdData = DecodeVlc(bits, (cmd >> 26) & 3, s);
        }
        g_ipu.st.ecd = g_ipu.st.cmdData == 0;
        bits.Peek(32);
        return true;
    case Fdec:
        bits.Skip(cmd & 0x3F);
        g_ipu.st.cmdData = bits.Peek(32);
        return true;
    case Setiq:
    {
        bits.Skip(cmd & 0x3F);
        u8 matrix[64];
        bits.GetBytes(matrix, 64);
        if (!bits.Starved())
        {
            std::memcpy((cmd >> 27) & 1 ? g_ipu.matrices.nonIntra : g_ipu.matrices.intra, matrix, 64);
        }
        return true;
    }
    case Setvq:
    {
        u16 clut[16];
        bits.GetBytes(reinterpret_cast<u8*>(clut), 32);
        if (!bits.Starved())
        {
            std::memcpy(g_ipu.clut, clut, sizeof(clut));
        }
        return true;
    }
    case Csc:
    {
        u32 count = cmd & 0x7FF;
        if (count == 0)
        {
            return true;
        }
        Raw8 mb;
        bits.GetBytes(reinterpret_cast<u8*>(&mb), sizeof(mb));
        Rgb32Out(mb, false, (cmd >> 27) & 1, (cmd >> 26) & 1);
        return g_ipu.progress + 1 >= count;
    }
    case Pack:
    {
        u32 count = cmd & 0x7FF;
        if (count == 0)
        {
            return true;
        }
        alignas(16) u8 rgb32[1024];
        bits.GetBytes(rgb32, sizeof(rgb32));
        alignas(16) u16 rgb16[256];
        Rgb32ToRgb16(rgb32, (cmd >> 26) & 1, rgb16);
        if ((cmd >> 27) & 1)
        {
            std::memcpy(g_ipu.unitOut, rgb16, sizeof(rgb16));
            g_ipu.unitOutQw = 32;
        }
        else
        {
            Rgb16ToIndx4(rgb16, g_ipu.clut, g_ipu.unitOut);
            g_ipu.unitOutQw = 8;
        }
        return g_ipu.progress + 1 >= count;
    }
    default:
        Fatal("IPU: command %u has no units", cmd >> 28);
    }
}

void Finish()
{
    g_ipu.busy = false;
    g_ipu.ending = false;
    PS2_LOG(LogIpu, "IPU: command %08x done (cbp %02x%s%s)", g_ipu.command, g_ipu.st.cbp, g_ipu.st.scd ? ", start code" : "",
            g_ipu.st.ecd ? ", error" : "");
    RaiseIntc(IntcIpu);
}

// The current command as far as its input and output let it go; between commands the FIFO is kept full from toIPU's DMA, as
// the console's DMA keeps it
void Run()
{
    if (g_ipu.running)
    {
        return;
    }
    g_ipu.running = true;
    Bitstream& bits = g_ipu.bits;
    for (;;)
    {
        DrainOutput();
        if (!g_ipu.busy)
        {
            break;
        }
        if (g_ipu.ending)
        {
            if (g_ipu.outCount > FifoQw)
            {
                break; // the output waits for fromIPU
            }
            Finish();
            break;
        }
        if (g_ipu.outCount >= FifoQw)
        {
            break;
        }
        u64 mark = bits.Mark();
        UnitState saved = g_ipu.st;
        bool last = RunUnit();
        if (bits.Starved())
        {
            // Out of input: taken back, to run again when toIPU's DMA or the EE brings more
            bits.Rewind(mark);
            g_ipu.st = saved;
            break;
        }
        bits.DropRead();
        QueueOutput(g_ipu.unitOut, g_ipu.unitOutQw);
        g_ipu.progress++;
        g_ipu.ending = last;
    }
    bits.TopUp(FifoQw + InternalQw);
    g_ipu.running = false;
}

void Reset()
{
    bool wasBusy = g_ipu.busy;
    g_ipu.bits.Clear(0);
    g_ipu.outHead = g_ipu.outCount = 0;
    g_ipu.busy = g_ipu.ending = false;
    g_ipu.dataResult = false;
    UnitState st;
    for (s32& d : st.dc)
    {
        d = DcReset(Settings());
    }
    g_ipu.st = st;
    g_ipu.thresholds = Thresholds{};
    if (wasBusy)
    {
        RaiseIntc(IntcIpu);
    }
}

void WriteCommand(u32 v)
{
    if (g_ipu.busy && Once("ipu-busy-command"))
    {
        Log("IPU: command %08x written while %08x was running: the old one is dropped", v, g_ipu.command);
    }
    PS2_LOG(LogIpu, "IPU: command %08x (bp %u, %u quadwords held)", v, g_ipu.bits.BitInQw(), g_ipu.bits.HeldQw());
    g_ipu.command = v;
    g_ipu.progress = 0;
    g_ipu.ending = false;
    g_ipu.st.ecd = g_ipu.st.scd = false;
    g_ipu.dataResult = (v >> 28) == Vdec || (v >> 28) == Fdec;
    switch (v >> 28)
    {
    case Bclr:
        g_ipu.busy = false;
        g_ipu.bits.Clear(v & 0x7F);
        RaiseIntc(IntcIpu);
        Run();
        return;
    case Setth:
        g_ipu.busy = false;
        g_ipu.thresholds = Thresholds{v & 0x1FF, (v >> 16) & 0x1FF};
        RaiseIntc(IntcIpu);
        return;
    case Idec:
        // IDEC decodes an I picture's slice, and IPU_CTRL shows it
        g_ipu.settings = (g_ipu.settings & ~0x07000000u) | 0x01000000u;
        break;
    case Bdec:
    case Vdec:
    case Fdec:
    case Setiq:
    case Setvq:
    case Csc:
    case Pack:
        break;
    default:
        if (Once("ipu-command" + std::to_string(v >> 28)))
        {
            Log("IPU: command %u (%08x) isn't one of the IPU's: ignored", v >> 28, v);
        }
        return;
    }
    g_ipu.busy = true;
    Run();
}

// IPU_BP's counts: the quadwords taken in and not passed, the decoder's buffer's first (FP), the FIFO's the rest (IFC)
u32 InternalCount() { return std::min(g_ipu.bits.HeldQw(), InternalQw); }
u32 FifoCount() { return std::min(g_ipu.bits.HeldQw() - InternalCount(), FifoQw); }
}

u32 IpuRead32(u32 a)
{
    Unit& u = g_ipu;
    switch (a & 0xFFFFFFFC)
    {
    case 0x10002000:
        if (!u.dataResult && u.bits.Holds(32))
        {
            // After commands other than FDEC and VDEC, IPU_CMD shows the next 32 bits
            return u.bits.PeekHeld();
        }
        return u.st.cmdData;
    case 0x10002004:
        return u.busy ? 0x80000000u : 0;
    case 0x10002010:
        return FifoCount() | (std::min(u.outCount, FifoQw) << 4) | ((u.st.cbp & 0x3F) << 8) | (u.st.ecd ? 0x4000u : 0) |
               (u.st.scd ? 0x8000u : 0) | u.settings | (u.busy ? 0x80000000u : 0);
    case 0x10002020:
        return (u.bits.BitInQw() & 0x7F) | (FifoCount() << 8) | (InternalCount() << 16);
    case 0x10002030:
        return u.bits.PeekHeld();
    case 0x10002034:
        // Busy, or fewer than 32 bits to show
        return u.busy || !u.bits.Holds(32) ? 0x80000000u : 0;
    default:
        if (Once("ipuread" + std::to_string(a)))
        {
            Log("IPU: read of unknown register %08x", a);
        }
        return 0;
    }
}

void IpuWrite32(u32 a, u32 v)
{
    switch (a & 0xFFFFFFFC)
    {
    case 0x10002000:
        WriteCommand(v);
        return;
    case 0x10002004:
        // IPU_CMD's upper word (a 64 bit write's): nothing
        return;
    case 0x10002010:
        if (((v >> 16) & 3) == 3 && Once("ipu-idp3"))
        {
            Log("IPU: IPU_CTRL's intra DC precision set to 3 (the IPU names 8 to 10 bits): decoded as 11 bits");
        }
        g_ipu.settings = v & 0x07F30000u;
        if (v & 0x40000000u)
        {
            Reset();
        }
        return;
    default:
        if (Once("ipuwrite" + std::to_string(a)))
        {
            Log("IPU: write %08x to %08x (read only, or not a register): ignored", v, a);
        }
        return;
    }
}

u128 IpuReadFifo()
{
    if (g_ipu.outCount == 0)
    {
        if (Once("ipu-out-empty"))
        {
            Log("IPU: IPU_OUT_FIFO read while empty: 0");
        }
        return 0;
    }
    u128 v;
    std::memcpy(&v, g_ipu.out + g_ipu.outHead * 16, 16);
    g_ipu.outHead = (g_ipu.outHead + 1) % Unit::OutQw;
    g_ipu.outCount--;
    Run();
    return v;
}

void IpuWriteFifo(u128 quadword)
{
    if (g_ipu.bits.HeldQw() >= FifoQw + InternalQw && Once("ipu-in-full"))
    {
        Log("IPU: IPU_IN_FIFO written while full: kept (the console's EE would wait)");
    }
    g_ipu.bits.Append(reinterpret_cast<const u8*>(&quadword));
    Run();
}

void IpuDmaStarted(int channel)
{
    PS2_LOG(LogIpu, "IPU: %s started", channel == 3 ? "fromIPU" : "toIPU");
    Run();
}
}
