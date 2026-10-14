// The DMA controller: ten channels whose transfers run to their end when they're started (the game waits for each before it
// reuses what it reads), in normal, source chain (tags CNT, NEXT, REF, REFS, REFE, CALL, RET, END with the two level call
// stack, TTE for the VIFs) and interleave mode; the end raises the channel's interrupt. The MFIFO: with D_CTRL's MFD set,
// fromSPR writes into a ring in memory (D_RBOR, D_RBSR) and VIF1 or GIF, started in chain mode, drains it as far as fromSPR has
// filled it, then waits (still busy) for more.
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
};

Channel g_ch[10];
u32 g_ctrl = 0;
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

// Data sent from memory to a channel's device
void Send(int ch, const u8* data, u32 qwc, bool isTag)
{
    if (qwc == 0)
    {
        return;
    }
    Hardware& hw = Hw();
    const u32* words = reinterpret_cast<const u32*>(data);
    switch (ch)
    {
    case 0:
        hw.vif0->Process(words, qwc * 4);
        break;
    case 1:
        if (g_vifTrace > 0)
        {
            g_vifTracer.Words(words, qwc * 4);
        }
        hw.vif1->Process(words, qwc * 4);
        break;
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
}

bool RunChain(int ch, Channel& c);
void Finish(int ch);

// Quadwords sent from the MFIFO's ring, from where the address falls in it, wrapping at its end
void SendFromRing(int ch, u32 a, u32 qwc)
{
    while (qwc > 0)
    {
        a = InRing(a);
        u32 n = std::min(qwc, (g_rbor + g_rbsr + 16 - a) / 16);
        Send(ch, DmaHost(a), n, false);
        a += n * 16;
        qwc -= n;
    }
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
    if ((d.chcr & 0x100) && ((d.chcr >> 2) & 3) == 1 && RunChain(drain, d))
    {
        Finish(drain);
    }
}

void RunNormal(int ch, Channel& c)
{
    if (ch == 9)
    {
        // toSPR: memory to the scratchpad
        std::memcpy(DmaHost(0x80000000u | c.sadr), DmaHost(c.madr), c.qwc * 16);
        c.madr += c.qwc * 16;
        c.sadr += c.qwc * 16;
        c.qwc = 0;
        return;
    }
    if (ch == 8 && MfifoDrain() >= 0)
    {
        FillMfifo(c);
        return;
    }
    if (ch == 8)
    {
        std::memcpy(DmaHost(c.madr), DmaHost(0x80000000u | c.sadr), c.qwc * 16);
        c.madr += c.qwc * 16;
        c.sadr += c.qwc * 16;
        c.qwc = 0;
        return;
    }
    Send(ch, DmaHost(c.madr), c.qwc, false);
    c.madr += c.qwc * 16;
    c.qwc = 0;
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

// A source chain from TADR to its end. The MFIFO's drain reads its tags and their data from the ring (REF, REFS and REFE's
// data are outside it) and stops where fromSPR has filled it up to: false then, the channel waiting for more
bool RunChain(int ch, Channel& c)
{
    bool tte = (c.chcr & 0x40) != 0;
    bool tie = (c.chcr & 0x80) != 0;
    bool ring = ch == MfifoDrain();
    u32 asp = (c.chcr >> 4) & 3;
    bool ended = true;
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
                ended = false;
                break;
            }
        }
        u8* tagp = DmaHost(c.tadr);
        u64 tag;
        std::memcpy(&tag, tagp, 8);
        u32 qwc = static_cast<u32>(tag & 0xFFFF);
        u32 id = static_cast<u32>((tag >> 28) & 7);
        bool irq = (tag >> 31) & 1;
        u32 addr = static_cast<u32>((tag >> 32) & 0xFFFFFFF0u);
        c.chcr = (c.chcr & 0xFFFF) | (static_cast<u32>(tag >> 16) & 0xFFFF0000u);
        if (guard < 64 || (guard >= 100000 && guard < 100040))
        {
            PS2_LOG(LogDma, "  tag at %08x: id %u qwc %u addr %08x irq %d", c.tadr, id, qwc, addr, irq ? 1 : 0);
        }
        if (guard == 999999)
        {
            Log("DMA %s: a chain of a million tags (at %08x): stopped", ChannelName(ch), c.tadr);
        }
        if (tte && (ch == 0 || ch == 1))
        {
            // The tag's upper two words to the VIF
            Hardware& hw = Hw();
            const u32* w = reinterpret_cast<const u32*>(tagp) + 2;
            if (ch == 1 && g_vifTrace > 0)
            {
                g_vifTracer.Words(w, 2);
            }
            (ch == 0 ? hw.vif0 : hw.vif1)->Process(w, 2);
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
        if (ch == 9)
        {
            std::memcpy(DmaHost(0x80000000u | c.sadr), DmaHost(dataAt), qwc * 16);
            c.sadr += qwc * 16;
        }
        else if (ring && id != 0 && id != 3 && id != 4)
        {
            SendFromRing(ch, dataAt, qwc);
        }
        else
        {
            Send(ch, DmaHost(dataAt), qwc, false);
        }
        c.madr = dataAt + qwc * 16;
        c.tadr = next;
        if (end || (irq && tie))
        {
            PS2_LOG(LogDma, "  chain ended after %d tags", guard + 1);
            break;
        }
    }
    c.chcr = (c.chcr & ~0x30u) | (asp << 4);
    c.qwc = 0;
    return ended;
}

// A transfer's end: the channel idle, its GS data handed to the render thread now (the game may wait for what it draws), its
// interrupt
void Finish(int ch)
{
    g_ch[ch].chcr &= ~0x100u;
    RenderFlush();
    RaiseDmac(static_cast<u32>(ch));
}

void Start(int ch)
{
    Channel& c = g_ch[ch];
    u32 mode = (c.chcr >> 2) & 3;
    PS2_LOG(LogDma, "DMA %s start chcr %08x madr %08x qwc %x tadr %08x", ChannelName(ch), c.chcr, c.madr, c.qwc, c.tadr);
    if (!(g_ctrl & 1))
    {
        // The DMAC is off: the transfer waits (it never starts here)
        PS2_LOG(LogDma, "DMA %s started with the DMAC off", ChannelName(ch));
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
        if (c.qwc > 0)
        {
            RunNormal(ch, c);
        }
        else if (ch <= 2 || ch == 9)
        {
            // QWC 0 in normal mode: nothing to move
        }
    }
    else if (mode == 1)
    {
        if (c.qwc > 0)
        {
            // A chain started with data left over first sends it (MADR, QWC) then goes on at TADR
            RunNormal(ch, c);
        }
        if (ch == 8)
        {
            Log("fromSPR in chain mode not done");
        }
        else if (!RunChain(ch, c))
        {
            // The MFIFO's drain waits for fromSPR to fill the ring, still busy
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
            PS2_LOG(LogDma, "D_CTRL = %08x", v);
            g_ctrl = v;
            return;
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
