// The EE's side of the IPU as the game's libraries drive it, through the runtime's registers only (ps2_io_*): the commands
// and the waits on IPU_CTRL's BUSY (Sony's libmpeg polls it), bits read with FDEC and IPU_TOP, VDEC, and toIPU fed from a
// ring of 2 KB blocks in chain mode (REF tags, REFE on the last block filled, a NEXT back to the start), suspended, refilled
// and let go again as CRI's ring code does it (0x170b20 in File 2's executable); sceIpuStopDMA / sceIpuRestartDMA around
// the colour conversion, as libmpeg does them
#pragma once

#include "ps2/recomp.h"

#include <chrono>
#include <cstdint>
#include <vector>

namespace IpuTest
{
// The guest's memory layout of the test
constexpr u32 TagBase = 0x00100000;
constexpr u32 RingBase = 0x00110000;
constexpr u32 RingBlocks = 64; // at most; --ring-blocks uses fewer (a ring smaller than a picture starves the IPU)
constexpr u32 BlockQw = 0x80;
constexpr u32 Raw8Base = 0x00200000;   // a picture's macroblocks for CSC (384 bytes each)
constexpr u32 RgbBase = 0x00300000;    // CSC's RGB32 out
constexpr u32 SpareBase = 0x00500000;  // other checks' data
constexpr u32 SprMacroblock = 0x80000000; // BDEC's output: the scratchpad's start, as libmpeg puts it

u32 Read32(u32 a);
u64 Read64(u32 a);
void Write32(u32 a, u32 v);
u8* Guest(u32 a);

// Seconds spent in the IPU's own work (the register writes that run its commands), for the timing
double IpuSeconds();

class Driver
{
public:
    Driver(const std::vector<u8>& video, u32 ringBlocks) : video_(video), blocks_(ringBlocks) {}

    // A command written and waited for; false when the IPU waits for data the stream has no more of
    bool Command(u32 word);
    bool WaitIdle();
    u32 Ctrl() { return Read32(0x10002010); }
    // The bitstream's next bits (1-32) through IPU_TOP, and skipping with FDEC's FB
    bool Peek(u32 bits, u32& value);
    bool Skip(u32 bits);
    bool Get(u32 bits, u32& value);
    bool AlignToByte();
    // The next start code's last byte (the position after it), or false at the stream's end
    bool NextStartCode(u32& code);
    // VDEC with a table: IPU_CMD's word
    bool Vdec(u32 table, u32& word);

    // toIPU: the ring filled as far as there's room (suspending a running transfer and letting it go), the transfer started
    // the first time. Returns whether anything was added
    bool Feed();
    bool StreamDone() const { return fedQw_ * 16 >= video_.size(); }

    // sceIpuStopDMA / sceIpuRestartDMA: toIPU stopped and its place kept with the IPU's (IPU_BP), then put back: rewound by
    // what the IPU held (IFC + FP), BCLR with BP, toIPU going on
    void StopDma();
    void RestartDma();

    // Counts for the report
    u32 feedsWhileRunning = 0;
    u32 feedsAfterEnd = 0;
    u32 restarts = 0;
    u32 restartsAcrossBlocks = 0;

private:
    // The stream's quadword the toIPU transfer gives next, from its registers
    u64 DmaPosition();
    u32 BlockOfDma();

    const std::vector<u8>& video_;
    u32 blocks_;
    u64 fedQw_ = 0;         // stream quadwords put in the ring
    bool started_ = false;
    u32 head_ = 0;          // oldest block not yet passed by the DMA
    u32 count_ = 0;         // blocks filled and not passed
    u64 blockStream_[RingBlocks] = {}; // each block's first stream quadword
    u32 blockQw_[RingBlocks] = {};
    // StopDma's saved state
    u32 savedChcr_ = 0, savedMadr_ = 0, savedQwc_ = 0, savedTadr_ = 0, savedBp_ = 0;
};
}
