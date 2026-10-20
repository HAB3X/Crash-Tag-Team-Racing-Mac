#include "ee.h"

#include <cstdio>
#include <cstring>

namespace IpuTest
{
namespace
{
constexpr u32 IpuCmd = 0x10002000;
constexpr u32 IpuCtrl = 0x10002010;
constexpr u32 IpuBp = 0x10002020;
constexpr u32 IpuTop = 0x10002030;
constexpr u32 D4Chcr = 0x1000B400, D4Madr = 0x1000B410, D4Qwc = 0x1000B420, D4Tadr = 0x1000B430;
constexpr u32 D3Chcr = 0x1000B000;
constexpr u32 Busy = 0x80000000u;

double g_ipuSeconds = 0;

// The writes that run the IPU (its commands, its DMA channels started): timed
bool RunsIpu(u32 a) { return a == IpuCmd || a == IpuCtrl || a == D3Chcr || a == D4Chcr; }

void WriteTag(u32 index, u32 id, u32 qwc, u32 address)
{
    u64 tag = qwc | (u64(id) << 28) | (u64(address) << 32);
    std::memcpy(Guest(TagBase + index * 16), &tag, 8);
    std::memset(Guest(TagBase + index * 16 + 8), 0, 8);
}
}

u32 Read32(u32 a) { return ps2_io_read32(a); }
u64 Read64(u32 a) { return ps2_io_read64(a); }
void Write32(u32 a, u32 v)
{
    if (!RunsIpu(a))
    {
        ps2_io_write32(a, v);
        return;
    }
    auto t0 = std::chrono::steady_clock::now();
    ps2_io_write32(a, v);
    g_ipuSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
u8* Guest(u32 a) { return ps2_mem + a; }
double IpuSeconds() { return g_ipuSeconds; }

bool Driver::WaitIdle()
{
    // libmpeg's wait: IPU_CTRL's BUSY polled; with the IPU waiting for data and toIPU idle, more is fed (its no-data
    // callback). The IPU decodes as soon as data comes, so one poll a step is enough here
    for (;;)
    {
        u32 ctrl = Ctrl();
        if ((ctrl & Busy) == 0)
        {
            return true;
        }
        if ((Read32(D3Chcr) & 0x100) == 0 && ((ctrl >> 4) & 15) == 8)
        {
            fprintf(stderr, "the IPU waits for fromIPU, which isn't running (command %08x)\n", Read32(IpuCmd));
            return false;
        }
        if (!Feed())
        {
            return false;
        }
    }
}

bool Driver::Command(u32 word)
{
    Write32(IpuCmd, word);
    return WaitIdle();
}

bool Driver::Peek(u32 bits, u32& value)
{
    if (!WaitIdle())
    {
        return false;
    }
    for (;;)
    {
        u64 top = Read64(IpuTop);
        value = static_cast<u32>(top) >> (32 - bits);
        if ((top >> 63) == 0)
        {
            return true;
        }
        if (!Feed())
        {
            // At the stream's end the bits held are all there are (zeros after)
            return StreamDone();
        }
    }
}

bool Driver::Skip(u32 bits)
{
    if (bits == 0)
    {
        return true;
    }
    return Command(0x40000000u | bits); // FDEC with FB
}

bool Driver::Get(u32 bits, u32& value) { return Peek(bits, value) && Skip(bits); }

bool Driver::AlignToByte() { return Skip((8 - (Read32(IpuBp) & 7)) & 7); }

bool Driver::NextStartCode(u32& code)
{
    if (!AlignToByte())
    {
        return false;
    }
    for (;;)
    {
        u32 v;
        if (!Peek(24, v))
        {
            return false;
        }
        if (v == 1)
        {
            u32 word;
            if (!Peek(32, word))
            {
                return false;
            }
            code = word & 0xFF;
            // The last start code (the sequence's end) may have nothing after it to fill FDEC's 32 bits
            return code == 0xB7 || Skip(32);
        }
        if (!Skip(8))
        {
            return false;
        }
    }
}

bool Driver::Vdec(u32 table, u32& word)
{
    if (!Command(0x30000000u | (table << 26)))
    {
        return false;
    }
    word = Read32(IpuCmd);
    return true;
}

u32 Driver::BlockOfDma()
{
    u32 madr = Read32(D4Madr);
    u32 at = Read32(D4Qwc) > 0 ? madr : madr - 16;
    return (at - RingBase) / (BlockQw * 16);
}

u64 Driver::DmaPosition()
{
    if (!started_)
    {
        return 0;
    }
    u32 b = BlockOfDma();
    return blockStream_[b] + (blockQw_[b] - Read32(D4Qwc));
}

bool Driver::Feed()
{
    bool running = false;
    if (started_)
    {
        u32 chcr = Read32(D4Chcr);
        if (chcr & 0x100)
        {
            // Suspended while the ring is changed (CRI's code writes CHCR without STR)
            Write32(D4Chcr, chcr & ~0x100u);
            running = true;
        }
        // Blocks the DMA is past by more than the IPU can hold (10 quadwords) are free again: StopDma's rewind may need
        // what the IPU holds
        u64 position = DmaPosition();
        while (count_ > 0 && blockStream_[head_] + blockQw_[head_] + 16 <= position)
        {
            head_ = (head_ + 1) % blocks_;
            count_--;
        }
    }
    bool added = false;
    while (count_ < blocks_ - 1 && fedQw_ * 16 < video_.size())
    {
        u32 b = (head_ + count_) % blocks_;
        size_t from = fedQw_ * 16;
        size_t bytes = std::min<size_t>(BlockQw * 16, video_.size() - from);
        u32 qw = static_cast<u32>((bytes + 15) / 16);
        u8* block = Guest(RingBase + b * BlockQw * 16);
        std::memset(block, 0, BlockQw * 16);
        std::memcpy(block, video_.data() + from, bytes);
        blockStream_[b] = fedQw_;
        blockQw_[b] = qw;
        fedQw_ += qw;
        if (count_ > 0)
        {
            u32 last = (b + blocks_ - 1) % blocks_;
            WriteTag(last, 3, blockQw_[last], RingBase + last * BlockQw * 16); // REFE becomes REF
        }
        WriteTag(b, 0, qw, RingBase + b * BlockQw * 16); // REFE: the DMA ends here unless more comes
        count_++;
        added = true;
    }
    WriteTag(blocks_, 2, 0, TagBase); // NEXT: back to the first block's tag
    if (!started_)
    {
        if (!added)
        {
            return false;
        }
        started_ = true;
        Write32(D4Qwc, 0);
        Write32(D4Tadr, TagBase + head_ * 16);
        Write32(D4Chcr, 0x105); // chain mode, from memory, STR
        return true;
    }
    bool more = DmaPosition() < fedQw_;
    if (running || (added && more) || (more && (Read32(D4Chcr) & 0x100) == 0))
    {
        (running ? feedsWhileRunning : feedsAfterEnd)++;
        Write32(D4Chcr, Read32(D4Chcr) | 0x100);
        return true;
    }
    return false;
}

void Driver::StopDma()
{
    u32 chcr = Read32(D4Chcr);
    Write32(D4Chcr, chcr & ~0x100u);
    while (((Ctrl() >> 4) & 15) != 0)
    {
        // libipu waits for the output FIFO to drain; nothing is running here, so this doesn't loop
    }
    savedChcr_ = Read32(D4Chcr);
    savedMadr_ = Read32(D4Madr);
    savedQwc_ = Read32(D4Qwc);
    savedTadr_ = Read32(D4Tadr);
    savedBp_ = Read32(IpuBp);
}

void Driver::RestartDma()
{
    restarts++;
    u32 held = ((savedBp_ >> 8) & 15) + ((savedBp_ >> 16) & 3);
    u32 bp = savedBp_ & 0x7F;
    u32 at = savedQwc_ > 0 ? savedMadr_ : savedMadr_ - 16;
    u32 current = (at - RingBase) / (BlockQw * 16);
    u64 position = blockStream_[current] + (blockQw_[current] - savedQwc_) - held;
    Command(bp); // BCLR with the bit position
    if (position >= fedQw_)
    {
        Write32(D4Madr, savedMadr_);
        Write32(D4Qwc, savedQwc_);
        Write32(D4Tadr, savedTadr_);
        return;
    }
    u32 b = current;
    while (position < blockStream_[b])
    {
        b = (b + blocks_ - 1) % blocks_;
    }
    if (b != current)
    {
        restartsAcrossBlocks++;
    }
    u32 offset = static_cast<u32>(position - blockStream_[b]);
    u64 tag;
    std::memcpy(&tag, Guest(TagBase + b * 16), 8);
    Write32(D4Madr, RingBase + b * BlockQw * 16 + offset * 16);
    Write32(D4Qwc, blockQw_[b] - offset);
    Write32(D4Tadr, TagBase + (b + 1) * 16);
    Write32(D4Chcr, (savedChcr_ & 0xFFFF) | (static_cast<u32>(tag) & 0xFFFF0000u) | 0x100);
}
}
