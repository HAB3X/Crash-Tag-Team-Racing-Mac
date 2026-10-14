#pragma once

// A VIF (VIF0 or VIF1): the codes of a DMA channel's data that feed its vector unit (UNPACKs of data into its memory with the
// write cycles, masks and modes, MPG code uploads, the double buffer's BASE, OFFSET and TOPS, ITOP) and start its programs
// (MSCAL, MSCALF, MSCNT), and VIF1's DIRECT that passes GS packets to the GIF on path 2. The data may arrive in any number of
// pieces; a code waits for all of its data before it's done. Programs run to their end when they're started. A code with its
// interrupt bit (31) stalls the VIF once it's done, until the stall is cancelled (FBRST's STC): the interrupt function runs
// then, and the rest of the stream waits.

#include "common.h"

#include <vector>

namespace Ee
{
class Vu;
class Gif;

class Vif
{
public:
    Vif(u32 index, Vu& vu, Gif* gif) : index_(index), vu_(vu), gif_(gif) {}

    // The words of the stream (a DMA tag's two VIF words with TTE, or its data). Returns the words taken: fewer than count when
    // a code's interrupt bit stalled the VIF (the rest wait for the DMA to send them again after the stall's cancelled)
    u32 Process(const u32* words, u32 count);
    // The stall cancelled (FBRST's STC)
    void CancelStall() { stalled_ = false; }
    bool stalled() const { return stalled_; }
    // The last code (VIFn_CODE), and the function told of a code's interrupt bit (the VIF is stalled when it runs)
    u32 lastCode = 0;
    void (*interrupt)(u32 index) = nullptr;

    u32 index() const { return index_; }
    Vu& vu() { return vu_; }

    // The registers
    u32 cycleLength = 0;
    u32 writeLength = 0;
    u32 mode = 0;
    u32 mask = 0;
    u32 row[4] = {};
    u32 col[4] = {};
    u32 base = 0;
    u32 offset = 0;
    u32 tops = 0;
    u32 top = 0;
    u32 itops = 0;
    u32 itop = 0;
    bool doubleBuffer = false;
    u32 mark = 0;

    // The registers and a code waiting for its data, for a recording (capture.h)
    template <typename Io>
    void Serialize(Io& io)
    {
        io.Field(cycleLength);
        io.Field(writeLength);
        io.Field(mode);
        io.Field(mask);
        io.Field(row);
        io.Field(col);
        io.Field(base);
        io.Field(offset);
        io.Field(tops);
        io.Field(top);
        io.Field(itops);
        io.Field(itop);
        io.Field(doubleBuffer);
        io.Field(mark);
        io.Field(pending_);
        io.Field(pendingCode_);
        io.Field(pendingWords_);
        io.Vector(pendingData_);
    }

    // Programs started (statistics)
    u32 programs = 0;
    bool warnedAddress_ = false;

private:
    void Execute(u32 code, const u32* data);
    void Unpack(u32 code, const u32* data, u32 words);
    void StartProgram(u32 address, bool resume);
    u32 DataWords(u32 code) const;

    u32 index_;
    Vu& vu_;
    Gif* gif_;
    bool stalled_ = false;
    // A code waiting for its data
    bool pending_ = false;
    u32 pendingCode_ = 0;
    u32 pendingWords_ = 0;
    std::vector<u32> pendingData_;
};
}
