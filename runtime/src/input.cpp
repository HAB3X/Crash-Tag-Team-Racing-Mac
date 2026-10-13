// Port 0's controller from the host: the keyboard and the first gamepad SDL finds (when there's a window), and the presses
// asked for with --press (always). Read on the presenter's thread, given to the pad server (iop/padman.cpp).
//
// Keyboard: arrows the d-pad, W A S D the left stick, I J K L the right stick, Return start, Backspace or right Shift select,
// X cross, C circle, Z square, V triangle, Q L1, E R1, 1 L2, 3 R2.
#include "runtime.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace Ps2
{
namespace
{
enum Button : u32
{
    Select = 1 << 0,
    L3 = 1 << 1,
    R3 = 1 << 2,
    Start = 1 << 3,
    Up = 1 << 4,
    Right = 1 << 5,
    Down = 1 << 6,
    Left = 1 << 7,
    L2 = 1 << 8,
    R2 = 1 << 9,
    L1 = 1 << 10,
    R1 = 1 << 11,
    Triangle = 1 << 12,
    Circle = 1 << 13,
    Cross = 1 << 14,
    Square = 1 << 15,
};

const struct
{
    const char* name;
    u32 bit;
} g_names[] = {{"select", Select}, {"l3", L3},       {"r3", R3},         {"start", Start},   {"up", Up},
               {"right", Right},   {"down", Down},   {"left", Left},     {"l2", L2},         {"r2", R2},
               {"l1", L1},         {"r1", R1},       {"triangle", Triangle}, {"circle", Circle}, {"cross", Cross},
               {"square", Square}};

SDL_Gamepad* g_gamepad = nullptr;

u8 Axis(Sint16 v) { return static_cast<u8>(std::clamp((static_cast<int>(v) + 32768) >> 8, 0, 255)); }
}

u32 ButtonsFromNames(const std::string& names)
{
    u32 bits = 0;
    size_t start = 0;
    while (start <= names.size())
    {
        size_t plus = names.find('+', start);
        std::string name = names.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        bool found = false;
        for (const auto& n : g_names)
        {
            if (name == n.name)
            {
                bits |= n.bit;
                found = true;
            }
        }
        if (!found)
        {
            Log("--press: no button \"%s\"", name.c_str());
        }
        if (plus == std::string::npos)
        {
            break;
        }
        start = plus + 1;
    }
    return bits;
}

void InputEvent(const SDL_Event& e)
{
    if (e.type == SDL_EVENT_GAMEPAD_ADDED && g_gamepad == nullptr)
    {
        g_gamepad = SDL_OpenGamepad(e.gdevice.which);
        if (g_gamepad != nullptr)
        {
            Log("gamepad: %s", SDL_GetGamepadName(g_gamepad));
        }
    }
    else if (e.type == SDL_EVENT_GAMEPAD_REMOVED && g_gamepad != nullptr &&
             SDL_GetGamepadID(g_gamepad) == e.gdevice.which)
    {
        SDL_CloseGamepad(g_gamepad);
        g_gamepad = nullptr;
    }
}

void InputUpdate(double t, bool window)
{
    u32 pressed = 0;
    u8 sticks[4] = {0x80, 0x80, 0x80, 0x80}; // right x, right y, left x, left y
    for (const Options::Press& p : g_options.presses)
    {
        if (t >= p.at && t < p.at + p.hold)
        {
            pressed |= p.buttons;
        }
    }
    if (window)
    {
        const bool* k = SDL_GetKeyboardState(nullptr);
        const struct
        {
            SDL_Scancode key;
            u32 bit;
        } keys[] = {{SDL_SCANCODE_UP, Up},         {SDL_SCANCODE_DOWN, Down},       {SDL_SCANCODE_LEFT, Left},
                    {SDL_SCANCODE_RIGHT, Right},   {SDL_SCANCODE_RETURN, Start},    {SDL_SCANCODE_BACKSPACE, Select},
                    {SDL_SCANCODE_RSHIFT, Select}, {SDL_SCANCODE_X, Cross},         {SDL_SCANCODE_C, Circle},
                    {SDL_SCANCODE_Z, Square},      {SDL_SCANCODE_V, Triangle},      {SDL_SCANCODE_Q, L1},
                    {SDL_SCANCODE_E, R1},          {SDL_SCANCODE_1, L2},            {SDL_SCANCODE_3, R2}};
        for (const auto& key : keys)
        {
            if (k[key.key])
            {
                pressed |= key.bit;
            }
        }
        auto stick = [&](SDL_Scancode minus, SDL_Scancode plus, u8& v) {
            if (k[minus] != k[plus])
            {
                v = k[minus] ? 0x00 : 0xFF;
            }
        };
        stick(SDL_SCANCODE_A, SDL_SCANCODE_D, sticks[2]);
        stick(SDL_SCANCODE_W, SDL_SCANCODE_S, sticks[3]);
        stick(SDL_SCANCODE_J, SDL_SCANCODE_L, sticks[0]);
        stick(SDL_SCANCODE_I, SDL_SCANCODE_K, sticks[1]);
        if (g_gamepad != nullptr)
        {
            const struct
            {
                SDL_GamepadButton button;
                u32 bit;
            } buttons[] = {{SDL_GAMEPAD_BUTTON_SOUTH, Cross},         {SDL_GAMEPAD_BUTTON_EAST, Circle},
                           {SDL_GAMEPAD_BUTTON_WEST, Square},         {SDL_GAMEPAD_BUTTON_NORTH, Triangle},
                           {SDL_GAMEPAD_BUTTON_BACK, Select},         {SDL_GAMEPAD_BUTTON_START, Start},
                           {SDL_GAMEPAD_BUTTON_LEFT_STICK, L3},       {SDL_GAMEPAD_BUTTON_RIGHT_STICK, R3},
                           {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, L1},    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, R1},
                           {SDL_GAMEPAD_BUTTON_DPAD_UP, Up},          {SDL_GAMEPAD_BUTTON_DPAD_DOWN, Down},
                           {SDL_GAMEPAD_BUTTON_DPAD_LEFT, Left},      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Right}};
            for (const auto& b : buttons)
            {
                if (SDL_GetGamepadButton(g_gamepad, b.button))
                {
                    pressed |= b.bit;
                }
            }
            if (SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000)
            {
                pressed |= L2;
            }
            if (SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000)
            {
                pressed |= R2;
            }
            // The gamepad's sticks unless the keyboard moved them
            const SDL_GamepadAxis axes[4] = {SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFTX,
                                             SDL_GAMEPAD_AXIS_LEFTY};
            for (int i = 0; i < 4; i++)
            {
                if (sticks[i] == 0x80)
                {
                    sticks[i] = Axis(SDL_GetGamepadAxis(g_gamepad, axes[i]));
                }
            }
        }
    }
    PadSetInput(0, pressed, sticks);
}
}
