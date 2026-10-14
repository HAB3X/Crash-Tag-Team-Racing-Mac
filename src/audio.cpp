// Radical's audio IOP module (AUDIO.IRX: runtime/code/audio/src/hal/device/ps2/iop/), high level: its two RPC servers and its
// voice thread, read from the module's code (addresses below are the module's, relocated to 0). Both servers check the EE's
// version (0x145) first. The SPU2 is the runtime's, driven through libsd's calls as the module makes them.
//
// The sound memory the module plays from is cut into pages (ioppage.c): pages 0 up to the second run's first page from one
// address with one page size, the rest from another address with another size. Page 0 is the silent loop the load server's
// init puts at its address. Each page has flags (bit 0: the last page of a sound, which ends at an offset the EE gives; bit 1:
// a page no sound has, a voice reaching it is sent to the silence) and the page after it, both the EE's. A sound is a chain of
// pages whose data's first line in each page is flagged loop start (6: start and repeat) and its last loop end (3: end and
// repeat): a voice loops on the page it plays until the voice thread points its loop address (LSAX) at the page after it, so
// a chain plays on whatever order its pages are in, and a stream's pages are refilled behind the voice.
//
// The load server (0x11):
//   0 init: { version, fill the SPU with garbage (a debug option), the DVD transfer buffer's bytes, the memory transfer
//     buffer's, the uninterleave buffer's, the clear buffer's, the debug channel, the silence's SPU address, its size }. The
//     four buffers are allocated in the IOP's memory; a silent looping block of the size is put at the SPU address (ADPCM
//     lines of zeros, each header 0xC0, the first's flags 6, the last's 3). Answers { 1, the DVD buffer, the memory buffer }
//   0x11 ClearBuffer (0xC14), 128 bytes: { the start offset in the first page, the end offset in the last, the bytes per
//     channel, the channels, each channel's first page }: silence (the clear buffer) written over each channel's pages from
//     the start offset, with the pages' loop flags
//   0x12 UpdateAudioPageNexts (0xFBC), 0x2040 bytes: 0x800 pages' { next page, flags } (16 bits each), then a first page and
//     an end page (0x2000, 0x2004): the entries from the first page up to the end page taken (the module loops to the end
//     page, though its assert reads the second word as a count)
//   0x13 LoadBuffer (0xA8C), 128 bytes: { the data's IOP address, the start offset, the end offset, the bytes per channel,
//     the channels, each channel's first page }: the data (the channels interleaved a 16 byte line at a time, read from the
//     disc or sent by the EE into the module's buffers) gets the pages' loop flags, then each channel is taken out of it and
//     written into its pages from the start offset
// The voice server (0x12):
//   0 init (0x1F20): { version, the voice thread's period in microseconds, its priority, the pages: the first run's address,
//     the end address, the second run's address, the second run's first page, the first run's page size, the second's, the
//     page count }: answers 1
//   0x10 the voices (0x1830), every frame, 1024 bytes: { a word echoed back, then for each core's 24 voices 20 bytes (the
//     pitch, VOLL, VOLR, what to set: bit 0 SSA, bit 1 LSA (not supported: the module says so), bit 2 the pitch, bit 3 VOLL,
//     bit 4 VOLR; SSA at 8, LSA at 12, the loop mode at 16), then from 0x3C4 each core's KON, KOFF, VMIXEL, VMIXER, VMIXL,
//     VMIXR (core 0's, core 1's), and the voice whose addresses the module reads back for its debugging (0x3F4, 0x3F8) }. A
//     voice's entry is taken only when its bits aren't all 0. The mixes are set, the keys off, then on (with libsd's wait
//     after each), the loop addresses updated, and each voice keyed on or off noted as playing or not. Answers 256 bytes:
//     { the word, each core's voices in the silent page (a bit each), each voice's NAX }
//   The voice thread (0x202C) runs every period: for each voice, the page its NAX is in decides its loop address. A page with
//   bit 1: the silence. A sound's last page (bit 0): the silence unless the voice plays and its loop mode isn't 0, then the
//   next page (its second line for loop mode 1). Any other page: the next page
#include "ps2/iop.h"
#include "ps2/sound.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
using namespace Ps2;

constexpr u32 Version = 0x145;
constexpr u32 Line = 16;
constexpr u8 FlagsStart = 6; // loop start and repeat
constexpr u8 FlagsEnd = 3;   // loop end and repeat
constexpr u32 Voices = 48;

bool g_trace = getenv("CTTR_AUDIO_TRACE") != nullptr;

void Unhandled(const RpcCall& c)
{
    if (Once("audio " + std::to_string(c.sid) + " " + std::to_string(c.fno)))
    {
        Log("audio module: server %x function %#x not done (%u bytes in, %u out)", c.sid, c.fno, c.inSize, c.recvSize);
    }
}

bool VersionOk(const RpcCall& c)
{
    if (c.In32(0) != Version)
    {
        Log("audio module: server %x's version %#x, not %#x", c.sid, c.In32(0), Version);
        return false;
    }
    return true;
}

bool SizeOk(const RpcCall& c, u32 size, const char* name)
{
    if (c.inSize != size)
    {
        Log("audio module: %s with %u bytes, not %u: not done", name, c.inSize, size);
        return false;
    }
    return true;
}

// --- the pages (ioppage.c, 0x0-0x660)
struct Pages
{
    u32 first = 0;      // page 0's address
    u32 end = 0;        // past the last page
    u32 second = 0;     // the second run's address
    u32 secondPage = 0; // its first page
    u32 firstSize = 0;  // the first run's page size
    u32 secondSize = 0; // the second's
    u32 count = 0;
    std::vector<u16> flags;
    std::vector<u16> next;

    bool Valid(u32 page) const
    {
        if (page < count)
        {
            return true;
        }
        if (Once("audio page " + std::to_string(page)))
        {
            Log("audio module: page %u of %u", page, count);
        }
        return false;
    }
    // An address outside the pages is page 0's
    u32 PageOf(u32 address) const
    {
        if (address < first || address >= end)
        {
            return 0;
        }
        if (address < second)
        {
            return (address - first) / firstSize;
        }
        return (address - second) / secondSize + secondPage;
    }
    u32 Address(u32 page) const
    {
        return page < secondPage ? first + page * firstSize : second + (page - secondPage) * secondSize;
    }
    u32 Size(u32 page) const { return page < secondPage ? firstSize : secondSize; }
    u32 Next(u32 page) const { return Valid(page) ? next[page] : 0; }
    bool Last(u32 page) const { return Valid(page) && (flags[page] & 1) != 0; }
    bool Free(u32 page) const { return Valid(page) && (flags[page] & 2) != 0; }
    bool InSilence(u32 address) const { return address >= first && address < first + firstSize; }
};
Pages g_pages;

// --- the load server's buffers (0x6EC8: each one's size and address)
u32 g_clear = 0;
u32 g_clearBytes = 0;
u32 g_uninterleave = 0;
u32 g_uninterleaveBytes = 0;

// DMA_PAGES_TO_SPU (0x139C): bytes from the IOP's memory into the sound memory, from an offset in a page on along its chain.
// The module's transfers (0x1484) send the whole 64 byte blocks, then the rest over a 64 byte block read back from the sound
// memory: the bytes given are what changes either way
void PagesToSpu(u32 source, u32 bytes, u32 page, u32 offset)
{
    for (u32 done = 0; done < bytes;)
    {
        u32 n = std::min(g_pages.Size(page) - offset, bytes - done);
        SdVoiceTransWrite(g_pages.Address(page) + offset, IopMemory(source + done), n);
        done += n;
        offset = 0;
        page = g_pages.Next(page);
    }
}

void LoadInit(RpcCall& c)
{
    if (!VersionOk(c))
    {
        c.Out32(0, 0);
        return;
    }
    if (c.In32(4) != 0)
    {
        // A debug option: the sound memory from 0x6000 on filled with the module's 64 bytes of garbage, which nothing should
        // play
        Log("audio module: load server: the SPU's garbage fill asked for, not done");
    }
    u32 dvdBytes = c.In32(8), memoryBytes = c.In32(12);
    g_uninterleaveBytes = c.In32(16);
    g_clearBytes = c.In32(20);
    u32 silenceAddress = c.In32(28), silenceBytes = c.In32(32);
    u32 dvd = IopAlloc(dvdBytes), memory = IopAlloc(memoryBytes);
    g_uninterleave = IopAlloc(g_uninterleaveBytes);
    g_clear = IopAlloc(g_clearBytes);
    // The clear buffer: ADPCM lines of zeros (headers 0xC0); the silent loop made of it, its first line flagged loop start,
    // its last loop end
    u8* block = IopMemory(g_clear);
    std::memset(block, 0, g_clearBytes);
    for (u32 at = 0; at < g_clearBytes; at += Line)
    {
        block[at] = 0xC0;
    }
    block[1] = FlagsStart;
    block[silenceBytes - 15] = FlagsEnd;
    {
        auto lock = SoundLock();
        SdVoiceTransWrite(silenceAddress, block, silenceBytes);
    }
    block[1] = 0;
    block[silenceBytes - 15] = 0;
    Log("audio module: load server: buffers at %05x (DVD, %u bytes), %05x (memory, %u bytes), %05x (uninterleave, %u bytes), "
        "%05x (clear, %u bytes); silence at %05x",
        dvd, dvdBytes, memory, memoryBytes, g_uninterleave, g_uninterleaveBytes, g_clear, g_clearBytes, silenceAddress);
    c.Out32(0, 1);
    c.Out32(4, dvd);
    c.Out32(8, memory);
}

// ClearBuffer (0xC14)
void ClearBuffer(RpcCall& c)
{
    if (!SizeOk(c, 0x80, "ClearBuffer"))
    {
        return;
    }
    u32 startOffset = c.In32(0), endOffset = c.In32(4), bytesPerChannel = c.In32(8), channels = c.In32(12);
    if (g_trace)
    {
        Log("audio clear: offsets %x-%x, %u bytes, %u channels, pages %u %u", startOffset, endOffset, bytesPerChannel,
            channels, c.In32(16), c.In32(20));
    }
    auto lock = SoundLock();
    u8* clear = IopMemory(g_clear);
    for (u32 channel = 0; channel < channels; channel++)
    {
        u32 page = c.In32(16 + channel * 4);
        u32 offset = startOffset;
        for (u32 done = 0; done < bytesPerChannel;)
        {
            u32 size = g_pages.Size(page);
            if (size - offset > g_clearBytes)
            {
                Log("audio module: ClearBuffer of %u bytes of page %u, the clear buffer has %u: not done", size - offset,
                    page, g_clearBytes);
                return;
            }
            // The loop flags put in the clear buffer for this page and taken out after
            u8* start = nullptr;
            if (offset == 0)
            {
                start = clear + 1;
                *start = FlagsStart;
            }
            u8* end = nullptr;
            if (!g_pages.Last(page))
            {
                end = clear + size - offset - 15;
            }
            else if (offset < endOffset)
            {
                end = clear + endOffset - offset - 15;
            }
            if (end != nullptr)
            {
                *end = FlagsEnd;
            }
            u32 n = std::min(size - offset, bytesPerChannel - done);
            PagesToSpu(g_clear, n, page, offset);
            done += n;
            offset = 0;
            if (start != nullptr)
            {
                *start = 0;
            }
            if (end != nullptr)
            {
                *end = 0;
            }
            page = g_pages.Next(page);
        }
    }
}

// UpdateAudioPageNexts (0xFBC, 0x1B0)
void UpdatePageNexts(RpcCall& c)
{
    if (!SizeOk(c, 0x2040, "UpdateAudioPageNexts"))
    {
        return;
    }
    u32 firstPage = c.In32(0x2000), endPage = c.In32(0x2004);
    auto lock = SoundLock();
    if (firstPage + endPage > g_pages.count)
    {
        Log("audio module: UpdateAudioPageNexts of pages %u and %u on, of %u: not done", firstPage, endPage, g_pages.count);
        return;
    }
    for (u32 page = firstPage; page < endPage; page++)
    {
        g_pages.next[page] = c.In16(page * 4);
        g_pages.flags[page] = c.In16(page * 4 + 2);
    }
}

// The loop flags put into data about to be loaded (0x102C): the channels interleaved a line at a time, so each place in a page
// is the channels' lines together. Every channel's pages are taken to be laid out as the first channel's
void MarkLoops(u32 data, u32 startOffset, u32 endOffset, u32 bytesPerChannel, u32 channels, u32 page)
{
    u32 end = data + bytesPerChannel * channels;
    u32 offset = startOffset * channels;
    u32 last = endOffset * channels;
    for (u32 at = data; at < end; page = g_pages.Next(page))
    {
        if (offset == 0)
        {
            for (u32 i = 0; i < channels; i++)
            {
                IopMemory(at + i * Line)[1] = FlagsStart;
            }
        }
        bool lastPage = g_pages.Last(page);
        if (!lastPage)
        {
            at += g_pages.Size(page) * channels - offset;
        }
        else if (offset < last)
        {
            at += last - offset;
        }
        if (at > end)
        {
            break;
        }
        offset = 0;
        for (u32 i = 0; i < channels; i++)
        {
            IopMemory(at - (i + 1) * Line)[1] = FlagsEnd;
        }
        if (lastPage)
        {
            break;
        }
    }
}

// LoadBuffer (0xA8C)
void LoadBuffer(RpcCall& c)
{
    if (!SizeOk(c, 0x80, "LoadBuffer"))
    {
        return;
    }
    u32 data = c.In32(0), startOffset = c.In32(4), endOffset = c.In32(8), bytesPerChannel = c.In32(12),
        channels = c.In32(16);
    if (g_trace)
    {
        Log("audio load: %05x, offsets %x-%x, %u bytes, %u channels, pages %u %u", data, startOffset, endOffset,
            bytesPerChannel, channels, c.In32(20), c.In32(24));
    }
    auto lock = SoundLock();
    MarkLoops(data, startOffset, endOffset, bytesPerChannel, channels, c.In32(20));
    if (channels < 2)
    {
        PagesToSpu(data, bytesPerChannel, c.In32(20), startOffset);
        return;
    }
    if (bytesPerChannel > g_uninterleaveBytes)
    {
        Log("audio module: LoadBuffer of %u bytes a channel, the uninterleave buffer has %u: not done", bytesPerChannel,
            g_uninterleaveBytes);
        return;
    }
    for (u32 channel = 0; channel < channels; channel++)
    {
        // The channel's lines out of the data (0x1338)
        for (u32 at = 0; at < bytesPerChannel; at += Line)
        {
            std::memcpy(IopMemory(g_uninterleave + at), IopMemory(data + (at / Line * channels + channel) * Line), Line);
        }
        PagesToSpu(g_uninterleave, bytesPerChannel, c.In32(20 + channel * 4), startOffset);
    }
}

void LoadServer(RpcCall& c)
{
    switch (c.fno)
    {
    case 0:
        LoadInit(c);
        break;
    case 0x11:
        ClearBuffer(c);
        break;
    case 0x12:
        UpdatePageNexts(c);
        break;
    case 0x13:
        LoadBuffer(c);
        break;
    default:
        Unhandled(c);
        break;
    }
}

// --- the voice server and its thread
u16 Entry(u32 voice) { return static_cast<u16>((voice / 24) | ((voice % 24) << 1)); }

// Each voice keyed on and not off since (0x6EF4), and its loop mode (0x6FB4)
bool g_playing[Voices] = {};
u32 g_loopMode[Voices] = {};
// The loop addresses of voices keyed on, written once the SPU2 has started them (KeyAndLoop), and the timer that writes them
std::vector<std::pair<u32, u32>> g_keyedLoops;
int g_keyedTimer = -1;

// The loop address the voice thread gives a voice whose NAX is at an address (0x1BF0)
u32 LoopAddress(u32 voice, u32 nax)
{
    u32 page = g_pages.PageOf(nax);
    if (g_pages.Free(page))
    {
        return g_pages.first;
    }
    if (!g_pages.Last(page))
    {
        return g_pages.Address(g_pages.Next(page));
    }
    if (!g_playing[voice] || g_loopMode[voice] == 0)
    {
        return g_pages.first;
    }
    return g_pages.Address(g_pages.Next(page)) + (g_loopMode[voice] == 1 ? Line : 0);
}

// The voice thread's work (0x202C)
void UpdateLoops()
{
    for (u32 v = 0; v < Voices; v++)
    {
        SdSetAddr(SD_VA_LSAX | Entry(v), LoopAddress(v, SdGetAddr(SD_VA_NAX | Entry(v))));
    }
}

void VoiceInit(RpcCall& c)
{
    if (!VersionOk(c))
    {
        c.Out32(0, 0);
        return;
    }
    auto lock = SoundLock();
    Pages& p = g_pages;
    p.first = c.In32(12);
    p.end = c.In32(16);
    p.second = c.In32(20);
    p.secondPage = c.In32(24);
    p.firstSize = c.In32(28);
    p.secondSize = c.In32(32);
    p.count = c.In32(36);
    p.flags.assign(p.count, 0);
    p.next.assign(p.count, 0);
    // The thread's period in microseconds, as the SPU2's samples
    u32 period = std::max<u32>(1, static_cast<u32>(static_cast<u64>(c.In32(4)) * 48000 / 1000000));
    SoundSetTimer(period, UpdateLoops);
    Log("audio module: voice server: %u pages: from %05x of %u bytes, from %05x (page %u) of %u bytes, to %05x; the thread "
        "every %u us",
        p.count, p.first, p.firstSize, p.second, p.secondPage, p.secondSize, p.end, c.In32(4));
    c.Out32(0, 1);
}

// Each voice's entry, taken when its bits aren't all 0
void SetVoices(const RpcCall& c)
{
    for (u32 v = 0; v < Voices; v++)
    {
        u32 at = 4 + (v / 24) * 0x1E0 + (v % 24) * 0x14;
        u16 set = c.In16(at + 6);
        if (set == 0)
        {
            continue;
        }
        if ((set & 4) != 0)
        {
            SdSetParam(SD_VP_PITCH | Entry(v), c.In16(at));
        }
        if ((set & 8) != 0)
        {
            SdSetParam(SD_VP_VOLL | Entry(v), c.In16(at + 2));
        }
        if ((set & 0x10) != 0)
        {
            SdSetParam(SD_VP_VOLR | Entry(v), c.In16(at + 4));
        }
        if ((set & 1) != 0)
        {
            SdSetAddr(SD_VA_SSA | Entry(v), c.In32(at + 8));
        }
        if ((set & 2) != 0 && Once("audio set loop"))
        {
            Log("audio module: a voice's loop address set: not supported (the module says so)");
        }
        g_loopMode[v] = c.In32(at + 16);
        if (g_trace)
        {
            Log("audio voice %u: set %x pitch %04x vol %04x %04x ssa %05x loop %u", v, set, c.In16(at), c.In16(at + 2),
                c.In16(at + 4), c.In32(at + 8), c.In32(at + 16));
        }
    }
}

// The keys, then the loop addresses as the module's update after them: the NAX each voice has for the module in nax.
// The module waits after the keys (libsd's two samples: 84 us), so the voices it reads back have started. The SPU2 takes the
// keys at its next sample: a voice keyed on is at its SSA for the module, and its loop address is written once it has started
// (a voice's start takes its loop address from its first line's flags), 4 samples on as the module's is
void KeyAndLoop(const u32 keyOn[2], const u32 keyOff[2], u32 nax[Voices])
{
    for (u16 core = 0; core < 2; core++)
    {
        SdSetSwitch(SD_S_KOFF | core, keyOff[core]);
    }
    for (u16 core = 0; core < 2; core++)
    {
        SdSetSwitch(SD_S_KON | core, keyOn[core]);
    }
    for (u32 v = 0; v < Voices; v++)
    {
        bool on = (keyOn[v / 24] >> (v % 24) & 1) != 0;
        nax[v] = SdGetAddr((on ? SD_VA_SSA : SD_VA_NAX) | Entry(v));
        u32 loop = LoopAddress(v, nax[v]);
        if (on)
        {
            g_keyedLoops.emplace_back(v, loop);
        }
        else
        {
            SdSetAddr(SD_VA_LSAX | Entry(v), loop);
        }
    }
    if (!g_keyedLoops.empty() && g_keyedTimer < 0)
    {
        g_keyedTimer = SoundAddTimer(4, [] {
            for (auto [v, loop] : g_keyedLoops)
            {
                SdSetAddr(SD_VA_LSAX | Entry(v), loop);
            }
            g_keyedLoops.clear();
            SoundRemoveTimer(g_keyedTimer);
            g_keyedTimer = -1;
        });
    }
    // Noted after the update, as the module does
    for (u32 v = 0; v < Voices; v++)
    {
        u32 bit = 1u << (v % 24);
        if ((keyOff[v / 24] & bit) != 0)
        {
            g_playing[v] = false;
        }
        if ((keyOn[v / 24] & bit) != 0)
        {
            g_playing[v] = true;
        }
    }
    if (g_trace && (keyOn[0] | keyOn[1] | keyOff[0] | keyOff[1]) != 0)
    {
        Log("audio voices: on %06x %06x off %06x %06x", keyOn[0], keyOn[1], keyOff[0], keyOff[1]);
    }
}

// The voices (0x1830)
void VoiceUpdate(RpcCall& c)
{
    if (c.inSize != 0x400)
    {
        Log("audio module: voice update with %u bytes, not 1024: not done", c.inSize);
        c.Out32(0, 0);
        return;
    }
    auto lock = SoundLock();
    SetVoices(c);
    for (u16 core = 0; core < 2; core++)
    {
        SdSetSwitch(SD_S_VMIXEL | core, c.In32(0x3D4 + core * 4));
        SdSetSwitch(SD_S_VMIXER | core, c.In32(0x3DC + core * 4));
        SdSetSwitch(SD_S_VMIXL | core, c.In32(0x3E4 + core * 4));
        SdSetSwitch(SD_S_VMIXR | core, c.In32(0x3EC + core * 4));
    }
    u32 keyOn[2] = {c.In32(0x3C4), c.In32(0x3C8)};
    u32 keyOff[2] = {c.In32(0x3CC), c.In32(0x3D0)};
    u32 nax[Voices];
    KeyAndLoop(keyOn, keyOff, nax);
    // The answer: the word, each core's voices in the silent page, each voice's NAX
    c.out.assign(0x100, 0);
    c.Out32(0, c.In32(0));
    u32 silent[2] = {};
    for (u32 v = 0; v < Voices; v++)
    {
        if (g_pages.InSilence(nax[v]))
        {
            silent[v / 24] |= 1u << (v % 24);
        }
        c.Out32(12 + v * 4, nax[v]);
    }
    c.Out32(4, silent[0]);
    c.Out32(8, silent[1]);
}

void VoiceServer(RpcCall& c)
{
    switch (c.fno)
    {
    case 0:
        VoiceInit(c);
        break;
    case 0x10:
        VoiceUpdate(c);
        break;
    default:
        Unhandled(c);
        break;
    }
}
}

void CttrRegisterAudio()
{
    Ps2::IopRegisterServer(0x11, "audio load", LoadServer);
    Ps2::IopRegisterServer(0x12, "audio voice", VoiceServer);
}
