// modhsyn's voices (ps2/hsyn.h): their allocation and stealing (0x6578, 0x6CBC, 0x6D60, 0x6F64), notes on and off (0x66FC,
// 0x5DF4), the damper and the channel-wide stops (0x60D4, 0x6214, 0x6354, 0x70C4), a note's parameters from the HD
// (0x3D3C), its key on (0x4110), and each tick's work (0x394C: key offs, key ons once the SPU2 voice is quiet; 0x4F2C: ended
// voices freed, portamento, the LFOs, pitch and volume written). Everything through libsd as the module calls it
#include "hsyn_state.h"
#include "ps2/sound.h"

namespace Ps2::Hsyn
{
namespace
{
constexpr u32 VoicesPerCore = 24;
// The ADSR2 written to cut a voice (the fastest linear release)
constexpr u16 CutRelease = 0xC021;

struct Core
{
    std::array<Voice, VoicesPerCore> voices;
    VoiceList free;    // 0xBDE0
    VoiceList active;  // 0xBE20: keyed on
    VoiceList pending; // 0xFD70: waiting for their key on
    u8 freeCount = 0;  // 0xFD88
    u32 keyOff = 0;    // 0xBE38
    u32 keyOn = 0;     // 0xBE00
    u32 noise = 0;     // 0xBDF8
    u32 mixDryLeft = 0, mixDryRight = 0, mixWetLeft = 0, mixWetRight = 0; // 0xBE40, 0xBE44, 0xFD90, 0xFD94
};
std::array<Core, 2> g_cores;
// 0xBE34: the tick reads ENVX to find ended voices (a note's reclaiming takes it for the tick)
bool g_endsChecked = true;
// 0xFDA1: HD voices playing
u32 g_hdVoices = 0;
// 0xBD80: the noise LFO's generator, its first value the module's .data
u32 g_seed = 0x02CEE9D7;

// Each voice's channel and global list, so it can be taken out of them (the module's links in the voice)
std::array<std::array<VoiceList*, VoicesPerCore>, 2> g_channelList = {};
std::array<std::array<VoiceList*, VoicesPerCore>, 2> g_globalList = {};

u16 VoiceEntry(const Voice& v, u16 reg) { return static_cast<u16>(reg | (v.index << 1) | v.core); }
u32 Bit(const Voice& v) { return 1u << v.index; }

void LinkChannel(Voice& v, VoiceList& list)
{
    list.PushFront(&v);
    g_channelList[v.core][v.index] = &list;
}
void UnlinkChannel(Voice& v)
{
    VoiceList*& list = g_channelList[v.core][v.index];
    if (list == nullptr)
    {
        Fatal("hsyn: voice %u of core %u in no channel's list", v.index, v.core);
    }
    list->Remove(&v);
    list = nullptr;
}
void LinkGlobal(Voice& v, VoiceList& list)
{
    list.PushFront(&v);
    g_globalList[v.core][v.index] = &list;
}
void UnlinkGlobal(Voice& v)
{
    VoiceList*& list = g_globalList[v.core][v.index];
    list->Remove(&v);
    list = nullptr;
}

s16 Random()
{
    g_seed = g_seed * 0x41C64E6D + 0x3039;
    return static_cast<s16>(static_cast<s32>(g_seed) >> 16);
}

// Signed division truncating toward zero by a power of two, as the module's shifts with their rounding add
s32 Shift(s32 x, int bits) { return x < 0 ? -((-x) >> bits) : x >> bits; }
s32 Clamp16(s32 x) { return std::clamp(x, -0x8000, 0x7FFF); }
s32 Clamp8(s32 x) { return std::clamp(x, -0x80, 0x7F); }

// --- the curves (0x8B20-0x918C)
// 0x902C's functions (0xBBA0): a velocity's factor (16.16, signed) against a centre velocity
s32 FollowCurve(u32 curve, s32 v, s32 c)
{
    auto sq14 = [](s32 a) { return Shift(a * a, 14); };
    switch (curve)
    {
    case 0:
        return ((v - c) << 16) / 126;
    case 1:
        return ((c - v) << 16) / 126;
    case 2:
    {
        s32 a = ((v - 1) << 15) / 126, b = ((c - 1) << 15) / 126;
        return Shift(a * a - b * b, 14);
    }
    case 3:
    {
        s32 a = ((v - 1) << 15) / 126, b = ((c - 1) << 15) / 126;
        return (0x10000 - sq14(a)) - (0x10000 - sq14(b));
    }
    case 4:
    {
        s32 a = 0x10000 - ((v - 1) << 15) / 126, b = 0x10000 - ((c - 1) << 15) / 126;
        return (0x10000 - sq14(a)) - (0x10000 - sq14(b));
    }
    case 5:
    {
        s32 a = 0x10000 - ((v - 1) << 15) / 126, b = 0x10000 - ((c - 1) << 15) / 126;
        return sq14(a) - sq14(b);
    }
    case 6:
    case 7:
    {
        if (v == c)
        {
            return 0;
        }
        s32 d = c < v ? 0x7F - c : c - 1;
        return ((curve == 6 ? v - c : c - v) << 16) / d;
    }
    case 8:
    case 9:
    {
        if (v == c)
        {
            return 0;
        }
        s32 x = (v - c) << 15;
        if (c < v)
        {
            s32 y = x / (0x7F - c);
            return curve == 8 ? sq14(y) : 0x10000 - sq14(0x8000 - y);
        }
        s32 y = x / (c - 1);
        return curve == 8 ? -sq14(y) : sq14(y + 0x8000) - 0x10000;
    }
    case 10:
        // The table's eleventh entry is the next table's first (the velocity itself)
        return v & 0xFF;
    default:
        return 0;
    }
}

// 0x91B4's functions (0xBBC8): the velocity's volume (0-128)
u8 VelocityCurve(u32 curve, u8 v)
{
    // 0x90EC: v * v / 127, at least 1
    auto square = [](u8 x) -> u8 {
        u32 q = (static_cast<u32>(x) * x) / 127;
        return q == 0 ? 1 : static_cast<u8>(q);
    };
    switch (curve)
    {
    case 0:
    case 6:
        return v;
    case 1:
    case 7:
        return static_cast<u8>(0x80 - v);
    case 2:
    case 8:
        return square(v);
    case 3:
    case 9:
        return static_cast<u8>(0x80 - square(v));
    case 4:
        return static_cast<u8>(0x80 - square(static_cast<u8>(0x80 - v)));
    case 5:
        return square(static_cast<u8>(0x80 - v));
    default:
        // The eleventh entry is the sine table's first words, not a function
        Fatal("hsyn: velocity curve %u (the module would jump into its data)", curve);
    }
}

// 0x91B4: a curve's high nibble picks one of the port's custom velocity tables, which only an export no driver here calls
// sets: with none, the module skips it as out of range, as here
u8 Velocity(u8 velocity, u8 curve)
{
    if (velocity == 0)
    {
        velocity = 1;
    }
    return VelocityCurve((curve & 0xF) < 0xB ? curve & 0xF : 0, velocity);
}

// 0x49F4 (0x902C): a velocity follow's amount (its custom tables skipped as in Velocity)
s32 VelocityFollow(const Voice& v, s8 amount, u8 centre, u8 curve)
{
    s32 velocity = v.velocity == 0 ? 1 : v.velocity;
    s32 f = FollowCurve((curve & 0xF) < 0xB ? curve & 0xF : 0, velocity, centre);
    return Shift(f * amount, 16);
}

// 0x3B94: a key follow on one of the ADSR's fields: (key - centre) * amount / 6 added, clamped to the field
u16 KeyFollow(u16 adsr, s8 amount, s32 delta, u16 mask, int shift)
{
    s32 d = delta * amount / 6;
    if (d == 0)
    {
        return adsr;
    }
    s32 field = std::clamp<s32>(((adsr >> shift) & mask) + d, 0, mask);
    return static_cast<u16>((adsr & ~(mask << shift)) | (field << shift));
}

// 0x3C1C and 0x3CC0: the sample's ADSR with its key follows (attack, decay and sustain level; sustain and release rates)
u16 Adsr1(const Voice& v)
{
    u32 s = v.sample;
    u16 a = Hd16(s + 0x12);
    if (HdS8(s + 0x16) != 0)
    {
        a = KeyFollow(a, HdS8(s + 0x16), v.key - Hd8(s + 0x17), 0x7F, 8);
    }
    if (HdS8(s + 0x18) != 0)
    {
        a = KeyFollow(a, HdS8(s + 0x18), v.key - Hd8(s + 0x19), 0xF, 4);
    }
    if (HdS8(s + 0x1E) != 0)
    {
        a = KeyFollow(a, HdS8(s + 0x1E), v.key - Hd8(s + 0x1F), 0xF, 0);
    }
    return a;
}

u16 Adsr2(const Voice& v)
{
    u32 s = v.sample;
    u16 a = Hd16(s + 0x14);
    if (HdS8(s + 0x1A) != 0)
    {
        a = KeyFollow(a, HdS8(s + 0x1A), v.key - Hd8(s + 0x1B), 0x7F, 6);
    }
    if (HdS8(s + 0x1C) != 0)
    {
        a = KeyFollow(a, HdS8(s + 0x1C), v.key - Hd8(s + 0x1D), 0x1F, 0);
    }
    return a;
}

// 0x4DD4: the sample's velocity cross-fade and the split's key cross-fade (16.16): where the range's bit 7 is set, the
// edge fades over the band between it and the cross-fade point
u32 CrossFade(const Voice& v)
{
    auto fade = [](u32 gain, u8 at, u32 range) {
        u8 low = Hd8(range + 2), cross = Hd8(range + 3), high = Hd8(range + 4);
        if (at < cross)
        {
            if ((low & 0x80) == 0 || (low & 0x7F) == cross)
            {
                return gain;
            }
            u32 edge = (low & 0x7F) - 1u;
            return gain * (at - edge) / (cross - edge);
        }
        if ((high & 0x80) == 0 || (high & 0x7F) == cross)
        {
            return gain;
        }
        u32 edge = (high & 0x7Fu) + 1;
        return gain * (edge - at) / (edge - cross);
    };
    u32 g = 0x10000;
    {
        // The velocity's: the gain itself is the quotient (16.16 from a 16 bit shift)
        u32 s = v.sample;
        u8 low = Hd8(s + 2), cross = Hd8(s + 3), high = Hd8(s + 4);
        if (v.velocity < cross)
        {
            if ((low & 0x80) != 0 && (low & 0x7F) != cross)
            {
                u32 edge = (low & 0x7Fu) - 1;
                g = ((v.velocity - edge) << 16) / (cross - edge);
            }
        }
        else if ((high & 0x80) != 0 && (high & 0x7F) != cross)
        {
            u32 edge = (high & 0x7Fu) + 1;
            g = ((edge - v.velocity) << 16) / (edge - cross);
        }
    }
    return fade(g, v.key, v.split);
}

// --- the LFOs (0x9554, 0x959C, 0x98E0, 0x937C)
// The waves (0x9240-0x9348) and the sine table (0xBBF0: 200 entries of sin, 32767 at the peak)
const s16 g_sine[200] = {
    0,      1029,   2057,   3083,   4106,   5125,   6139,   7147,   8148,   9141,   10125,  11099,  12062,  13013,  13951,
    14875,  15785,  16679,  17557,  18417,  19259,  20083,  20886,  21669,  22430,  23169,  23886,  24578,  25247,  25891,
    26509,  27100,  27666,  28203,  28713,  29195,  29648,  30072,  30465,  30829,  31163,  31465,  31737,  31977,  32186,
    32363,  32508,  32621,  32702,  32750,  32767,  32750,  32702,  32621,  32508,  32363,  32186,  31977,  31737,  31465,
    31163,  30829,  30465,  30072,  29648,  29195,  28713,  28203,  27666,  27100,  26509,  25891,  25247,  24578,  23886,
    23169,  22430,  21669,  20886,  20083,  19259,  18417,  17557,  16679,  15785,  14875,  13951,  13013,  12062,  11099,
    10125,  9141,   8148,   7147,   6139,   5125,   4106,   3083,   2057,   1029,   0,      -1029,  -2057,  -3083,  -4106,
    -5125,  -6139,  -7147,  -8148,  -9141,  -10125, -11099, -12062, -13013, -13951, -14875, -15785, -16679, -17557, -18417,
    -19259, -20083, -20886, -21669, -22430, -23169, -23886, -24578, -25247, -25891, -26509, -27100, -27666, -28203, -28713,
    -29195, -29648, -30072, -30465, -30829, -31163, -31465, -31737, -31977, -32186, -32363, -32508, -32621, -32702, -32750,
    -32767, -32750, -32702, -32621, -32508, -32363, -32186, -31977, -31737, -31465, -31163, -30829, -30465, -30072, -29648,
    -29195, -28713, -28203, -27666, -27100, -26509, -25891, -25247, -24578, -23886, -23169, -22430, -21669, -20886, -20083,
    -19259, -18417, -17557, -16679, -15785, -14875, -13951, -13013, -12062, -11099, -10125, -9141,  -8148,  -7147,  -6139,
    -5125,  -4106,  -3083,  -2057,  -1029};

s16 Wave(Lfo& l, bool wrapped)
{
    s32 p = l.phase;
    switch (l.wave)
    {
    case 1: // saw up
        return static_cast<s16>(((p - 0x8000) << 15) >> 16);
    case 2: // saw down
        return static_cast<s16>(((0xFFFF - p - 0x8000) << 15) >> 16);
    case 3: // triangle
        if (p > 0xBFFF)
        {
            p -= 0x10000;
        }
        else if (p > 0x4000)
        {
            p = 0x8000 - p;
        }
        return static_cast<s16>(p);
    case 4: // square
        return p > 0x7FFF ? 0x4000 : -0x4000;
    case 5: // noise: a new value each cycle
        return wrapped ? static_cast<s16>(Random() >> 2) : l.out;
    case 6: // the sine table, interpolated
    {
        u32 x = static_cast<u32>(p) * 200;
        u32 i = x >> 16;
        u32 j = i + 1 < 200 ? i + 1 : 0;
        s32 a = g_sine[i];
        s32 y = a + (((g_sine[j] - a) * static_cast<s32>(x & 0xFFFF)) >> 16);
        y += y < 0 ? 1 : 0;
        return static_cast<s16>(static_cast<s32>(static_cast<u32>(y) << 15) >> 16);
    }
    default:
        return 0;
    }
}

void LfoStart(Lfo& l)
{
    if (l.delay != 0)
    {
        l.state = 1;
        l.counter = l.delay;
    }
    else if (l.fadeStep != 0)
    {
        l.state = 2;
        l.fade = 0;
    }
    else
    {
        l.state = 3;
    }
}

// 0x959C: an LFO from the program's wave (pitch: its first, volume: its second), the sample's delay and fade (ms) and its
// attribute bits (0x55 start at the key on, 0x66 react to the release)
void LfoSetup(Lfo& l, u32 program, u8 attributes, u16 delayMs, u16 fadeMs, bool second)
{
    l = {};
    if ((attributes & 0x77) == 0)
    {
        return;
    }
    u8 wave = Hd8(program + (second ? 0xF : 0xE));
    if ((wave & 0x80) != 0)
    {
        Fatal("hsyn: a custom LFO wave (%#x) asked for: not done", wave);
    }
    if (wave == 0 || wave >= 7)
    {
        return;
    }
    l.wave = wave;
    u32 tick = g_tickUnit;
    l.delay = static_cast<u16>((delayMs * 256 + tick / 2) / tick);
    u32 fadeTicks = (fadeMs * 256 + tick / 2) / tick;
    if (fadeTicks != 0)
    {
        u32 step = 0x10000 / fadeTicks;
        l.fadeStep = static_cast<s32>(step == 0 ? 1 : step);
    }
    s32 phase = Hd8(program + (second ? 0x13 : 0x12)) << 8;
    phase = Shift(phase * Random(), 16);
    phase += Hd8(program + (second ? 0x11 : 0x10)) << 8;
    if (phase < 0)
    {
        phase += 0x10000;
    }
    else if (phase > 0xFFFF)
    {
        phase -= 0x10000;
    }
    l.phase = phase;
    u32 cycle = static_cast<u32>(Hd16(program + (second ? 0x16 : 0x14))) << 8;
    u32 cycleTicks = (cycle + tick / 2) / tick;
    if (cycleTicks == 0)
    {
        // The module divides by it
        Fatal("hsyn: an LFO with no cycle (program at IOP %06x)", program);
    }
    l.phaseStep = static_cast<s32>(0x10000 / cycleTicks);
    if ((attributes & 0x66) != 0)
    {
        l.releases = 1;
    }
    if ((attributes & 0x55) != 0)
    {
        LfoStart(l);
    }
}

// 0x98E0: the key released
void LfoRelease(Lfo& l)
{
    if (l.releases == 0)
    {
        return;
    }
    switch (l.state)
    {
    case 0:
        LfoStart(l);
        break;
    case 1:
        l.state = 6;
        break;
    case 2:
    case 3:
        if (l.delay != 0)
        {
            l.state = 4;
            l.counter = l.delay;
        }
        else if (l.fadeStep != 0)
        {
            l.state = 5;
            l.fade = 0x10000;
        }
        else
        {
            l.state = 6;
        }
        break;
    default:
        l.state = 6;
        break;
    }
}

// 0x937C: a tick of the LFO
void LfoTick(Lfo& l)
{
    s32 out = 0;
    u8 st = l.state;
    if (st == 0 || st >= 6)
    {
        l.out = 0;
        return;
    }
    if (st >= 2)
    {
        s32 phase = l.phase + l.phaseStep;
        bool wrapped = false;
        if (phase > 0xFFFF)
        {
            phase -= 0x10000;
            wrapped = true;
        }
        l.phase = phase;
        out = Wave(l, wrapped);
    }
    switch (st)
    {
    case 1:
        if (--l.counter == 0)
        {
            if (l.fadeStep != 0)
            {
                l.state = 2;
                l.fade = 0;
            }
            else
            {
                l.state = 3;
            }
        }
        break;
    case 2:
        out = static_cast<s16>(Shift(out * l.fade, 16));
        l.fade += l.fadeStep;
        if (l.fade > 0xFFFF)
        {
            l.state = 3;
        }
        break;
    case 4:
        if (--l.counter == 0)
        {
            if (l.fadeStep != 0)
            {
                l.fade = 0x10000;
                l.state = 5;
            }
            else
            {
                l.state = 6;
            }
        }
        break;
    case 5:
        out = static_cast<s16>(Shift(out * l.fade, 16));
        l.fade -= l.fadeStep;
        if (l.fade <= 0)
        {
            l.state = 6;
        }
        break;
    default:
        break;
    }
    l.out = static_cast<s16>(out);
}

// --- a voice's parameters
// 0x45A8: the pitch from the key, the program's, split's and sample's transposes and tunings, the bend, the pitch LFO and the
// portamento; written when it changed
void UpdatePitch(Voice& v)
{
    s32 lfo = v.pitchLfo.out;
    s32 cents = Shift((lfo >= 0 ? v.pitchDepth[0] : v.pitchDepth[1]) * lfo, 14);
    cents += v.bend + Shift(v.glide, 4);
    if (cents == v.lastPitch)
    {
        return;
    }
    v.lastPitch = cents;
    u8 centre = Hd8(v.sample + 0xB);
    s32 note = v.key + HdS8(v.program + 8) + HdS8(v.split + 0x12);
    cents += HdS8(v.program + 9) + HdS8(v.sample + 0xC) + HdS8(v.split + 0x13);
    if (cents < 0)
    {
        s32 octaves = Shift(cents, 7);
        note += octaves;
        cents -= octaves * 128;
        if (cents != 0)
        {
            cents += 0x80;
            note -= 1;
        }
    }
    u32 pitch = SdNote2Pitch(centre, 0, static_cast<u16>(note), static_cast<s16>(cents));
    pitch = (pitch * v.rateScale) >> 16;
    SdSetParam(VoiceEntry(v, SD_VP_PITCH), static_cast<u16>(std::min<u32>(pitch, 0x3FFF)));
}

// 0x4800: the volumes from the level, the port's volume, the volume LFO and the pan; written when they changed (negative,
// the phase inverted, when the pan says so)
void UpdateVolume(Voice& v)
{
    s32 level = Shift(v.level * v.port->volume, 8);
    s32 lfo = v.ampLfo.out;
    s32 swing = Shift(lfo * (lfo >= 0 ? v.ampDepth[0] : v.ampDepth[1]), 7) + 0x4000;
    s32 l = std::max(Shift(level * swing, 14), 0);
    u32 left = std::min<u32>(static_cast<u32>(v.panLeft * l) >> 7, 0x3FFF);
    u32 right = std::min<u32>(static_cast<u32>(v.panRight * l) >> 7, 0x3FFF);
    if (left == v.lastLeft && right == v.lastRight)
    {
        return;
    }
    v.lastLeft = static_cast<u16>(left);
    v.lastRight = static_cast<u16>(right);
    if (v.inPhase != 0)
    {
        SdSetParam(VoiceEntry(v, SD_VP_VOLL), static_cast<u16>(left));
        SdSetParam(VoiceEntry(v, SD_VP_VOLR), static_cast<u16>(right));
    }
    else
    {
        SdSetParam(VoiceEntry(v, SD_VP_VOLL), static_cast<u16>(-static_cast<s32>(left) & 0x7FFF));
        SdSetParam(VoiceEntry(v, SD_VP_VOLR), static_cast<u16>(-static_cast<s32>(right) & 0x7FFF));
    }
}

// 0x4BB0: the pan's base from the program's, split's and sample's pans and the key follow (pan by phase: wrapped round)
void PanBase(Voice& v, s32 keyFollow)
{
    s32 programPan = HdS8(v.program + 7), splitPan = HdS8(v.split + 0x11), samplePan = HdS8(v.sample + 0xD);
    s32 p;
    if (v.panPhase != 0)
    {
        p = programPan - 0x80 + splitPan + samplePan + keyFollow;
        while (p >= 0x80)
        {
            p -= 0xFF;
        }
        while (p < -0x80)
        {
            p += 0xFF;
        }
    }
    else
    {
        p = std::abs(programPan) - 0x80 + std::abs(splitPan) + std::abs(samplePan) + keyFollow;
        p = std::clamp(p, 0, 0x7F);
        v.inPhase = 1;
    }
    v.panBase = static_cast<s16>(p);
}

// 0x53DC: portamento from the channel's last note over its time
void StartGlide(Voice& v)
{
    Channel& c = *v.channel;
    s32 glide = (c.portamentoNote - v.key) << 11;
    v.glide = glide;
    if (glide < 0)
    {
        v.flags |= GlideUp;
        glide = -glide;
    }
    else
    {
        v.flags |= GlideDown;
    }
    u32 step = static_cast<u32>(glide) * g_tickUnit / c.portamentoTime;
    v.glideStep = static_cast<u16>(step >> 8);
}

// 0x3D3C: a new note's parameters: its LFOs and their depths, its gain, pan, expression and bend
void Setup(Voice& v)
{
    Channel& c = *v.channel;
    u32 program = v.program, split = v.split, sample = v.sample;
    u8 attributes = Hd8(sample + 0x28);
    LfoSetup(v.pitchLfo, program, attributes & 7, Hd16(sample + 0x20), Hd16(sample + 0x22), false);
    s32 keyFollow = (v.key - Hd8(split + 0xB)) * HdS8(split + 0xA) / 6;
    s32 velocityFollow = VelocityFollow(v, HdS8(sample + 5), Hd8(sample + 6), Hd8(sample + 7));
    v.pitchBase[0] = static_cast<s16>(Clamp16(HdS16(program + 0x18) + keyFollow + velocityFollow));
    v.pitchBase[1] = static_cast<s16>(Clamp16(HdS16(program + 0x1A) + keyFollow + velocityFollow));
    LfoSetup(v.ampLfo, program, attributes & 0x70, Hd16(sample + 0x24), Hd16(sample + 0x26), true);
    keyFollow = (v.key - Hd8(split + 0xD)) * HdS8(split + 0xC) / 6;
    velocityFollow = VelocityFollow(v, HdS8(sample + 8), Hd8(sample + 9), Hd8(sample + 0xA));
    v.ampBase[0] = static_cast<s8>(Clamp8(HdS8(program + 0x20) + keyFollow + velocityFollow));
    v.ampBase[1] = static_cast<s8>(Clamp8(HdS8(program + 0x21) + keyFollow + velocityFollow));
    SetBreath(v, c.breath);
    SetModulation(v, c.modulation);
    // The products in 32 bits, as the module's multiplies keep them
    u8 velocity = Velocity(v.velocity, v.velocityCurve);
    s32 gain = Shift(static_cast<s32>(static_cast<u32>(Hd8(program + 6)) * Hd8(split + 0x10) * Hd8(sample + 0x10) * velocity), 14);
    v.gain = static_cast<u16>((static_cast<u32>(gain) * CrossFade(v)) >> 16);
    s32 pan = ((v.key - Hd8(split + 0xF)) * HdS8(split + 0xE) + (v.key - Hd8(program + 0xB)) * HdS8(program + 0xA)) / 6;
    v.panPhase = Hd8(program + 0xC) & 1;
    PanBase(v, pan);
    if ((v.panOverride & 0x80) != 0)
    {
        SetPan(v, c.pan);
    }
    else
    {
        SetPan(v, std::max<s32>(static_cast<s8>(v.panOverride - 0x40), -0x3F));
    }
    v.expression = v.volumeOverride != 0xFF ? v.volumeOverride : c.expression;
    UpdateLevel(v);
    v.lastLeft = v.lastRight = 0xFFFF;
    if (v.vag != 0)
    {
        v.lastPitch = 0x7FFFFFFF;
        // The sample's rate / 48 kHz in 16.16, by the module's multiply (0x057619F1, then 42 bits down)
        v.rateScale = static_cast<u32>((static_cast<u64>(static_cast<u32>(Hd16(v.vag + 4)) << 16) * 0x057619F1u) >> 42);
        SetBend(v, (v.noteBend & 0x8000) != 0 ? c.bend : static_cast<s16>(v.noteBend - 0x2000));
    }
}

// 0x4110: the key on: volume, pitch, the sample's start and ADSR (or the noise generator), the mixes, KON's bit
void KeyOn(Voice& v)
{
    Core& core = g_cores[v.core];
    u32 bit = Bit(v);
    UpdateVolume(v);
    if (v.vag != 0)
    {
        core.noise &= ~bit;
        UpdatePitch(v);
        SdSetAddr(VoiceEntry(v, SD_VA_SSA), v.spuBase + Hd32(v.vag));
        SdSetParam(VoiceEntry(v, SD_VP_ADSR1), Adsr1(v));
        SdSetParam(VoiceEntry(v, SD_VP_ADSR2), Adsr2(v));
        if ((Hd8(v.vag + 6) & 1) == 0)
        {
            v.flags |= OneShot;
        }
    }
    else
    {
        SdSetCoreAttr(static_cast<u16>(SD_C_NOISE_CLK | v.core), std::min<u16>(v.key, 0x3F));
        core.noise |= bit;
    }
    SdSetSwitch(static_cast<u16>(SD_S_NON | v.core), core.noise);
    u8 mix = Hd8(v.sample + 0x29);
    auto setBit = [&](u32& mask, bool on) { mask = on ? mask | bit : mask & ~bit; };
    setBit(core.mixDryLeft, (mix & 1) != 0);
    setBit(core.mixDryRight, (mix & 2) != 0);
    setBit(core.mixWetLeft, (mix & 4) != 0);
    setBit(core.mixWetRight, (mix & 8) != 0);
    SdSetSwitch(static_cast<u16>(SD_S_VMIXL | v.core), core.mixDryLeft);
    SdSetSwitch(static_cast<u16>(SD_S_VMIXR | v.core), core.mixDryRight);
    SdSetSwitch(static_cast<u16>(SD_S_VMIXEL | v.core), core.mixWetLeft);
    SdSetSwitch(static_cast<u16>(SD_S_VMIXER | v.core), core.mixWetRight);
    core.keyOn |= bit;
    v.grace = 3;
    v.flags &= ~Pending;
}

// 0x5FA0 (0x6434 takes it out of its channel's list first): back to its core's free voices
void Free(Voice& v)
{
    v.flags = 0;
    if (v.port->voices != 0)
    {
        v.port->voices--;
    }
    if (v.kind == 0 && g_hdVoices != 0)
    {
        g_hdVoices--;
    }
    UnlinkGlobal(v);
    Core& core = g_cores[v.core];
    core.freeCount++;
    LinkGlobal(v, core.free);
    v.id = 0xFF;
    v.kind = v.kind2 = 0xFF;
}

void FreeFromChannel(Voice& v)
{
    UnlinkChannel(v);
    Free(v);
}

// A voice cut: KOFF's bit, the fastest release, freed
void Cut(Voice& v, bool keyOff)
{
    if (keyOff)
    {
        g_cores[v.core].keyOff |= Bit(v);
    }
    SdSetParam(VoiceEntry(v, SD_VP_ADSR2), CutRelease);
    FreeFromChannel(v);
}

// 0x6CBC: a free voice of the core the sample asks for (attribute bits 4-5), or of the core with more free
Voice* TakeFree(u8 attributes)
{
    u32 which = attributes & 0x30;
    int core = which == 0x10 ? 0 : which == 0x20 ? 1 : (g_cores[0].freeCount < g_cores[1].freeCount ? 1 : 0);
    Core& c = g_cores[core];
    if (c.freeCount == 0)
    {
        return nullptr;
    }
    c.freeCount--;
    Voice* v = c.free.Back();
    UnlinkGlobal(*v);
    return v;
}

// A stolen voice taken out of everything (0x6D60 and 0x6F64)
void Unhook(Voice& v)
{
    if (v.port->voices != 0)
    {
        v.port->voices--;
    }
    if (v.kind == 0 && g_hdVoices != 0)
    {
        g_hdVoices--;
    }
    UnlinkChannel(v);
    UnlinkGlobal(v);
    v.flags = 0;
    v.id = 0xFF;
    v.kind = v.kind2 = 0xFF;
}

// 0x6D60: the keyed-on voice least worth keeping: released before held, then the lowest priority not above the new note's,
// then the quietest
Voice* StealActive(u16 priority, u8 attributes)
{
    u32 cores = (attributes & 0x30) != 0 ? attributes & 0x30 : 0x30;
    u16 bestClass = 0x300, bestPriority = priority, bestEnvx = 0xFFFF;
    Voice* best = nullptr;
    for (int core = 1; core >= 0; core--)
    {
        if ((cores & (core == 0 ? 0x10u : 0x20u)) == 0)
        {
            continue;
        }
        for (Voice* v : g_cores[core].active.BackToFront())
        {
            if (v->kind != 0)
            {
                continue;
            }
            u16 held = v->flags & (Held | Keyed);
            if (held < bestClass)
            {
                if (priority < v->priority)
                {
                    continue;
                }
                bestPriority = v->priority;
                bestClass = held;
            }
            else if (held == bestClass)
            {
                if (v->priority == bestPriority)
                {
                    if (v->envx >= bestEnvx)
                    {
                        continue;
                    }
                }
                else if (v->priority < bestPriority)
                {
                    bestPriority = v->priority;
                }
                else
                {
                    continue;
                }
            }
            else
            {
                continue;
            }
            bestEnvx = v->envx;
            best = v;
        }
    }
    if (best != nullptr)
    {
        g_cores[best->core].keyOff |= Bit(*best);
        Unhook(*best);
    }
    return best;
}

// 0x6F64: the waiting voice of the lowest priority below the new note's
Voice* StealPending(u16 priority, u8 attributes)
{
    u32 cores = (attributes & 0x30) != 0 ? attributes & 0x30 : 0x30;
    u32 bestPriority = priority + 1u;
    Voice* best = nullptr;
    for (int core = 1; core >= 0; core--)
    {
        if ((cores & (core == 0 ? 0x10u : 0x20u)) == 0)
        {
            continue;
        }
        for (Voice* v : g_cores[core].pending.BackToFront())
        {
            if (v->kind == 0 && v->priority < bestPriority)
            {
                bestPriority = v->priority;
                best = v;
            }
        }
    }
    if (best != nullptr)
    {
        Unhook(*best);
    }
    return best;
}

// 0x6578: the port at its voices' limit: its voice of the lowest priority not above the new note's, a releasing one first
bool StealInPort(Port& p, u16 priority)
{
    u32 threshold = priority;
    Voice* best = nullptr;
    auto consider = [&](VoiceList& list) {
        for (Voice* v : list.FrontToBack())
        {
            if (v->priority <= threshold)
            {
                best = v;
                threshold = v->priority;
            }
        }
    };
    for (Channel& c : p.channels)
    {
        consider(c.lists[2]);
    }
    if (best == nullptr)
    {
        for (Channel& c : p.channels)
        {
            consider(c.lists[0]);
            consider(c.lists[1]);
        }
    }
    if (best == nullptr)
    {
        return false;
    }
    Cut(*best, true);
    return true;
}

// 0x70C4: the channel's voices of the sample's exclusive group cut
void ExclusiveGroup(Channel& c, u8 group)
{
    for (size_t l = 0; l < c.lists.size(); l++)
    {
        for (Voice* v : c.lists[l].BackToFront())
        {
            if (v->group != group)
            {
                continue;
            }
            if ((v->flags & Pending) != 0)
            {
                FreeFromChannel(*v);
                continue;
            }
            Cut(*v, l < 2);
        }
    }
}

// 0x5230: voices whose sound has ended (ENDX's bit and a silent envelope; held ones after their grace ticks) freed
bool Reclaim()
{
    bool freed = false;
    for (u8 core = 0; core < 2; core++)
    {
        u32 ended = SdGetSwitch(static_cast<u16>(SD_S_ENDX | core));
        for (Voice* v : g_cores[core].active.BackToFront())
        {
            u16 envx = SdGetParam(VoiceEntry(*v, SD_VP_ENVX));
            v->envx = envx;
            bool end = (ended & Bit(*v)) != 0;
            if ((v->flags & (Held | Keyed)) != 0)
            {
                if (v->grace != 0)
                {
                    v->grace = end ? v->grace - 1 : 0;
                    v->envx = 0x7FFF;
                    continue;
                }
                if (!end || envx != 0)
                {
                    continue;
                }
            }
            else if (envx != 0)
            {
                continue;
            }
            FreeFromChannel(*v);
            freed = true;
        }
    }
    return freed;
}
}

void VoicesInit()
{
    for (u8 core = 0; core < 2; core++)
    {
        Core& c = g_cores[core];
        c = Core{};
        for (u8 i = 0; i < VoicesPerCore; i++)
        {
            Voice& v = c.voices[i];
            v = Voice{};
            v.core = core;
            v.index = i;
            g_channelList[core][i] = nullptr;
            c.free.PushBack(&v);
            g_globalList[core][i] = &c.free;
        }
        c.freeCount = VoicesPerCore;
    }
    g_endsChecked = true;
    g_hdVoices = 0;
}

void NoteOn(Port& p, Channel& c, u8 key, u8 velocity, u32 id)
{
    u32 program = c.program;
    if (program == 0)
    {
        return;
    }
    Bank& bank = *c.bank;
    bool started = false;
    u32 split = program + Hd32(program);
    for (u32 n = Hd8(program + 4); n > 0; n--, split += 0x14)
    {
        if (key < (Hd8(split + 2) & 0x7F) || (Hd8(split + 4) & 0x7F) < key)
        {
            continue;
        }
        u32 set = HdEntry(bank.sampleSets, Hd16(split));
        if (set == 0 || velocity < Hd8(set + 1) || Hd8(set + 2) < velocity)
        {
            continue;
        }
        for (u32 k = 0; k < Hd8(set + 3); k++)
        {
            u32 sample = HdEntry(bank.samples, Hd16(set + 4 + k * 2));
            if (sample == 0 || velocity < (Hd8(sample + 2) & 0x7F) || (Hd8(sample + 4) & 0x7F) < velocity)
            {
                continue;
            }
            // Its VAG; an index past the bank's plays noise
            if (bank.vagInfo == 0)
            {
                Fatal("hsyn: a note on a bank with no VAG info (port %d)", p.index);
            }
            u32 vag = 0;
            u32 vagIndex = Hd16(sample);
            if (vagIndex <= Hd32(bank.vagInfo + 0xC))
            {
                vag = HdEntry(bank.vagInfo, vagIndex);
                if (vag == 0 || Hd32(vag) == 0xFFFFFFFF)
                {
                    continue;
                }
            }
            if (u8 group = Hd8(sample + 0xE); group != 0)
            {
                ExclusiveGroup(c, group);
            }
            u16 priority = static_cast<u16>(Hd8(sample + 0xF) + p.priorityBase);
            if (p.voices >= p.maxVoices && !StealInPort(p, priority))
            {
                return;
            }
            u8 attributes = Hd8(sample + 0x29);
            Voice* v = TakeFree(attributes);
            if (v == nullptr && g_endsChecked)
            {
                g_endsChecked = false;
                if (Reclaim())
                {
                    v = TakeFree(attributes);
                }
            }
            if (v == nullptr)
            {
                v = StealActive(priority, attributes);
            }
            if (v == nullptr)
            {
                v = StealPending(priority, attributes);
            }
            if (v == nullptr)
            {
                continue;
            }
            v->flags = Keyed | Pending;
            v->port = &p;
            v->channel = &c;
            v->program = program;
            v->sample = sample;
            v->vag = vag;
            v->split = split;
            v->envx = 0;
            v->glide = 0;
            v->priority = priority;
            v->key = key;
            v->id = id;
            v->spuBase = bank.spu;
            v->velocity = velocity;
            v->group = Hd8(sample + 0xE);
            v->velocityCurve = Hd8(set);
            v->noteBend = c.noteBend;
            v->volumeOverride = c.noteVolume;
            v->kind = 0;
            v->panOverride = c.notePan;
            v->portType = p.type;
            v->pitchLfo = {};
            v->ampLfo = {};
            p.voices++;
            g_hdVoices++;
            LinkChannel(*v, c.lists[0]);
            started = true;
            if ((c.portamentoNote & 0x80) == 0 && c.portamentoTime != 0)
            {
                StartGlide(*v);
            }
            Setup(*v);
            LinkGlobal(*v, g_cores[v->core].pending);
        }
    }
    if (started)
    {
        // The next note's own bend, expression and pan were for this one
        c.noteBend = 0xFFFF;
        c.noteVolume = 0xFF;
        c.notePan = 0xFF;
    }
}

void NoteOff(Channel& c, u8 key, u32 id)
{
    if (c.portamentoNote <= 0x80)
    {
        c.portamentoNote = key;
    }
    for (Voice* v : c.lists[0].BackToFront())
    {
        if (v->key != key || (v->id != id && id != 0xFF))
        {
            continue;
        }
        UnlinkChannel(*v);
        v->flags &= ~(Held | Keyed);
        if (c.damper != 0)
        {
            v->flags |= Held;
            LinkChannel(*v, c.lists[1]);
        }
        else if ((v->flags & Pending) != 0)
        {
            Free(*v);
        }
        else
        {
            LfoRelease(v->pitchLfo);
            LfoRelease(v->ampLfo);
            g_cores[v->core].keyOff |= Bit(*v);
            LinkChannel(*v, c.lists[2]);
        }
    }
}

void Damper(Channel& c, u8 value)
{
    if (value != 0)
    {
        c.damper = value;
        return;
    }
    if (c.damper == 0)
    {
        return;
    }
    c.damper = 0;
    while (!c.lists[1].Empty())
    {
        Voice& v = *c.lists[1].FrontToBack().front();
        UnlinkChannel(v);
        if ((v.flags & Pending) != 0)
        {
            Free(v);
            continue;
        }
        LfoRelease(v.pitchLfo);
        LfoRelease(v.ampLfo);
        g_cores[v.core].keyOff |= Bit(v);
        LinkChannel(v, c.lists[2]);
        v.flags &= ~(Held | Keyed);
    }
}

void AllNotesOff(Channel& c)
{
    for (size_t l = 0; l < 2; l++)
    {
        while (!c.lists[l].Empty())
        {
            Voice& v = *c.lists[l].Back();
            UnlinkChannel(v);
            if ((v.flags & Pending) != 0)
            {
                Free(v);
                continue;
            }
            v.flags &= ~(Held | Keyed);
            g_cores[v.core].keyOff |= Bit(v);
            LinkChannel(v, c.lists[2]);
        }
    }
}

void AllSoundOff(Channel& c)
{
    for (VoiceList& list : c.lists)
    {
        while (!list.Empty())
        {
            Cut(*list.Back(), true);
        }
    }
}

// 0x47AC: the bend (14 bits, centred) through the split's bend range (up or down) to 1/128 semitones
void SetBend(Voice& v, s16 bend)
{
    if (v.split == 0)
    {
        return;
    }
    u16 range = Hd16(v.split + (bend >= 0 ? 8 : 6));
    v.bend = Shift(range * bend, 13);
}

// 0x49BC
void UpdateLevel(Voice& v)
{
    v.level = static_cast<u16>((static_cast<u32>(v.channel->volume) * v.expression * v.gain) >> 14);
}

void SetModulation(Voice& v, u8 modulation)
{
    v.pitchDepth[0] = static_cast<s16>(Clamp16(Shift(modulation * HdS16(v.program + 0x1C), 7) + v.pitchBase[0]));
    v.pitchDepth[1] = static_cast<s16>(Clamp16(Shift(modulation * HdS16(v.program + 0x1E), 7) + v.pitchBase[1]));
}

void SetBreath(Voice& v, u8 breath)
{
    v.ampDepth[0] = static_cast<s8>(Clamp8(Shift(breath * HdS8(v.program + 0x22), 7) + v.ampBase[0]));
    v.ampDepth[1] = static_cast<s8>(Clamp8(Shift(breath * HdS8(v.program + 0x23), 7) + v.ampBase[1]));
}

// 0x4C94: the pan's gains (0x80 each centred; one side down to 0 over a quarter turn); pan by phase can invert the voice
void SetPan(Voice& v, s32 pan)
{
    v.panLeft = v.panRight = 0x80;
    v.pan = 0;
    if (g_mono)
    {
        return;
    }
    pan = std::clamp(pan, -0xBF, 0x3F);
    s32 p;
    if (v.panPhase != 0)
    {
        s32 base = v.panBase;
        u8 inPhase = 1;
        if (base < 0)
        {
            base = -base;
            inPhase = 0;
        }
        base += pan;
        if (base < 0)
        {
            inPhase ^= 1;
            base = -base;
        }
        else if (base >= 0x80)
        {
            inPhase ^= 1;
            base = 0x100 - base;
        }
        v.inPhase = inPhase;
        p = base - 0x40;
    }
    else
    {
        p = pan + v.panBase - 0x40;
    }
    if (p > 0)
    {
        v.panLeft = p >= 0x3F ? 0 : static_cast<u8>(((0x3F - p) << 7) / 63);
    }
    else if (p < 0)
    {
        v.panRight = p < -0x3E ? 0 : static_cast<u8>(((p + 0x3F) << 7) / 63);
    }
    v.pan = static_cast<s8>(p);
}

void VoicesTick()
{
    // 0x394C: key offs, then each core's waiting voices keyed on once their SPU2 voice is quiet (ENVX under 1000)
    for (u8 core = 0; core < 2; core++)
    {
        if (g_cores[core].keyOff != 0)
        {
            SdSetSwitch(static_cast<u16>(SD_S_KOFF | core), g_cores[core].keyOff);
            g_cores[core].keyOff = 0;
        }
    }
    for (u8 core = 0; core < 2; core++)
    {
        Core& c = g_cores[core];
        for (Voice* v : c.pending.BackToFront())
        {
            if (SdGetParam(VoiceEntry(*v, SD_VP_ENVX)) >= 1000)
            {
                continue;
            }
            KeyOn(*v);
            UnlinkGlobal(*v);
            LinkGlobal(*v, c.active);
        }
        if (c.keyOn != 0)
        {
            SdSetSwitch(static_cast<u16>(SD_S_KON | core), c.keyOn);
            c.keyOn = 0;
        }
    }
    // 0x4F2C: each core's keyed-on voices: freed when their sound has ended, else their portamento, LFOs, pitch and volume
    for (u8 core = 0; core < 2; core++)
    {
        u32 ended = SdGetSwitch(static_cast<u16>(SD_S_ENDX | core));
        for (Voice* vp : g_cores[core].active.BackToFront())
        {
            Voice& v = *vp;
            if (g_endsChecked)
            {
                u16 envx = SdGetParam(VoiceEntry(v, SD_VP_ENVX));
                bool end = (ended & Bit(v)) != 0;
                bool free;
                if ((v.flags & (Held | Keyed)) != 0)
                {
                    if (v.grace != 0)
                    {
                        v.grace = end ? v.grace - 1 : 0;
                        free = false;
                    }
                    else
                    {
                        free = end && envx == 0;
                    }
                }
                else
                {
                    free = envx == 0;
                }
                if (free)
                {
                    FreeFromChannel(v);
                    continue;
                }
            }
            if (v.vag != 0)
            {
                if ((v.flags & GlideUp) != 0)
                {
                    v.glide += v.glideStep;
                    if (v.glide >= 0)
                    {
                        v.glide = 0;
                        v.flags &= ~GlideUp;
                    }
                }
                else if ((v.flags & GlideDown) != 0)
                {
                    v.glide -= v.glideStep;
                    if (v.glide <= 0)
                    {
                        v.glide = 0;
                        v.flags &= ~GlideDown;
                    }
                }
                LfoTick(v.pitchLfo);
                UpdatePitch(v);
            }
            LfoTick(v.ampLfo);
            UpdateVolume(v);
        }
    }
    g_endsChecked = true;
}
}
