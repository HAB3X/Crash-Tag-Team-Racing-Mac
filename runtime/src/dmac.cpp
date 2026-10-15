// The DMA controller: ten channels whose transfers run to their end when they're started (the game waits for each before it
// reuses what it reads), in normal, source chain (tags CNT, NEXT, REF, REFS, REFE, CALL, RET, END with the two level call
// stack, TTE for the VIFs) and interleave mode; the end raises the channel's interrupt. The MFIFO: with D_CTRL's MFD set,
// fromSPR writes into a ring in memory (D_RBOR, D_RBSR) and VIF1 or GIF, started in chain mode, drains it as far as fromSPR has
// filled it, then waits (still busy) for more. fromIPU and toIPU instead move data as the IPU gives and takes it (ipu/ipu.cpp).
#include "runtime.h"

#include "../../hw/ee/gif.h"
#include "../../hw/ee/vif.h"

namespace Ps2
{
namespace
{
struct Channel
{
    u32 chcr = 0;
    u32 madr = 0;
    u32 qwc = 0;
    u32 tadr = 0;
    u32 asr[2] = {};
    u32 sadr = 0;
    // A chain stopped by its VIF's stall: the tag whose data is left (MADR, QWC) ends it
    bool chainEnds = false;
    // Started (STR) while D_CTRL's DMAE was off: it begins when DMAE is set; and whether it had begun already (it goes on then)
    bool deferred = false;
    bool begun = false;
};

// What a chain's run came to: its end, the MFIFO's drain waiting for fromSPR, or its VIF stalled at a code's interrupt bit
enum class Run
{
    Ended,
    Waiting,
    Stalled,
};

// The words of a quadword a stalled VIF didn't take: they're in its FIFO, and go first when the stall ends
std::vector<u32> g_vifHeld[2];

Channel g_ch[10];
// D_CTRL: DMAE on, as the kernel leaves it (CTTR starts its first transfers before it writes D_CTRL)
u32 g_ctrl = 1;
u32 g_pcr = 0;
u32 g_sqwc = 0;
u32 g_rbor = 0;
u32 g_rbsr = 0;
u32 g_stadr = 0;

int ChannelIndex(u32 a)
{
    switch (a & 0xFFFFFC00)
    {
    case 0x10008000: return 0;
    case 0x10009000: return 1;
    case 0x1000A000: return 2;
    case 0x1000B000: return 3;
    case 0x1000B400: return 4;
    case 0x1000C000: return 5;
    case 0x1000C400: return 6;
    case 0x1000C800: return 7;
    case 0x1000D000: return 8;
    case 0x1000D400: return 9;
    default: return -1;
    }
}

const char* ChannelName(int n)
{
    static const char* names[] = {"VIF0", "VIF1", "GIF", "fromIPU", "toIPU", "SIF0", "SIF1", "SIF2", "fromSPR", "toSPR"};
    return names[n];
}

// $PS2_VIF_TRACE: VIF1's codes as they're sent (a debugging aid; the VIF itself is the hardware layer's)
int g_vifTrace = getenv("PS2_VIF_TRACE") != nullptr ? atoi(getenv("PS2_VIF_TRACE")) : 0;
struct VifTracer
{
    u32 skip = 0;
    u32 cl = 0, wl = 0;
    void Words(const u32* w, u32 n)
    {
        for (u32 i = 0; i < n; i++)
        {
            if (skip > 0)
            {
                skip--;
                continue;
            }
            if (g_vifTrace <= 0)
            {
                return;
            }
            g_vifTrace--;
            u32 code = w[i];
            u32 cmd = (code >> 24) & 0x7F;
            u32 num = (code >> 16) & 0xFF;
            u32 imm = code & 0xFFFF;
            static const u8 sizes[16] = {4, 2, 1, 0, 8, 4, 2, 0, 12, 6, 3, 0, 16, 8, 4, 2};
            if ((cmd & 0x60) == 0x60)
            {
                u32 count = num == 0 ? 256 : num;
                u32 wlen = wl != 0 ? wl : 256;
                u32 elements = cl < wlen ? cl * (count / wlen) + std::min(count % wlen, cl) : count;
                skip = (elements * sizes[cmd & 0xF] + 3) / 4;
                Log("vif1: UNPACK %s%x num %u addr %x%s%s mask %d (%u words)", "V", cmd & 0xF, count, imm & 0x3FF,
                    (imm & 0x8000) ? " +tops" : "", (imm & 0x4000) ? " usn" : "", (cmd >> 4) & 1, skip);
                if (skip > 0 && i + 1 < n)
                {
                    Log("vif1:   data %08x %08x %08x %08x", w[i + 1], i + 2 < n ? w[i + 2] : 0, i + 3 < n ? w[i + 3] : 0,
                        i + 4 < n ? w[i + 4] : 0);
                }
                continue;
            }
            switch (cmd)
            {
            case 0x01: cl = imm & 0xFF; wl = (imm >> 8) & 0xFF; Log("vif1: STCYCL cl %u wl %u", cl, wl); break;
            case 0x02: Log("vif1: OFFSET %x", imm & 0x3FF); break;
            case 0x03: Log("vif1: BASE %x", imm & 0x3FF); break;
            case 0x04: Log("vif1: ITOP %x", imm & 0x3FF); break;
            case 0x05: Log("vif1: STMOD %u", imm & 3); break;
            case 0x14:
            {
                Ee::Vif* v = Hw().vif1;
                Log("vif1: MSCAL %x (before: base %x offset %x tops %x top %x dbf %d)", imm * 8, v->base, v->offset, v->tops,
                    v->top, v->doubleBuffer ? 1 : 0);
                break;
            }
            case 0x15: Log("vif1: MSCALF %x", imm * 8); break;
            case 0x17: Log("vif1: MSCNT"); break;
            case 0x20: skip = 1; Log("vif1: STMASK"); break;
            case 0x30: skip = 4; Log("vif1: STROW"); break;
            case 0x31: skip = 4; Log("vif1: STCOL"); break;
            case 0x4A: skip = (num == 0 ? 256 : num) * 2; Log("vif1: MPG %x num %u", imm * 8, num); break;
            case 0x50: case 0x51: skip = (imm == 0 ? 0x10000 : imm) * 4; Log("vif1: DIRECT %u", imm); break;
            case 0x00: break;
            default: Log("vif1: code %08x", code); break;
            }
        }
    }
};
VifTracer g_vifTracer;

// The channel D_CTRL's MFD makes the MFIFO's drain (VIF1 or GIF), or -1 with no MFIFO
int MfifoDrain()
{
    switch ((g_ctrl >> 2) & 3)
    {
    case 2: return 1;
    case 3: return 2;
    default: return -1;
    }
}

// An address in the MFIFO's ring: the ring's base and the address's offset within it
u32 InRing(u32 a) { return g_rbor + (a & g_rbsr); }

// Data sent from memory to a channel's device: the quadwords taken, fewer when a VIF stalls at a code's interrupt bit
u32 Send(int ch, const u8* data, u32 qwc, bool isTag)
{
    if (qwc == 0)
    {
        return 0;
    }
    Hardware& hw = Hw();
    const u32* words = reinterpret_cast<const u32*>(data);
    switch (ch)
    {
    case 0:
    case 1:
    {
        if (ch == 1 && g_vifTrace > 0)
        {
            g_vifTracer.Words(words, qwc * 4);
        }
        Ee::Vif* vif = ch == 0 ? hw.vif0 : hw.vif1;
        u32 taken = vif->Process(words, qwc * 4);
        if (taken < qwc * 4)
        {
            u32 quadwords = (taken + 3) / 4;
            g_vifHeld[ch].assign(words + taken, words + quadwords * 4);
            return quadwords;
        }
        break;
    }
    case 2:
        if (!isTag)
        {
            hw.gif->Transfer(2, data, qwc);
        }
        break;
    default:
        if (Once(std::string("dmasend") + ChannelName(ch)))
        {
            Log("DMA to %s not done (%u quadwords)", ChannelName(ch), qwc);
        }
        break;
    }
    return qwc;
}

Run RunChain(int ch, Channel& c);
void Finish(int ch);
void Continue(int ch);

// Quadwords sent from the MFIFO's ring, from where the address falls in it, wrapping at its end: the quadwords taken
u32 SendFromRing(int ch, u32 a, u32 qwc)
{
    u32 sent = 0;
    while (sent < qwc)
    {
        a = InRing(a);
        u32 n = std::min(qwc - sent, (g_rbor + g_rbsr + 16 - a) / 16);
        u32 taken = Send(ch, DmaHost(a), n, false);
        sent += taken;
        a += taken * 16;
        if (taken < n)
        {
            break;
        }
    }
    return sent;
}

// A channel's data at an address (in the MFIFO's ring, wrapping, when it's the drain's and the address is there)
u32 SendAt(int ch, u32 address, u32 qwc)
{
    bool ring = ch == MfifoDrain() && (address & ~g_rbsr) == g_rbor;
    return ring ? SendFromRing(ch, address, qwc) : Send(ch, DmaHost(address), qwc, false);
}

// fromSPR into the MFIFO's ring (MADR taken within it, wrapping at its end), then the drain goes on if it's waiting
void FillMfifo(Channel& c)
{
    u32 qwc = c.qwc;
    while (qwc > 0)
    {
        u32 a = InRing(c.madr);
        u32 n = std::min(qwc, (g_rbor + g_rbsr + 16 - a) / 16);
        std::memcpy(DmaHost(a), DmaHost(0x80000000u | c.sadr), n * 16);
        c.madr = InRing(a + n * 16);
        c.sadr += n * 16;
        qwc -= n;
    }
    c.qwc = 0;
    int drain = MfifoDrain();
    Channel& d = g_ch[drain];
    if ((d.chcr & 0x100) && ((d.chcr >> 2) & 3) == 1)
    {
        Continue(drain);
    }
}

// MADR's QWC quadwords: false when a VIF stalled before taking them all (MADR and QWC are what's left)
bool RunNormal(int ch, Channel& c)
{
    if (ch == 9)
    {
        // toSPR: memory to the scratchpad
        std::memcpy(DmaHost(0x80000000u | c.sadr), DmaHost(c.madr), c.qwc * 16);
        c.madr += c.qwc * 16;
        c.sadr += c.qwc * 16;
        c.qwc = 0;
        return true;
    }
    if (ch == 8 && MfifoDrain() >= 0)
    {
        FillMfifo(c);
        return true;
    }
    if (ch == 8)
    {
        std::memcpy(DmaHost(c.madr), DmaHost(0x80000000u | c.sadr), c.qwc * 16);
        c.madr += c.qwc * 16;
        c.sadr += c.qwc * 16;
        c.qwc = 0;
        return true;
    }
    u32 taken = SendAt(ch, c.madr, c.qwc);
    c.madr += taken * 16;
    c.qwc -= taken;
    return c.qwc == 0;
}

void RunInterleave(int ch, Channel& c)
{
    // SPR channels only: SQWC's TQWC quadwords moved, SQWC's SQWC skipped, until QWC are moved
    u32 tqwc = (g_sqwc >> 16) & 0xFF;
    u32 sqwc = g_sqwc & 0xFF;
    while (c.qwc > 0)
    {
        u32 n = std::min(tqwc == 0 ? c.qwc : tqwc, c.qwc);
        if (ch == 9)
        {
            std::memcpy(DmaHost(0x80000000u | c.sadr), DmaHost(c.madr), n * 16);
        }
        else
        {
            std::memcpy(DmaHost(c.madr), DmaHost(0x80000000u | c.sadr), n * 16);
        }
        c.madr += (n + sqwc) * 16;
        c.sadr += n * 16;
        c.qwc -= n;
    }
}

// A source chain's tag at TADR taken: CHCR's upper half is the tag's, MADR and QWC its data, TADR where the chain goes next,
// and whether the chain ends after the data (REFE, END, RET with nothing to return to, a CALL with a full address stack, or
// the tag's IRQ with CHCR's TIE). `asp` is the address stack's depth (CHCR's ASP), kept by the caller. Returns the tag
const u8* ReadTag(int ch, Channel& c, u32& asp, bool log)
{
    const u8* tagp = DmaHost(c.tadr);
    u64 tag;
    std::memcpy(&tag, tagp, 8);
    u32 qwc = static_cast<u32>(tag & 0xFFFF);
    u32 id = static_cast<u32>((tag >> 28) & 7);
    bool irq = (tag >> 31) & 1;
    u32 addr = static_cast<u32>((tag >> 32) & 0xFFFFFFF0u);
    // CHCR's TAG: the upper half of the tag's first word (its PCE, ID and IRQ), as the last tag read left it. A game's
    // handler reads the IRQ bit there to know its chain's last tag was reached (Biohazard Outbreak's VIF1 handler)
    c.chcr = (c.chcr & 0xFFFF) | (static_cast<u32>(tag) & 0xFFFF0000u);
    if (log)
    {
        PS2_LOG(LogDma, "  tag at %08x: id %u qwc %u addr %08x irq %d", c.tadr, id, qwc, addr, irq ? 1 : 0);
    }
    bool end = false;
    u32 dataAt = 0;
    u32 next = 0;
    switch (id)
    {
    case 0: // REFE
        dataAt = addr;
        next = c.tadr + 16;
        end = true;
        break;
    case 1: // CNT
        dataAt = c.tadr + 16;
        next = dataAt + qwc * 16;
        break;
    case 2: // NEXT
        dataAt = c.tadr + 16;
        next = addr;
        break;
    case 3: // REF
    case 4: // REFS
        dataAt = addr;
        next = c.tadr + 16;
        break;
    case 5: // CALL
        dataAt = c.tadr + 16;
        if (asp >= 2)
        {
            Log("DMA %s: CALL with a full address stack", ChannelName(ch));
            end = true;
            break;
        }
        c.asr[asp++] = dataAt + qwc * 16;
        next = addr;
        break;
    case 6: // RET
        dataAt = c.tadr + 16;
        if (asp > 0)
        {
            next = c.asr[--asp];
        }
        else
        {
            end = true;
        }
        break;
    case 7: // END
        dataAt = c.tadr + 16;
        end = true;
        break;
    }
    c.tadr = next;
    c.madr = dataAt;
    c.qwc = qwc;
    c.chainEnds = end || (irq && (c.chcr & 0x80) != 0);
    return tagp;
}

// A source chain from TADR to its end. The MFIFO's drain reads its tags from the ring and stops where fromSPR has filled it
// up to (Waiting); data at an address in the ring is read there, wrapping. A VIF stalled by a code's interrupt bit stops it
// (Stalled), with the tag's data left in MADR and QWC
Run RunChain(int ch, Channel& c)
{
    bool tte = (c.chcr & 0x40) != 0;
    bool ring = ch == MfifoDrain();
    u32 asp = (c.chcr >> 4) & 3;
    Run result = Run::Ended;
    for (int guard = 0; guard < 1000000; guard++)
    {
        if (ring)
        {
            c.tadr = InRing(c.tadr);
            if (c.tadr == InRing(g_ch[8].madr))
            {
                // The ring is empty: D_STAT's MEIS, and the drain waits
                PS2_LOG(LogDma, "  MFIFO empty at %08x after %d tags", c.tadr, guard);
                RaiseDmac(14);
                result = Run::Waiting;
                break;
            }
        }
        if (guard == 999999)
        {
            Log("DMA %s: a chain of a million tags (at %08x): stopped", ChannelName(ch), c.tadr);
        }
        const u8* tagp = ReadTag(ch, c, asp, guard < 64 || (guard >= 100000 && guard < 100040));
        if (tte && (ch == 0 || ch == 1))
        {
            // The tag's upper two words to the VIF
            const u32* w = reinterpret_cast<const u32*>(tagp) + 2;
            if (ch == 1 && g_vifTrace > 0)
            {
                g_vifTracer.Words(w, 2);
            }
            Ee::Vif* vif = ch == 0 ? Hw().vif0 : Hw().vif1;
            u32 taken = vif->Process(w, 2);
            if (vif->stalled())
            {
                g_vifHeld[ch].assign(w + taken, w + 2);
                result = Run::Stalled;
                break;
            }
        }
        if (ch == 9)
        {
            std::memcpy(DmaHost(0x80000000u | c.sadr), DmaHost(c.madr), c.qwc * 16);
            c.sadr += c.qwc * 16;
            c.madr += c.qwc * 16;
            c.qwc = 0;
        }
        else if (!RunNormal(ch, c))
        {
            result = Run::Stalled;
            break;
        }
        if (c.chainEnds)
        {
            PS2_LOG(LogDma, "  chain ended after %d tags", guard + 1);
            break;
        }
    }
    c.chcr = (c.chcr & ~0x30u) | (asp << 4);
    return result;
}

// A transfer's end: the channel idle, its GS data handed to the render thread now (the game may wait for what it draws), its
// interrupt
void Finish(int ch)
{
    g_ch[ch].chcr &= ~0x100u;
    g_ch[ch].chainEnds = false;
    if (ch != 3 && ch != 4)
    {
        // (The IPU's channels carry nothing for the GS, and end once or twice a macroblock)
        RenderFlush();
    }
    RaiseDmac(static_cast<u32>(ch));
}

// A started transfer on from where it is: the data left of its tag (or of a normal transfer), then the rest of its chain; it
// ends unless its VIF stalls or the MFIFO's drain waits
void Continue(int ch)
{
    Channel& c = g_ch[ch];
    if (!(g_ctrl & 1))
    {
        // The DMAC is off: the transfer stands still until DMAE is set again
        c.deferred = true;
        return;
    }
    if (ch == 3 || ch == 4)
    {
        IpuDmaStarted(ch);
        return;
    }
    if (ch <= 1 && (ch == 0 ? Hw().vif0 : Hw().vif1)->stalled())
    {
        return;
    }
    u32 mode = (c.chcr >> 2) & 3;
    if (c.qwc > 0 && !RunNormal(ch, c))
    {
        return;
    }
    if (mode == 1 && !c.chainEnds && RunChain(ch, c) != Run::Ended)
    {
        return;
    }
    Finish(ch);
}

// fromIPU and toIPU move data as the IPU takes and gives it (DmacToIpu, DmacFromIpu), not all at once: a started transfer
// stays busy until its data has gone into the IPU's FIFO, or its QWC has been filled from the IPU's output. toIPU's chain is
// read a tag at a time; started again with data left of a tag (Sony's libipu stops and restarts it so), whether the chain
// ends after that data is the tag's in CHCR (REFE, END, or IRQ with TIE), as the hardware keeps it
void StartIpuChannel(int ch, Channel& c)
{
    u32 mode = (c.chcr >> 2) & 3;
    if (ch == 3 ? mode != 0 : mode > 1)
    {
        Log("DMA %s in mode %u not done: ended at once", ChannelName(ch), mode);
        Finish(ch);
        return;
    }
    if (mode == 0 && c.qwc == 0)
    {
        Finish(ch);
        return;
    }
    if (mode == 1 && c.qwc > 0)
    {
        u32 id = (c.chcr >> 28) & 7;
        c.chainEnds = id == 0 || id == 7 || ((c.chcr & 0x80000000u) != 0 && (c.chcr & 0x80) != 0);
    }
    IpuDmaStarted(ch);
}

void Start(int ch)
{
    Channel& c = g_ch[ch];
    u32 mode = (c.chcr >> 2) & 3;
    PS2_LOG(LogDma, "DMA %s start chcr %08x madr %08x qwc %x tadr %08x", ChannelName(ch), c.chcr, c.madr, c.qwc, c.tadr);
    c.chainEnds = false;
    c.begun = false;
    if (!(g_ctrl & 1))
    {
        // The DMAC is off (D_CTRL's DMAE): the transfer begins when it's set (CTTR re-arms its MFIFO's drain this way: VIF1
        // started, then its TADR and fromSPR's MADR put back at the ring's start, then DMAE; begun at once, the drain would
        // run the frame it had already drained again)
        PS2_LOG(LogDma, "DMA %s started with the DMAC off: it waits for DMAE", ChannelName(ch));
        c.deferred = true;
        return;
    }
    c.begun = true;
    if (ch == 3 || ch == 4)
    {
        StartIpuChannel(ch, c);
        return;
    }
    if (ch == 5 || ch == 6 || ch == 7)
    {
        // The SIF's channels are the kernel's and the IOP's (sif.cpp); started directly they finish at once
        c.chcr &= ~0x100u;
        return;
    }
    if (ch == 1 && !(c.chcr & 1))
    {
        if (Once("vif1read"))
        {
            Log("VIF1 to memory (a GS read-back) not done");
        }
    }
    else if (mode == 0)
    {
        // QWC 0 in normal mode: nothing to move
        if (c.qwc > 0 && !RunNormal(ch, c))
        {
            return;
        }
    }
    else if (mode == 1)
    {
        if (ch == 8)
        {
            Log("fromSPR in chain mode not done");
        }
        else
        {
            // A chain started with data left over first sends it (MADR, QWC) then goes on at TADR
            Continue(ch);
            return;
        }
    }
    else
    {
        RunInterleave(ch, c);
    }
    Finish(ch);
}
}

void DmacInit() {}

u32 DmacToIpu(u8* out, u32 quadwords)
{
    Channel& c = g_ch[4];
    u32 moved = 0;
    u32 emptyTags = 0;
    while (moved < quadwords && (c.chcr & 0x100) != 0 && (g_ctrl & 1) != 0)
    {
        bool chain = ((c.chcr >> 2) & 3) == 1;
        if (c.qwc == 0)
        {
            if (!chain || c.chainEnds)
            {
                Finish(4);
                break;
            }
            if (++emptyTags > 100000)
            {
                Fatal("DMA toIPU: 100,000 tags without data from %08x", c.tadr);
            }
            u32 asp = (c.chcr >> 4) & 3;
            ReadTag(4, c, asp, (g_logMask & LogDma) != 0);
            c.chcr = (c.chcr & ~0x30u) | (asp << 4);
            continue;
        }
        u32 n = std::min(c.qwc, quadwords - moved);
        std::memcpy(out + moved * 16, DmaHost(c.madr), n * 16);
        c.madr += n * 16;
        c.qwc -= n;
        moved += n;
        if (c.qwc == 0 && (!chain || c.chainEnds))
        {
            Finish(4);
            break;
        }
    }
    return moved;
}

u32 DmacFromIpu(const u8* data, u32 quadwords)
{
    Channel& c = g_ch[3];
    if ((c.chcr & 0x100) == 0 || (g_ctrl & 1) == 0 || c.qwc == 0)
    {
        return 0;
    }
    u32 n = std::min(c.qwc, quadwords);
    std::memcpy(DmaHost(c.madr), data, n * 16);
    c.madr += n * 16;
    c.qwc -= n;
    if (c.qwc == 0)
    {
        Finish(3);
    }
    return n;
}

void DmacVifUnstall(int vif)
{
    Ee::Vif* v = vif == 0 ? Hw().vif0 : Hw().vif1;
    if (!v->stalled())
    {
        return;
    }
    v->CancelStall();
    // The words left in its FIFO first, then the transfer it stopped
    std::vector<u32>& held = g_vifHeld[vif];
    if (!held.empty())
    {
        u32 taken = v->Process(held.data(), static_cast<u32>(held.size()));
        held.erase(held.begin(), held.begin() + taken);
        if (v->stalled())
        {
            return;
        }
    }
    if (g_ch[vif].chcr & 0x100)
    {
        Continue(vif);
    }
}

u32 DmacRead(u32 a)
{
    if (a >= 0x1000E000)
    {
        switch (a)
        {
        case 0x1000E000: return g_ctrl;
        case 0x1000E010: return g_dmacStat;
        case 0x1000E020: return g_pcr;
        case 0x1000E030: return g_sqwc;
        case 0x1000E040: return g_rbsr;
        case 0x1000E050: return g_rbor;
        case 0x1000E060: return g_stadr;
        default: return 0;
        }
    }
    int ch = ChannelIndex(a);
    if (ch < 0)
    {
        return 0;
    }
    Channel& c = g_ch[ch];
    switch (a & 0xF0)
    {
    case 0x00: return c.chcr;
    case 0x10: return c.madr;
    case 0x20: return c.qwc;
    case 0x30: return c.tadr;
    case 0x40: return c.asr[0];
    case 0x50: return c.asr[1];
    case 0x80: return c.sadr;
    default: return 0;
    }
}

void DmacWrite(u32 a, u32 v)
{
    if (a >= 0x1000E000)
    {
        switch (a)
        {
        case 0x1000E000:
        {
            PS2_LOG(LogDma, "D_CTRL = %08x", v);
            if (((v >> 4) & 3) == 3 && Once("dmac-sts-ipu"))
            {
                // STS: fromIPU's address as a drain's stall limit (D_STADR) isn't kept
                Log("D_CTRL's stall control with fromIPU as its source (%08x) not done", v);
            }
            bool enabled = !(g_ctrl & 1) && (v & 1);
            g_ctrl = v;
            if (enabled)
            {
                // DMAE set: the transfers that waited for it begin, or go on, in the channels' order
                for (int ch = 0; ch < 10; ch++)
                {
                    Channel& c = g_ch[ch];
                    if (c.deferred && (c.chcr & 0x100))
                    {
                        c.deferred = false;
                        if (c.begun)
                        {
                            Continue(ch);
                        }
                        else
                        {
                            Start(ch);
                        }
                    }
                    c.deferred = false;
                }
            }
            return;
        }
        case 0x1000E010:
            // The status bits are cleared by ones, the masks toggled by ones
            g_dmacStat &= ~(v & 0xFFFF);
            g_dmacStat ^= v & 0xFFFF0000u;
            return;
        case 0x1000E020:
            g_pcr = v;
            return;
        case 0x1000E030:
            g_sqwc = v;
            return;
        case 0x1000E040:
            PS2_LOG(LogDma, "D_RBSR = %08x", v);
            g_rbsr = v;
            return;
        case 0x1000E050:
            PS2_LOG(LogDma, "D_RBOR = %08x", v);
            g_rbor = v;
            return;
        case 0x1000E060:
            g_stadr = v;
            return;
        default:
            return;
        }
    }
    int ch = ChannelIndex(a);
    if (ch < 0)
    {
        return;
    }
    Channel& c = g_ch[ch];
    switch (a & 0xF0)
    {
    case 0x00:
        c.chcr = v;
        if (v & 0x100)
        {
            Start(ch);
        }
        return;
    case 0x10:
        c.madr = v;
        return;
    case 0x20:
        c.qwc = v & 0xFFFF;
        return;
    case 0x30:
        c.tadr = v;
        return;
    case 0x40:
        c.asr[0] = v;
        return;
    case 0x50:
        c.asr[1] = v;
        return;
    case 0x80:
        c.sadr = v & 0x3FF0;
        return;
    default:
        return;
    }
}
}
