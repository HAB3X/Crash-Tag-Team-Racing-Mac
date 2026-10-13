// Port 0's controller from the host: the keyboard and mouse (the PC controls, rebindable in bindings.txt), the first gamepad
// SDL finds, and the presses asked for with --press. Read on the presenter's thread, given to the pad server (iop/padman.cpp).
//
// The keyboard and mouse defaults are the Crash Twinsanity native port's: W A S D move (the left stick, a diagonal as a stick's
// diagonal), Space jumps (cross), the mouse's left button or J spins (square), Left Shift or K crouches and slides (circle),
// Tab or I is triangle, Z and C the left and right shoulders, Escape or Return pauses (start), Left Ctrl toggles walking (the
// stick at half), and Q and E turn the right stick. The arrows are the d-pad (WoC's menus read it). Every key can be changed in
// the settings folder's bindings.txt, which is written with the defaults the first time.
//
// While the game takes typed text (its typing hook says so: WoC's name entry), the keyboard types instead: the characters, and
// Backspace and Return, go to the game (ps2_take_typed) and none of them presses a button.
#include "runtime.h"

#include "ps2/runtime.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace Ps2
{
extern const Ps2GameInfo* g_game;

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

// A bound key: a scancode, or a mouse button
constexpr int MouseBase = 10000;
constexpr int MouseLeft = MouseBase + SDL_BUTTON_LEFT;
constexpr int MouseMiddle = MouseBase + SDL_BUTTON_MIDDLE;
constexpr int MouseRight = MouseBase + SDL_BUTTON_RIGHT;

// The actions: a PS2 button, a stick's direction (stick 0 left, 1 right; axis 0 x, 1 y; sign), or walking
struct Action
{
    const char* name;
    u32 button;
    int stick;
    int axis;
    int sign;
    std::vector<int> defaults;
    std::vector<int> keys;
};

std::vector<Action> MakeActions()
{
    return {
        {"cross", Cross, -1, 0, 0, {SDL_SCANCODE_SPACE}, {}},
        {"square", Square, -1, 0, 0, {MouseLeft, SDL_SCANCODE_J}, {}},
        {"circle", Circle, -1, 0, 0, {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_K}, {}},
        {"triangle", Triangle, -1, 0, 0, {SDL_SCANCODE_TAB, SDL_SCANCODE_I}, {}},
        {"l1", L1, -1, 0, 0, {SDL_SCANCODE_Z}, {}},
        {"r1", R1, -1, 0, 0, {SDL_SCANCODE_C}, {}},
        {"l2", L2, -1, 0, 0, {SDL_SCANCODE_1}, {}},
        {"r2", R2, -1, 0, 0, {SDL_SCANCODE_3}, {}},
        {"start", Start, -1, 0, 0, {SDL_SCANCODE_ESCAPE, SDL_SCANCODE_RETURN}, {}},
        {"select", Select, -1, 0, 0, {SDL_SCANCODE_BACKSPACE}, {}},
        {"l3", L3, -1, 0, 0, {}, {}},
        {"r3", R3, -1, 0, 0, {MouseMiddle}, {}},
        {"up", Up, -1, 0, 0, {SDL_SCANCODE_UP}, {}},
        {"down", Down, -1, 0, 0, {SDL_SCANCODE_DOWN}, {}},
        {"left", Left, -1, 0, 0, {SDL_SCANCODE_LEFT}, {}},
        {"right", Right, -1, 0, 0, {SDL_SCANCODE_RIGHT}, {}},
        {"move_forward", 0, 0, 1, -1, {SDL_SCANCODE_W}, {}},
        {"move_back", 0, 0, 1, 1, {SDL_SCANCODE_S}, {}},
        {"move_left", 0, 0, 0, -1, {SDL_SCANCODE_A}, {}},
        {"move_right", 0, 0, 0, 1, {SDL_SCANCODE_D}, {}},
        {"camera_up", 0, 1, 1, -1, {}, {}},
        {"camera_down", 0, 1, 1, 1, {}, {}},
        {"camera_left", 0, 1, 0, -1, {SDL_SCANCODE_Q}, {}},
        {"camera_right", 0, 1, 0, 1, {SDL_SCANCODE_E}, {}},
        {"walk", 0, -2, 0, 0, {SDL_SCANCODE_LCTRL}, {}},
    };
}

std::vector<Action> g_actions = MakeActions();
SDL_Gamepad* g_gamepad = nullptr;
SDL_MouseButtonFlags g_mouse = 0;
bool g_walking = false;
bool g_walkHeld = false;
bool g_typing = false;
std::mutex g_typedLock;
std::string g_typed;
// Buttons the game's side pressed itself (ps2_press_buttons): asked for, then held until a time
u32 g_synthAsked = 0;
double g_synthFor = 0;
u32 g_synthButtons = 0;
double g_synthUntil = 0;

std::string KeyName(int key)
{
    switch (key)
    {
    case MouseLeft: return "Mouse Left";
    case MouseMiddle: return "Mouse Middle";
    case MouseRight: return "Mouse Right";
    default: return SDL_GetScancodeName(static_cast<SDL_Scancode>(key));
    }
}

int KeyFromName(std::string name)
{
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
    {
        name.pop_back();
    }
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front())))
    {
        name.erase(0, 1);
    }
    if (name.empty())
    {
        return -1;
    }
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (lower == "mouse left")
    {
        return MouseLeft;
    }
    if (lower == "mouse middle")
    {
        return MouseMiddle;
    }
    if (lower == "mouse right")
    {
        return MouseRight;
    }
    SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
    return code == SDL_SCANCODE_UNKNOWN ? -1 : static_cast<int>(code);
}

std::string BindingsPath() { return g_options.settingsDir + "/bindings.txt"; }

void WriteBindings()
{
    std::ofstream out(BindingsPath());
    out << "# The keyboard and mouse controls: an action, then its keys (as SDL names them: Space, Left Shift, J, Up) or\n"
           "# mouse buttons (Mouse Left, Mouse Middle, Mouse Right), separated by commas. Delete this file for the defaults.\n";
    for (const Action& a : g_actions)
    {
        out << a.name << " =";
        for (size_t n = 0; n < a.keys.size(); n++)
        {
            out << (n == 0 ? " " : ", ") << KeyName(a.keys[n]);
        }
        out << "\n";
    }
}

void LoadBindings()
{
    for (Action& a : g_actions)
    {
        a.keys = a.defaults;
    }
    std::error_code ec;
    std::filesystem::create_directories(g_options.settingsDir, ec);
    std::ifstream in(BindingsPath());
    if (!in)
    {
        WriteBindings();
        return;
    }
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        std::string name = line.substr(0, eq);
        name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c); }), name.end());
        for (Action& a : g_actions)
        {
            if (name != a.name)
            {
                continue;
            }
            a.keys.clear();
            std::stringstream keys(line.substr(eq + 1));
            std::string key;
            while (std::getline(keys, key, ','))
            {
                int k = KeyFromName(key);
                if (k >= 0)
                {
                    a.keys.push_back(k);
                }
                else if (key.find_first_not_of(" \t") != std::string::npos)
                {
                    Log("bindings.txt: no key \"%s\" (%s)", key.c_str(), a.name);
                }
            }
        }
    }
}

bool Held(int key, const bool* keyboard)
{
    if (key >= MouseBase)
    {
        return (g_mouse & SDL_BUTTON_MASK(key - MouseBase)) != 0;
    }
    return key > SDL_SCANCODE_UNKNOWN && key < SDL_SCANCODE_COUNT && keyboard[key];
}

u8 Axis(Sint16 v) { return static_cast<u8>(std::clamp((static_cast<int>(v) + 32768) >> 8, 0, 255)); }

void Typed(const std::string& text)
{
    std::lock_guard<std::mutex> lock(g_typedLock);
    g_typed += text;
}
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

void InputInit() { LoadBindings(); }

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
    else if (e.type == SDL_EVENT_TEXT_INPUT && g_typing)
    {
        Typed(e.text.text);
    }
    else if (e.type == SDL_EVENT_KEY_DOWN && g_typing)
    {
        if (e.key.scancode == SDL_SCANCODE_BACKSPACE)
        {
            Typed("\b");
        }
        else if (e.key.scancode == SDL_SCANCODE_RETURN || e.key.scancode == SDL_SCANCODE_KP_ENTER)
        {
            Typed("\r");
        }
    }
}

void InputUpdate(double t, SDL_Window* window)
{
    u32 pressed = 0;
    float sticks[2][2] = {};
    for (const Options::Press& p : g_options.presses)
    {
        if (t >= p.at && t < p.at + p.hold)
        {
            pressed |= p.buttons;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_typedLock);
        if (g_synthAsked != 0)
        {
            g_synthButtons = g_synthAsked;
            g_synthUntil = t + g_synthFor;
            g_synthAsked = 0;
        }
    }
    if (t < g_synthUntil)
    {
        pressed |= g_synthButtons;
    }
    static size_t typedNext = 0;
    while (typedNext < g_options.typed.size() && t >= g_options.typed[typedNext].first)
    {
        Typed(g_options.typed[typedNext++].second);
    }
    // The game taking typed text: the keyboard types (SDL's text input on while it does)
    bool typing = g_game != nullptr && g_game->typing != nullptr && g_game->typing() != 0;
    if (window != nullptr && typing != g_typing)
    {
        if (typing)
        {
            SDL_StartTextInput(window);
        }
        else
        {
            SDL_StopTextInput(window);
        }
    }
    g_typing = typing;
    if (window != nullptr)
    {
        const bool* keyboard = SDL_GetKeyboardState(nullptr);
        g_mouse = SDL_GetMouseState(nullptr, nullptr);
        bool walkHeld = false;
        for (const Action& a : g_actions)
        {
            bool held = false;
            for (int key : a.keys)
            {
                // While typing only the mouse and the arrows still press buttons
                if (typing && key < MouseBase && key != SDL_SCANCODE_UP && key != SDL_SCANCODE_DOWN &&
                    key != SDL_SCANCODE_LEFT && key != SDL_SCANCODE_RIGHT)
                {
                    continue;
                }
                held |= Held(key, keyboard);
            }
            if (!held)
            {
                continue;
            }
            if (a.stick >= 0)
            {
                sticks[a.stick][a.axis] += static_cast<float>(a.sign);
            }
            else if (a.stick == -2)
            {
                walkHeld = true;
            }
            else
            {
                pressed |= a.button;
            }
        }
        if (walkHeld && !g_walkHeld)
        {
            g_walking = !g_walking;
        }
        g_walkHeld = walkHeld;
    }
    // A stick from keys: a diagonal is a stick's (both axes at 1/sqrt(2)), walking at half
    u8 out[4] = {0x80, 0x80, 0x80, 0x80}; // right x, right y, left x, left y
    for (int s = 0; s < 2; s++)
    {
        float x = std::clamp(sticks[s][0], -1.0f, 1.0f), y = std::clamp(sticks[s][1], -1.0f, 1.0f);
        float length = std::sqrt(x * x + y * y);
        if (length > 1.0f)
        {
            x /= length;
            y /= length;
        }
        if (s == 0 && g_walking)
        {
            x *= 0.5f;
            y *= 0.5f;
        }
        int at = s == 0 ? 2 : 0;
        out[at] = static_cast<u8>(std::clamp(static_cast<int>(std::lround(128.0f + x * 127.5f)), 0, 255));
        out[at + 1] = static_cast<u8>(std::clamp(static_cast<int>(std::lround(128.0f + y * 127.5f)), 0, 255));
    }
    if (window != nullptr && g_gamepad != nullptr)
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
        // The gamepad's sticks unless the keys moved them
        const SDL_GamepadAxis axes[4] = {SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFTX,
                                         SDL_GAMEPAD_AXIS_LEFTY};
        for (int i = 0; i < 4; i++)
        {
            if (out[i] == 0x80)
            {
                out[i] = Axis(SDL_GetGamepadAxis(g_gamepad, axes[i]));
            }
        }
    }
    PadSetInput(0, pressed, out);
}
}

using namespace Ps2;

extern "C" void ps2_press_buttons(uint32_t buttons, double seconds)
{
    std::lock_guard<std::mutex> lock(g_typedLock);
    g_synthAsked = buttons;
    g_synthFor = seconds;
}

extern "C" int ps2_take_typed(char* out, int max)
{
    std::lock_guard<std::mutex> lock(g_typedLock);
    int n = std::min(max, static_cast<int>(g_typed.size()));
    std::memcpy(out, g_typed.data(), static_cast<size_t>(n));
    g_typed.erase(0, static_cast<size_t>(n));
    return n;
}
