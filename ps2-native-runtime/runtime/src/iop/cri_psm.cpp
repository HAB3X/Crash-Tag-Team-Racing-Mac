// The ADX driver's sound output (PSM Ver 1.01 in its module): each stream's channels read 16 bit PCM from SJs, RCV converts
// it to 48 kHz into a 0x400 byte ring per channel (PSMSLT, PSMTBF), PSMMIX mixes the rings 256 samples at a time with each
// channel's volumes into an output's two SJs (left and right), and PSMOUT plays the output's 0x1000 byte buffer round through a
// core's input (sceSdBlockTrans's loop), refilling the half just played at each of the transfer's interrupts. Offsets are the
// module's (PSM_* 0x1530-0x29B0, PSMMIX_* 0x2B50-0x3C60, PSMOUT_* 0x3C60-0x4700, PSMSLT_* 0x4700-0x4D60, RCV_* 0x6F70-0x8960)
#include "cri_adx.h"
#include "ps2/sound.h"

#include <algorithm>
#include <cstring>

namespace Ps2::CriAdx
{
constexpr s32 OutRate = 48000;

// RCV: a channel's converter from its input's rate to 48 kHz, at most 512 samples and at least 32 a time (PSMSLT_Create)
struct Converter
{
    enum class Kind
    {
        Copy,    // rcv_copy (0x7E88): the same rate
        Twofold, // rcv_twofold (0x7F04): twice the rate
    };
    bool running = false;
    s32 inRate = 0;
    s32 outRate = OutRate;
    s32 most = 0x200;
    s32 least = 0x20;
    Kind kind = Kind::Copy;
    s32 ratio = 1;     // output samples an input sample makes at most (+0x3C)
    bool owed = false; // twofold: the last input sample still to be output after its midpoint (+0x14)
    s16 last = 0;      // twofold: the last input sample (+0x18)
};

struct Slot
{
    bool used = false;
    bool started = false;
    Sj* input = nullptr;
    Sj* ring = nullptr;
    Converter rcv;
};

struct Psm
{
    bool used = false;
    bool playing = false;
    s32 channels = 0;  // its channels (maxnch, +2)
    s32 active = 0;    // the ones played (+3)
    std::array<Slot*, 2> slots = {};
    std::array<Sj*, 2> inputs = {};
    std::array<std::unique_ptr<Sj>, 2> rings;
    std::array<s32, 2> taps = {-1, -1};
    std::array<s32, 2> buffers = {-1, -1};
    s32 rate = 0;
    std::array<s32, 2> volumes = {};
    std::array<s32, 2> pans = {};
    s32 mix = -1;      // the core whose mixer has its channels, -1 none (+0x48)
};

namespace
{
constexpr u32 OutBytes = 0x1000;
constexpr u32 BlockBytes = 0x200;
constexpr u32 OutBlocks = OutBytes / BlockBytes;
constexpr u32 RingBytes = 0x400;
constexpr s32 Taps = 32;

struct Mixer
{
    bool used = false;
    bool playing = false;
    s32 core = 0;
    std::array<Sj*, Taps> taps = {};
    std::array<std::array<s32, 2>, Taps> volumes = {};
    std::array<Sj*, 2> outs = {};
    s32 total = 0; // the outputs' bytes (+0xC)
};

struct Output
{
    bool used = false;
    bool started = false;
    s32 core = 0;
    u32 buffer = 0;
    u32 lastHalf = 0;
    std::array<std::unique_ptr<Sj>, 2> sjs;
};

struct State
{
    s32 core = 1;  // psm_core_no (ADXI_SetupSnd: the module's spucore= argument)
    s32 inits = 0;
    u32 moduleMemory = 0;
    std::array<Psm, 16> psms;
    std::array<Slot, 32> slots;  // as many as RCV has converters
    std::array<u32, 64> buffers = {}; // PSMTBF: each one's 0x400 bytes (0: none given)
    std::array<bool, 64> buffersUsed = {};
    std::array<Mixer, 2> mixers;
    std::array<Output, 2> outputs;
};
State g_psm;

// The module's error function (PSMERR_CallErrFunc1 with its message): the call fails
void PsmError(const char* message)
{
    if (Once(std::string("cri psm ") + message))
    {
        Log("CRI ADX: %s", message);
    }
}

s16 Sample(u32 address)
{
    s16 v;
    std::memcpy(&v, IopMemory(address), 2);
    return v;
}

// --- RCV: the converters (rcv_conv_init 0x7238 chooses one from the rates)
void ConverterStart(Converter& c)
{
    // rcv_check_param: a rate not set is 44.1 kHz
    if (c.inRate <= 0)
    {
        c.inRate = 44100;
    }
    if (c.outRate <= 0)
    {
        c.outRate = 44100;
    }
    c.last = 0;
    if (c.inRate == c.outRate)
    {
        c.kind = Converter::Kind::Copy;
        c.ratio = 1;
        c.owed = false;
    }
    else if (c.outRate == 2 * c.inRate)
    {
        // Its first output is the zero before the stream
        c.kind = Converter::Kind::Twofold;
        c.ratio = 2;
        c.owed = true;
    }
    else
    {
        // The module also has three- and fourfold, pitch-stepped up and down sampling and decimation; neither Outbreak
        // disc has a stream at a rate that needs them (their ADX files are 48 kHz stereo and 24 kHz mono)
        Fatal("CRI ADX: a stream at %d Hz played at %d Hz: RCV's conversion between them isn't done", c.inRate, c.outRate);
    }
    c.running = true;
}

// One input chunk converted into one output chunk (in and out: their samples; set to the samples used and made)
void Convert(Converter& c, u32 from, s32& in, u32 to, s32& out)
{
    auto put = [&](s16 v) {
        std::memcpy(IopMemory(to), &v, 2);
        to += 2;
    };
    if (c.kind == Converter::Kind::Copy)
    {
        s32 n = std::min(in, out);
        std::memcpy(IopMemory(to), IopMemory(from), static_cast<size_t>(n) * 2);
        in = out = n;
        return;
    }
    // rcv_twofold: each input sample makes the midpoint from the one before, then itself; a sample whose midpoint fits but
    // not itself is owed to the next call's first output
    s32 room = out - (c.owed ? 1 : 0);
    s32 pairs = std::min(room >> 1, in - 1);
    s32 made = pairs * 2;
    if (c.owed)
    {
        put(c.last);
        made++;
        c.owed = false;
    }
    s32 used = 0;
    for (; used < pairs; used++)
    {
        s16 x = Sample(from + static_cast<u32>(used) * 2);
        put(static_cast<s16>((c.last + x) >> 1));
        put(x);
        c.last = x;
    }
    if (room & 1)
    {
        s16 x = Sample(from + static_cast<u32>(used) * 2);
        put(static_cast<s16>((c.last + x) >> 1));
        c.last = x;
        c.owed = true;
        used++;
        made++;
    }
    in = used;
    out = made;
}

// rcv_convert_proc2 (0x7980): as many samples as the input has, the ring has room for and the converter makes at once, chunk
// by chunk: what each chunk gave is put back as room, its rest back as data, and the ring's likewise
void ConverterRun(Slot& s)
{
    Converter& c = s.rcv;
    s32 in = static_cast<s32>(static_cast<u32>(s.input->Data(1)) >> 1);
    if (in <= 0)
    {
        return;
    }
    s32 room = static_cast<s32>(static_cast<u32>(s.ring->Data(0)) >> 1);
    if (room <= 0)
    {
        return;
    }
    s32 make = std::min(in * c.ratio, room);
    if (make < c.least)
    {
        return;
    }
    make = std::min(make, c.most);
    s32 take = make / c.ratio;
    s32 made = 0, taken = 0;
    while (made < make)
    {
        Chunk from = s.input->Get(1, (take - taken) * 2);
        Chunk to = s.ring->Get(0, (make - made) * 2);
        s32 used = static_cast<s32>(static_cast<u32>(from.size) >> 1);
        s32 out = static_cast<s32>(static_cast<u32>(to.size) >> 1);
        if (used <= 0 || out <= 0)
        {
            // The module's converters would run on with no input (twofold reads before it) or spin here for good
            Fatal("CRI ADX: RCV has an input chunk of %d bytes for an output chunk of %d (%d of %d made)", from.size, to.size,
                  made, make);
        }
        Convert(c, from.data, used, to.data, out);
        s.input->Put(0, {from.data, used * 2});
        s.input->Unget(1, {from.data + static_cast<u32>(used * 2), from.size - used * 2});
        s.ring->Put(1, {to.data, out * 2});
        s.ring->Unget(0, {to.data + static_cast<u32>(out * 2), to.size - out * 2});
        if (out == 0)
        {
            Fatal("CRI ADX: RCV made no samples from %d bytes into %d (%d of %d made): the module would spin here", from.size,
                  to.size, made, make);
        }
        made += out;
        taken += used;
    }
}

// --- PSMMIX
s32 MixerAllocTap(Mixer& m, Sj* source)
{
    for (s32 i = 0; i < Taps; i++)
    {
        if (m.taps[i] == nullptr)
        {
            m.taps[i] = source;
            return i;
        }
    }
    // PSMMIX_AllocTap answers 32, which PSM_SetMix takes for a tap: the channel is never mixed
    PsmError("PSMMIX: no tap left: a channel isn't mixed");
    return Taps;
}

void MixerFreeTap(Mixer& m, s32 tap)
{
    if (tap >= 0 && tap < Taps)
    {
        m.taps[tap] = nullptr;
    }
}

void MixerSetVolume(Mixer& m, s32 tap, s32 side, s32 volume)
{
    if (tap >= 0 && tap < Taps && side >= 0 && side < 2)
    {
        m.volumes[tap][side] = std::clamp(volume, 0, 255);
    }
}

// psmmix_sji_to_tmpbuf3: a tap's 256 samples added at a volume (255 as they are, 128 and 64 by shifts, others times v / 256)
void MixTap(s32* sum, u32 samples, s32 volume)
{
    for (u32 i = 0; i < BlockBytes / 2; i++)
    {
        s32 v = Sample(samples + i * 2);
        switch (volume)
        {
        case 255:
            sum[i] += v;
            break;
        case 0:
            break;
        case 128:
            sum[i] += v >> 1;
            break;
        case 64:
            sum[i] += v >> 2;
            break;
        default:
            sum[i] += (v * volume) >> 8;
            break;
        }
    }
}

// psmmix_trans_blk2 (0x3360): one block of 256 samples from every tap that has a whole one, into each output
void MixBlock(Mixer& m)
{
    struct Taken
    {
        s32 tap;
        Chunk chunk;
    };
    std::vector<Taken> taken;
    for (s32 t = 0; t < Taps; t++)
    {
        if (m.taps[t] == nullptr)
        {
            continue;
        }
        Chunk c = m.taps[t]->Get(1, BlockBytes);
        if (c.size < static_cast<s32>(BlockBytes))
        {
            m.taps[t]->Unget(1, c);
            continue;
        }
        taken.push_back({t, c});
    }
    for (s32 side = 0; side < 2; side++)
    {
        Chunk out = m.outs[side]->Get(0, BlockBytes);
        if (taken.empty())
        {
            std::memset(IopMemory(out.data), 0, static_cast<size_t>(std::max(out.size, 0)));
        }
        else
        {
            s32 sum[BlockBytes / 2] = {};
            for (const Taken& t : taken)
            {
                MixTap(sum, t.chunk.data, m.volumes[t.tap][side]);
            }
            // psmmix_clip_dat3: to +-0x7FFF
            s16 clipped[BlockBytes / 2];
            for (u32 i = 0; i < BlockBytes / 2; i++)
            {
                clipped[i] = static_cast<s16>(std::clamp(sum[i], -0x7FFF, 0x7FFF));
            }
            std::memcpy(IopMemory(out.data), clipped, static_cast<size_t>(std::clamp<s32>(out.size, 0, BlockBytes)));
        }
        m.outs[side]->Put(1, out);
    }
    for (const Taken& t : taken)
    {
        m.taps[t.tap]->Put(0, t.chunk);
    }
}

// PSMMIX_ExecHndl (0x3778): half the outputs' room a time, when there's that much
void MixerRun(Mixer& m)
{
    if (!m.playing)
    {
        return;
    }
    s32 bytes = m.total / 2 / 2;
    if (m.outs[0]->Data(0) < bytes)
    {
        return;
    }
    for (s32 i = 0; i < bytes / static_cast<s32>(BlockBytes); i++)
    {
        MixBlock(m);
    }
}

// --- PSMOUT
// psm_trans_cb (0x19C0): every started channel's conversion (PSMSLT_ExecServer), then the core's mixer
void Transferred(s32 core)
{
    for (Slot& s : g_psm.slots)
    {
        if (s.used && s.started && s.rcv.running)
        {
            ConverterRun(s);
        }
    }
    MixerRun(g_psm.mixers[core]);
}

// PSMOUT_ExecHndl (0x3C88), at the transfer's interrupt: the half just played back to the room, then the callback fills it
void OutputInterrupt(s32 core)
{
    Output& o = g_psm.outputs[core & 1];
    if (!o.started)
    {
        return;
    }
    u32 half = SdBlockTransStatus(core) >> 24;
    if (half != o.lastHalf)
    {
        o.lastHalf = half;
        u32 first = half != 0 ? 0 : OutBlocks / 2;
        for (u32 b = first; b < first + OutBlocks / 2; b++)
        {
            Sj& sj = *o.sjs[b % 2];
            sj.Put(0, sj.Get(1, BlockBytes));
        }
    }
    Transferred(core & 1);
}

void OutputStart(Output& o)
{
    for (u32 b = 0; b < OutBlocks; b++)
    {
        Sj& sj = *o.sjs[b % 2];
        Chunk c = sj.Get(0, BlockBytes);
        std::memset(IopMemory(c.data), 0, static_cast<size_t>(std::max(c.size, 0)));
        sj.Put(1, c);
    }
    SdBlockTransLoop(o.core, o.buffer, OutBytes, OutputInterrupt);
    SdSetParam(static_cast<u16>(SD_P_BVOLL | o.core), 0x7FFF);
    SdSetParam(static_cast<u16>(SD_P_BVOLR | o.core), 0x7FFF);
    o.started = true;
    PS2_LOG(LogIop, "CRI ADX: core %d's output started (IOP %06x)", o.core, o.buffer);
}

void OutputStop(Output& o)
{
    if (!o.started)
    {
        return;
    }
    SdBlockTransStop(o.core);
    SdSetParam(static_cast<u16>(SD_P_BVOLL | o.core), 0);
    SdSetParam(static_cast<u16>(SD_P_BVOLR | o.core), 0);
    o.started = false;
}

// psm_create_core (0x1AA8): the core's output (its buffer's blocks given round its two SJs, left and right) and mixer
void CreateCore(s32 core)
{
    Output& o = g_psm.outputs[core];
    o = {};
    o.used = true;
    o.core = core;
    o.buffer = g_psm.moduleMemory + static_cast<u32>(core) * OutBytes;
    // Each SJ's links (0x40 bytes: four) in the module's memory after the buffers
    for (u32 i = 0; i < 2; i++)
    {
        o.sjs[i] = MakeSjUni(0, g_psm.moduleMemory + 2 * OutBytes + (static_cast<u32>(core) * 2 + i) * 0x40, 0x40);
    }
    for (u32 b = 0; b < OutBlocks; b++)
    {
        o.sjs[b % 2]->Put(0, {o.buffer + b * BlockBytes, static_cast<s32>(BlockBytes)});
    }
    Mixer& m = g_psm.mixers[core];
    m = {};
    m.used = true;
    m.core = core;
    m.outs = {o.sjs[0].get(), o.sjs[1].get()};
    m.total = m.outs[0]->Data(1) + m.outs[0]->Data(0) + m.outs[1]->Data(1) + m.outs[1]->Data(0);
    m.playing = true;
}

// --- PSMSLT and PSMTBF
Slot* SlotCreate()
{
    for (Slot& s : g_psm.slots)
    {
        if (!s.used)
        {
            s = {};
            s.used = true;
            return &s;
        }
    }
    return nullptr;
}

void SlotStop(Slot* s)
{
    if (s != nullptr && s->started)
    {
        s->rcv.running = false;
        s->started = false;
    }
}

void SlotStart(Slot* s)
{
    SlotStop(s);
    ConverterStart(s->rcv);
    s->started = true;
}

s32 BufferCreate()
{
    for (s32 i = 0; i < static_cast<s32>(g_psm.buffersUsed.size()); i++)
    {
        if (!g_psm.buffersUsed[i])
        {
            g_psm.buffersUsed[i] = true;
            return i;
        }
    }
    return -1;
}

bool ValidMix(s32 mix) { return mix >= 0 && mix < 2 && g_psm.mixers[mix].used; }

void CheckChannel(const Psm* psm, s32 channel, const char* call)
{
    if (channel < 0 || channel >= 2)
    {
        Fatal("CRI ADX: %s on a stream's channel %d (the module keeps two)", call, channel);
    }
    (void)psm;
}

// The volumes PSM_SetVol and PSM_SetPan give a channel's tap: the volume (0-255) through the pan table (psm_pantbl, the
// module's .data at 0x9FD0 + 0x50: 31 levels from left to right), rounded as the module does in tenths
const s32 PanTable[31] = {255, 254, 253, 251, 249, 246, 242, 238, 232, 227, 220, 213, 206, 198, 189, 180,
                          170, 160, 149, 138, 127, 115, 103, 91,  78,  65,  53,  39,  26,  13,  0};
s32 PanVolume(s32 volume, s32 level)
{
    u32 tenths = static_cast<u32>(volume * level * 10) / 255;
    return static_cast<s32>(tenths / 10 + (tenths % 10 >= 5 ? 1 : 0));
}

void SetTapVolumes(Psm* psm, s32 channel, s32 left, s32 right)
{
    if (ValidMix(psm->mix))
    {
        MixerSetVolume(g_psm.mixers[psm->mix], psm->taps[channel], 0, left);
        MixerSetVolume(g_psm.mixers[psm->mix], psm->taps[channel], 1, right);
    }
}
}

void PsmSetupCore(s32 core)
{
    if (core >= 0 && core < 2)
    {
        g_psm.core = core;
    }
}

void PsmReinitBuffers(s32 channels, u32 work, s32 workSize)
{
    // PSMTBF_ReinitBuf: the first channels' buffers one after another in the work area (its size isn't checked)
    if (channels > 0 && work != 0 && workSize > 0)
    {
        if (channels > static_cast<s32>(g_psm.buffers.size()))
        {
            Fatal("CRI ADX: buffers for %d channels asked for (the module has 64)", channels);
        }
        for (s32 i = 0; i < channels; i++)
        {
            g_psm.buffers[i] = work + static_cast<u32>(i) * RingBytes;
        }
    }
    // PSM_ReinitSj then makes the streams of the first ones again with the new buffers
    for (s32 i = 0; i < std::min<s32>(channels, static_cast<s32>(g_psm.psms.size())); i++)
    {
        Psm& p = g_psm.psms[i];
        if (p.used)
        {
            s32 mix = p.mix, count = p.channels;
            std::vector<Sj*> inputs(p.inputs.begin(), p.inputs.begin() + count);
            PsmDestroy(&p);
            if (PsmCreate(mix, count, inputs) != &p)
            {
                Fatal("CRI ADX: PSM_ReinitSj couldn't make stream %d again in its place", i);
            }
        }
    }
}

void PsmInit(u32 moduleMemory)
{
    if (g_psm.inits++ != 0)
    {
        return;
    }
    g_psm.moduleMemory = moduleMemory;
    for (s32 core = 0; core < 2; core++)
    {
        CreateCore(core);
    }
    // Only the chosen core's output plays (both only for Dolby Digital's stream type, not done)
    OutputStart(g_psm.outputs[g_psm.core]);
    for (Psm& p : g_psm.psms)
    {
        p = {};
    }
}

void PsmFinish()
{
    if (--g_psm.inits != 0)
    {
        return;
    }
    for (Psm& p : g_psm.psms)
    {
        if (p.used)
        {
            PsmDestroy(&p);
        }
    }
    for (s32 core = 0; core < 2; core++)
    {
        OutputStop(g_psm.outputs[core]);
        g_psm.mixers[core] = {};
        g_psm.outputs[core] = {};
    }
}

Psm* PsmCreate(s32 core, s32 channels, const std::vector<Sj*>& inputs)
{
    if (core < 0 || core >= 2)
    {
        PsmError("E00101706:Illigal parameter'core_no'.(PSM_Create)");
        return nullptr;
    }
    if (channels < 0 || channels > 2 || static_cast<s32>(inputs.size()) < channels)
    {
        Fatal("CRI ADX: a stream of %d channels (%zu inputs): the module keeps two", channels, inputs.size());
    }
    for (s32 ch = 0; ch < channels; ch++)
    {
        if (inputs[ch] == nullptr)
        {
            PsmError("E00101709:'sji[chno] is NULL'.(PSM_Create)");
            return nullptr;
        }
    }
    Psm* psm = nullptr;
    for (Psm& p : g_psm.psms)
    {
        if (!p.used)
        {
            psm = &p;
            break;
        }
    }
    if (psm == nullptr)
    {
        return nullptr;
    }
    *psm = {};
    for (s32 ch = 0; ch < channels; ch++)
    {
        Slot* slot = SlotCreate();
        s32 buffer = slot != nullptr ? BufferCreate() : -1;
        if (slot == nullptr || buffer < 0)
        {
            PsmError(slot == nullptr ? "E00101710:PSMSLT_Create fails.(PSM_Create)" : "E00101711:PSMTBF_Create fails.(PSM_Create)");
            if (slot != nullptr)
            {
                slot->used = false;
            }
            psm->channels = ch;
            psm->used = true;
            PsmDestroy(psm);
            return nullptr;
        }
        if (g_psm.buffers[buffer] == 0)
        {
            // The module would make the ring at address 0 (PSM_ReinitSj gave no buffer for it) and write the IOP's vectors
            Fatal("CRI ADX: a stream's channel has no buffer (PSM_ReinitSj gave buffers for fewer channels)");
        }
        psm->slots[ch] = slot;
        psm->buffers[ch] = buffer;
        psm->rings[ch] = MakeSjRbf(g_psm.buffers[buffer], RingBytes, 0);
        slot->ring = psm->rings[ch].get();
        slot->input = inputs[ch];
        psm->inputs[ch] = inputs[ch];
    }
    psm->channels = channels;
    psm->used = true;
    psm->mix = -1;
    PsmSetMix(psm, g_psm.core);
    PsmSetChannels(psm, channels);
    for (s32 ch = 0; ch < channels; ch++)
    {
        PsmSetVolume(psm, ch, 255);
    }
    PsmSetRate(psm, OutRate);
    return psm;
}

void PsmDestroy(Psm* psm)
{
    if (psm == nullptr)
    {
        return;
    }
    PsmStop(psm);
    for (s32 ch = 0; ch < psm->channels; ch++)
    {
        if (psm->slots[ch] != nullptr)
        {
            psm->slots[ch]->used = false;
        }
        if (ValidMix(psm->mix))
        {
            MixerFreeTap(g_psm.mixers[psm->mix], psm->taps[ch]);
        }
        if (psm->buffers[ch] >= 0)
        {
            g_psm.buffersUsed[psm->buffers[ch]] = false;
        }
    }
    *psm = {};
}

void PsmStart(Psm* psm)
{
    PsmStop(psm);
    for (s32 ch = 0; ch < psm->active; ch++)
    {
        SlotStart(psm->slots[ch]);
    }
    psm->playing = true;
}

void PsmStop(Psm* psm)
{
    if (!psm->playing)
    {
        return;
    }
    for (s32 ch = 0; ch < psm->active; ch++)
    {
        SlotStop(psm->slots[ch]);
    }
    psm->playing = false;
}

void PsmSetRate(Psm* psm, s32 rate)
{
    for (s32 ch = 0; ch < psm->channels; ch++)
    {
        // RCV_SetInSfreq refuses while it converts
        Converter& c = psm->slots[ch]->rcv;
        if (rate <= 0 || c.running)
        {
            PsmError("RCV: a rate not taken (E00103009 or E00112703)");
            continue;
        }
        c.inRate = rate;
    }
    psm->rate = rate;
}

void PsmSetVolume(Psm* psm, s32 channel, s32 volume)
{
    CheckChannel(psm, channel, "PSM_SetVol");
    s32 pan = psm->active == 2 ? psm->pans[channel] : psm->pans[0];
    SetTapVolumes(psm, channel, PanVolume(volume, PanTable[pan + 15]), PanVolume(volume, PanTable[15 - pan]));
    psm->volumes[channel] = volume;
}

void PsmSetPan(Psm* psm, s32 channel, s32 pan)
{
    CheckChannel(psm, channel, "PSM_SetPan");
    // The first channel's volume, whichever channel it is (as the module reads it)
    s32 volume = psm->volumes[0];
    s32 left, right;
    if (pan < -14)
    {
        psm->pans[channel] = -15;
        left = volume;
        right = 0;
    }
    else if (pan >= 15)
    {
        psm->pans[channel] = 15;
        left = 0;
        right = volume;
    }
    else
    {
        psm->pans[channel] = pan;
        left = PanVolume(volume, PanTable[pan + 15]);
        right = PanVolume(volume, PanTable[15 - pan]);
    }
    SetTapVolumes(psm, channel, left, right);
}

void PsmSetMix(Psm* psm, s32 mix)
{
    if (mix < 0 || mix >= 2 || mix == psm->mix)
    {
        return;
    }
    if (ValidMix(psm->mix))
    {
        for (s32 ch = 0; ch < psm->channels; ch++)
        {
            MixerFreeTap(g_psm.mixers[psm->mix], psm->taps[ch]);
        }
    }
    if (!g_psm.mixers[mix].used)
    {
        // Before PSM_Init the module's mixers are its zeroed objects: no taps can be had
        psm->mix = -1;
        return;
    }
    for (s32 ch = 0; ch < psm->channels; ch++)
    {
        psm->taps[ch] = MixerAllocTap(g_psm.mixers[mix], psm->rings[ch].get());
    }
    psm->mix = mix;
}

void PsmSetChannels(Psm* psm, s32 channels)
{
    if (channels < 0 || channels > psm->channels || psm->playing)
    {
        return;
    }
    psm->active = channels;
    for (s32 ch = 0; ch < psm->channels; ch++)
    {
        PsmSetVolume(psm, ch, psm->volumes[ch]);
    }
}
}
