// The IPU's input as its decoder reads it: the quadwords it has taken (from toIPU's DMA or the EE's writes to IPU_IN_FIFO) and
// not yet passed, read most significant bit first, byte by byte in memory order, from a bit position in the first one.
//
// A command's work is done in units (a macroblock, a VLC, a matrix) that either finish or are taken back: a unit that runs out
// of data is marked starved (reads past the end give zeros, so its decoding stays bounded) and the IPU puts the position back
// to where the unit began, keeping what it took, to run the unit again when more has come. The quadwords a unit needs past
// those held are pulled from the source as it reads, as the hardware's FIFO is refilled by the DMA while it decodes
#pragma once

#include "ps2/recomp.h"

#include <algorithm>
#include <cstring>

namespace Ps2::Ipu
{
class Bitstream
{
public:
    // The most a unit can need at once: a macroblock of the largest legal size is under 600 bytes; 16 KB is a corrupt
    // stream's bound, which stops the game naming the IPU (Fatal) rather than overflow
    static constexpr u32 CapacityQw = 1024;

    // Where more quadwords come from (toIPU's DMA): up to `max` written at `out`, how many returned
    using Source = u32 (*)(u8* out, u32 max);
    explicit Bitstream(Source source) : source_(source) {}

    // The next `bits` (1-32) bits, not consumed
    u32 Peek(u32 bits)
    {
        if (pos_ + bits > end_)
        {
            Refill(bits);
            if (pos_ >= end_)
            {
                return 0; // starved: zeros past the end
            }
        }
        u64 word;
        std::memcpy(&word, data_ + (pos_ >> 3), 8);
        word = __builtin_bswap64(word) << (pos_ & 7);
        return static_cast<u32>(word >> (64 - bits));
    }
    // As Peek, signed (two's complement in `bits` bits)
    s32 PeekSigned(u32 bits) { return static_cast<s32>(Peek(bits) << (32 - bits)) >> (32 - bits); }
    void Skip(u32 bits) { pos_ += bits; }
    u32 Get(u32 bits)
    {
        u32 v = Peek(bits);
        pos_ += bits;
        return v;
    }
    // The bits to the next byte boundary skipped
    void AlignToByte() { pos_ = (pos_ + 7) & ~u64(7); }
    // `count` bytes from the position on (at any bit position)
    void GetBytes(u8* out, u32 count);
    // The next 32 bits as far as they're held (zeros after), without pulling more: what IPU_TOP shows
    u32 PeekHeld() const
    {
        if (pos_ >= end_)
        {
            return 0;
        }
        u64 word;
        std::memcpy(&word, data_ + (pos_ >> 3), 8);
        word = __builtin_bswap64(word) << (pos_ & 7);
        return static_cast<u32>(word >> 32);
    }
    // Whether `bits` more bits are held (without pulling more)
    bool Holds(u32 bits) const { return pos_ + bits <= end_; }

    // A unit's start, and its end: the position put back when it starved
    u64 Mark()
    {
        starved_ = false;
        return pos_;
    }
    bool Starved() const { return starved_; }
    void Rewind(u64 mark) { pos_ = mark; }

    // The quadwords wholly read are passed on: the first held is the one the position is in
    void DropRead();
    // Quadwords held (from the one the position is in), and the position's bit in the first (IPU_BP's BP)
    u32 HeldQw() const { return (tail_ - head_) / 16; }
    u32 BitInQw() const { return static_cast<u32>(pos_ - u64(head_) * 8); }
    // Quadwords put in from outside the decoding (the EE's IPU_IN_FIFO writes; the DMA's top-up between commands)
    void Append(const u8* quadword);
    // Pulls from the source until `heldQw` quadwords are held or it has no more
    void TopUp(u32 heldQw);
    // Everything held dropped, the position `bit` bits into the next quadword to come (BCLR, a reset)
    void Clear(u32 bit);

private:
    void Refill(u32 bits);
    void Compact();

    Source source_;
    // The held bytes are data_[head_, tail_); the position, in bits from data_'s start, is at or after head_. Sixteen bytes
    // past the tail are kept zero, for reads past the end
    alignas(16) u8 data_[CapacityQw * 16 + 16] = {};
    u32 head_ = 0;
    u32 tail_ = 0;
    u64 pos_ = 0;
    u64 end_ = 0; // tail_ in bits
    bool starved_ = false;
};
}
