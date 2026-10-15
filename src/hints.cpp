// The game's control hints follow what the player plays with: the PS2's buttons in its texts ("Press ¹ to accelerate", "¾
// NAVIGATE", "Press  START  button") are said as the keys bound to them when the keyboard and mouse were used last, or as the
// gamepad's buttons in its style (Xbox, Nintendo Switch); with PlayStation prompts they're the game's own button pictures. It
// changes as soon as the player switches device (ps2_prompt_style, the PC menus' Button prompts setting).
//
// The buttons are characters of the game's fonts drawn as the PS2's buttons (seen on screen, and by the controller maps: the
// accelerator's line has ¹ where the car's GasDigital is cross, the brake's ¼ where BrakeDigital is square): ¹ cross, º
// circle, » and ³ triangle, ¼ square, ½ and ² the left stick, ¾ the d-pad, ¶ Select, · L1, ¸ R1; START is written out.
// Every text is drawn by Text::Draw (0x30C488; a0 the text, +140 its UTF-16 string): one with a button in it is drawn with a
// string of its own, put on the game's stack below the frame it's drawn from, the text's layout (0x30C718: its alignment's
// offsets, +100 and +104) worked out for it, and the text's own string and layout put back afterwards. A name longer than the
// button's picture (about two letters wide) would push the text past its box (+88 wide), where it wraps out of sight ("¶
// DISABLE" is a short box): the text is drawn smaller (its size, +156) by as much as it grew, down to two thirds.
#include "ps2/iop.h"
#include "ps2/recomp.h"
#include "ps2/runtime.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

extern "C" void f_0030c488(Ps2Cpu* c);
extern "C" void f_0030c718(Ps2Cpu* c);

namespace
{
constexpr int PlayStation = 1;
constexpr u32 Room = 1024; // bytes of the game's stack taken for the string (512 characters)

const char* ControlOf(char16_t ch)
{
    switch (ch)
    {
    case 0xB9: return "cross";
    case 0xBA: return "circle";
    case 0xBB:
    case 0xB3: return "triangle";
    case 0xBC: return "square";
    case 0xBD:
    case 0xB2: return "left";
    case 0xBE: return "dpad";
    case 0xB6: return "select";
    case 0xB7: return "l1";
    case 0xB8: return "r1";
    default: return nullptr;
    }
}

std::u16string ReadString(u32 address)
{
    std::u16string s;
    for (u32 n = 0; n < Room / 2 - 1; n++)
    {
        char16_t ch;
        std::memcpy(&ch, Ps2::EeMemory(address + 2 * n), 2);
        if (ch == 0)
        {
            break;
        }
        s.push_back(ch);
    }
    return s;
}

std::u16string Name(const char* control, int style)
{
    char name[64];
    int n = ps2_control_name(control, style, name, sizeof(name));
    std::u16string out;
    for (int i = 0; i < n; i++)
    {
        out.push_back(static_cast<char16_t>(std::toupper(static_cast<unsigned char>(name[i]))));
    }
    return out.empty() ? u"-" : out;
}

// The string said for the device, or empty when it has no button in it
std::u16string Said(const std::u16string& text, int style)
{
    std::u16string out;
    bool changed = false;
    for (size_t i = 0; i < text.size(); i++)
    {
        if (const char* control = ControlOf(text[i]))
        {
            out += Name(control, style);
            changed = true;
        }
        else if (text.compare(i, 5, u"START") == 0 && (i == 0 || text[i - 1] == u' ') &&
                 (i + 5 == text.size() || text[i + 5] == u' '))
        {
            out += Name("start", style);
            changed = true;
            i += 4;
        }
        else
        {
            out.push_back(text[i]);
        }
    }
    return changed ? out : std::u16string();
}

void Layout(Ps2Cpu* c, u32 text)
{
    c->r[4].ud[0] = text;
    f_0030c718(c);
}
}

void CttrDrawText(Ps2Cpu* c)
{
    u32 text = c->r[4].uw[0];
    int style = ps2_prompt_style();
    u32 own;
    std::memcpy(&own, Ps2::EeMemory(text + 140), 4);
    std::u16string said = style == PlayStation || own == 0 ? std::u16string() : Said(ReadString(own), style);
    if (said.empty())
    {
        f_0030c488(c);
        return;
    }
    // The button's picture is about two letters wide
    std::u16string original = ReadString(own);
    size_t pictures = 0;
    for (char16_t ch : original)
    {
        pictures += ControlOf(ch) != nullptr ? 1 : 0;
    }
    float grown = static_cast<float>(said.size()) / static_cast<float>(original.size() + pictures);
    float size;
    std::memcpy(&size, Ps2::EeMemory(text + 156), 4);
    float smaller = grown > 1 ? size * std::max(2.0f / 3.0f, 1 / grown) : size;
    std::memcpy(Ps2::EeMemory(text + 156), &smaller, 4);
    u64 sp = c->r[29].ud[0];
    u32 at = static_cast<u32>(sp) - Room;
    std::memcpy(Ps2::EeMemory(at), said.c_str(), 2 * (said.size() + 1));
    c->r[29].ud[0] = at - 16;
    std::memcpy(Ps2::EeMemory(text + 140), &at, 4);
    Layout(c, text);
    c->r[4].ud[0] = text;
    f_0030c488(c);
    std::memcpy(Ps2::EeMemory(text + 140), &own, 4);
    std::memcpy(Ps2::EeMemory(text + 156), &size, 4);
    Layout(c, text);
    c->r[29].ud[0] = sp;
}
