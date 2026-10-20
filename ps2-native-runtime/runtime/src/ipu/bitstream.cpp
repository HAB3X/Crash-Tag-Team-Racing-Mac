#include "bitstream.h"

#include "../runtime.h"

namespace Ps2::Ipu
{
void Bitstream::Refill(u32 bits)
{
    while (pos_ + bits > end_)
    {
        if (tail_ + 16 > CapacityQw * 16)
        {
            Compact();
            if (tail_ + 16 > CapacityQw * 16)
            {
                Fatal("IPU: one unit of a command read past %u quadwords (a corrupt stream?)", CapacityQw);
            }
        }
        // As many quadwords as the read needs, in one pull
        u64 needQw = (pos_ + bits - end_ + 127) / 128;
        u32 room = (CapacityQw * 16 - tail_) / 16;
        u32 got = source_(data_ + tail_, static_cast<u32>(std::min<u64>(needQw, room)));
        if (got == 0)
        {
            // Nothing more to come now: the unit is taken back when it ends. The bytes past the tail are zeros
            starved_ = true;
            return;
        }
        tail_ += got * 16;
        end_ = u64(tail_) * 8;
        std::memset(data_ + tail_, 0, 16);
    }
}

void Bitstream::GetBytes(u8* out, u32 count)
{
    if ((pos_ & 7) == 0)
    {
        // Byte aligned (CSC's and PACK's macroblocks, the matrices): straight copies
        while (count > 0)
        {
            if (!Holds(8))
            {
                Refill(std::min<u32>(count, 1024) * 8);
                if (starved_)
                {
                    std::memset(out, 0, count);
                    return;
                }
            }
            u32 n = std::min<u32>(count, static_cast<u32>((end_ - pos_) / 8));
            std::memcpy(out, data_ + (pos_ >> 3), n);
            pos_ += u64(n) * 8;
            out += n;
            count -= n;
        }
        return;
    }
    for (u32 i = 0; i < count; i++)
    {
        out[i] = static_cast<u8>(Get(8));
    }
}

void Bitstream::DropRead()
{
    u32 first = static_cast<u32>(pos_ >> 7) * 16;
    head_ = std::min(first, tail_);
    if (first > tail_)
    {
        // The position is past what's held (BCLR's bit into a quadword not yet come, or a FB skip past the end)
        return;
    }
    Compact();
}

void Bitstream::Compact()
{
    if (head_ == 0)
    {
        return;
    }
    u32 held = tail_ - head_;
    std::memmove(data_, data_ + head_, held + 16);
    pos_ -= u64(head_) * 8;
    tail_ = held;
    head_ = 0;
    end_ = u64(tail_) * 8;
}

void Bitstream::Append(const u8* quadword)
{
    if (tail_ + 16 > CapacityQw * 16)
    {
        Compact();
        if (tail_ + 16 > CapacityQw * 16)
        {
            Fatal("IPU: more than %u quadwords put in its FIFO without being read", CapacityQw);
        }
    }
    std::memcpy(data_ + tail_, quadword, 16);
    tail_ += 16;
    end_ = u64(tail_) * 8;
    std::memset(data_ + tail_, 0, 16);
}

void Bitstream::TopUp(u32 heldQw)
{
    while (HeldQw() < heldQw)
    {
        if (tail_ + 16 > CapacityQw * 16)
        {
            Compact();
            if (tail_ + 16 > CapacityQw * 16)
            {
                return;
            }
        }
        u32 want = std::min(heldQw - HeldQw(), (CapacityQw * 16 - tail_) / 16);
        u32 got = source_(data_ + tail_, want);
        if (got == 0)
        {
            return;
        }
        tail_ += got * 16;
        end_ = u64(tail_) * 8;
        std::memset(data_ + tail_, 0, 16);
    }
}

void Bitstream::Clear(u32 bit)
{
    head_ = 0;
    tail_ = 0;
    end_ = 0;
    pos_ = bit;
    starved_ = false;
    std::memset(data_, 0, 16);
}
}
