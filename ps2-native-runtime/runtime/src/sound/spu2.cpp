// The SPU2 at its registers (spu2.h). The hardware's behaviour is PCSX2's SPU2 (pcsx2/SPU2: Mixer.cpp, ADSR.cpp, Reverb.cpp,
// ReverbResample.cpp, spu2sys.cpp, Dma.cpp, ReadInput.cpp), written again here: the ADPCM decoding and its block flags, the
// decoder's queue that NAX runs ahead of by four samples a step, the gaussian interpolation, the pitch counter (and the previous
// voice's modulation), the ADSR's and the volume sweeps' rates, the noise generator, the reverb at 24 kHz between its 39 tap
// resampling filters, the cores' mixing gates and volumes, the writes of the mixes into the memory's output areas, IRQA and the
// key on and off that take effect at the next sample
#include "spu2.h"

#include "gaussian.h"
#include "../runtime.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace
{
using Spu2::Stereo;

alignas(64) u16 g_Memory[Spu2::MemoryHalfwords];

u16* Memory()
{
    return g_Memory;
}

s32 Clamp16(s32 value)
{
    return std::clamp(value, -0x8000, 0x7FFF);
}

Stereo Clamp16(Stereo value)
{
    return {Clamp16(value.left), Clamp16(value.right)};
}

s32 ApplyVolume(s32 sample, s32 volume)
{
    return (volume * sample) >> 15;
}

// A volume register: a fixed volume (bit 15 clear: 15 bits, doubled) or a sweep (bit 15 set: its rate, direction, curve and
// phase), the current value (VOLX) moving at the sweep's rate
struct Sweep
{
    u16 reg = 0;
    u32 counter = 0;
    s32 value = 0;

    bool Enabled() const
    {
        return (reg & 0x8000) != 0;
    }

    void Set(u16 value16)
    {
        reg = value16;
        if (!Enabled())
        {
            value = static_cast<s16>(static_cast<u16>(value16 << 1));
        }
    }

    void Update()
    {
        if (!Enabled())
        {
            return;
        }

        s32 stepField = reg & 3;
        s32 shift = (reg >> 2) & 0x1F;
        bool phase = (reg & 0x1000) != 0;
        bool decreasing = (reg & 0x2000) != 0;
        bool exponential = (reg & 0x4000) != 0;
        s32 step = 7 - stepField;
        if (decreasing)
        {
            step = ~step;
        }

        u32 counterStep = 0x8000u >> std::max(0, shift - 11);
        s32 levelStep = step << std::max(0, 11 - shift);
        if (exponential)
        {
            if (!decreasing && value > 0x6000)
            {
                counterStep >>= 2;
            }

            if (decreasing)
            {
                levelStep = static_cast<s16>((levelStep * value) >> 15);
            }
        }

        // The counter stands still only when every bit of the rate is set
        if (stepField != 3 && shift != 0x1F)
        {
            counterStep = std::max<u32>(1, counterStep);
        }

        counter += counterStep;
        if (!(exponential && decreasing))
        {
            levelStep = phase ? -levelStep : levelStep;
        }

        if (counter >= 0x8000)
        {
            counter = 0;
            if (!decreasing)
            {
                value = std::clamp(value + levelStep, -0x8000, 0x7FFF);
            }
            else
            {
                s32 low = phase ? -0x8000 : 0;
                s32 high = phase ? 0 : 0x7FFF;
                if (exponential)
                {
                    low = 0;
                    high = 0x7FFF;
                }

                value = std::clamp(value + levelStep, low, high);
            }
        }
    }
};

// The envelope: ADSR1 (attack's curve and rate, decay's rate, sustain's level) and ADSR2 (sustain's curve, direction and rate,
// release's curve and rate), its phase, counter and level (ENVX)
struct Envelope
{
    enum Phase : u8
    {
        Stopped,
        Attack,
        Decay,
        Sustain,
        Release,
    };

    u16 adsr1 = 0;
    u16 adsr2 = 0;
    u8 phase = Stopped;
    u32 counter = 0;
    s32 value = 0;

    struct Rate
    {
        bool decreasing;
        bool exponential;
        s32 shift;
        s32 step;
        s32 target;
    };

    Rate PhaseRate() const
    {
        switch (phase)
        {
        case Attack:
            return {false, (adsr1 & 0x8000) != 0, (adsr1 >> 10) & 0x1F, 7 - ((adsr1 >> 8) & 3), 0x7FFF};
        case Decay:
            return {true, true, (adsr1 >> 4) & 0xF, -8, ((adsr1 & 0xF) + 1) << 11};
        case Sustain:
        {
            bool decreasing = (adsr2 & 0x4000) != 0;
            s32 step = 7 - ((adsr2 >> 6) & 3);
            return {decreasing, (adsr2 & 0x8000) != 0, (adsr2 >> 8) & 0x1F, decreasing ? ~step : step, 0};
        }
        default:
            return {true, (adsr2 & 0x20) != 0, adsr2 & 0x1F, -8, 0};
        }
    }

    // False once the envelope ends the voice
    bool Step()
    {
        Rate rate = PhaseRate();
        u32 counterStep = 0x8000u >> std::max(0, rate.shift - 11);
        s32 levelStep = rate.step << std::max(0, 11 - rate.shift);
        if (rate.exponential)
        {
            if (!rate.decreasing && value > 0x6000)
            {
                counterStep >>= 2;
            }

            if (rate.decreasing)
            {
                levelStep = static_cast<s16>((levelStep * value) >> 15);
            }
        }

        counterStep = std::max<u32>(1, counterStep);
        counter += counterStep;
        if (counter >= 0x8000)
        {
            counter = 0;
            value = std::clamp(value + levelStep, 0, 0x7FFF);
        }

        if (phase == Sustain)
        {
            return value != 0;
        }

        if ((!rate.decreasing && value >= rate.target) || (rate.decreasing && value <= rate.target))
        {
            phase++;
        }

        return phase <= Release;
    }
};

// The ADPCM blocks' flags: the end of a loop (NAX goes to LSA, ENDX is set), its staying on (else the voice stops), its start
// (LSA is set to the block, unless it was written since the voice started)
constexpr u8 FlagLoopEnd = 1;
constexpr u8 FlagLoop = 2;
constexpr u8 FlagLoopStart = 4;

struct Voice
{
    Sweep left;
    Sweep right;
    Envelope envelope;
    u16 pitch = 0;
    u32 startAddress = 0;
    u32 loopAddress = 0;
    u32 nextAddress = 0;
    bool loopWritten = false;
    u8 flags = 0;
    s32 previous1 = 0;
    s32 previous2 = 0;
    // The block being decoded (28 samples) and whether it's decoded yet
    s16 block[28] = {};
    bool decoded = false;
    // The decoder's queue and the pitch counter (12 bits of fraction)
    s16 queue[32] = {};
    u32 queueRead = 0;
    u32 queueWrite = 0;
    u32 counter = 0;
    bool modulated = false;
    bool noise = false;
    s32 output = 0;

    void Start()
    {
        envelope.phase = Envelope::Attack;
        envelope.counter = 0;
        envelope.value = 0;
        loopWritten = false;
        counter = 0;
        flags = 0;
        nextAddress = (startAddress & ~7u) | 1;
        previous1 = 0;
        previous2 = 0;
        decoded = false;
        queueRead = 0;
        queueWrite = 0;
    }

    void Stop()
    {
        envelope.value = 0;
        envelope.phase = Envelope::Stopped;
    }
};

// The reverb's registers: the all-pass filters' sizes and volumes, the IIR's, the combs', the wall's, the input's coefficients,
// and the offsets of what it reads and writes in its area
struct Reverb
{
    u32 apf1Size, apf2Size;
    u32 sameLeftDestination, sameRightDestination;
    u32 comb1Left, comb1Right, comb2Left, comb2Right;
    u32 sameLeftSource, sameRightSource;
    u32 diffLeftDestination, diffRightDestination;
    u32 comb3Left, comb3Right, comb4Left, comb4Right;
    u32 diffLeftSource, diffRightSource;
    u32 apf1LeftDestination, apf1RightDestination, apf2LeftDestination, apf2RightDestination;
    s16 iirVolume, comb1Volume, comb2Volume, comb3Volume, comb4Volume, wallVolume, apf1Volume, apf2Volume;
    s16 inLeft, inRight;
};
// The order of the address registers from 0x2E4 on, a pair of halfwords each
constexpr u32 ReverbAddressCount = 22;

struct Core
{
    s32 index;
    Voice voices[24];
    u32 pmon, non, vmixl, vmixel, vmixr, vmixer;
    u16 mmix, attr;
    u32 irqa, tsa, activeTsa;
    u32 keyOn, keyOff;
    u32 endx;
    u16 statx;
    u16 admas;
    u32 effectsStart, effectsEnd;
    Reverb reverb;
    Sweep masterLeft, masterRight;
    s32 effectLeft, effectRight, externalLeft, externalRight, inputLeft, inputRight;
    bool irqEnable, fxEnable;
    u8 noiseClock;
    u32 noiseCount, noiseOut;
    s16 reverbDown[2][128];
    s16 reverbUp[2][128];
    u32 reverbPosition;
    // AutoDMA: the data a write gave the input, how much of it is left, and the half of the input buffer that wants it next
    const u16* admaData;
    u32 admaLeft;
    bool admaPending;
    // The half of the input buffer that wants a block AutoDMA didn't have (-1 for none)
    s32 admaWantedHalf;
    // A plain DMA read's destination
    u16* readDestination;
};

Core g_Cores[2];
// Samples made (the reverb works on the left at even ones, on the right at odd ones) and the output areas' position
u32 g_Cycles = 0;
u32 g_OutputPosition = 0;
Spu2::InterruptFunction g_Interrupt = nullptr;
// The player's volumes by voice (core * 24 + voice) and muting (volumes.h)
struct Gains
{
    std::atomic<s32> gain[48];
    Gains()
    {
        for (auto& g : gain)
        {
            g.store(0x8000);
        }
    }
};
Gains g_Gains;
// The player's volume of the cores' inputs (the movies' sound)
std::atomic<s32> g_InputGain = 0x8000;
std::atomic<bool> g_Muted = false;
// SPDIF and its IRQ info (which cores' IRQA were reached)
u16 g_SpdifRegisters[8];
constexpr u32 SpdifIrqInfo = 1;

void RaiseIrq(s32 core)
{
    g_SpdifRegisters[SpdifIrqInfo] |= static_cast<u16>(4 << core);
    if (g_Interrupt != nullptr)
    {
        g_Interrupt(Spu2::SpuInterrupt);
    }
}

// Both cores' IRQA are tested against every address read or written
void CheckIrq(u32 address)
{
    for (s32 core = 0; core < 2; core++)
    {
        if (g_Cores[core].irqEnable && (g_Cores[core].irqa & Spu2::AddressMask) == address)
        {
            RaiseIrq(core);
        }
    }
}

void WriteMemory(u32 address, s16 value)
{
    CheckIrq(address);
    Memory()[address & Spu2::AddressMask] = static_cast<u16>(value);
}

// The ADPCM filters' coefficients (64ths)
constexpr s32 Filters[16][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

void DecodeBlock(Voice& voice, u32 address)
{
    const u16* block = Memory() + (address & Spu2::AddressMask & ~7u);
    s32 header = block[0];
    s32 shift = (header & 0xF) + 16;
    s32 filter = (header >> 4) & 0xF;
    s32 coefficient1 = Filters[filter][0];
    s32 coefficient2 = Filters[filter][1];
    const auto* bytes = reinterpret_cast<const s8*>(&block[1]);
    for (s32 i = 0; i < 14; i++)
    {
        s32 data = (bytes[i] << 28) & static_cast<s32>(0xF0000000);
        s32 sample = (data >> shift) + ((coefficient1 * voice.previous1 + coefficient2 * voice.previous2 + 32) >> 6);
        sample = Clamp16(sample);
        voice.block[i * 2] = static_cast<s16>(sample);
        data = (bytes[i] << 24) & static_cast<s32>(0xF0000000);
        s32 second = (data >> shift) + ((coefficient1 * sample + coefficient2 * voice.previous1 + 32) >> 6);
        second = Clamp16(second);
        voice.block[i * 2 + 1] = static_cast<s16>(second);
        voice.previous2 = sample;
        voice.previous1 = second;
    }

    voice.decoded = true;
}

void AdvanceAddress(Voice& voice)
{
    CheckIrq(voice.nextAddress & Spu2::AddressMask);
    voice.nextAddress = (voice.nextAddress + 1) & Spu2::AddressMask;
}

// The block's header is read every sample: its flags, and LSA set at a loop's start
void ReadHeader(Voice& voice)
{
    u32 block = voice.nextAddress & ~7u;
    CheckIrq(block);
    voice.flags = static_cast<u8>(Memory()[block] >> 8);
    if ((voice.flags & FlagLoopStart) != 0 && !voice.loopWritten)
    {
        voice.loopAddress = block;
    }
}

void Decode(Core& core, s32 index)
{
    Voice& voice = core.voices[index];
    ReadHeader(voice);
    // A voice started at pitch 0 gets NAX to SSA + 5: the queue holds about 12 samples
    if (static_cast<s32>(voice.queueWrite - voice.queueRead) > 12)
    {
        return;
    }

    if (voice.envelope.phase != Envelope::Stopped)
    {
        if (!voice.decoded)
        {
            DecodeBlock(voice, voice.nextAddress);
        }

        u32 sample = ((voice.nextAddress & 7) - 1) * 4;
        for (u32 i = 0; i < 4; i++)
        {
            voice.queue[(voice.queueWrite + i) % 32] = voice.block[sample + i];
        }
    }

    voice.queueWrite += 4;
    AdvanceAddress(voice);
    if ((voice.nextAddress & 7) == 0)
    {
        if ((voice.flags & FlagLoopEnd) != 0)
        {
            core.endx |= 1u << index;
            voice.nextAddress = voice.loopAddress;
            if ((voice.flags & FlagLoop) == 0)
            {
                voice.Stop();
            }
        }

        AdvanceAddress(voice);
        voice.decoded = false;
    }
}

s32 Interpolate(const Voice& voice)
{
    u32 phase = (voice.counter & 0xFF0) >> 4;
    s32 out = 0;
    for (u32 i = 0; i < 4; i++)
    {
        out += (Spu2::Gaussian[phase][i] * voice.queue[(voice.queueRead + i) % 32]) >> 15;
    }

    return out;
}

// Dr. Hell's noise generator
void UpdateNoise(Core& core)
{
    static constexpr u8 NoiseAdd[64] = {1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0,
                                        0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1};
    static constexpr u16 NoiseFrequencyAdd[5] = {0, 84, 140, 180, 210};
    u32 level = (0x8000u >> (core.noiseClock >> 2)) << 16;
    core.noiseCount += 0x10000;
    core.noiseCount += NoiseFrequencyAdd[core.noiseClock & 3];
    if ((core.noiseCount & 0xFFFF) >= NoiseFrequencyAdd[4])
    {
        core.noiseCount += 0x10000;
        core.noiseCount -= NoiseFrequencyAdd[core.noiseClock & 3];
    }

    if (core.noiseCount >= level)
    {
        while (core.noiseCount >= level)
        {
            core.noiseCount -= level;
        }

        core.noiseOut = (core.noiseOut << 1) | NoiseAdd[(core.noiseOut >> 10) & 63];
    }
}

Stereo MixVoice(Core& core, s32 index)
{
    Voice& voice = core.voices[index];
    voice.left.Update();
    voice.right.Update();
    Decode(core, index);
    Stereo out = {0, 0};
    s32 value = 0;
    if (voice.envelope.phase != Envelope::Stopped)
    {
        value = voice.noise ? static_cast<s16>(core.noiseOut) : Interpolate(voice);
        if (!voice.envelope.Step())
        {
            voice.Stop();
        }

        value = ApplyVolume(value, voice.envelope.value);
        voice.output = value;
        out = {ApplyVolume(value, voice.left.value), ApplyVolume(value, voice.right.value)};
        s32 gain = g_Gains.gain[core.index * 24 + index].load(std::memory_order_relaxed);
        if (gain != 0x8000)
        {
            out = {(out.left * gain) >> 15, (out.right * gain) >> 15};
        }
    }
    else
    {
        voice.envelope.value = 0;
    }

    // The voice runs on whether it sounds or not
    s32 pitch = voice.pitch;
    if (voice.modulated && index != 0)
    {
        pitch = std::clamp((voice.pitch * (32768 + core.voices[index - 1].output)) >> 15, 0, 0x3FFF);
    }

    pitch = std::min(pitch, 0x3FFF);
    voice.counter += static_cast<u32>(pitch);
    voice.queueRead += voice.counter >> 12;
    voice.counter &= 0xFFF;
    // Voices 1 and 3 are written to the output areas after their envelope
    if (index == 1)
    {
        WriteMemory((core.index == 0 ? 0x400 : 0xC00) + g_OutputPosition, static_cast<s16>(value));
    }
    else if (index == 3)
    {
        WriteMemory((core.index == 0 ? 0x600 : 0xE00) + g_OutputPosition, static_cast<s16>(value));
    }

    return out;
}

u32 ReverbIndex(const Core& core, s32 offset)
{
    u32 start = core.effectsStart & 0x3FFFFF;
    u32 end = (core.effectsEnd & 0x3FFFFF) | 0xFFFF;
    u32 x = static_cast<u32>((g_Cycles >> 1) + static_cast<u32>(offset)) % ((end - start) + 1);
    x += start;
    return x & Spu2::AddressMask;
}

// The reverb's 39 tap halfband filters, down to 24 kHz and back up
constexpr s32 ReverbTaps = 39;
constexpr s16 DownCoefficients[ReverbTaps] = {-1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0, -2960, 0, 10246,
                                              16384, 10246, 0, -2960, 0, 1332, 0, -616, 0, 266, 0, -103, 0, 35, 0, -10, 0, 2, 0, -1};

s32 ReverbDownsample(const Core& core, s32 right)
{
    s32 index = (core.reverbPosition - ReverbTaps) & 63;
    s32 out = 0;
    for (s32 i = 0; i < ReverbTaps; i++)
    {
        out += core.reverbDown[right][index + i] * DownCoefficients[i];
    }

    return Clamp16(out >> 15);
}

Stereo ReverbUpsample(const Core& core)
{
    s32 index = (core.reverbPosition - ReverbTaps) & 63;
    s32 left = 0;
    s32 right = 0;
    for (s32 i = 0; i < ReverbTaps; i++)
    {
        s32 coefficient = Clamp16(DownCoefficients[i] * 2);
        left += core.reverbUp[0][index + i] * coefficient;
        right += core.reverbUp[1][index + i] * coefficient;
    }

    return {Clamp16(left >> 15), Clamp16(right >> 15)};
}

s32 Multiply(s32 a, s32 b)
{
    return (a * b) >> 15;
}

s16 ReadReverb(u32 address)
{
    return static_cast<s16>(Memory()[address]);
}

Stereo DoReverb(Core& core, Stereo input)
{
    if (core.effectsStart >= core.effectsEnd)
    {
        return {0, 0};
    }

    input = Clamp16(input);
    core.reverbDown[0][core.reverbPosition] = static_cast<s16>(input.left);
    core.reverbDown[1][core.reverbPosition] = static_cast<s16>(input.right);
    core.reverbDown[0][core.reverbPosition | 64] = static_cast<s16>(input.left);
    core.reverbDown[1][core.reverbPosition | 64] = static_cast<s16>(input.right);
    bool right = (g_Cycles & 1) != 0;
    const Reverb& r = core.reverb;
    u32 sameSource = ReverbIndex(core, static_cast<s32>(right ? r.sameRightSource : r.sameLeftSource));
    u32 sameDestination = ReverbIndex(core, static_cast<s32>(right ? r.sameRightDestination : r.sameLeftDestination));
    u32 samePrevious = ReverbIndex(core, static_cast<s32>(right ? r.sameRightDestination : r.sameLeftDestination) - 1);
    u32 diffSource = ReverbIndex(core, static_cast<s32>(right ? r.diffLeftSource : r.diffRightSource));
    u32 diffDestination = ReverbIndex(core, static_cast<s32>(right ? r.diffRightDestination : r.diffLeftDestination));
    u32 diffPrevious = ReverbIndex(core, static_cast<s32>(right ? r.diffRightDestination : r.diffLeftDestination) - 1);
    u32 comb1 = ReverbIndex(core, static_cast<s32>(right ? r.comb1Right : r.comb1Left));
    u32 comb2 = ReverbIndex(core, static_cast<s32>(right ? r.comb2Right : r.comb2Left));
    u32 comb3 = ReverbIndex(core, static_cast<s32>(right ? r.comb3Right : r.comb3Left));
    u32 comb4 = ReverbIndex(core, static_cast<s32>(right ? r.comb4Right : r.comb4Left));
    u32 apf1Destination = right ? r.apf1RightDestination : r.apf1LeftDestination;
    u32 apf2Destination = right ? r.apf2RightDestination : r.apf2LeftDestination;
    u32 apf1Source = ReverbIndex(core, static_cast<s32>(apf1Destination - r.apf1Size));
    u32 apf1 = ReverbIndex(core, static_cast<s32>(apf1Destination));
    u32 apf2Source = ReverbIndex(core, static_cast<s32>(apf2Destination - r.apf2Size));
    u32 apf2 = ReverbIndex(core, static_cast<s32>(apf2Destination));
    if (core.fxEnable)
    {
        for (s32 other = 0; other < 2; other++)
        {
            const Core& c = g_Cores[other];
            u32 irqa = c.irqa & Spu2::AddressMask;
            if (c.irqEnable && irqa >= core.effectsStart && irqa <= core.effectsEnd &&
                (irqa == sameSource || irqa == diffSource || irqa == sameDestination || irqa == diffDestination ||
                 irqa == samePrevious || irqa == diffPrevious || irqa == comb1 || irqa == comb2 || irqa == comb3 ||
                 irqa == comb4 || irqa == apf1 || irqa == apf1Source || irqa == apf2 || irqa == apf2Source))
            {
                RaiseIrq(other);
            }
        }
    }

    s32 in = Multiply(right ? r.inRight : r.inLeft, ReverbDownsample(core, right ? 1 : 0));
    s32 same = Multiply(r.iirVolume, in + Multiply(r.wallVolume, ReadReverb(sameSource)) - ReadReverb(samePrevious)) +
               ReadReverb(samePrevious);
    s32 diff = Multiply(r.iirVolume, in + Multiply(r.wallVolume, ReadReverb(diffSource)) - ReadReverb(diffPrevious)) +
               ReadReverb(diffPrevious);
    s32 out = Multiply(r.comb1Volume, ReadReverb(comb1)) + Multiply(r.comb2Volume, ReadReverb(comb2)) +
              Multiply(r.comb3Volume, ReadReverb(comb3)) + Multiply(r.comb4Volume, ReadReverb(comb4));
    s32 allPass1 = out - Multiply(r.apf1Volume, ReadReverb(apf1Source));
    out = ReadReverb(apf1Source) + Multiply(r.apf1Volume, allPass1);
    s32 allPass2 = out - Multiply(r.apf2Volume, ReadReverb(apf2Source));
    out = ReadReverb(apf2Source) + Multiply(r.apf2Volume, allPass2);
    // The effect always runs, but writes its area only while it's enabled
    if (core.fxEnable)
    {
        Memory()[sameDestination] = static_cast<u16>(Clamp16(same));
        Memory()[diffDestination] = static_cast<u16>(Clamp16(diff));
        Memory()[apf1] = static_cast<u16>(Clamp16(allPass1));
        Memory()[apf2] = static_cast<u16>(Clamp16(allPass2));
    }

    out = Clamp16(out);
    s32 side = right ? 1 : 0;
    core.reverbUp[side][core.reverbPosition] = static_cast<s16>(out);
    core.reverbUp[1 - side][core.reverbPosition] = 0;
    core.reverbUp[side][core.reverbPosition | 64] = static_cast<s16>(out);
    core.reverbUp[1 - side][core.reverbPosition | 64] = 0;
    core.reverbPosition = (core.reverbPosition + 1) & 63;
    return ReverbUpsample(core);
}

// AutoDMA's next block (0x100 halfwords of left, then of right) into a half of the core's input buffer; the transfer's end once it
// has given all of it
void TransferAdmaBlock(Core& core, u32 half)
{
    u32 base = 0x2000 + (static_cast<u32>(core.index) << 10);
    if (core.admaLeft < 0x200)
    {
        return;
    }

    std::memcpy(Memory() + base + half, core.admaData, 0x100 * sizeof(u16));
    std::memcpy(Memory() + base + 0x200 + half, core.admaData + 0x100, 0x100 * sizeof(u16));
    core.admaData += 0x200;
    core.admaLeft -= 0x200;
    if (core.admaLeft < 0x200)
    {
        core.admaLeft = 0;
        core.admaData = nullptr;
        if (core.admaPending)
        {
            core.admaPending = false;
            core.statx |= 0x80;
            if (g_Interrupt != nullptr)
            {
                g_Interrupt(core.index == 0 ? Spu2::Dma4Interrupt : Spu2::Dma7Interrupt);
            }
        }
    }
}

// The core's input (its AutoDMA buffer at 0x2000 + core * 0x400: 0x200 halfwords of left, then of right, played round), which
// asks for its next block when a half of it has been played
Stereo ReadInput(Core& core)
{
    u32 base = 0x2000 + (static_cast<u32>(core.index) << 10);
    u32 position = g_OutputPosition;
    for (s32 i = 0; i < 2; i++)
    {
        if (g_Cores[i].irqEnable && base + position == (g_Cores[i].irqa & 0xFFDFF))
        {
            RaiseIrq(i);
        }
    }

    Stereo value = {static_cast<s16>(Memory()[base + position]), static_cast<s16>(Memory()[base + 0x200 + position])};
    if (position == 0 || position == 0x100)
    {
        // The half just played takes the next block, or asks for it
        u32 half = position == 0x100 ? 0 : 0x100;
        core.admaWantedHalf = core.admaLeft >= 0x200 ? -1 : static_cast<s32>(half);
        TransferAdmaBlock(core, half);
    }

    return value;
}

struct VoiceMix
{
    Stereo dry;
    Stereo wet;
};

VoiceMix MixVoices(Core& core)
{
    VoiceMix mix = {{0, 0}, {0, 0}};
    for (s32 index = 0; index < 24; index++)
    {
        Stereo out = MixVoice(core, index);
        u32 bit = 1u << index;
        if ((core.vmixl & bit) != 0)
        {
            mix.dry.left += out.left;
        }

        if ((core.vmixr & bit) != 0)
        {
            mix.dry.right += out.right;
        }

        if ((core.vmixel & bit) != 0)
        {
            mix.wet.left += out.left;
        }

        if ((core.vmixer & bit) != 0)
        {
            mix.wet.right += out.right;
        }
    }

    return mix;
}

// MMIX's gates: core 0's lacks its external input
bool Gate(const Core& core, u16 bit)
{
    u16 mask = core.index == 0 ? 0xFF0 : 0xFFF;
    return (core.mmix & mask & bit) != 0;
}

Stereo MixCore(Core& core, const VoiceMix& voices, Stereo input, Stereo external)
{
    core.masterLeft.Update();
    core.masterRight.Update();
    UpdateNoise(core);
    VoiceMix mix = {Clamp16(voices.dry), Clamp16(voices.wet)};
    u32 base = core.index == 0 ? 0x1000 : 0x1800;
    WriteMemory(base + g_OutputPosition, static_cast<s16>(mix.dry.left));
    WriteMemory(base + 0x200 + g_OutputPosition, static_cast<s16>(mix.dry.right));
    WriteMemory(base + 0x400 + g_OutputPosition, static_cast<s16>(mix.wet.left));
    WriteMemory(base + 0x600 + g_OutputPosition, static_cast<s16>(mix.wet.right));
    Stereo dry = {0, 0};
    Stereo wet = {0, 0};
    if (Gate(core, 0x080))
    {
        dry.left += input.left;
    }

    if (Gate(core, 0x040))
    {
        dry.right += input.right;
    }

    if (Gate(core, 0x800))
    {
        dry.left += mix.dry.left;
    }

    if (Gate(core, 0x400))
    {
        dry.right += mix.dry.right;
    }

    if (Gate(core, 0x008))
    {
        dry.left += external.left;
    }

    if (Gate(core, 0x004))
    {
        dry.right += external.right;
    }

    if (Gate(core, 0x020))
    {
        wet.left += input.left;
    }

    if (Gate(core, 0x010))
    {
        wet.right += input.right;
    }

    if (Gate(core, 0x200))
    {
        wet.left += mix.wet.left;
    }

    if (Gate(core, 0x100))
    {
        wet.right += mix.wet.right;
    }

    if (Gate(core, 0x002))
    {
        wet.left += external.left;
    }

    if (Gate(core, 0x001))
    {
        wet.right += external.right;
    }

    Stereo reverb = DoReverb(core, wet);
    return {dry.left + ApplyVolume(reverb.left, core.effectLeft), dry.right + ApplyVolume(reverb.right, core.effectRight)};
}

void StartVoices(Core& core, u32 bits)
{
    core.endx &= ~bits;
    for (s32 index = 0; index < 24; index++)
    {
        if ((bits >> index & 1) != 0)
        {
            core.voices[index].Start();
        }
    }
}

void StopVoices(Core& core, u32 bits)
{
    for (s32 index = 0; index < 24; index++)
    {
        if ((bits >> index & 1) != 0 && core.voices[index].envelope.phase != Envelope::Stopped)
        {
            core.voices[index].envelope.phase = Envelope::Release;
            core.voices[index].envelope.counter = 0;
        }
    }
}

void SetLow(u32& value, u16 low)
{
    value = (value & 0xFFFF0000) | low;
}

void SetHigh(u32& value, u16 high)
{
    value = (value & 0xFFFF) | (static_cast<u32>(high) << 16);
}

u32* ReverbAddress(Reverb& reverb, u32 index)
{
    u32* addresses[ReverbAddressCount] = {
        &reverb.apf1Size,         &reverb.apf2Size,         &reverb.sameLeftDestination, &reverb.sameRightDestination,
        &reverb.comb1Left,        &reverb.comb1Right,       &reverb.comb2Left,           &reverb.comb2Right,
        &reverb.sameLeftSource,   &reverb.sameRightSource,  &reverb.diffLeftDestination, &reverb.diffRightDestination,
        &reverb.comb3Left,        &reverb.comb3Right,       &reverb.comb4Left,           &reverb.comb4Right,
        &reverb.diffLeftSource,   &reverb.diffRightSource,  &reverb.apf1LeftDestination, &reverb.apf1RightDestination,
        &reverb.apf2LeftDestination, &reverb.apf2RightDestination};
    return addresses[index];
}

// Core 1's DIFF_L_SRC and DIFF_R_SRC registers (0x324, 0x328) are the other way round (PCSX2's register tables)
u32 ReverbRegisterOf(const Core& core, u32 index)
{
    constexpr u32 DiffLeftSource = 16;
    constexpr u32 DiffRightSource = 17;
    if (core.index == 1 && (index == DiffLeftSource || index == DiffRightSource))
    {
        return index == DiffLeftSource ? DiffRightSource : DiffLeftSource;
    }

    return index;
}

s16* ReverbVolume(Reverb& reverb, u32 index)
{
    s16* volumes[10] = {&reverb.iirVolume,  &reverb.comb1Volume, &reverb.comb2Volume, &reverb.comb3Volume, &reverb.comb4Volume,
                        &reverb.wallVolume, &reverb.apf1Volume,  &reverb.apf2Volume,  &reverb.inLeft,      &reverb.inRight};
    return volumes[index];
}

// Registers kept as written that have no behaviour of their own
u16 g_Plain[Spu2::RegisterSize / 2];

void WriteCore(Core& core, u32 offset, u16 value)
{
    if (offset < 0x180)
    {
        Voice& voice = core.voices[offset >> 4];
        switch ((offset >> 1) & 7)
        {
        case 0:
            voice.left.Set(value);
            break;
        case 1:
            voice.right.Set(value);
            break;
        case 2:
            voice.pitch = value;
            break;
        case 3:
            voice.envelope.adsr1 = value;
            break;
        case 4:
            voice.envelope.adsr2 = value;
            break;
        case 5:
            voice.envelope.value = static_cast<s16>(value);
            break;
        default:
            // VOLX can't be written
            break;
        }

        return;
    }

    if (offset >= 0x1C0 && offset < 0x2E0)
    {
        u32 relative = offset - 0x1C0;
        Voice& voice = core.voices[relative / 12];
        switch ((relative % 12) / 2)
        {
        case 0:
            voice.startAddress = (static_cast<u32>(value & 0xF) << 16) | (voice.startAddress & 0xFFF8);
            break;
        case 1:
            voice.startAddress = (voice.startAddress & 0xF0000) | (value & 0xFFF8);
            break;
        case 2:
            voice.loopWritten = true;
            voice.loopAddress = (static_cast<u32>(value & 0xF) << 16) | (voice.loopAddress & 0xFFF8);
            break;
        case 3:
            voice.loopWritten = true;
            voice.loopAddress = (voice.loopAddress & 0xF0000) | (value & 0xFFF8);
            break;
        case 4:
            voice.nextAddress = (static_cast<u32>(value & 0xF) << 16) | (voice.nextAddress & 0xFFF8) | 1;
            voice.decoded = false;
            break;
        default:
            voice.nextAddress = (voice.nextAddress & 0xF0000) | (value & 0xFFF8) | 1;
            voice.decoded = false;
            break;
        }

        return;
    }

    if (offset >= 0x2E4 && offset < 0x2E4 + ReverbAddressCount * 4)
    {
        u32 relative = offset - 0x2E4;
        u32* address = ReverbAddress(core.reverb, ReverbRegisterOf(core, relative / 4));
        if ((relative & 2) == 0)
        {
            SetHigh(*address, value);
        }
        else
        {
            SetLow(*address, value);
        }

        return;
    }

    auto setBits = [&](u32& bits, bool high) {
        if (high)
        {
            SetHigh(bits, value & 0xFF);
        }
        else
        {
            SetLow(bits, value);
        }
    };
    switch (offset)
    {
    case 0x180:
    case 0x182:
        setBits(core.pmon, offset == 0x182);
        for (s32 index = 0; index < 24; index++)
        {
            core.voices[index].modulated = index != 0 && (core.pmon >> index & 1) != 0;
        }

        break;
    case 0x184:
    case 0x186:
        setBits(core.non, offset == 0x186);
        for (s32 index = 0; index < 24; index++)
        {
            core.voices[index].noise = (core.non >> index & 1) != 0;
        }

        break;
    case 0x188:
    case 0x18A:
        setBits(core.vmixl, offset == 0x18A);
        break;
    case 0x18C:
    case 0x18E:
        setBits(core.vmixel, offset == 0x18E);
        break;
    case 0x190:
    case 0x192:
        setBits(core.vmixr, offset == 0x192);
        break;
    case 0x194:
    case 0x196:
        setBits(core.vmixer, offset == 0x196);
        break;
    case 0x198:
        core.mmix = value;
        break;
    case 0x19A:
    {
        bool hadDma = ((core.attr >> 4) & 3) != 0;
        core.attr = value;
        core.irqEnable = (value & 0x40) != 0;
        core.fxEnable = (value & 0x80) != 0;
        core.noiseClock = static_cast<u8>((value >> 8) & 0x3F);
        if (((value >> 4) & 3) == 0 && (core.statx & 0x400) == 0)
        {
            core.statx &= ~0x80;
        }
        else if (!hadDma && ((value >> 4) & 3) != 0)
        {
            core.statx |= 0x80;
        }

        core.activeTsa = core.tsa & Spu2::AddressMask;
        if (!core.irqEnable)
        {
            g_SpdifRegisters[SpdifIrqInfo] &= static_cast<u16>(~(4 << core.index));
        }

        break;
    }
    case 0x19C:
        SetHigh(core.irqa, value & 0xF);
        break;
    case 0x19E:
        SetLow(core.irqa, value);
        break;
    case 0x1A0:
        core.keyOn |= value;
        break;
    case 0x1A2:
        core.keyOn |= static_cast<u32>(value & 0xFF) << 16;
        break;
    case 0x1A4:
        core.keyOff |= value;
        break;
    case 0x1A6:
        core.keyOff |= static_cast<u32>(value & 0xFF) << 16;
        break;
    case 0x1A8:
        SetHigh(core.tsa, value & 0xF);
        break;
    case 0x1AA:
        SetLow(core.tsa, value);
        break;
    case 0x1AC:
        // The data port: a halfword written at TSA, which moves on
        core.activeTsa = core.tsa & Spu2::AddressMask;
        CheckIrq(core.activeTsa);
        Memory()[core.activeTsa] = value;
        core.tsa = (core.activeTsa + 1) & Spu2::AddressMask;
        break;
    case 0x1B0:
        core.admas = value;
        if ((value & 3) == 0 && core.admaData != nullptr)
        {
            // AutoDMA off kills what was left of its transfer and clears the input
            core.admaData = nullptr;
            core.admaLeft = 0;
            core.admaPending = false;
            std::memset(Memory() + 0x2000 + (static_cast<u32>(core.index) << 10), 0, 0x400 * sizeof(u16));
        }

        break;
    case 0x2E0:
        SetHigh(core.effectsStart, value & 0x3F);
        break;
    case 0x2E2:
        SetLow(core.effectsStart, value);
        break;
    case 0x33C:
        SetHigh(core.effectsEnd, value & 0x3F);
        break;
    case 0x340:
        core.endx &= 0xFF0000;
        break;
    case 0x342:
        core.endx &= 0xFFFF;
        break;
    case 0x344:
        // STATX is read only
        break;
    default:
        g_Plain[((core.index != 0 ? 0x400 : 0) + offset) >> 1] = value;
        break;
    }
}

u16 ReadCore(Core& core, u32 offset)
{
    if (offset < 0x180)
    {
        const Voice& voice = core.voices[offset >> 4];
        switch ((offset >> 1) & 7)
        {
        case 0:
            return voice.left.reg;
        case 1:
            return voice.right.reg;
        case 2:
            return voice.pitch;
        case 3:
            return voice.envelope.adsr1;
        case 4:
            return voice.envelope.adsr2;
        case 5:
            return static_cast<u16>(voice.envelope.value);
        case 6:
            return static_cast<u16>(voice.left.value);
        default:
            return static_cast<u16>(voice.right.value);
        }
    }

    if (offset >= 0x1C0 && offset < 0x2E0)
    {
        u32 relative = offset - 0x1C0;
        const Voice& voice = core.voices[relative / 12];
        u32 address = 0;
        switch ((relative % 12) / 4)
        {
        case 0:
            address = voice.startAddress;
            break;
        case 1:
            address = voice.loopAddress;
            break;
        default:
            address = voice.nextAddress;
            break;
        }

        return (relative & 2) == 0 ? static_cast<u16>(address >> 16) : static_cast<u16>(address);
    }

    if (offset >= 0x2E4 && offset < 0x2E4 + ReverbAddressCount * 4)
    {
        u32 relative = offset - 0x2E4;
        u32 address = *ReverbAddress(core.reverb, ReverbRegisterOf(core, relative / 4));
        return (relative & 2) == 0 ? static_cast<u16>(address >> 16) : static_cast<u16>(address);
    }

    switch (offset)
    {
    case 0x180:
        return static_cast<u16>(core.pmon);
    case 0x182:
        return static_cast<u16>(core.pmon >> 16);
    case 0x184:
        return static_cast<u16>(core.non);
    case 0x186:
        return static_cast<u16>(core.non >> 16);
    case 0x188:
        return static_cast<u16>(core.vmixl);
    case 0x18A:
        return static_cast<u16>(core.vmixl >> 16);
    case 0x18C:
        return static_cast<u16>(core.vmixel);
    case 0x18E:
        return static_cast<u16>(core.vmixel >> 16);
    case 0x190:
        return static_cast<u16>(core.vmixr);
    case 0x192:
        return static_cast<u16>(core.vmixr >> 16);
    case 0x194:
        return static_cast<u16>(core.vmixer);
    case 0x196:
        return static_cast<u16>(core.vmixer >> 16);
    case 0x198:
        return core.mmix;
    case 0x19A:
        return core.attr;
    case 0x19C:
        return static_cast<u16>(core.irqa >> 16);
    case 0x19E:
        return static_cast<u16>(core.irqa);
    case 0x1A0:
        return static_cast<u16>(core.keyOn);
    case 0x1A2:
        return static_cast<u16>(core.keyOn >> 16);
    case 0x1A4:
        return static_cast<u16>(core.keyOff);
    case 0x1A6:
        return static_cast<u16>(core.keyOff >> 16);
    case 0x1A8:
        return static_cast<u16>(core.tsa >> 16);
    case 0x1AA:
        return static_cast<u16>(core.tsa);
    case 0x1AC:
    {
        core.activeTsa = core.tsa & Spu2::AddressMask;
        CheckIrq(core.activeTsa);
        u16 value = Memory()[core.activeTsa];
        core.tsa = (core.activeTsa + 1) & Spu2::AddressMask;
        return value;
    }
    case 0x1B0:
        return core.admas;
    case 0x2E0:
        return static_cast<u16>(core.effectsStart >> 16);
    case 0x2E2:
        return static_cast<u16>(core.effectsStart);
    case 0x33C:
        return static_cast<u16>(core.effectsEnd >> 16);
    case 0x33E:
        return static_cast<u16>(core.effectsEnd);
    case 0x340:
        return static_cast<u16>(core.endx);
    case 0x342:
        return static_cast<u16>(core.endx >> 16);
    case 0x344:
        return core.statx;
    default:
        return g_Plain[((core.index != 0 ? 0x400 : 0) + offset) >> 1];
    }
}

// The cores' registers from 0x760 on (0x28 bytes each): the master, effect, external and input volumes, the current master
// volumes, then the reverb's volumes
void WriteCoreExtra(Core& core, u32 offset, u16 value)
{
    switch (offset)
    {
    case 0x00:
        core.masterLeft.Set(value);
        break;
    case 0x02:
        core.masterRight.Set(value);
        break;
    case 0x04:
        core.effectLeft = static_cast<s16>(value);
        break;
    case 0x06:
        core.effectRight = static_cast<s16>(value);
        break;
    case 0x08:
        core.externalLeft = static_cast<s16>(value);
        break;
    case 0x0A:
        core.externalRight = static_cast<s16>(value);
        break;
    case 0x0C:
        core.inputLeft = static_cast<s16>(value);
        break;
    case 0x0E:
        core.inputRight = static_cast<s16>(value);
        break;
    case 0x10:
    case 0x12:
        // MVOLX can't be written
        break;
    default:
        if (offset >= 0x14 && offset < 0x28)
        {
            *ReverbVolume(core.reverb, (offset - 0x14) / 2) = static_cast<s16>(value);
        }

        break;
    }
}

u16 ReadCoreExtra(Core& core, u32 offset)
{
    switch (offset)
    {
    case 0x00:
        return core.masterLeft.reg;
    case 0x02:
        return core.masterRight.reg;
    case 0x04:
        return static_cast<u16>(core.effectLeft);
    case 0x06:
        return static_cast<u16>(core.effectRight);
    case 0x08:
        return static_cast<u16>(core.externalLeft);
    case 0x0A:
        return static_cast<u16>(core.externalRight);
    case 0x0C:
        return static_cast<u16>(core.inputLeft);
    case 0x0E:
        return static_cast<u16>(core.inputRight);
    case 0x10:
        return static_cast<u16>(core.masterLeft.value);
    case 0x12:
        return static_cast<u16>(core.masterRight.value);
    default:
        if (offset >= 0x14 && offset < 0x28)
        {
            return static_cast<u16>(*ReverbVolume(core.reverb, (offset - 0x14) / 2));
        }

        return 0;
    }
}

void ResetCore(Core& core, s32 index)
{
    core = Core{};
    core.index = index;
    core.mmix = index != 0 ? 0xFFC : 0xFF0;
    core.vmixl = core.vmixr = core.vmixel = core.vmixer = 0xFFFFFF;
    core.effectsStart = index != 0 ? 0xFFFF8 : 0xEFFF8;
    core.effectsEnd = index != 0 ? 0xFFFFF : 0xEFFFF;
    core.irqa = 0x800;
    core.admaWantedHalf = -1;
    core.statx = 0x80;
    core.endx = 0xFFFFFF;
    for (Voice& voice : core.voices)
    {
        voice.pitch = 0x3FFF;
        voice.nextAddress = 0x2801;
        voice.startAddress = 0x2800;
        voice.loopAddress = 0x2800;
    }
}
}

u16* Spu2::Memory()
{
    return g_Memory;
}

void Spu2::SetVoiceGain(s32 voice, s32 gain)
{
    g_Gains.gain[voice].store(gain, std::memory_order_relaxed);
}

void Spu2::LogVoice(s32 index)
{
    const Core& core = g_Cores[index / 24];
    const Voice& v = core.voices[index % 24];
    const u16* block = Memory() + (v.startAddress & ~7u);
    Ps2::Log("spu2: voice %d: ssa %#x lsa %#x nax %#x pitch %#x adsr %04x %04x env phase %d value %#x vol %04x/%04x (%d/%d) "
                "vmix %d%d%d%d, first block %04x %04x %04x",
                index, v.startAddress, v.loopAddress, v.nextAddress, v.pitch, v.envelope.adsr1, v.envelope.adsr2,
                v.envelope.phase, v.envelope.value, v.left.reg, v.right.reg, v.left.value, v.right.value,
                (core.vmixl >> (index % 24)) & 1, (core.vmixr >> (index % 24)) & 1, (core.vmixel >> (index % 24)) & 1,
                (core.vmixer >> (index % 24)) & 1, block[0], block[1], block[2]);
    Ps2::Log("spu2: core %d: attr %04x mmix %03x mvol %04x/%04x (%d/%d), core 1 mvol %d/%d avol %d/%d", core.index, core.attr,
                core.mmix, core.masterLeft.reg, core.masterRight.reg, core.masterLeft.value, core.masterRight.value,
                g_Cores[1].masterLeft.value, g_Cores[1].masterRight.value, g_Cores[1].externalLeft, g_Cores[1].externalRight);
}

void Spu2::SetInputGain(s32 gain)
{
    g_InputGain.store(gain, std::memory_order_relaxed);
}

u32 Spu2::AdmaRemaining(s32 core)
{
    return g_Cores[core].admaLeft;
}

void Spu2::SetMuted(bool muted)
{
    g_Muted.store(muted, std::memory_order_relaxed);
}

void Spu2::SetInterruptFunction(InterruptFunction function)
{
    g_Interrupt = function;
}

void Spu2::Reset()
{

    ResetCore(g_Cores[0], 0);
    ResetCore(g_Cores[1], 1);
    std::memset(g_Plain, 0, sizeof(g_Plain));
    std::memset(g_SpdifRegisters, 0, sizeof(g_SpdifRegisters));
    g_Cycles = 0;
    g_OutputPosition = 0;
}

void Spu2::Write(u32 address, u16 value)
{
    u32 offset = (address - RegisterBase) & (RegisterSize - 1) & ~1u;
    if (offset < 0x760)
    {
        WriteCore(g_Cores[(offset & 0x400) != 0 ? 1 : 0], offset & 0x3FF, value);
    }
    else if (offset < 0x7B0)
    {
        u32 relative = offset - 0x760;
        WriteCoreExtra(g_Cores[relative / 0x28], relative % 0x28, value);
    }
    else if (offset >= 0x7C0 && offset < 0x7D0)
    {
        g_SpdifRegisters[(offset - 0x7C0) / 2] = value;
    }
    else
    {
        g_Plain[offset >> 1] = value;
    }
}

u16 Spu2::Read(u32 address)
{
    u32 offset = (address - RegisterBase) & (RegisterSize - 1) & ~1u;
    if (offset < 0x760)
    {
        return ReadCore(g_Cores[(offset & 0x400) != 0 ? 1 : 0], offset & 0x3FF);
    }

    if (offset < 0x7B0)
    {
        u32 relative = offset - 0x760;
        return ReadCoreExtra(g_Cores[relative / 0x28], relative % 0x28);
    }

    if (offset >= 0x7C0 && offset < 0x7D0)
    {
        return g_SpdifRegisters[(offset - 0x7C0) / 2];
    }

    return g_Plain[offset >> 1];
}

u32 Spu2::DmaWrite(s32 core, const u16* data, u32 halfwords)
{
    Core& c = g_Cores[core];
    if ((c.admas & (core + 1)) != 0)
    {
        c.admaData = data;
        c.admaLeft = halfwords;
        c.admaPending = true;
        c.statx &= ~0x80;
        // A half of the input buffer that asked for a block while there was none gets it at once
        if (c.admaWantedHalf >= 0)
        {
            TransferAdmaBlock(c, static_cast<u32>(c.admaWantedHalf));
            c.admaWantedHalf = -1;
        }
        return 0;
    }

    c.activeTsa = c.tsa & AddressMask;
    u32 start = c.activeTsa;
    for (u32 i = 0; i < halfwords; i++)
    {
        Memory()[(start + i) & AddressMask] = data[i];
    }

    u32 end = (start + halfwords) & AddressMask;
    // IRQA between the start and the end (exclusive) of what was written
    for (s32 i = 0; i < 2; i++)
    {
        u32 irqa = g_Cores[i].irqa & AddressMask;
        bool wrapped = start + halfwords > MemoryHalfwords;
        if (g_Cores[i].irqEnable && (wrapped ? (irqa > start || irqa < end) : (irqa > start && irqa < end)))
        {
            RaiseIrq(i);
        }
    }

    c.activeTsa = end;
    c.tsa = end;
    c.statx &= ~0x80;
    c.statx |= 0x400;
    return halfwords * CyclesPerDmaWord;
}

u32 Spu2::DmaRead(s32 core, u16* data, u32 halfwords)
{
    Core& c = g_Cores[core];
    c.activeTsa = c.tsa & AddressMask;
    u32 start = c.activeTsa;
    for (u32 i = 0; i < halfwords; i++)
    {
        data[i] = Memory()[(start + i) & AddressMask];
    }

    u32 end = (start + halfwords) & AddressMask;
    for (s32 i = 0; i < 2; i++)
    {
        u32 irqa = g_Cores[i].irqa & AddressMask;
        bool wrapped = start + halfwords > MemoryHalfwords;
        if (g_Cores[i].irqEnable && (wrapped ? (irqa > start || irqa < end) : (irqa > start && irqa < end)))
        {
            RaiseIrq(i);
        }
    }

    c.activeTsa = end;
    c.tsa = end;
    c.statx &= ~0x80;
    c.statx |= 0x400;
    return halfwords * CyclesPerDmaWord;
}

void Spu2::DmaFinished(s32 core)
{
    Core& c = g_Cores[core];
    c.statx &= ~0x400;
    c.statx |= 0x80;
    if (g_Interrupt != nullptr)
    {
        g_Interrupt(core == 0 ? Dma4Interrupt : Dma7Interrupt);
    }
}

Spu2::Stereo Spu2::Tick()
{
    for (Core& core : g_Cores)
    {
        if (core.keyOff != 0)
        {
            StopVoices(core, core.keyOff);
            core.keyOff = 0;
        }

        if (core.keyOn != 0)
        {
            StartVoices(core, core.keyOn);
            core.keyOn = 0;
        }
    }

    Stereo inputs[2];
    for (s32 i = 0; i < 2; i++)
    {
        Stereo input = ReadInput(g_Cores[i]);
        s32 inputGain = g_InputGain.load(std::memory_order_relaxed);
        if (inputGain != 0x8000)
        {
            input = {(input.left * inputGain) >> 15, (input.right * inputGain) >> 15};
        }

        inputs[i] = {ApplyVolume(input.left, g_Cores[i].inputLeft), ApplyVolume(input.right, g_Cores[i].inputRight)};
    }

    VoiceMix voices[2] = {MixVoices(g_Cores[0]), MixVoices(g_Cores[1])};
    Stereo external = MixCore(g_Cores[0], voices[0], inputs[0], {0, 0});
    external = Clamp16(external);
    external = {ApplyVolume(external.left, g_Cores[0].masterLeft.value), ApplyVolume(external.right, g_Cores[0].masterRight.value)};
    // Core 0's output is written to the memory before core 1 mixes it as its external input
    WriteMemory(0x800 + g_OutputPosition, static_cast<s16>(external.left));
    WriteMemory(0xA00 + g_OutputPosition, static_cast<s16>(external.right));
    external = {ApplyVolume(external.left, g_Cores[1].externalLeft), ApplyVolume(external.right, g_Cores[1].externalRight)};
    Stereo out = MixCore(g_Cores[1], voices[1], inputs[1], external);
    out = Clamp16(out);
    out = {ApplyVolume(out.left, g_Cores[1].masterLeft.value), ApplyVolume(out.right, g_Cores[1].masterRight.value)};
    g_OutputPosition = (g_OutputPosition + 1) & 0x1FF;
    g_Cycles++;
    if (g_Muted.load(std::memory_order_relaxed))
    {
        return {0, 0};
    }

    return out;
}
