// Crash Tag Team Racing's PC controls: the keyboard and mouse in three modes (Ps2GameInfo's controls and modes), the menus, on
// foot in the park and driving, because one key can't serve all three: W is the stick on foot but accelerates (cross) in a car.
// A gamepad is the PS2's controller as before. The game's own controls are its controller maps (design\permanent\controllers.god
// in COMMON.RCF): PresentationMap (the menus: Select is cross, Back triangle, Aux1 circle, Aux2 square, the d-pad and the left
// stick), OnFootMap (Jump cross, Spin square, Flop circle, Interact triangle, ToggleFP L1, Zoom R1, CameraReset R3, HudDisplay
// L3, SkipAllTutorials Select, the right stick the camera) and CarMap (GasDigital cross, BrakeDigital square or L2, GasAnalog and
// TurretFire R2, Clash triangle, WeaponFire circle, SwapPlayer R1, Zoom L1, Taunt L3, Reset Select, the left stick steering and
// aiming the turret). A powerslide is the brake tapped while turning.
//
// The mode is what the game reads: every input is read through Controller::GetValue (0x196560, the controller and the input's
// index), overridden to note which controllers are read. A controller is known by its inputs (from +16, 24 bytes each: a hash
// of the input's name first; their count at +784): the car's have GasDigital, on foot's Jump. A car's read
// is driving, on foot's on foot, and neither for a while (a third of a second: the park draws 15-30 frames a second) the menus,
// the pause menus included (the game stops reading the player's controls while they're up). The title and the attract demo
// read the cars' controls: they're driving, and Return, Start there, starts the game. Escape, the runtime's menu key, is the
// game's Start in every mode as well, and the frontend menus' back (src/pause.cpp).
#include "ps2/iop.h"
#include "ps2/recomp.h"
#include "ps2/runtime.h"

#include <atomic>
#include <cstring>
#include <unordered_map>

extern "C" void f_00196560(Ps2Cpu* c);

namespace
{
enum Mode : int
{
    Menus,
    OnFoot,
    Driving,
};

const char* const Modes[] = {"Menus", "On foot", "Driving"};

const Ps2GameControl Controls[] = {
    {"menu_up", "Up", "up", "Up, W", Menus},
    {"menu_down", "Down", "down", "Down, S", Menus},
    {"menu_left", "Left", "left", "Left, A", Menus},
    {"menu_right", "Right", "right", "Right, D", Menus},
    {"menu_select", "Select", "cross", "Return, Space", Menus},
    {"menu_back", "Back", "triangle", "Backspace", Menus},
    {"menu_square", "Square (new name, change outfit)", "square", "Tab", Menus},
    {"menu_circle", "Circle", "circle", "Left Shift", Menus},
    {"foot_forward", "Move forward", "left_up", "W", OnFoot},
    {"foot_back", "Move back", "left_down", "S", OnFoot},
    {"foot_left", "Move left", "left_left", "A", OnFoot},
    {"foot_right", "Move right", "left_right", "D", OnFoot},
    {"foot_jump", "Jump", "cross", "Space", OnFoot},
    {"foot_spin", "Spin attack", "square", "Mouse Left, J", OnFoot},
    {"foot_slam", "Body slam", "circle", "Left Shift, K", OnFoot},
    {"foot_talk", "Talk", "triangle", "E, I", OnFoot},
    {"foot_camera_left", "Camera left", "right_left", "Left", OnFoot},
    {"foot_camera_right", "Camera right", "right_right", "Right", OnFoot},
    {"foot_camera_up", "Camera up", "right_up", "Up", OnFoot},
    {"foot_camera_down", "Camera down", "right_down", "Down", OnFoot},
    {"foot_look", "First person camera", "l1", "Q", OnFoot},
    {"foot_zoom", "Zoom", "r1", "Mouse Right", OnFoot},
    {"foot_camera_reset", "Reset the camera", "r3", "R, Mouse Middle", OnFoot},
    {"foot_map", "Show the map", "l3", "Tab", OnFoot},
    {"foot_tutorials", "Skip the tutorials", "select", "Backspace", OnFoot},
    {"foot_pause", "Pause", "start", "Return", OnFoot},
    {"foot_walk", "Walk", "walk", "Left Ctrl", OnFoot},
    {"drive_accelerate", "Accelerate", "cross", "W, Up", Driving},
    {"drive_brake", "Brake, reverse, powerslide", "square", "S, Down", Driving},
    {"drive_left", "Steer left", "left_left", "A, Left", Driving},
    {"drive_right", "Steer right", "left_right", "D, Right", Driving},
    {"drive_up", "Aim or tilt up", "left_up", "I", Driving},
    {"drive_down", "Aim or tilt down", "left_down", "K", Driving},
    {"drive_powerup", "Use a power-up", "circle", "Space", Driving},
    {"drive_turret", "Fire the turret", "r2", "Mouse Left", Driving},
    {"drive_clash", "Clash", "triangle", "Left Shift, E", Driving},
    {"drive_swap", "Swap driver and gunner", "r1", "Q", Driving},
    {"drive_zoom", "Zoom (gunner)", "l1", "Mouse Right", Driving},
    {"drive_taunt", "Taunt", "l3", "T", Driving},
    {"drive_reset", "Reset the car", "select", "R", Driving},
    {"drive_pause", "Pause", "start", "Return", Driving},
};

// An input's name hashed as the game does (0x3A1F60): h = h * 65599 ^ the character (signed), from 0, 64 bits
constexpr u64 Hash(const char* name)
{
    u64 h = 0;
    for (; *name != 0; name++)
    {
        h = (h * 65599) ^ static_cast<u64>(static_cast<s64>(static_cast<s8>(*name)));
    }
    return h;
}

enum class Kind
{
    Other,
    OnFoot,
    Car,
};

Kind KindOf(u32 controller)
{
    u32 count;
    std::memcpy(&count, Ps2::EeMemory(controller + 784), 4);
    for (u32 n = 0; n < count && n < 64; n++)
    {
        u64 h;
        std::memcpy(&h, Ps2::EeMemory(controller + 16 + 24 * n), 8);
        if (h == Hash("GasDigital"))
        {
            return Kind::Car;
        }
        if (h == Hash("Jump"))
        {
            return Kind::OnFoot;
        }
    }
    return Kind::Other;
}

// On the game's side: the controllers known, and what was read since the last vblank
std::unordered_map<u32, Kind> g_kinds;
bool g_carRead = false;
bool g_onFootRead = false;
std::atomic<int> g_mode{Menus};
}

// Controller::GetValue (0x196560: a0 the controller, a1 the input's index): noted, then read as before
extern "C" void CttrControllerValue(Ps2Cpu* c)
{
    u32 controller = c->r[4].uw[0];
    auto known = g_kinds.find(controller);
    if (known == g_kinds.end())
    {
        known = g_kinds.emplace(controller, KindOf(controller)).first;
    }
    g_carRead |= known->second == Kind::Car;
    g_onFootRead |= known->second == Kind::OnFoot;
    f_00196560(c);
}

void CttrControlsVblank()
{
    // The menus once neither the car's nor on foot's controls have been read for a third of a second
    static int unread = 0;
    if (g_carRead || g_onFootRead)
    {
        g_mode = g_carRead ? Driving : OnFoot;
        unread = 0;
    }
    else if (++unread > 20)
    {
        g_mode = Menus;
    }
    static int logged = -1;
    if (g_mode != logged)
    {
        logged = g_mode;
        Ps2::Log("controls: %s", Modes[logged]);
    }
    g_carRead = g_onFootRead = false;
}

int CttrMode() { return g_mode; }

int CttrMenu() { return g_mode == Menus ? 1 : 0; }

const Ps2GameControl* CttrControls() { return Controls; }

int CttrControlCount() { return static_cast<int>(sizeof(Controls) / sizeof(Controls[0])); }

const char* const* CttrModes() { return Modes; }

int CttrModeCount() { return static_cast<int>(sizeof(Modes) / sizeof(Modes[0])); }
