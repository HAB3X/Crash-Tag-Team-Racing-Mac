// Port 0's controller from the host: the keyboard and mouse (the PC controls, rebindable in bindings.txt), the first gamepad
// SDL finds, and the presses asked for with --press. Read on the presenter's thread, given to the pad server (iop/padman.cpp).
//
// The keyboard and mouse defaults are the Crash Twinsanity native port's: W A S D move (the left stick, a diagonal as a stick's
// diagonal), Space jumps (cross), the mouse's left button or J spins (square), Left Shift or K crouches and slides (circle),
// Tab or I is triangle, Z and C the left and right shoulders, Return pauses (start), Left Ctrl toggles walking (the stick at
// half), and Q and E turn the right stick. The arrows are the d-pad (WoC's menus read it). Escape opens the PC menus (ui/), as
// a gamepad's Back button does unless it's set to be Select. Every key can be changed in the PC menus' Controls or in the
// settings folder's bindings.txt, which is written with the defaults the first time; a key of the PC menus' is only theirs.
//
// While the PC menus are up the game is paused and gets nothing; what was held as they closed counts again only once it's let
// go (the Return that chose Resume doesn't pause the game).
//
// While the game takes typed text (its typing hook says so: WoC's name entry), the keyboard types instead: the characters, and
// Backspace and Return, go to the game (ps2_take_typed) and none of them presses a button.
#include "runtime.h"

#include "ps2/runtime.h"
#include "ui/menus.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
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
constexpr int MouseX1 = MouseBase + SDL_BUTTON_X1;
constexpr int MouseX2 = MouseBase + SDL_BUTTON_X2;

// The actions: a PS2 button, a stick's direction (stick 0 left, 1 right; axis 0 x, 1 y; sign), walking (-2) or the PC menus
// (-3); the PC menus' Controls table names each and shows the gamepad's control for it. A game's own control has its mode
// (an index into Ps2GameInfo::modes); the runtime's have none (-1: every mode)
struct Action
{
    const char* name;
    u32 button;
    int stick;
    int axis;
    int sign;
    std::vector<int> defaults;
    std::vector<int> keys;
    const char* label;
    Ui::PadControl pad;
    int mode = -1;
};

int KeyFromName(std::string name);

bool GameControls() { return g_game != nullptr && g_game->controls != nullptr && g_game->controlCount > 0; }

// The gamepad's control for a PS2 button (the first of several)
Ui::PadControl PadFor(u32 buttons)
{
    using P = Ui::PadControl;
    const struct
    {
        u32 bit;
        P pad;
    } pads[] = {{Cross, P::South},          {Circle, P::East},          {Square, P::West},        {Triangle, P::North},
                {L1, P::LeftShoulder},      {R1, P::RightShoulder},     {L2, P::LeftTrigger},     {R2, P::RightTrigger},
                {Start, P::Start},          {Select, P::BackAsSelect},  {L3, P::LeftStick},       {R3, P::RightStick},
                {Up, P::DpadUp},            {Down, P::DpadDown},        {Left, P::DpadLeft},      {Right, P::DpadRight}};
    for (const auto& p : pads)
    {
        if (buttons & p.bit)
        {
            return p.pad;
        }
    }
    return P::None;
}

// A game's control as an action: what it presses (a stick's direction, walking or buttons) and its default keys
Action GameAction(const Ps2GameControl& control)
{
    using P = Ui::PadControl;
    Action a{control.name, 0, -1, 0, 0, {}, {}, control.label, P::None, control.mode};
    const struct
    {
        const char* name;
        int stick;
        int axis;
        int sign;
        P pad;
    } sticks[] = {{"left_up", 0, 1, -1, P::LeftStickUp},     {"left_down", 0, 1, 1, P::LeftStickDown},
                  {"left_left", 0, 0, -1, P::LeftStickLeft}, {"left_right", 0, 0, 1, P::LeftStickRight},
                  {"right_up", 1, 1, -1, P::RightStickUp},   {"right_down", 1, 1, 1, P::RightStickDown},
                  {"right_left", 1, 0, -1, P::RightStickLeft}, {"right_right", 1, 0, 1, P::RightStickRight}};
    std::string press = control.press;
    bool found = false;
    for (const auto& stick : sticks)
    {
        if (press == stick.name)
        {
            a.stick = stick.stick;
            a.axis = stick.axis;
            a.sign = stick.sign;
            a.pad = stick.pad;
            found = true;
        }
    }
    if (press == "walk")
    {
        a.stick = -2;
    }
    else if (!found)
    {
        a.button = ButtonsFromNames(press);
        a.pad = PadFor(a.button);
    }
    std::stringstream keys(control.keys != nullptr ? control.keys : "");
    std::string key;
    while (std::getline(keys, key, ','))
    {
        int k = KeyFromName(key);
        if (k >= 0)
        {
            a.defaults.push_back(k);
        }
        else if (key.find_first_not_of(" \t") != std::string::npos)
        {
            Log("the game's control %s: no key \"%s\"", control.name, key.c_str());
        }
    }
    return a;
}

// The PC menus' key: the runtime's own action, in every mode
Action MenuAction() { return {"menu", 0, -3, 0, 0, {SDL_SCANCODE_ESCAPE}, {}, "PC menu", Ui::PadControl::BackAsMenu}; }

std::vector<Action> MakeActions()
{
    if (GameControls())
    {
        std::vector<Action> actions{MenuAction()};
        for (int n = 0; n < g_game->controlCount; n++)
        {
            actions.push_back(GameAction(g_game->controls[n]));
        }
        return actions;
    }
    using P = Ui::PadControl;
    return {
        {"move_forward", 0, 0, 1, -1, {SDL_SCANCODE_W}, {}, "Move forward", P::LeftStickUp},
        {"move_back", 0, 0, 1, 1, {SDL_SCANCODE_S}, {}, "Move back", P::LeftStickDown},
        {"move_left", 0, 0, 0, -1, {SDL_SCANCODE_A}, {}, "Move left", P::LeftStickLeft},
        {"move_right", 0, 0, 0, 1, {SDL_SCANCODE_D}, {}, "Move right", P::LeftStickRight},
        {"camera_up", 0, 1, 1, -1, {}, {}, "Camera up", P::RightStickUp},
        {"camera_down", 0, 1, 1, 1, {}, {}, "Camera down", P::RightStickDown},
        {"camera_left", 0, 1, 0, -1, {SDL_SCANCODE_Q}, {}, "Camera left", P::RightStickLeft},
        {"camera_right", 0, 1, 0, 1, {SDL_SCANCODE_E}, {}, "Camera right", P::RightStickRight},
        {"cross", Cross, -1, 0, 0, {SDL_SCANCODE_SPACE}, {}, "Cross", P::South},
        {"square", Square, -1, 0, 0, {MouseLeft, SDL_SCANCODE_J}, {}, "Square", P::West},
        {"circle", Circle, -1, 0, 0, {SDL_SCANCODE_LSHIFT, SDL_SCANCODE_K}, {}, "Circle", P::East},
        {"triangle", Triangle, -1, 0, 0, {SDL_SCANCODE_TAB, SDL_SCANCODE_I}, {}, "Triangle", P::North},
        {"l1", L1, -1, 0, 0, {SDL_SCANCODE_Z}, {}, "L1", P::LeftShoulder},
        {"r1", R1, -1, 0, 0, {SDL_SCANCODE_C}, {}, "R1", P::RightShoulder},
        {"l2", L2, -1, 0, 0, {SDL_SCANCODE_1}, {}, "L2", P::LeftTrigger},
        {"r2", R2, -1, 0, 0, {SDL_SCANCODE_3}, {}, "R2", P::RightTrigger},
        {"start", Start, -1, 0, 0, {SDL_SCANCODE_RETURN}, {}, "Start (the game's pause)", P::Start},
        {"select", Select, -1, 0, 0, {SDL_SCANCODE_BACKSPACE}, {}, "Select", P::BackAsSelect},
        {"l3", L3, -1, 0, 0, {}, {}, "L3", P::LeftStick},
        {"r3", R3, -1, 0, 0, {MouseMiddle}, {}, "R3", P::RightStick},
        {"up", Up, -1, 0, 0, {SDL_SCANCODE_UP}, {}, "D-pad up", P::DpadUp},
        {"down", Down, -1, 0, 0, {SDL_SCANCODE_DOWN}, {}, "D-pad down", P::DpadDown},
        {"left", Left, -1, 0, 0, {SDL_SCANCODE_LEFT}, {}, "D-pad left", P::DpadLeft},
        {"right", Right, -1, 0, 0, {SDL_SCANCODE_RIGHT}, {}, "D-pad right", P::DpadRight},
        {"walk", 0, -2, 0, 0, {SDL_SCANCODE_LCTRL}, {}, "Walk", P::None},
        MenuAction(),
    };
}

std::vector<Action> g_actions = MakeActions();
SDL_Gamepad* g_gamepad = nullptr;
// What was held as the PC menus closed (or a shortcut was pressed), not counted until it's let go: keys, mouse buttons, the
// gamepad's buttons (bits by SDL's numbers; 30 and 31 the triggers)
std::vector<bool> g_ignoredKeys(SDL_SCANCODE_COUNT, false);
SDL_MouseButtonFlags g_ignoredMouse = 0;
u32 g_ignoredPad = 0;
bool g_ignoreAll = false;
// The device the player used last (the prompts follow it): the keyboard and mouse, or the gamepad
std::atomic<bool> g_lastKeyboard{true};
SDL_MouseButtonFlags g_mouse = 0;
bool g_walking = false;
bool g_walkHeld = false;
bool g_typing = false;
bool g_menu = false;
// The picture's place in the window's render coordinates (the presenter's), the mouse over it, and its clicks
SDL_FRect g_picture = {0, 0, 0, 0};
float g_mouseX = -1, g_mouseY = -1;
bool g_mouseMoved = false;
std::vector<std::pair<float, float>> g_clicks;
// Whether any of the player's buttons (or the left stick) was held at the last update, and the prompt over the picture
std::atomic<bool> g_anyHeld{false};
std::string g_prompt;
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
    case MouseX1: return "Mouse 4";
    case MouseX2: return "Mouse 5";
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
    if (lower == "mouse 4")
    {
        return MouseX1;
    }
    if (lower == "mouse 5")
    {
        return MouseX2;
    }
    SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
    return code == SDL_SCANCODE_UNKNOWN ? -1 : static_cast<int>(code);
}

std::string BindingsPath() { return g_options.settingsDir + "/bindings.txt"; }

void WriteBindings()
{
    std::ofstream out(BindingsPath());
    out << "# The keyboard and mouse controls: an action, then its keys (as SDL names them: Space, Left Shift, J, Up) or\n"
           "# mouse buttons (Mouse Left, Mouse Middle, Mouse Right, Mouse 4, Mouse 5), separated by commas. menu opens the PC\n"
           "# menus. The PC menus' Controls change them too. Delete this file for the defaults.\n";
    int mode = -1;
    for (const Action& a : g_actions)
    {
        if (a.mode != mode && a.mode >= 0 && a.mode < g_game->modeCount)
        {
            out << "# " << g_game->modes[a.mode] << "\n";
        }
        mode = a.mode;
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
        return (g_mouse & ~g_ignoredMouse & SDL_BUTTON_MASK(key - MouseBase)) != 0;
    }
    return key > SDL_SCANCODE_UNKNOWN && key < SDL_SCANCODE_COUNT && keyboard[key] && !g_ignoredKeys[static_cast<size_t>(key)];
}

// A key of the PC menus' (theirs only: it presses nothing of the PS2's)
bool MenuKey(int key)
{
    for (const Action& a : g_actions)
    {
        if (a.stick == -3)
        {
            return std::find(a.keys.begin(), a.keys.end(), key) != a.keys.end();
        }
    }
    return false;
}

// What's let go of is counted again
void ForgetReleased(const bool* keyboard)
{
    for (size_t k = 0; k < g_ignoredKeys.size(); k++)
    {
        if (g_ignoredKeys[k] && !keyboard[k])
        {
            g_ignoredKeys[k] = false;
        }
    }
    g_ignoredMouse &= g_mouse;
    if (g_gamepad == nullptr)
    {
        g_ignoredPad = 0;
        return;
    }
    for (int b = 0; b < 30; b++)
    {
        if ((g_ignoredPad & (1u << b)) && !SDL_GetGamepadButton(g_gamepad, static_cast<SDL_GamepadButton>(b)))
        {
            g_ignoredPad &= ~(1u << b);
        }
    }
    if ((g_ignoredPad & (1u << 30)) && SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) <= 16000)
    {
        g_ignoredPad &= ~(1u << 30);
    }
    if ((g_ignoredPad & (1u << 31)) && SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) <= 16000)
    {
        g_ignoredPad &= ~(1u << 31);
    }
}

bool PadHeld(SDL_GamepadButton button)
{
    return SDL_GetGamepadButton(g_gamepad, button) && !(g_ignoredPad & (1u << button));
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

void InputInit()
{
    // The game's own controls, now that the game is known
    g_actions = MakeActions();
    LoadBindings();
}

void InputPicture(float x, float y, float w, float h) { g_picture = {x, y, w, h}; }

void InputEvent(const SDL_Event& e)
{
    InputNoteDevice(e);
    if ((e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) && g_picture.w > 0)
    {
        float mx = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.x : e.button.x;
        float my = e.type == SDL_EVENT_MOUSE_MOTION ? e.motion.y : e.button.y;
        float x = (mx - g_picture.x) / g_picture.w, y = (my - g_picture.y) / g_picture.h;
        std::lock_guard<std::mutex> lock(g_typedLock);
        if (e.type == SDL_EVENT_MOUSE_MOTION)
        {
            g_mouseX = x;
            g_mouseY = y;
            g_mouseMoved = true;
        }
        else if (e.button.button == SDL_BUTTON_LEFT && g_menu && x >= 0 && x < 1 && y >= 0 && y < 1)
        {
            g_clicks.emplace_back(x, y);
        }
    }
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
    static size_t clickNext = 0;
    while (clickNext < g_options.clicks.size() && t >= g_options.clicks[clickNext].at)
    {
        const Options::Click& click = g_options.clicks[clickNext++];
        std::lock_guard<std::mutex> lock(g_typedLock);
        g_mouseX = click.x;
        g_mouseY = click.y;
        g_mouseMoved = true;
        g_clicks.emplace_back(click.x, click.y);
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
    g_menu = g_game != nullptr && g_game->menu != nullptr && g_game->menu() != 0;
    // The game's mode: only its controls press anything (and the runtime's own, in every mode)
    int mode = GameControls() && g_game->mode != nullptr ? g_game->mode() : 0;
    // The runtime's menus' keys (Return is cross, Backspace triangle) are for its own controls: a game's are its own
    bool remap = g_menu && !GameControls();
    // The PC menus up: the game gets nothing (it's paused); what's held as they close waits to be let go
    bool menus = Ui::MenusOpen();
    if (window != nullptr && (menus || g_ignoreAll))
    {
        InputIgnoreHeld();
        g_ignoreAll = menus;
    }
    if (menus)
    {
        pressed = 0;
    }
    if (window != nullptr && !menus)
    {
        const bool* keyboard = SDL_GetKeyboardState(nullptr);
        g_mouse = SDL_GetMouseState(nullptr, nullptr);
        ForgetReleased(keyboard);
        bool walkHeld = false;
        for (const Action& a : g_actions)
        {
            if (a.mode >= 0 && a.mode != mode)
            {
                continue;
            }
            bool held = false;
            for (int key : a.keys)
            {
                // While typing only the mouse and the arrows still press buttons
                if (typing && key < MouseBase && key != SDL_SCANCODE_UP && key != SDL_SCANCODE_DOWN &&
                    key != SDL_SCANCODE_LEFT && key != SDL_SCANCODE_RIGHT)
                {
                    continue;
                }
                // In a menu the mouse's left button clicks (ps2_take_click), Return confirms and Backspace goes back
                if ((g_menu && key == MouseLeft) || (remap && (key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_BACKSPACE)))
                {
                    continue;
                }
                // The PC menus' keys are theirs
                if (a.stick != -3 && MenuKey(key))
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
            else if (a.stick == -3)
            {
                // A game with its own pause menu has the menu key as its Start
                if (g_game != nullptr && g_game->ownPause != 0)
                {
                    pressed |= Start;
                }
                continue;
            }
            else
            {
                pressed |= a.button;
            }
        }
        if (remap && !typing && Held(SDL_SCANCODE_RETURN, keyboard))
        {
            pressed |= Cross;
        }
        if (remap && !typing && Held(SDL_SCANCODE_BACKSPACE, keyboard))
        {
            pressed |= Triangle;
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
    if (window != nullptr && g_gamepad != nullptr && !menus)
    {
        // The Back button is the PC menus' (its press opens them: ui/menus.cpp) unless it's set to be Select
        bool backIsSelect = Ui::PadBackIsSelect();
        const struct
        {
            SDL_GamepadButton button;
            u32 bit;
        } buttons[] = {{SDL_GAMEPAD_BUTTON_SOUTH, Cross},         {SDL_GAMEPAD_BUTTON_EAST, Circle},
                       {SDL_GAMEPAD_BUTTON_WEST, Square},         {SDL_GAMEPAD_BUTTON_NORTH, Triangle},
                       {SDL_GAMEPAD_BUTTON_BACK, backIsSelect ? Select : 0u}, {SDL_GAMEPAD_BUTTON_START, Start},
                       {SDL_GAMEPAD_BUTTON_LEFT_STICK, L3},       {SDL_GAMEPAD_BUTTON_RIGHT_STICK, R3},
                       {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, L1},    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, R1},
                       {SDL_GAMEPAD_BUTTON_DPAD_UP, Up},          {SDL_GAMEPAD_BUTTON_DPAD_DOWN, Down},
                       {SDL_GAMEPAD_BUTTON_DPAD_LEFT, Left},      {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, Right}};
        for (const auto& b : buttons)
        {
            if (PadHeld(b.button))
            {
                pressed |= b.bit;
            }
        }
        if (!(g_ignoredPad & (1u << 30)) && SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000)
        {
            pressed |= L2;
        }
        if (!(g_ignoredPad & (1u << 31)) && SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000)
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
    if (menus)
    {
        out[0] = out[1] = out[2] = out[3] = 0x80;
    }
    g_anyHeld = pressed != 0 || out[2] != 0x80 || out[3] != 0x80;
    PadSetInput(0, pressed, out);
}

void InputIgnoreHeld()
{
    const bool* keyboard = SDL_GetKeyboardState(nullptr);
    for (size_t k = 0; k < g_ignoredKeys.size(); k++)
    {
        g_ignoredKeys[k] = g_ignoredKeys[k] || keyboard[k];
    }
    g_ignoredMouse |= SDL_GetMouseState(nullptr, nullptr);
    if (g_gamepad != nullptr)
    {
        for (int b = 0; b < 30 && b < SDL_GAMEPAD_BUTTON_COUNT; b++)
        {
            if (SDL_GetGamepadButton(g_gamepad, static_cast<SDL_GamepadButton>(b)))
            {
                g_ignoredPad |= 1u << b;
            }
        }
        if (SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000)
        {
            g_ignoredPad |= 1u << 30;
        }
        if (SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000)
        {
            g_ignoredPad |= 1u << 31;
        }
    }
}

void InputNoteDevice(const SDL_Event& e)
{
    switch (e.type)
    {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_WHEEL:
        g_lastKeyboard = true;
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        g_lastKeyboard = false;
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (e.gaxis.value > 20000 || e.gaxis.value < -20000)
        {
            g_lastKeyboard = false;
        }
        break;
    default:
        break;
    }
}

bool InputLastKeyboard() { return g_lastKeyboard || g_gamepad == nullptr; }

SDL_Gamepad* InputGamepad() { return g_gamepad; }

int InputActionCount() { return static_cast<int>(g_actions.size()); }

std::string InputActionLabel(int action)
{
    const Action& a = g_actions[static_cast<size_t>(action)];
    // With the game's own pause menu the menu key is its pause, and the gamepad's Back its Select
    if (a.stick == -3 && g_game != nullptr && g_game->ownPause != 0)
    {
        return "Pause";
    }
    return a.label;
}

Ui::PadControl InputActionPad(int action) { return g_actions[static_cast<size_t>(action)].pad; }

std::string InputActionGroup(int action)
{
    int mode = g_actions[static_cast<size_t>(action)].mode;
    return mode >= 0 && mode < g_game->modeCount ? g_game->modes[mode] : "";
}

int InputActionKey(int action, int slot)
{
    const std::vector<int>& keys = g_actions[static_cast<size_t>(action)].keys;
    return slot < static_cast<int>(keys.size()) ? keys[static_cast<size_t>(slot)] : -1;
}

std::string InputKeyName(int key)
{
    if (key < 0)
    {
        return "";
    }
    std::string name = KeyName(key);
    return name.empty() ? "?" : name;
}

int InputMouseKey(int button) { return button >= SDL_BUTTON_LEFT && button <= SDL_BUTTON_X2 ? MouseBase + button : -1; }

int InputBindKey(int action, int slot, int key)
{
    // The key taken from wherever else it was (that slot given this one's old key: the two swap), then put in the slot
    std::vector<int>& keys = g_actions[static_cast<size_t>(action)].keys;
    int old = slot < static_cast<int>(keys.size()) ? keys[static_cast<size_t>(slot)] : -1;
    int swapped = -1;
    int mode = g_actions[static_cast<size_t>(action)].mode;
    for (size_t other = 0; other < g_actions.size(); other++)
    {
        // A key can be a different control in each of a game's modes
        int theirMode = g_actions[other].mode;
        if (mode >= 0 && theirMode >= 0 && mode != theirMode)
        {
            continue;
        }
        std::vector<int>& theirs = g_actions[other].keys;
        for (size_t n = 0; n < theirs.size(); n++)
        {
            if (theirs[n] == key && !(static_cast<int>(other) == action && static_cast<int>(n) == slot))
            {
                if (old >= 0)
                {
                    theirs[n] = old;
                }
                else
                {
                    theirs.erase(theirs.begin() + static_cast<long>(n));
                }
                swapped = static_cast<int>(other);
                break;
            }
        }
    }
    slot = std::min(slot, static_cast<int>(keys.size()));
    if (slot < static_cast<int>(keys.size()))
    {
        keys[static_cast<size_t>(slot)] = key;
    }
    else
    {
        keys.push_back(key);
    }
    WriteBindings();
    return swapped == action ? -1 : swapped;
}

void InputClearKey(int action, int slot)
{
    std::vector<int>& keys = g_actions[static_cast<size_t>(action)].keys;
    if (slot < static_cast<int>(keys.size()))
    {
        keys.erase(keys.begin() + slot);
        WriteBindings();
    }
}

void InputResetKeys()
{
    for (Action& a : g_actions)
    {
        a.keys = a.defaults;
    }
    WriteBindings();
}

bool InputIsMenuKey(int key) { return MenuKey(key); }

std::string InputPrompt()
{
    std::lock_guard<std::mutex> lock(g_typedLock);
    return g_prompt;
}
}

using namespace Ps2;

extern "C" void ps2_press_buttons(uint32_t buttons, double seconds)
{
    std::lock_guard<std::mutex> lock(g_typedLock);
    g_synthAsked = buttons;
    g_synthFor = seconds;
}

extern "C" int ps2_any_input_held(void) { return g_anyHeld ? 1 : 0; }

// The PC menus' overlay draws the prompt over the picture
extern "C" void ps2_show_prompt(const char* text)
{
    std::lock_guard<std::mutex> lock(g_typedLock);
    g_prompt = text != nullptr ? text : "";
}

extern "C" int ps2_menus_open(void) { return Ui::MenusOpen() ? 1 : 0; }

extern "C" int ps2_mouse_moved(float* x, float* y)
{
    std::lock_guard<std::mutex> lock(g_typedLock);
    if (!g_mouseMoved || g_mouseX < 0 || g_mouseX >= 1 || g_mouseY < 0 || g_mouseY >= 1)
    {
        return 0;
    }
    g_mouseMoved = false;
    *x = g_mouseX;
    *y = g_mouseY;
    return 1;
}

extern "C" int ps2_take_click(float* x, float* y)
{
    std::lock_guard<std::mutex> lock(g_typedLock);
    if (g_clicks.empty())
    {
        return 0;
    }
    *x = g_clicks.front().first;
    *y = g_clicks.front().second;
    g_clicks.erase(g_clicks.begin());
    return 1;
}

extern "C" int ps2_take_typed(char* out, int max)
{
    std::lock_guard<std::mutex> lock(g_typedLock);
    int n = std::min(max, static_cast<int>(g_typed.size()));
    std::memcpy(out, g_typed.data(), static_cast<size_t>(n));
    g_typed.erase(0, static_cast<size_t>(n));
    return n;
}
