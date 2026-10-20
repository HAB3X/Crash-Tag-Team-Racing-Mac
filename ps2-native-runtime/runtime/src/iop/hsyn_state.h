// modhsyn's state (ps2/hsyn.h), shared by hsyn.cpp (ports, channels, messages) and hsyn_voice.cpp (voices). Offsets are into
// SDK 2.8's MODHSYN.IRX ("PsIImodhsyn 2800", no symbols): functions by their address, fields by their offset in the module's
// structures. The sound banks stay in the IOP's memory, read through Hd* below as the module reads them
#pragma once

#include "iop.h"
#include "ps2/hsyn.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace Ps2::Hsyn
{
struct Voice;
struct Port;

// The module's circular lists of voices kept as arrays in the same order: Front is the sentinel's next (where 0x3894 and
// 0x38EC insert), Back its previous (where 0x38B0 inserts and the module's loops over +4 start)
class VoiceList
{
public:
    void PushFront(Voice* v) { m_items.insert(m_items.begin(), v); }
    void PushBack(Voice* v) { m_items.push_back(v); }
    void Remove(Voice* v) { m_items.erase(std::find(m_items.begin(), m_items.end(), v)); }
    bool Empty() const { return m_items.empty(); }
    Voice* Back() const { return m_items.back(); }
    // Copies for the module's loops, which go on from where they were while the voice they're at moves away
    std::vector<Voice*> FrontToBack() const { return m_items; }
    std::vector<Voice*> BackToFront() const { return {m_items.rbegin(), m_items.rend()}; }

private:
    std::vector<Voice*> m_items;
};

// A sound bank (0x1C bytes at a port's +0x384 + n * 0x1C): its HD's chunks' addresses in the IOP's memory (0: absent) and its
// samples' address in the sound memory
struct Bank
{
    u32 head = 0;
    u32 programs = 0;
    u32 sampleSets = 0;
    u32 samples = 0;
    u32 vagInfo = 0;
    u32 seTimbres = 0;
    u32 spu = 0;
};

// A MIDI channel (0x38 bytes from a port's +0x18, initialised by 0x770)
struct Channel
{
    Port* port = nullptr;
    std::array<VoiceList, 3> lists; // keyed (+0 from -8), held by the damper (+8), releasing (+0x10)
    u32 program = 0;                // the program's parameters in the HD (+0x18)
    Bank* bank = nullptr;           // +0x20
    u16 portamentoTime = 0;         // +0x24: CC 5 times 20
    s16 bend = 0;                   // +0x26
    u16 noteBend = 0xFFFF;          // +0x28: the next note's own bend (0xF9 message 2), bit 15 none
    u8 bankNumber = 0;              // +0x2A: CC 0
    u8 volume = 0x80;               // +0x2B: CC 7 * 128 / 126
    u8 expression = 0x80;           // +0x2C: CC 11 * 128 / 126
    u8 breath = 0;                  // +0x2D: CC 2 * 128 / 126 (the volume LFO's depth)
    u8 modulation = 0;              // +0x2E: CC 1 * 128 / 126 (the pitch LFO's depth)
    u8 noteVolume = 0xFF;           // +0x2F: the next note's own expression (0xF9 message 0), 0xFF none
    u8 notePan = 0xFF;              // +0x30: the next note's own pan (0xF9 message 1), 0xFF none
    s8 pan = 0;                     // +0x31: CC 10 - 64
    u8 damper = 0;                  // +0x32: CC 64
    u8 portamentoNote = 0xFF;       // +0x33: the note portamento starts from (0xFF off, 0x80 none yet)
    u8 portamentoControl = 0;       // +0x34: CC 84
    u8 nrpnMsb = 0xFF;              // +0x35
    u8 nrpnLsb = 0xFF;              // +0x36
    u8 dataEntry = 0xFF;            // +0x37
};

// A port (its "environment", 0x558 bytes, as the driver lays it out; the port's own part from +0x14)
struct Port
{
    int index = 0;
    u8 priorityBase = 0;  // +0: added to a sample's priority
    u8 maxVoices = 0x30;  // +1
    u8 type = 0;          // +3: 0 MIDI, 1 SE (not done)
    u16 volume = 0x100;   // +0x14
    u8 voices = 0;        // +0x16: voices playing
    std::array<Channel, 16> channels;
    std::array<Bank, 16> banks;
    std::vector<u8> input; // modmsin's buffer for the port: up to 0x800 bytes of messages
};

// An LFO (0x28 bytes at a voice's +0x78 for the pitch and +0xA0 for the volume)
struct Lfo
{
    u8 state = 0;      // +0: 0 off, 1 delay, 2 fading in, 3 running, 4 delay before fading out, 5 fading out, 6 ended
    u8 releases = 0;   // +1: reacts to the key's release
    u16 delay = 0;     // +2: ticks
    u16 counter = 0;   // +4
    s32 fade = 0;      // +8: 0-0x10000
    s32 fadeStep = 0;  // +0xC
    s16 out = 0;       // +0x10
    u8 wave = 0;       // +0x14: the wave's function (1-6)
    s32 phase = 0;     // +0x18: 0-0xFFFF
    s32 phaseStep = 0; // +0x1C
};

// Voice flags (+0x10)
constexpr u16 OneShot = 0x1;      // its sample doesn't loop
constexpr u16 GlideUp = 0x2;      // portamento from below
constexpr u16 GlideDown = 0x4;
constexpr u16 Pending = 0x8;      // waiting for the SPU2 voice to be quiet before its key on
constexpr u16 Held = 0x100;       // released but held by the damper
constexpr u16 Keyed = 0x200;

// A voice (0x150 bytes, 24 a core from 0xBE70)
struct Voice
{
    u8 core = 0;            // +0x40
    u8 index = 0;           // +0x41
    u16 flags = 0;          // +0x10
    u16 envx = 0;           // +0x12: ENVX as last read
    u16 priority = 0;       // +0x14
    u16 gain = 0;           // +0x16: the program's, split's and sample's volumes and the velocity's
    u16 level = 0;          // +0x18: gain with the channel's volume and the expression
    u16 lastLeft = 0xFFFF;  // +0x1A: the volumes last written
    u16 lastRight = 0xFFFF; // +0x1C
    s16 panBase = 0;        // +0x1E
    s16 pitchDepth[2] = {}; // +0x20: the pitch LFO's depths (positive, negative half), with the breath
    s16 pitchBase[2] = {};  // +0x24: without it
    u16 noteBend = 0xFFFF;  // +0x28
    u16 glideStep = 0;      // +0x2A
    s32 lastPitch = 0;      // +0x30
    s32 bend = 0;           // +0x34: 1/128 semitones
    s32 glide = 0;          // +0x38: portamento's offset, 1/2048 semitones
    u32 rateScale = 0;      // +0x3C: the sample's rate / 48 kHz, 16.16
    u8 key = 0;             // +0x43
    u32 id = 0xFF;          // +0x44
    u8 velocity = 0;        // +0x48
    u8 group = 0;           // +0x49
    u8 velocityCurve = 0;   // +0x4A
    u8 grace = 0;           // +0x4B: ticks after its key on before its end counts
    u8 panLeft = 0x80;      // +0x4C
    u8 panRight = 0x80;     // +0x4D
    u8 panPhase = 0;        // +0x4E: the program's attribute bit 0 (pan by phase inversion)
    s8 ampDepth[2] = {};    // +0x4F: the volume LFO's depths, with the modulation
    s8 ampBase[2] = {};     // +0x51
    u8 inPhase = 1;         // +0x53
    u8 expression = 0;      // +0x54
    u8 volumeOverride = 0xFF; // +0x55
    u8 panOverride = 0x80;  // +0x56
    u8 kind = 0xFF;         // +0x57: 0 an HD voice, 0xFF free
    u8 kind2 = 0xFF;        // +0x58
    u8 portType = 0;        // +0x59
    s8 pan = 0;             // +0x5A
    Port* port = nullptr;   // +0x5C
    Channel* channel = nullptr; // +0x60
    u32 program = 0;        // +0x64
    u32 split = 0;          // +0x68
    u32 sample = 0;         // +0x6C
    u32 vag = 0;            // +0x70: its VAG info; 0 plays noise
    u32 spuBase = 0;        // +0x74
    Lfo pitchLfo;           // +0x78
    Lfo ampLfo;             // +0xA0
};

// The HD in the IOP's memory
inline u8 Hd8(u32 a) { return *IopMemory(a); }
inline s8 HdS8(u32 a) { return static_cast<s8>(*IopMemory(a)); }
inline u16 Hd16(u32 a)
{
    u16 v;
    std::memcpy(&v, IopMemory(a), 2);
    return v;
}
inline s16 HdS16(u32 a) { return static_cast<s16>(Hd16(a)); }
inline u32 Hd32(u32 a)
{
    u32 v;
    std::memcpy(&v, IopMemory(a), 4);
    return v;
}
// A chunk's entry by index (a chunk's +0xC is its last index, +0x10 its offsets, 0xFFFFFFFF none): 0 when there's none
inline u32 HdEntry(u32 chunk, u32 index)
{
    if (chunk == 0 || index > Hd32(chunk + 0xC))
    {
        return 0;
    }
    u32 offset = Hd32(chunk + 0x10 + index * 4);
    return offset == 0xFFFFFFFF ? 0 : chunk + offset;
}

// --- hsyn_voice.cpp
void VoicesInit();
void NoteOn(Port& port, Channel& c, u8 key, u8 velocity, u32 id);
void NoteOff(Channel& c, u8 key, u32 id);
void Damper(Channel& c, u8 value);
void AllNotesOff(Channel& c);
void AllSoundOff(Channel& c);
// Every voice of the channel's lists (the module's three nested loops over them)
template <typename F> void ForEachVoice(Channel& c, F f)
{
    for (VoiceList& list : c.lists)
    {
        for (Voice* v : list.FrontToBack())
        {
            f(*v);
        }
    }
}
void SetBend(Voice& v, s16 bend);
void UpdateLevel(Voice& v);
// The pitch LFO's depth from the modulation (0x4B00), the volume LFO's from the breath (0x4A58)
void SetModulation(Voice& v, u8 modulation);
void SetBreath(Voice& v, u8 breath);
void SetPan(Voice& v, s32 pan);
// The tick's voice work: key offs and the pending key ons, then the voices' updates
void VoicesTick();
// 0xFDA0: every voice centred (sceHSyn_SetOutputMode 0); 0xBDF0: the tick's period in 1/256 ms
extern bool g_mono;
extern u32 g_tickUnit;
}
