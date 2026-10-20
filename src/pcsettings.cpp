// The PC menus in the game's own pause menu, as Wrath of Cortex's options' ADJUST SCREEN opens them there: the pause menu's
// PREFERENCES (PausePreferencesScreen, src/pause.cpp) has a VIBRATION line, the PS2 controller's rumble, which the runtime
// hasn't got; while those preferences are up it's said PC SETTINGS, its ON and OFF left out, and choosing it opens the PC menus
// over the paused game (ps2_open_settings); the preferences close as they do when a line is chosen, so closing the PC menus
// goes back to the game's pause menu.
//
// The screen's lines aren't a menu's but texts of its own in a column (seen drawn: their boxes' left 0.266 of the picture,
// right 0.578), seven of them a sixteenth of the picture apart (DRIVING HUD MAP, CLASH F/X, AUTO-REJECT CLASHING, INVERT
// CAMERA, INVERTED AIMING, VIBRATION, REARVIEW MIRROR): VIBRATION is the sixth from the top, in any language. Its ON and OFF
// are texts on its row, to its right. Choosing it is the screen's script calling SetRumble (0x23F6E0, the preferences' rumble
// at +0x38, registered for the scripts by that name), overridden: while the preferences are up it opens the PC menus and leaves
// the rumble as it was.
#include "ps2/iop.h"
#include "ps2/recomp.h"
#include "ps2/runtime.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

extern "C" void f_0023f6e0(Ps2Cpu* c);

bool CttrPreferencesUp();
void CttrTextBox(u32 text, float* left, float* top, float* right, float* bottom);
void CttrDrawTextAs(Ps2Cpu* c, const std::u16string& string, float scale);

namespace
{
constexpr size_t Lines = 7;
constexpr size_t VibrationLine = 5;

// The column of the lines' names: boxes ending left of the picture's middle and right of its first third, between the title
// and the hints at the bottom
bool InColumn(float left, float top, float right, float bottom)
{
    return left < 0.4f && right > 0.5f && right < 0.6f && top > 0.2f && bottom < 0.75f;
}

// The names drawn this frame (text, top), and the line found in the last frame's: its text and row
std::vector<std::pair<u32, float>> g_drawing;
u32 g_line = 0;
float g_lineTop = 0, g_lineBottom = 0, g_lineRight = 0;
}

// At every vertical blank: the sixth of the seven names drawn since the last is the line
void CttrPcSettingsVblank()
{
    if (g_drawing.size() == Lines)
    {
        std::sort(g_drawing.begin(), g_drawing.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        g_line = g_drawing[VibrationLine].first;
    }
    else if (!g_drawing.empty())
    {
        g_line = 0;
    }
    g_drawing.clear();
}

// A text about to be drawn: the line drawn as PC SETTINGS, its ON and OFF left out (true), or drawn as the game draws it (false)
bool CttrPcSettingsDraw(Ps2Cpu* c)
{
    if (!CttrPreferencesUp())
    {
        g_line = 0;
        g_drawing.clear();
        return false;
    }
    u32 text = c->r[4].uw[0];
    float left, top, right, bottom;
    CttrTextBox(text, &left, &top, &right, &bottom);
    if (InColumn(left, top, right, bottom))
    {
        g_drawing.emplace_back(text, top);
    }
    if (text == g_line)
    {
        g_lineTop = top;
        g_lineBottom = bottom;
        g_lineRight = right;
        CttrDrawTextAs(c, u"PC SETTINGS", 1.0f);
        return true;
    }
    // A text on the line's row, right of it: its ON or OFF
    float middle = (top + bottom) / 2;
    return g_line != 0 && left >= g_lineRight && middle >= g_lineTop && middle <= g_lineBottom;
}

// SetRumble (0x23F6E0: a0 the preferences, a1 on or off)
extern "C" void CttrSetRumble(Ps2Cpu* c)
{
    if (CttrPreferencesUp())
    {
        Ps2::Log("PC SETTINGS chosen in the pause menu's preferences");
        ps2_open_settings();
        return;
    }
    f_0023f6e0(c);
}
