// modhsyn's ports and channels, and modmsin (ps2/hsyn.h): sceHSyn_Init (0x50), sceHSyn_Load (0x34C), sceHSyn_ATick (0x183C),
// the MIDI parser (0x15DC) and its channel messages (0x12F8), the controllers (0x1140), modhsyn's 0xF9/0xFD messages
// (0x1428), the exports used by drivers (0x1BD4-0x2054); MODMSIN.IRX's sceMSIn_PutMsg (0x184) and sceMSIn_PutHsMsg (0x2B4)
#include "hsyn_state.h"
#include "ps2/sound.h"

namespace Ps2::Hsyn
{
namespace
{
constexpr u32 InputBytes = 0x800;
std::vector<Port> g_ports;

Port* PortAt(int port)
{
    return port >= 0 && port < static_cast<int>(g_ports.size()) ? &g_ports[static_cast<size_t>(port)] : nullptr;
}

// 0x2E0: a chunk's 8 byte tag ("SCEI" then the chunk's kind, each word's bytes reversed in the file)
bool Tagged(u32 chunk, const char* kind)
{
    static const char creator[] = "SCEI";
    for (u32 i = 0; i < 4; i++)
    {
        if (Hd8(chunk + i) != static_cast<u8>(creator[3 - i]) || Hd8(chunk + 4 + i) != static_cast<u8>(kind[3 - i]))
        {
            return false;
        }
    }
    return true;
}

// The Head chunk (after the Vers chunk) and the chunks it points to (0xFFFFFFFF: none; SE timbres from the HD's version 2
// on), each checked for its tag
bool LoadBank(Bank& b, u32 spuAddress, u32 hd)
{
    u32 head = hd + 0x10;
    auto chunk = [&](u32 field) -> u32 {
        u32 offset = Hd32(head + field);
        return offset == 0xFFFFFFFF ? 0 : hd + offset;
    };
    Bank loaded = {head, chunk(0x14), chunk(0x18), chunk(0x1C), chunk(0x20), Hd8(hd + 0xE) >= 2 ? chunk(0x24) : 0, spuAddress};
    struct Check
    {
        u32 at;
        const char* kind;
    };
    const Check checks[] = {{hd, "Vers"},          {head, "Head"},          {loaded.programs, "Prog"}, {loaded.sampleSets, "Sset"},
                            {loaded.samples, "Smpl"}, {loaded.vagInfo, "Vagi"}, {loaded.seTimbres, "Setb"}};
    for (const Check& c : checks)
    {
        if (c.at != 0 && !Tagged(c.at, c.kind))
        {
            return false;
        }
    }
    b = loaded;
    return true;
}

// The module's x * 128 / 126 for a controller's value (its multiply by 0x81020409)
u8 Scale7(u8 value) { return static_cast<u8>((value << 7) / 126); }

// 0x07EC: the program from the channel's bank (a program with no splits is none)
void ProgramChange(Port& p, Channel& c, u8 program)
{
    u32 found = 0;
    Bank* bank = nullptr;
    if (c.bankNumber < 16)
    {
        bank = &p.banks[c.bankNumber];
        found = HdEntry(bank->programs, program);
        if (found != 0 && Hd8(found + 4) == 0)
        {
            found = 0;
        }
    }
    c.program = found;
    c.bank = found != 0 ? bank : nullptr;
}

// 0x0888: the channel's bend (14 bits, centred)
void PitchBend(Channel& c, u8 msb, u8 lsb)
{
    s16 bend = static_cast<s16>((msb << 7) + lsb - 0x2000);
    if (bend == c.bend)
    {
        return;
    }
    c.bend = bend;
    ForEachVoice(c, [&](Voice& v) { SetBend(v, bend); });
}

// 0x094C: the voices' levels again, their expression first when it changed
void LevelChanged(Channel& c, bool expression)
{
    ForEachVoice(c, [&](Voice& v) {
        if (expression)
        {
            v.expression = c.expression;
        }
        UpdateLevel(v);
    });
}

// 0x0CA8: CC 10
void Pan(Channel& c, u8 value)
{
    s32 pan = std::max<s32>(static_cast<s8>(value - 0x40), -0x3F);
    if (pan == c.pan)
    {
        return;
    }
    c.pan = static_cast<s8>(pan);
    ForEachVoice(c, [&](Voice& v) { SetPan(v, pan); });
}

// 0x0C68: CC 65
void Portamento(Channel& c, u8 value)
{
    if (value == 0)
    {
        c.portamentoNote = 0xFF;
    }
    else if (c.portamentoNote == 0xFF)
    {
        c.portamentoNote = 0x80;
        c.portamentoControl = 0x80;
    }
}

void Modulation(Channel& c, u8 value)
{
    u8 scaled = Scale7(value);
    if (scaled != c.modulation)
    {
        c.modulation = scaled;
        ForEachVoice(c, [&](Voice& v) { SetModulation(v, scaled); });
    }
}

void Breath(Channel& c, u8 value)
{
    u8 scaled = Scale7(value);
    if (scaled != c.breath)
    {
        c.breath = scaled;
        ForEachVoice(c, [&](Voice& v) { SetBreath(v, scaled); });
    }
}

// 0x10C0: CC 121
void ResetControllers(Channel& c)
{
    Damper(c, 0);
    PitchBend(c, 0x40, 0);
    Modulation(c, 0);
    Breath(c, 0);
    if (u8 v = Scale7(0x7F); v != c.volume)
    {
        c.volume = v;
        LevelChanged(c, false);
    }
    Pan(c, 0x40);
    if (u8 v = Scale7(0x7F); v != c.expression)
    {
        c.expression = v;
        LevelChanged(c, true);
    }
    Portamento(c, 0);
}

// 0x1E70 and 0x1EE0: NRPN 2 (a reverb preset, 0-9) and 3 (depth, delay, feedback) for core 0 or 1 (LSB bit 4), through libsd
void ReverbPreset(u8 lsb, u8 value)
{
    if (value >= 10 || (lsb != 0 && lsb != 0x10))
    {
        return;
    }
    s32 core = (lsb & 0x10) != 0 ? 1 : 0;
    SdEffectAttr a = SdGetEffectAttr(core);
    SdSetEffectAttr(core, value | 0x100, a.depthLeft, a.depthRight, a.delay, a.feedback);
}

void ReverbParameter(u8 lsb, u8 value)
{
    if ((lsb & 0xE0) != 0)
    {
        return;
    }
    s32 core = (lsb & 0x10) != 0 ? 1 : 0;
    SdEffectAttr a = SdGetEffectAttr(core);
    switch (lsb & 0xF)
    {
    case 0:
        a.depthLeft = a.depthRight = static_cast<u16>(value << 8);
        break;
    case 1:
        a.depthLeft = a.depthRight = static_cast<u16>(-(value << 8));
        break;
    case 2:
        a.delay = value;
        break;
    case 3:
        a.feedback = value;
        break;
    default:
        break;
    }
    SdSetEffectAttr(core, a.mode, a.depthLeft, a.depthRight, a.delay, a.feedback);
}

// 0x1140: control changes (the module's jump table at 0xB7E0 for 0-0x7B)
void ControlChange(Channel& c, u8 controller, u8 value)
{
    switch (controller)
    {
    case 0:
        c.bankNumber = value;
        break;
    case 1:
        Modulation(c, value);
        break;
    case 2:
        Breath(c, value);
        break;
    case 5:
        c.portamentoTime = static_cast<u16>(value * 20);
        break;
    case 6:
        // Data entry for the NRPN set, then the NRPN forgotten
        c.dataEntry = value;
        if (c.nrpnLsb != 0xFF && c.nrpnMsb == 2)
        {
            ReverbPreset(c.nrpnLsb, value);
        }
        else if (c.nrpnLsb != 0xFF && c.nrpnMsb == 3)
        {
            ReverbParameter(c.nrpnLsb, value);
        }
        c.dataEntry = c.nrpnLsb = c.nrpnMsb = 0xFF;
        break;
    case 7:
        if (u8 v = Scale7(value); v != c.volume)
        {
            c.volume = v;
            LevelChanged(c, false);
        }
        break;
    case 10:
        Pan(c, value);
        break;
    case 11:
        if (u8 v = Scale7(value); v != c.expression)
        {
            c.expression = v;
            LevelChanged(c, true);
        }
        break;
    case 64:
        Damper(c, value);
        break;
    case 65:
        Portamento(c, value);
        break;
    case 84:
        c.portamentoControl = value;
        break;
    case 98:
        c.nrpnLsb = value;
        break;
    case 99:
        c.nrpnMsb = value;
        c.dataEntry = c.nrpnLsb = 0xFF;
        break;
    case 120:
        AllSoundOff(c);
        break;
    case 121:
        ResetControllers(c);
        break;
    case 123:
        AllNotesOff(c);
        break;
    default:
        break;
    }
}

// 0x12F8: a channel message
void ChannelMessage(Port& p, u8 status, u8 d1, u8 d2)
{
    Channel& c = p.channels[status & 0xF];
    switch (status & 0xF0)
    {
    case 0x90:
        if (d2 != 0)
        {
            NoteOn(p, c, d1, d2, 0xFF);
            break;
        }
        NoteOff(c, d1, 0xFF);
        break;
    case 0x80:
        NoteOff(c, d1, 0xFF);
        break;
    case 0xB0:
        ControlChange(c, d1, d2);
        break;
    case 0xC0:
        ProgramChange(p, c, d1);
        break;
    case 0xE0:
        PitchBend(c, d2, d1);
        break;
    default:
        break;
    }
}

// 0x0E90, 0x0D74, 0x0FAC: a message for the channel's voices of an ID (0x7F any) and key (bit 7 any)
template <typename F> void ForVoicesOf(Channel& c, u8 key, u32 id, F f)
{
    ForEachVoice(c, [&](Voice& v) {
        if ((id == 0x7F || v.id == id) && ((key & 0x80) != 0 || v.key == key))
        {
            f(v);
        }
    });
}

// 0x1428: modhsyn's own messages. 0xF9 sub, channel, value, value 2: the next note's expression (0), pan (1), bend (2).
// 0xFD sub, channel (bit 4: any key), key, ID, value, value 2: the voices' expression (0), pan (1), bend (2); a note of the ID
// on (0x10, value its velocity) or off (velocity 0)
void HsMessage(Port& p, u8 status, const u8* m)
{
    Channel& c = p.channels[m[1] & 0xF];
    if (status == 0xF9)
    {
        switch (m[0])
        {
        case 0:
            c.noteVolume = m[2];
            break;
        case 1:
            c.notePan = m[2];
            break;
        case 2:
            c.noteBend = static_cast<u16>(m[2] + (m[3] << 7));
            break;
        default:
            break;
        }
        return;
    }
    u8 key = static_cast<u8>(m[2] | ((m[1] & 0x10) != 0 ? 0x80 : 0));
    u32 id = m[3];
    u8 value = m[4];
    switch (m[0])
    {
    case 0:
        ForVoicesOf(c, key, id, [&](Voice& v) {
            v.expression = Scale7(value);
            UpdateLevel(v);
        });
        break;
    case 1:
        ForVoicesOf(c, key, id, [&](Voice& v) { SetPan(v, std::max<s32>(static_cast<s8>(value - 0x40), -0x3F)); });
        break;
    case 2:
    {
        s16 bend = static_cast<s16>(value + (m[5] << 7) - 0x2000);
        ForVoicesOf(c, key, id, [&](Voice& v) { SetBend(v, bend); });
        break;
    }
    case 0x10:
        if (value != 0)
        {
            NoteOn(p, c, key & 0x7F, value, id);
        }
        else
        {
            NoteOff(c, key, id);
        }
        break;
    default:
        break;
    }
}

// 0x15DC: the port's messages, without running status (data bytes before a status byte are skipped); system exclusive
// skipped to its 0xF7
void Parse(Port& p)
{
    // The data bytes after each status's first (0xBB40: 8x one, 9x-Bx two, Cx-Dx one, Ex two)
    static const u8 extra[8] = {0, 1, 1, 1, 0, 0, 1, 0};
    const std::vector<u8> in = std::move(p.input);
    p.input.clear();
    size_t at = 0;
    while (at < in.size())
    {
        u8 status = in[at++];
        if ((status & 0x80) == 0)
        {
            continue;
        }
        if (status < 0xF0)
        {
            if (at >= in.size())
            {
                break;
            }
            u8 d1 = in[at++];
            u8 d2 = 0;
            if (extra[(status - 0x80) >> 4] != 0)
            {
                if (at >= in.size())
                {
                    break;
                }
                d2 = in[at++];
            }
            ChannelMessage(p, status, d1, d2);
            continue;
        }
        if (status == 0xF0)
        {
            while (at < in.size() && in[at++] != 0xF7)
            {
            }
            continue;
        }
        if (status == 0xF9 || status == 0xFD)
        {
            size_t n = status == 0xF9 ? 4 : 6;
            if (at + n > in.size())
            {
                break;
            }
            HsMessage(p, status, &in[at]);
            at += n;
        }
    }
}
}

bool g_mono = false;
u32 g_tickUnit = 0;

void Init(int ports, u32 tickMicroseconds)
{
    g_ports.assign(static_cast<size_t>(ports), Port{});
    for (int i = 0; i < ports; i++)
    {
        Port& p = g_ports[static_cast<size_t>(i)];
        p.index = i;
        for (Channel& c : p.channels)
        {
            c.port = &p;
        }
    }
    g_tickUnit = tickMicroseconds * 256 / 1000;
    g_mono = false;
    VoicesInit();
}

int Load(int port, u32 spuAddress, u32 hdAddress, int bank)
{
    Port* p = PortAt(port);
    if (p == nullptr || bank < 0 || bank >= 16)
    {
        return -1;
    }
    if (p->type == 1)
    {
        bank = 0;
    }
    Bank& b = p->banks[static_cast<size_t>(bank)];
    b = {};
    if (spuAddress != 0 && hdAddress != 0)
    {
        if (!LoadBank(b, spuAddress, hdAddress))
        {
            Log("hsyn: port %d bank %d: the HD at IOP %06x isn't SCEI's", port, bank, hdAddress);
            return -1;
        }
    }
    // The channels with a program of the bank lose it (their voices go on)
    for (Channel& c : p->channels)
    {
        if (c.bank == &b)
        {
            c.program = 0;
            c.bank = nullptr;
        }
    }
    return 0;
}

void ATick()
{
    for (Port& p : g_ports)
    {
        if (!p.input.empty())
        {
            if (p.type != 0)
            {
                Fatal("hsyn: port %d is of the SE type: its sequences aren't done", p.index);
            }
            Parse(p);
        }
    }
    VoicesTick();
}

int SetVolume(int port, u16 volume)
{
    Port* p = PortAt(port);
    if (p == nullptr)
    {
        return -1;
    }
    p->volume = volume;
    return 0;
}

int AllSoundOff(int port)
{
    Port* p = PortAt(port);
    if (p == nullptr)
    {
        return -1;
    }
    for (Channel& c : p->channels)
    {
        AllSoundOff(c);
    }
    return 0;
}

int GetChStat(int port, u8 status[16])
{
    Port* p = PortAt(port);
    if (p == nullptr)
    {
        return -1;
    }
    for (size_t i = 0; i < 16; i++)
    {
        const Channel& c = p->channels[i];
        status[i] = static_cast<u8>((c.lists[0].Empty() ? 0 : 1) | (c.lists[1].Empty() ? 0 : 2) | (c.lists[2].Empty() ? 0 : 4));
    }
    return 0;
}

int SetOutputMode(int mode)
{
    if (mode != 0 && mode != 1)
    {
        return -1;
    }
    g_mono = mode == 0;
    return 0;
}

int PutMsg(int port, u32 message)
{
    // The status's data bytes: two for notes off, programs and channel pressure, three for the rest (0x184)
    u8 status = static_cast<u8>(message);
    size_t n;
    switch (status & 0xF0)
    {
    case 0x80:
    case 0xC0:
    case 0xD0:
        n = 2;
        break;
    case 0x90:
    case 0xA0:
    case 0xB0:
    case 0xE0:
        n = 3;
        break;
    default:
        return -1;
    }
    Port* p = PortAt(port);
    if (p == nullptr || p->input.size() + n > InputBytes)
    {
        return -1;
    }
    for (size_t i = 0; i < n; i++)
    {
        p->input.push_back(static_cast<u8>(message >> (i * 8)));
    }
    return 0;
}

int PutHsMsg(int port, const u8* message)
{
    size_t n = message[0] == 0xF9 ? 5 : message[0] == 0xFD ? 7 : 0;
    Port* p = PortAt(port);
    if (n == 0 || p == nullptr || p->input.size() + n > InputBytes)
    {
        return -1;
    }
    p->input.insert(p->input.end(), message, message + n);
    return 0;
}
}
