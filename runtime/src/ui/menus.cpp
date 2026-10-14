// The PC menus' logic (menus.h): the screens (the pause menu, Options and Extras with their tabs, the boxes asking to quit and to
// keep a display change), their rows and what each does, the settings kept in options.txt, and the input: the keyboard (arrows
// or W A S D, Return or Space, Escape, Q and E or Tab for the tabs, Delete to clear a key), the mouse (hover, click, the wheel,
// a slider dragged, a right click to go back) and a gamepad (the D-pad or the left stick, the bottom button to choose, the right
// one to go back, the shoulders for the tabs). The view is menuview.cpp. From the Crash Twinsanity native port's
// ui/settings.cpp and ui/options.cpp (same author), its game's own menus and settings left out: a game's come through
// ps2/runtime.h
#include "menusinternal.h"
#include "overlayinternal.h"

#include "../runtime.h"
#include "ps2/runtime.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace Ps2
{
extern const Ps2GameInfo* g_game;
}

namespace Ps2::Ui
{
namespace
{
using Clock = std::chrono::steady_clock;

// ------------------------------------------------------------------------------------------------------------- the settings
// The runtime's own (options.txt): the master volume (0-10), muted and paused in the background, the prompts' style
// (0 automatic, 1 the keyboard, 2 PlayStation, 3 Xbox, 4 Nintendo Switch), what a gamepad's Back button is (0 the PC menus, 1
// Select); and the game's own settings' choices by their tab and name
int g_masterVolume = 10;
bool g_muteInBackground = true;
bool g_pauseInBackground = true;
int g_promptChoice = 0;
std::atomic<bool> g_backIsSelect{false};
std::map<std::string, std::string> g_savedGame;

std::string OptionsPath() { return g_options.settingsDir + "/options.txt"; }

std::string GameKey(const Ps2GameSetting& setting) { return std::string(setting.tab) + ": " + setting.name; }

int ChoiceCount(const Ps2GameSetting& setting)
{
    int n = 0;
    while (setting.choices != nullptr && setting.choices[n] != nullptr)
    {
        n++;
    }
    return n;
}

void SaveOptions()
{
    std::error_code ec;
    std::filesystem::create_directories(g_options.settingsDir, ec);
    std::ofstream out(OptionsPath());
    out << "# The PC menus' settings (Escape opens them). master_volume: 0-10; mute_in_background, pause_in_background: 0 or 1;\n"
           "# prompts: automatic, keyboard, playstation, xbox or switch; pad_back: menu (a gamepad's Back button opens the PC\n"
           "# menus) or select (it's the PS2's Select). The game's own settings follow, by their tab and name\n";
    static const char* const Prompts[] = {"automatic", "keyboard", "playstation", "xbox", "switch"};
    out << "master_volume = " << g_masterVolume << "\n";
    out << "mute_in_background = " << (g_muteInBackground ? 1 : 0) << "\n";
    out << "pause_in_background = " << (g_pauseInBackground ? 1 : 0) << "\n";
    out << "prompts = " << Prompts[std::clamp(g_promptChoice, 0, 4)] << "\n";
    out << "pad_back = " << (g_backIsSelect ? "select" : "menu") << "\n";
    for (const auto& [key, value] : g_savedGame)
    {
        out << key << " = " << value << "\n";
    }
}

void ReadOptions()
{
    std::ifstream in(OptionsPath());
    if (!in)
    {
        SaveOptions();
        return;
    }
    auto trim = [](std::string text) {
        text.erase(0, text.find_first_not_of(" \t\r"));
        text.erase(text.find_last_not_of(" \t\r") + 1);
        return text;
    };
    std::string line;
    while (std::getline(in, line))
    {
        // A game's setting's tab may hold '=' no more than its name does: the last one splits
        size_t eq = line.rfind('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos)
        {
            continue;
        }
        std::string name = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (name == "master_volume")
        {
            g_masterVolume = std::clamp(atoi(value.c_str()), 0, 10);
        }
        else if (name == "mute_in_background")
        {
            g_muteInBackground = atoi(value.c_str()) != 0;
        }
        else if (name == "pause_in_background")
        {
            g_pauseInBackground = atoi(value.c_str()) != 0;
        }
        else if (name == "prompts")
        {
            static const char* const Prompts[] = {"automatic", "keyboard", "playstation", "xbox", "switch"};
            for (int i = 0; i < 5; i++)
            {
                if (value == Prompts[i])
                {
                    g_promptChoice = i;
                }
            }
        }
        else if (name == "pad_back")
        {
            g_backIsSelect = value == "select";
        }
        else if (name.find(": ") != std::string::npos)
        {
            g_savedGame[name] = value;
        }
    }
}

// The sound's gain: the master volume (a square law: the ear's), none while muted in the background
bool g_focused = true;
void ApplyVolume()
{
    float level = static_cast<float>(g_masterVolume) / 10.0f;
    SoundSetGain(!g_focused && g_muteInBackground && !g_options.headless ? 0.0f : level * level);
}

// The game's settings' choices made in the menus but not yet set on the game's side (shown meanwhile), by the setting's index
std::mutex g_pendingLock;
std::map<int, int> g_pending;

int GameChoice(int index)
{
    {
        std::lock_guard<std::mutex> lock(g_pendingLock);
        auto found = g_pending.find(index);
        if (found != g_pending.end())
        {
            return found->second;
        }
    }
    const Ps2GameSetting& setting = g_game->settings[index];
    return setting.get != nullptr ? setting.get() : 0;
}

void SetGameChoice(int index, int choice)
{
    const Ps2GameSetting& setting = g_game->settings[index];
    {
        std::lock_guard<std::mutex> lock(g_pendingLock);
        g_pending[index] = choice;
    }
    int count = ChoiceCount(setting);
    if (choice >= 0 && choice < count)
    {
        g_savedGame[GameKey(setting)] = setting.choices[choice];
        SaveOptions();
    }
    KernelOnGameSide([index, choice] {
        const Ps2GameSetting& s = g_game->settings[index];
        if (s.set != nullptr)
        {
            s.set(choice);
        }
        std::lock_guard<std::mutex> lock(g_pendingLock);
        auto found = g_pending.find(index);
        if (found != g_pending.end() && found->second == choice)
        {
            g_pending.erase(found);
        }
    });
}

// --------------------------------------------------------------------------------------------------------------- the rows
Row Choice(const std::string& label, const std::string& description, std::function<int()> get, std::function<void(int)> set,
           std::vector<std::string> texts)
{
    Row row;
    row.kind = RowChoice;
    row.label = label;
    row.description = description;
    row.get = std::move(get);
    row.set = std::move(set);
    row.minimum = 0;
    row.maximum = static_cast<int>(texts.size()) - 1;
    row.valueText = [texts](int value) {
        return value >= 0 && value < static_cast<int>(texts.size()) ? texts[static_cast<size_t>(value)] : std::string();
    };
    return row;
}

Row Toggle(const std::string& label, const std::string& description, bool* value, std::function<void()> changed = nullptr)
{
    return Choice(
        label, description, [value] { return *value ? 1 : 0; },
        [value, changed](int v) {
            *value = v != 0;
            if (changed)
            {
                changed();
            }
            SaveOptions();
        },
        {"Off", "On"});
}

Row DisplayChoice(const std::string& label, const std::string& description, DisplaySetting setting, std::vector<std::string> texts)
{
    return Choice(
        label, description, [setting] { return DisplayGet(setting); }, [setting](int v) { DisplaySet(setting, v); },
        std::move(texts));
}

Row Action(const std::string& label, const std::string& description, std::function<void()> activate)
{
    Row row;
    row.kind = RowAction;
    row.label = label;
    row.description = description;
    row.activate = std::move(activate);
    return row;
}

Row Heading(const std::string& label)
{
    Row row;
    row.kind = RowHeading;
    row.label = label;
    return row;
}

bool Shown(const Row& row) { return !row.shown || row.shown(); }

bool Selectable(const Row& row) { return row.kind != RowHeader && row.kind != RowHeading && Shown(row); }

bool Enabled(const Row& row) { return !row.enabled || row.enabled(); }

bool SameName(const char* a, const std::string& b)
{
    std::string x = a != nullptr ? a : "";
    if (x.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < x.size(); i++)
    {
        if (std::tolower(static_cast<unsigned char>(x[i])) != std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------------------------------------------------------- the state
std::atomic<bool> g_open{false};
Screen g_screen = Screen::None;
std::vector<Page> g_pages;
int g_page = 0;
int g_row = 0;
int g_column = 0;
int g_scroll = 0;
unsigned g_pageChanges = 0;
bool g_capturing = false;
int g_flashRows[2] = {-1, -1};
Clock::time_point g_flashStart;
bool g_quitting = false;
// The pause menu: its items (Resume, Options, Extras, the game's pause actions, Quit) and what each does
std::vector<PauseItem> g_pauseItems;
std::vector<std::function<void()>> g_pauseDo;
int g_pauseSelected = 0;
bool g_confirmingQuit = false;
int g_boxChoice = 0;
// A display change waiting to be kept (the countdown: the display mode and the window's size)
bool g_keeping = false;
int g_keepFullscreen = 0;
int g_keepWindow = 0;
Clock::time_point g_keepStart;
constexpr float KeepSeconds = 15.0f;

// Input since the last frame
enum UiAction : u32
{
    UiUp,
    UiDown,
    UiLeft,
    UiRight,
    UiConfirm,
    UiBack,
    UiTabPrevious,
    UiTabNext,
    UiClear,
};
u32 g_actions = 0;
int g_capturedKey = -1;
// The mouse (reference pixels)
f32 g_mouseX = -1.0f;
f32 g_mouseY = -1.0f;
bool g_mouseMoved = false;
bool g_mouseDown = false;
bool g_mouseClicked = false;
bool g_mouseRightClicked = false;
int g_wheel = 0;
bool g_dragging = false;
// The stick's and a held D-pad's repeat (400 ms, then every 80 ms)
Clock::time_point g_repeatAt;
int g_heldDirection = -1;
// The hit regions the view recorded last frame
std::vector<HitRegion> g_regions;

// The script (--ui): its keys, the next one's time (the steady clock's), the pictures' count, the cursor it put somewhere
std::vector<std::string> g_script;
size_t g_scriptNext = 0;
size_t g_scriptStarted = 0;
Clock::time_point g_scriptAt;
int g_shots = 0;
std::string g_shotDue;
bool g_scriptedCursor = false;

void Act(UiAction action) { g_actions |= 1u << action; }

bool Took(UiAction action) { return (g_actions & (1u << action)) != 0; }

std::vector<Row>& Rows() { return g_pages[static_cast<size_t>(g_page)].rows; }

std::vector<int> VisibleIndexes()
{
    std::vector<int> indexes;
    if (g_pages.empty())
    {
        return indexes;
    }
    std::vector<Row>& rows = Rows();
    for (int i = 0; i < static_cast<int>(rows.size()); i++)
    {
        if (Shown(rows[static_cast<size_t>(i)]))
        {
            indexes.push_back(i);
        }
    }
    return indexes;
}

void MoveRow(int by)
{
    std::vector<int> visible = VisibleIndexes();
    if (visible.empty())
    {
        return;
    }
    int at = 0;
    for (int i = 0; i < static_cast<int>(visible.size()); i++)
    {
        if (visible[static_cast<size_t>(i)] == g_row)
        {
            at = i;
        }
    }
    // Wrapping round at the ends
    for (int tries = 0; tries < static_cast<int>(visible.size()); tries++)
    {
        at = (at + by + static_cast<int>(visible.size())) % static_cast<int>(visible.size());
        if (Selectable(Rows()[static_cast<size_t>(visible[static_cast<size_t>(at)])]))
        {
            g_row = visible[static_cast<size_t>(at)];
            return;
        }
    }
}

void SelectPage(int page)
{
    if (page != g_page)
    {
        g_pageChanges++;
    }
    g_page = page;
    g_row = 0;
    g_scroll = 0;
    g_column = 0;
    g_capturing = false;
    if (!Rows().empty() && !Selectable(Rows()[0]))
    {
        MoveRow(1);
    }
}

// ------------------------------------------------------------------------------------------------------------- the pages
void Close();
void OpenPause();

void ResetPage(const std::string& title)
{
    if (title == "Gameplay")
    {
        g_pauseInBackground = true;
    }
    else if (title == "Display")
    {
        DisplaySet(DisplaySetting::Scale, 3);
        DisplaySet(DisplaySetting::TextureUpscale, 0);
        DisplaySet(DisplaySetting::Filtering, 0);
        DisplaySet(DisplaySetting::AntiAliasing, 0);
        DisplaySet(DisplaySetting::Crt, 0);
        DisplaySet(DisplaySetting::VSync, 1);
    }
    else if (title == "Sound")
    {
        g_masterVolume = 10;
        g_muteInBackground = true;
        ApplyVolume();
    }
    SaveOptions();
}

// The game's settings and actions on a tab, added to a page (its actions' groups under headings)
void AddGameRows(Page& page, const std::string& tab)
{
    if (g_game == nullptr)
    {
        return;
    }
    for (int i = 0; i < g_game->settingCount; i++)
    {
        const Ps2GameSetting& setting = g_game->settings[i];
        if (!SameName(setting.tab, tab))
        {
            continue;
        }
        std::vector<std::string> texts;
        for (int c = 0; c < ChoiceCount(setting); c++)
        {
            texts.push_back(setting.choices[c]);
        }
        page.rows.push_back(Choice(
            setting.name, "", [i] { return GameChoice(i); }, [i](int v) { SetGameChoice(i, v); }, texts));
    }
    const char* group = nullptr;
    for (int i = 0; i < g_game->actionCount; i++)
    {
        const Ps2GameAction& action = g_game->actions[i];
        if (!SameName(action.tab, tab))
        {
            continue;
        }
        if (action.group != nullptr && (group == nullptr || std::string(group) != action.group))
        {
            page.rows.push_back(Heading(action.group));
        }
        group = action.group;
        Row row = Action(action.name, "", [i] {
            // The menus closed, then the action done on the game's side before it goes on
            void (*run)(void) = g_game->actions[i].run;
            if (run != nullptr)
            {
                KernelOnGameSide(run);
            }
            Close();
        });
        if (action.available != nullptr)
        {
            int (*available)(void) = action.available;
            row.enabled = [available] { return available() != 0; };
        }
        page.rows.push_back(row);
    }
}

// The game's tabs (its settings' and actions' in their order), less those named
std::vector<std::string> GameTabs(const std::vector<std::string>& except)
{
    std::vector<std::string> tabs;
    auto add = [&](const char* tab) {
        if (tab == nullptr)
        {
            return;
        }
        for (const std::string& name : except)
        {
            if (SameName(tab, name))
            {
                return;
            }
        }
        for (const std::string& name : tabs)
        {
            if (SameName(tab, name))
            {
                return;
            }
        }
        tabs.push_back(tab);
    };
    if (g_game != nullptr)
    {
        for (int i = 0; i < g_game->settingCount; i++)
        {
            add(g_game->settings[i].tab);
        }
        for (int i = 0; i < g_game->actionCount; i++)
        {
            add(g_game->actions[i].tab);
        }
    }
    return tabs;
}

std::string Times(int n) { return std::to_string(n) + "×"; }

void BuildOptions()
{
    g_pages.clear();
    Page gameplay{"Gameplay", {}};
    gameplay.rows.push_back(Toggle("Pause in the background",
                                   "The game pauses (this menu opens) when its window isn't the one in front", &g_pauseInBackground));
    AddGameRows(gameplay, "Gameplay");
    gameplay.rows.push_back(Action("Reset to defaults", "The PC settings on this tab as they were at first", nullptr));
    g_pages.push_back(gameplay);

    Page display{"Display", {}};
    Row mode = DisplayChoice("Display mode", "A window, or the whole screen (Cmd+F or Alt+Enter switch)", DisplaySetting::Fullscreen,
                             {"Windowed", "Fullscreen"});
    mode.asksToKeep = true;
    display.rows.push_back(mode);
    Row size = Choice(
        "Window size", "The window's size (the picture keeps the TV's shape in it)",
        [] { return DisplayGet(DisplaySetting::WindowSize); }, [](int v) { DisplaySet(DisplaySetting::WindowSize, v); }, {});
    size.maximum = DisplayWindowSizeCount() - 1;
    size.valueText = [](int v) { return DisplayWindowSizeText(v); };
    size.shown = [] { return DisplayGet(DisplaySetting::Fullscreen) == 0; };
    size.asksToKeep = true;
    display.rows.push_back(size);
    auto onGpu = [] { return DisplayGet(DisplaySetting::OnGpu) != 0; };
    Row scale = Choice(
        "Internal resolution", "The picture drawn at a multiple of the PS2's resolution: sharper, and slower, the higher it is",
        [] { return DisplayGet(DisplaySetting::Scale) - 1; }, [](int v) { DisplaySet(DisplaySetting::Scale, v + 1); }, {});
    scale.maximum = 7;
    scale.valueText = [](int v) { return Times(v + 1) + (v + 1 == 3 ? "  ·  default" : ""); };
    scale.shown = onGpu;
    display.rows.push_back(scale);
    Row filtering = DisplayChoice("Texture filtering", "How the game's textures are smoothed: as the PS2 did, or more",
                                  DisplaySetting::Filtering, {"The game's own", "Bilinear", "Trilinear"});
    filtering.shown = onGpu;
    display.rows.push_back(filtering);
    Row upscale = DisplayChoice("Texture upscale", "The game's textures made larger and smoother",
                                DisplaySetting::TextureUpscale, {"Off", Times(2), Times(4)});
    upscale.shown = onGpu;
    display.rows.push_back(upscale);
    display.rows.push_back(DisplayChoice("Anti-aliasing",
                                         "Smoother edges: FXAA on the picture, or supersampling (the resolution higher still; slower)",
                                         DisplaySetting::AntiAliasing,
                                         {"Off", "FXAA", "Supersampling " + Times(2), "Supersampling " + Times(4)}));
    display.rows.push_back(DisplayChoice("CRT filter", "Scanlines, as a television showed the picture", DisplaySetting::Crt,
                                         {"Off", "On"}));
    display.rows.push_back(DisplayChoice("V-sync", "The picture shown in step with the screen (no tearing)", DisplaySetting::VSync,
                                         {"Off", "On"}));
    AddGameRows(display, "Display");
    display.rows.push_back(Action("Reset to defaults", "The PC settings on this tab as they were at first", nullptr));
    g_pages.push_back(display);

    Page sound{"Sound", {}};
    Row volume;
    volume.kind = RowSlider;
    volume.label = "Master volume";
    volume.description = "Everything the game plays, louder or quieter";
    volume.get = [] { return g_masterVolume; };
    volume.set = [](int v) {
        g_masterVolume = v;
        ApplyVolume();
        SaveOptions();
    };
    volume.minimum = 0;
    volume.maximum = 10;
    sound.rows.push_back(volume);
    sound.rows.push_back(Toggle("Mute in the background", "No sound while the game's window isn't the one in front",
                                &g_muteInBackground, ApplyVolume));
    AddGameRows(sound, "Sound");
    sound.rows.push_back(Action("Reset to defaults", "The PC settings on this tab as they were at first", nullptr));
    g_pages.push_back(sound);

    Page controls{"Controls", {}};
    controls.rows.push_back(Choice(
        "Button prompts", "The controls the menus show: the device used last, or always one",
        [] { return g_promptChoice; },
        [](int v) {
            g_promptChoice = v;
            SaveOptions();
        },
        {"Automatic", "Keyboard", "PlayStation", "Xbox", "Nintendo Switch"}));
    controls.rows.push_back(Choice(
        "Controller's Back button", "A gamepad's Back (Select, View, Share) opens this menu, or is the PS2's Select",
        [] { return g_backIsSelect ? 1 : 0; },
        [](int v) {
            g_backIsSelect = v != 0;
            SaveOptions();
        },
        {"PC menu", "Select"}));
    AddGameRows(controls, "Controls");
    Row header;
    header.kind = RowHeader;
    controls.rows.push_back(header);
    for (int a = 0; a < InputActionCount(); a++)
    {
        Row row;
        row.kind = RowBinding;
        row.label = InputActionLabel(a);
        row.description = "Choose a key's cell and press Return, then the key or mouse button for it";
        row.action = a;
        controls.rows.push_back(row);
    }
    controls.rows.push_back(Action("Reset the keyboard controls", "Every key and mouse button back to the defaults", InputResetKeys));
    g_pages.push_back(controls);

    // The game's other tabs
    for (const std::string& tab : GameTabs({"Gameplay", "Display", "Sound", "Controls", "Extras", "Pause"}))
    {
        Page page{tab, {}};
        AddGameRows(page, tab);
        g_pages.push_back(page);
    }
}

void BuildExtras()
{
    g_pages.clear();
    Page extras{"Extras", {}};
    AddGameRows(extras, "Extras");
    g_pages.push_back(extras);
}

bool HasExtras()
{
    if (g_game == nullptr)
    {
        return false;
    }
    for (int i = 0; i < g_game->settingCount; i++)
    {
        if (SameName(g_game->settings[i].tab, "Extras"))
        {
            return true;
        }
    }
    for (int i = 0; i < g_game->actionCount; i++)
    {
        if (SameName(g_game->actions[i].tab, "Extras"))
        {
            return true;
        }
    }
    return false;
}

void OpenPages(Screen screen)
{
    if (screen == Screen::Options)
    {
        BuildOptions();
    }
    else
    {
        BuildExtras();
    }
    g_screen = screen;
    g_page = 0;
    g_pageChanges++;
    g_row = 0;
    g_column = 0;
    g_scroll = 0;
    g_capturing = false;
    g_regions.clear();
    if (!Rows().empty() && !Selectable(Rows()[0]))
    {
        MoveRow(1);
    }
}

// --------------------------------------------------------------------------------------------------------- the pause menu
void BuildPause()
{
    g_pauseItems.clear();
    g_pauseDo.clear();
    g_pauseItems.push_back({"Resume", true});
    g_pauseDo.push_back(Close);
    g_pauseItems.push_back({"Options", true});
    g_pauseDo.push_back([] { OpenPages(Screen::Options); });
    if (HasExtras())
    {
        g_pauseItems.push_back({"Extras", true});
        g_pauseDo.push_back([] { OpenPages(Screen::Extras); });
    }
    // The game's own pause actions ("Quit to title"...)
    if (g_game != nullptr)
    {
        for (int i = 0; i < g_game->actionCount; i++)
        {
            const Ps2GameAction& action = g_game->actions[i];
            if (!SameName(action.tab, "Pause"))
            {
                continue;
            }
            g_pauseItems.push_back({action.name, action.available == nullptr || action.available() != 0});
            g_pauseDo.push_back([i] {
                const Ps2GameAction& a = g_game->actions[i];
                if (a.available != nullptr && a.available() == 0)
                {
                    return;
                }
                if (a.run != nullptr)
                {
                    KernelOnGameSide(a.run);
                }
                Close();
            });
        }
    }
    g_pauseItems.push_back({"Quit", true});
    g_pauseDo.push_back([] {
        g_confirmingQuit = true;
        g_boxChoice = 0;
    });
}

void OpenPause()
{
    bool wasOpen = g_open;
    BuildPause();
    g_screen = Screen::Pause;
    g_regions.clear();
    g_confirmingQuit = false;
    if (!wasOpen)
    {
        g_pauseSelected = 0;
        g_open = true;
        KernelPause(true);
        SoundPause(true);
        Log("PC menus: the game paused");
    }
}

void EndKeeping(bool keep)
{
    if (!keep)
    {
        DisplaySet(DisplaySetting::Fullscreen, g_keepFullscreen);
        if (g_keepFullscreen == 0 && g_keepWindow >= 0)
        {
            DisplaySet(DisplaySetting::WindowSize, g_keepWindow);
        }
    }
    g_keeping = false;
}

void Close()
{
    if (g_keeping)
    {
        EndKeeping(false);
    }
    g_screen = Screen::None;
    g_capturing = false;
    g_confirmingQuit = false;
    g_pages.clear();
    if (g_open)
    {
        g_open = false;
        InputIgnoreHeld();
        KernelPause(false);
        SoundPause(false);
        Log("PC menus: the game goes on");
    }
}

// ------------------------------------------------------------------------------------------------------------ the frame's input
void Step(Row& row, int by)
{
    if ((row.kind != RowChoice && row.kind != RowSlider) || !Enabled(row))
    {
        return;
    }
    int value = row.get();
    int next = value + by;
    if (value < row.minimum || value > row.maximum)
    {
        // A value of its own (a window's size of its own): the first or last
        next = by > 0 ? row.minimum : row.maximum;
    }
    else if (row.kind == RowChoice)
    {
        // Choices wrap round
        next = next > row.maximum ? row.minimum : next < row.minimum ? row.maximum : next;
    }
    else
    {
        next = std::clamp(next, row.minimum, row.maximum);
    }
    if (next != value)
    {
        int fullscreen = DisplayGet(DisplaySetting::Fullscreen);
        int window = DisplayGet(DisplaySetting::WindowSize);
        row.set(next);
        if (row.asksToKeep && !g_options.headless)
        {
            g_keeping = true;
            g_keepFullscreen = fullscreen;
            g_keepWindow = window;
            g_keepStart = Clock::now();
        }
    }
}

void Activate(Row& row)
{
    if (!Enabled(row))
    {
        return;
    }
    switch (row.kind)
    {
    case RowChoice:
        Step(row, 1);
        break;
    case RowAction:
        if (row.activate)
        {
            row.activate();
        }
        else
        {
            ResetPage(g_pages[static_cast<size_t>(g_page)].title);
        }
        break;
    case RowBinding:
        if (g_column != ColumnPad)
        {
            g_capturing = true;
            g_capturedKey = -1;
        }
        break;
    default:
        break;
    }
}

void TakeCapture()
{
    if (g_capturedKey == SDL_SCANCODE_ESCAPE)
    {
        g_capturing = false;
        return;
    }
    Row& row = Rows()[static_cast<size_t>(g_row)];
    int swapped = InputBindKey(row.action, g_column, g_capturedKey);
    g_capturing = false;
    g_flashRows[0] = -1;
    g_flashRows[1] = -1;
    if (swapped >= 0)
    {
        g_flashRows[0] = g_row;
        for (int i = 0; i < static_cast<int>(Rows().size()); i++)
        {
            if (Rows()[static_cast<size_t>(i)].kind == RowBinding && Rows()[static_cast<size_t>(i)].action == swapped)
            {
                g_flashRows[1] = i;
            }
        }
        g_flashStart = Clock::now();
    }
}

struct Hit
{
    int row = -1;
    int column = -1;
    int tab = -1;
    // -1 the left arrow, 1 the right arrow, 2 the slider's bar
    int part = 0;
    const HitRegion* region = nullptr;
};

// The mouse against the regions the view recorded (the last one drawn over the others wins)
Hit HitTest()
{
    Hit hit;
    for (const HitRegion& region : g_regions)
    {
        if (!region.Contains(g_mouseX, g_mouseY))
        {
            continue;
        }
        switch (region.kind)
        {
        case HitKind::Tab: hit.tab = region.tab; break;
        case HitKind::Row:
        case HitKind::PauseItem: hit.row = region.row; break;
        case HitKind::Cell:
            hit.row = region.row;
            hit.column = region.column;
            break;
        case HitKind::ArrowLeft:
            hit.row = region.row;
            hit.part = -1;
            break;
        case HitKind::ArrowRight:
            hit.row = region.row;
            hit.part = 1;
            break;
        case HitKind::Slider:
            hit.row = region.row;
            hit.part = 2;
            break;
        default: break;
        }
        hit.region = &region;
    }
    return hit;
}

const HitRegion* SliderRegion(int row)
{
    for (const HitRegion& region : g_regions)
    {
        if (region.kind == HitKind::Slider && region.row == row)
        {
            return &region;
        }
    }
    return nullptr;
}

void ClearMouse()
{
    g_mouseMoved = false;
    g_mouseClicked = false;
    g_mouseRightClicked = false;
    g_wheel = 0;
}

// The stick and a held D-pad step as the keyboard's repeat does
void PollHeldDirections()
{
    SDL_Gamepad* gamepad = InputGamepad();
    if (gamepad == nullptr)
    {
        g_heldDirection = -1;
        return;
    }
    s16 x = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    s16 y = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
    constexpr s16 Pushed = 20000;
    bool up = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP);
    bool down = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    bool left = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    bool right = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    int direction = y < -Pushed || up ? UiUp : y > Pushed || down ? UiDown : x < -Pushed || left ? UiLeft : x > Pushed || right ? UiRight : -1;
    Clock::time_point now = Clock::now();
    if (direction < 0)
    {
        g_heldDirection = -1;
        return;
    }
    if (direction != g_heldDirection)
    {
        // The D-pad's first press is its event's; the stick's first push is here
        if (!(up || down || left || right))
        {
            Act(static_cast<UiAction>(direction));
        }
        g_heldDirection = direction;
        g_repeatAt = now + std::chrono::milliseconds(400);
        return;
    }
    if (now >= g_repeatAt)
    {
        Act(static_cast<UiAction>(direction));
        g_repeatAt = now + std::chrono::milliseconds(80);
    }
}

// The box asking to quit: yes, no, its buttons by the arrows and the mouse
void TakeQuitInput()
{
    Hit hit = HitTest();
    if (g_mouseMoved && hit.region != nullptr && (hit.region->kind == HitKind::BoxYes || hit.region->kind == HitKind::BoxNo))
    {
        g_boxChoice = hit.region->kind == HitKind::BoxYes ? 0 : 1;
    }
    bool yes = g_mouseClicked && hit.region != nullptr && hit.region->kind == HitKind::BoxYes;
    bool no = g_mouseClicked && hit.region != nullptr && hit.region->kind == HitKind::BoxNo;
    if (Took(UiLeft) || Took(UiUp))
    {
        g_boxChoice = 0;
    }
    else if (Took(UiRight) || Took(UiDown))
    {
        g_boxChoice = 1;
    }
    if (yes || (Took(UiConfirm) && g_boxChoice == 0))
    {
        g_quitting = true;
        Log("PC menus: quit");
    }
    else if (no || Took(UiBack) || g_mouseRightClicked || (Took(UiConfirm) && g_boxChoice == 1))
    {
        g_confirmingQuit = false;
    }
}

void TakePauseInput()
{
    if (g_confirmingQuit)
    {
        TakeQuitInput();
        return;
    }
    int count = static_cast<int>(g_pauseItems.size());
    Hit hit = HitTest();
    if (g_mouseMoved && hit.region != nullptr && hit.region->kind == HitKind::PauseItem)
    {
        g_pauseSelected = hit.row;
    }
    if (Took(UiUp))
    {
        g_pauseSelected = (g_pauseSelected + count - 1) % count;
    }
    else if (Took(UiDown))
    {
        g_pauseSelected = (g_pauseSelected + 1) % count;
    }
    bool clicked = g_mouseClicked && hit.region != nullptr && hit.region->kind == HitKind::PauseItem;
    if (clicked)
    {
        g_pauseSelected = hit.row;
    }
    if ((Took(UiConfirm) || clicked) && g_pauseItems[static_cast<size_t>(g_pauseSelected)].enabled)
    {
        g_pauseDo[static_cast<size_t>(g_pauseSelected)]();
    }
    else if (Took(UiBack) || g_mouseRightClicked ||
             (g_mouseClicked && hit.region != nullptr && hit.region->kind == HitKind::FooterBack))
    {
        Close();
    }
    else if (g_mouseClicked && hit.region != nullptr && hit.region->kind == HitKind::FooterSelect &&
             g_pauseItems[static_cast<size_t>(g_pauseSelected)].enabled)
    {
        g_pauseDo[static_cast<size_t>(g_pauseSelected)]();
    }
}

void TakePagesInput()
{
    if (g_capturing)
    {
        if (g_capturedKey >= 0)
        {
            TakeCapture();
            g_capturedKey = -1;
        }
        return;
    }
    // The display's countdown: keep (select), go back (back, or the time's up)
    if (g_keeping)
    {
        float waited = std::chrono::duration<float>(Clock::now() - g_keepStart).count();
        Hit hit = HitTest();
        bool yes = g_mouseClicked && hit.region != nullptr && hit.region->kind == HitKind::BoxYes;
        bool no = g_mouseClicked && hit.region != nullptr && hit.region->kind == HitKind::BoxNo;
        if (Took(UiConfirm) || yes)
        {
            EndKeeping(true);
        }
        else if (Took(UiBack) || no || g_mouseRightClicked || waited >= KeepSeconds)
        {
            EndKeeping(false);
        }
        return;
    }
    std::vector<Row>& rows = Rows();
    // The mouse: hovering selects, a click picks (or steps a choice by its arrow, sets a slider, picks a tab), the wheel steps
    // the hovered value or scrolls, a right click goes back
    if (g_mouseMoved || g_mouseClicked || g_wheel != 0 || g_mouseDown || g_mouseRightClicked)
    {
        Hit hit = HitTest();
        if (g_mouseMoved && hit.row >= 0 && Selectable(rows[static_cast<size_t>(hit.row)]))
        {
            g_row = hit.row;
            if (hit.column >= 0)
            {
                g_column = hit.column;
            }
        }
        if (g_mouseClicked)
        {
            if (hit.tab >= 0)
            {
                SelectPage(hit.tab);
                return;
            }
            if (hit.row >= 0 && Selectable(rows[static_cast<size_t>(hit.row)]))
            {
                Row& row = rows[static_cast<size_t>(hit.row)];
                g_row = hit.row;
                if (row.kind == RowChoice && hit.part != 0)
                {
                    Step(row, hit.part);
                }
                else if (row.kind == RowSlider && hit.part == 2)
                {
                    g_dragging = true;
                }
                else if (row.kind == RowBinding)
                {
                    if (hit.column >= 0)
                    {
                        g_column = hit.column;
                        Activate(row);
                    }
                }
                else if (row.kind != RowSlider)
                {
                    Activate(row);
                }
            }
            else if (hit.region != nullptr)
            {
                switch (hit.region->kind)
                {
                case HitKind::FooterSelect: Act(UiConfirm); break;
                case HitKind::FooterBack: Act(UiBack); break;
                case HitKind::FooterTabs: Act(UiTabNext); break;
                case HitKind::FooterClear: Act(UiClear); break;
                default: break;
                }
            }
        }
        if (g_dragging && g_mouseDown && g_row >= 0 && g_row < static_cast<int>(rows.size()))
        {
            Row& row = rows[static_cast<size_t>(g_row)];
            const HitRegion* slider = SliderRegion(g_row);
            if (row.kind == RowSlider && slider != nullptr)
            {
                float fraction = std::clamp((g_mouseX - slider->x) / slider->width, 0.0f, 1.0f);
                int value = row.minimum + static_cast<int>(fraction * static_cast<float>(row.maximum - row.minimum) + 0.5f);
                if (value != row.get())
                {
                    row.set(value);
                }
            }
        }
        if (!g_mouseDown)
        {
            g_dragging = false;
        }
        if (g_wheel != 0)
        {
            if (hit.row >= 0 &&
                (rows[static_cast<size_t>(hit.row)].kind == RowChoice || rows[static_cast<size_t>(hit.row)].kind == RowSlider))
            {
                g_row = hit.row;
                Step(rows[static_cast<size_t>(hit.row)], g_wheel > 0 ? 1 : -1);
            }
            else
            {
                MoveRow(g_wheel > 0 ? -1 : 1);
            }
        }
        if (g_mouseRightClicked)
        {
            Act(UiBack);
        }
    }
    int pages = static_cast<int>(g_pages.size());
    if (Took(UiTabPrevious))
    {
        SelectPage((g_page + pages - 1) % pages);
    }
    else if (Took(UiTabNext))
    {
        SelectPage((g_page + 1) % pages);
    }
    if (Took(UiUp))
    {
        MoveRow(-1);
    }
    else if (Took(UiDown))
    {
        MoveRow(1);
    }
    if (Rows().empty())
    {
        if (Took(UiBack))
        {
            OpenPause();
        }
        return;
    }
    Row& row = Rows()[static_cast<size_t>(std::clamp(g_row, 0, static_cast<int>(Rows().size()) - 1))];
    if (row.kind == RowBinding)
    {
        if (Took(UiLeft) && g_column > 0)
        {
            g_column--;
        }
        else if (Took(UiRight) && g_column + 1 < BindingColumns)
        {
            g_column++;
        }
        else if (Took(UiClear) && g_column != ColumnPad)
        {
            InputClearKey(row.action, g_column);
        }
    }
    else if (Took(UiLeft))
    {
        Step(row, -1);
    }
    else if (Took(UiRight))
    {
        Step(row, 1);
    }
    if (Took(UiConfirm))
    {
        Activate(row);
    }
    else if (Took(UiBack))
    {
        OpenPause();
    }
}

void TakeInput()
{
    if (g_screen == Screen::None)
    {
        g_actions = 0;
        ClearMouse();
        return;
    }
    PollHeldDirections();
    if (g_screen == Screen::Pause)
    {
        TakePauseInput();
    }
    else
    {
        TakePagesInput();
    }
    g_actions = 0;
    ClearMouse();
}

// --------------------------------------------------------------------------------------------------------------- the script
// A key of the script's as the keyboard's would be
void ScriptKey(SDL_Scancode code, SDL_Keymod mod = SDL_KMOD_NONE)
{
    SDL_Event e = {};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.scancode = code;
    e.key.mod = mod;
    MenusEvent(e);
}

void ScriptStep(const std::string& token)
{
    static const std::pair<const char*, SDL_Scancode> Keys[] = {
        {"esc", SDL_SCANCODE_ESCAPE}, {"enter", SDL_SCANCODE_RETURN}, {"space", SDL_SCANCODE_SPACE},
        {"back", SDL_SCANCODE_BACKSPACE}, {"delete", SDL_SCANCODE_DELETE}, {"up", SDL_SCANCODE_UP},
        {"down", SDL_SCANCODE_DOWN}, {"left", SDL_SCANCODE_LEFT}, {"right", SDL_SCANCODE_RIGHT},
        {"tab", SDL_SCANCODE_TAB}, {"q", SDL_SCANCODE_Q}, {"e", SDL_SCANCODE_E}, {"k", SDL_SCANCODE_K},
        {"f1", SDL_SCANCODE_F1},
    };
    for (const auto& [name, code] : Keys)
    {
        if (token == name)
        {
            ScriptKey(code);
            return;
        }
    }
    if (token == "prevtab")
    {
        ScriptKey(SDL_SCANCODE_TAB, SDL_KMOD_LSHIFT);
    }
    else if (token == "shot")
    {
        if (g_options.snapshotDir.empty())
        {
            Log("--ui: shot without --snapshots: no folder for it");
            return;
        }
        char name[64];
        snprintf(name, sizeof(name), "/ui%02d.png", g_shots++);
        g_shotDue = g_options.snapshotDir + name;
    }
    else if (token.rfind("mouse:", 0) == 0 || token.rfind("click:", 0) == 0)
    {
        // mouse:640x360 (reference pixels): the pointer there, clicked with click:
        float x = 0, y = 0;
        if (sscanf(token.c_str() + 6, "%fx%f", &x, &y) == 2)
        {
            g_scriptedCursor = true;
            g_mouseX = x;
            g_mouseY = y;
            g_mouseMoved = true;
            if (token[0] == 'c')
            {
                g_mouseClicked = true;
            }
        }
    }
    else if (token == "wheelup" || token == "wheeldown")
    {
        g_wheel += token == "wheelup" ? 1 : -1;
    }
    else if (token == "pad-back")
    {
        SDL_Event e = {};
        e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        e.gbutton.button = SDL_GAMEPAD_BUTTON_BACK;
        MenusEvent(e);
    }
    else if (token != "wait")
    {
        Log("--ui: no key \"%s\"", token.c_str());
    }
}

void PlayScript(double gameSeconds)
{
    // Each item's keys from its time of the game's (the game stands still while the menus are up: then the clock's), a third
    // of a second apart
    while (g_scriptStarted < g_options.ui.size() && gameSeconds >= g_options.ui[g_scriptStarted].first)
    {
        bool idle = g_scriptNext == g_script.size();
        std::string keys = g_options.ui[g_scriptStarted++].second;
        size_t start = 0;
        while (start <= keys.size())
        {
            size_t comma = keys.find(',', start);
            std::string token = keys.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!token.empty())
            {
                g_script.push_back(token);
            }
            if (comma == std::string::npos)
            {
                break;
            }
            start = comma + 1;
        }
        if (idle)
        {
            g_scriptAt = Clock::now();
        }
    }
    if (g_scriptNext < g_script.size() && Clock::now() >= g_scriptAt)
    {
        ScriptStep(g_script[g_scriptNext++]);
        g_scriptAt = Clock::now() + std::chrono::milliseconds(333);
    }
}

PromptStyle PadStyleOf(SDL_Gamepad* gamepad)
{
    if (gamepad == nullptr)
    {
        return PromptStyle::PlayStation;
    }
    switch (SDL_GetGamepadType(gamepad))
    {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return PromptStyle::PlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return PromptStyle::Switch;
    default: return PromptStyle::Xbox;
    }
}

PromptStyle CurrentPrompts()
{
    switch (g_promptChoice)
    {
    case 1: return PromptStyle::Keyboard;
    case 2: return PromptStyle::PlayStation;
    case 3: return PromptStyle::Xbox;
    case 4: return PromptStyle::Switch;
    default: return InputLastKeyboard() ? PromptStyle::Keyboard : PadStyleOf(InputGamepad());
    }
}

PromptStyle CurrentPadStyle()
{
    if (g_promptChoice >= 2)
    {
        return CurrentPrompts();
    }
    return InputGamepad() != nullptr ? PadStyleOf(InputGamepad()) : PromptStyle::PlayStation;
}
}

// --------------------------------------------------------------------------------------------------------------- the interface
std::string GameTitle()
{
    std::string title = WindowTitle();
    for (char& c : title)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return title;
}

int& MenuScroll() { return g_scroll; }

void MenusInit()
{
    ReadOptions();
    ApplyVolume();
}

void MenusGameVblank()
{
    // The saved choices of the game's settings set at its first vertical blank
    static bool applied = false;
    if (applied || g_game == nullptr)
    {
        return;
    }
    applied = true;
    for (int i = 0; i < g_game->settingCount; i++)
    {
        const Ps2GameSetting& setting = g_game->settings[i];
        auto found = g_savedGame.find(GameKey(setting));
        if (found == g_savedGame.end() || setting.set == nullptr)
        {
            continue;
        }
        for (int c = 0; c < ChoiceCount(setting); c++)
        {
            if (found->second == setting.choices[c])
            {
                setting.set(c);
            }
        }
    }
}

bool MenusOpen() { return g_open; }

bool PadBackIsSelect() { return g_backIsSelect; }

bool MenusEvent(const SDL_Event& e)
{
    InputNoteDevice(e);
    switch (e.type)
    {
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        g_focused = false;
        ApplyVolume();
        if (g_pauseInBackground && !g_open && !g_options.headless)
        {
            OpenPause();
        }
        return false;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        g_focused = true;
        ApplyVolume();
        return false;
    case SDL_EVENT_KEY_DOWN:
    {
        // Alt+Enter, and Cmd+F: fullscreen or the window (what's held for it doesn't reach the game)
        if (!e.key.repeat && (((e.key.scancode == SDL_SCANCODE_RETURN || e.key.scancode == SDL_SCANCODE_KP_ENTER) &&
                               (e.key.mod & SDL_KMOD_ALT) != 0) ||
                              (e.key.scancode == SDL_SCANCODE_F && (e.key.mod & SDL_KMOD_GUI) != 0)))
        {
            DisplaySet(DisplaySetting::Fullscreen, DisplayGet(DisplaySetting::Fullscreen) != 0 ? 0 : 1);
            InputIgnoreHeld();
            return true;
        }
        if (!g_open)
        {
            if (!e.key.repeat && InputIsMenuKey(e.key.scancode))
            {
                OpenPause();
                return true;
            }
            return false;
        }
        if (g_capturing)
        {
            if (!e.key.repeat)
            {
                g_capturedKey = e.key.scancode;
            }
            return true;
        }
        switch (e.key.scancode)
        {
        case SDL_SCANCODE_UP:
        case SDL_SCANCODE_W: Act(UiUp); break;
        case SDL_SCANCODE_DOWN:
        case SDL_SCANCODE_S: Act(UiDown); break;
        case SDL_SCANCODE_LEFT:
        case SDL_SCANCODE_A: Act(UiLeft); break;
        case SDL_SCANCODE_RIGHT:
        case SDL_SCANCODE_D: Act(UiRight); break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_SPACE:
            if (!e.key.repeat)
            {
                Act(UiConfirm);
            }
            break;
        case SDL_SCANCODE_ESCAPE:
            if (!e.key.repeat)
            {
                Act(UiBack);
            }
            break;
        case SDL_SCANCODE_BACKSPACE:
        case SDL_SCANCODE_DELETE:
            // On the Controls tab it clears a key; elsewhere Backspace goes back
            if (!e.key.repeat)
            {
                bool controls = g_screen == Screen::Options && !g_pages.empty() &&
                                g_pages[static_cast<size_t>(g_page)].title == "Controls";
                Act(controls || e.key.scancode == SDL_SCANCODE_DELETE ? UiClear : UiBack);
            }
            break;
        case SDL_SCANCODE_Q:
        case SDL_SCANCODE_PAGEUP: Act(UiTabPrevious); break;
        case SDL_SCANCODE_E:
        case SDL_SCANCODE_PAGEDOWN: Act(UiTabNext); break;
        case SDL_SCANCODE_TAB: Act((e.key.mod & SDL_KMOD_SHIFT) != 0 ? UiTabPrevious : UiTabNext); break;
        default:
            // The menu key (when it's another) goes back as Escape does
            if (!e.key.repeat && InputIsMenuKey(e.key.scancode))
            {
                Act(UiBack);
            }
            break;
        }
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (!g_open)
        {
            if (InputIsMenuKey(InputMouseKey(e.button.button)))
            {
                OpenPause();
                return true;
            }
            return false;
        }
        WindowToReference(e.button.x, e.button.y, &g_mouseX, &g_mouseY);
        g_scriptedCursor = false;
        if (g_capturing)
        {
            // A mouse button binds a key cell
            g_capturedKey = InputMouseKey(e.button.button);
            return true;
        }
        if (e.button.button == SDL_BUTTON_LEFT)
        {
            g_mouseClicked = true;
            g_mouseDown = true;
        }
        else if (e.button.button == SDL_BUTTON_RIGHT)
        {
            g_mouseRightClicked = true;
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button == SDL_BUTTON_LEFT)
        {
            g_mouseDown = false;
        }
        return g_open;
    case SDL_EVENT_MOUSE_MOTION:
        if (!g_open)
        {
            return false;
        }
        WindowToReference(e.motion.x, e.motion.y, &g_mouseX, &g_mouseY);
        g_mouseMoved = true;
        g_scriptedCursor = false;
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (!g_open)
        {
            return false;
        }
        if (!g_capturing)
        {
            float y = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
            g_wheel += y > 0 ? 1 : y < 0 ? -1 : 0;
        }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (!g_open)
        {
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK && !g_backIsSelect)
            {
                OpenPause();
                return true;
            }
            return false;
        }
        if (g_capturing)
        {
            return true;
        }
        switch (e.gbutton.button)
        {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: Act(UiUp); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: Act(UiDown); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: Act(UiLeft); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: Act(UiRight); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: Act(UiConfirm); break;
        case SDL_GAMEPAD_BUTTON_EAST:
        case SDL_GAMEPAD_BUTTON_NORTH:
        case SDL_GAMEPAD_BUTTON_START:
        case SDL_GAMEPAD_BUTTON_BACK: Act(UiBack); break;
        case SDL_GAMEPAD_BUTTON_WEST: Act(UiClear); break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: Act(UiTabPrevious); break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: Act(UiTabNext); break;
        default: break;
        }
        g_heldDirection = -1;
        return true;
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return g_open;
    default:
        return false;
    }
}

void MenusFrame(double gameSeconds)
{
    if (!g_options.ui.empty())
    {
        PlayScript(gameSeconds);
    }
    TakeInput();
}

bool MenusVisible() { return g_screen != Screen::None; }

bool MenusQuitting() { return g_quitting; }

std::string MenusShotDue()
{
    std::string shot;
    shot.swap(g_shotDue);
    return shot;
}

void MenusDraw(int pixelWidth, int pixelHeight, float pixelScale, const SDL_FRect& picture)
{
    NoteWindow(pixelWidth, pixelHeight, pixelScale);
    std::string prompt = g_screen == Screen::None ? InputPrompt() : std::string();
    if (g_screen == Screen::None && prompt.empty())
    {
        return;
    }
    static OverlayFrame frame;
    frame.Clear();
    if (g_screen == Screen::None)
    {
        f32 left, top, right, bottom;
        PixelsToReference(picture.x, picture.y, &left, &top);
        PixelsToReference(picture.x + picture.w, picture.y + picture.h, &right, &bottom);
        DrawGamePrompt(frame, prompt, left, top, right - left, bottom - top);
    }
    else
    {
        MenuView view{};
        view.screen = g_screen;
        view.pages = &g_pages;
        view.page = g_page;
        view.row = g_row;
        view.column = g_column;
        view.capturing = g_capturing;
        view.keeping = g_keeping;
        float waited = std::chrono::duration<float>(Clock::now() - g_keepStart).count();
        view.keepSecondsLeft = g_keeping ? static_cast<int>(KeepSeconds - waited + 0.999f) : 0;
        float flash = std::chrono::duration<float>(Clock::now() - g_flashStart).count();
        view.flashRows[0] = g_flashRows[0];
        view.flashRows[1] = g_flashRows[1];
        view.flashOn = flash < 0.6f && (static_cast<int>(flash * 10.0f) & 1) == 0;
        view.pageChanges = g_pageChanges;
        view.pauseItems = g_pauseItems;
        view.pauseSelected = g_pauseSelected;
        view.confirmingQuit = g_confirmingQuit;
        view.boxChoice = g_boxChoice;
        view.prompts = CurrentPrompts();
        view.padStyle = CurrentPadStyle();
        g_regions.clear();
        DrawMenus(frame, view, g_regions);
        if (g_scriptedCursor)
        {
            bool over = false;
            for (const HitRegion& region : g_regions)
            {
                over = over || region.Contains(g_mouseX, g_mouseY);
            }
            DrawCursor(frame, g_mouseX, g_mouseY, over);
        }
    }
    DrawOverlay(frame, pixelWidth, pixelHeight, pixelScale);
}

std::string PadControlText(PadControl control, PromptStyle style)
{
    bool ps = style == PromptStyle::PlayStation, sw = style == PromptStyle::Switch;
    switch (control)
    {
    case PadControl::None: return "-";
    case PadControl::South:
    case PadControl::East:
    case PadControl::West:
    case PadControl::North: return "";
    case PadControl::LeftShoulder: return ps ? "L1" : sw ? "L" : "LB";
    case PadControl::RightShoulder: return ps ? "R1" : sw ? "R" : "RB";
    case PadControl::LeftTrigger: return ps ? "L2" : sw ? "ZL" : "LT";
    case PadControl::RightTrigger: return ps ? "R2" : sw ? "ZR" : "RT";
    case PadControl::Start: return ps ? "Start" : sw ? "Plus" : "Menu";
    case PadControl::BackAsSelect:
        return !g_backIsSelect ? "-" : ps ? "Select" : sw ? "Minus" : "View";
    case PadControl::BackAsMenu:
        return g_backIsSelect ? "-" : ps ? "Select" : sw ? "Minus" : "View";
    case PadControl::LeftStick: return ps ? "L3" : "LS";
    case PadControl::RightStick: return ps ? "R3" : "RS";
    case PadControl::DpadUp: return "D-pad up";
    case PadControl::DpadDown: return "D-pad down";
    case PadControl::DpadLeft: return "D-pad left";
    case PadControl::DpadRight: return "D-pad right";
    case PadControl::LeftStickUp: return "Left stick up";
    case PadControl::LeftStickDown: return "Left stick down";
    case PadControl::LeftStickLeft: return "Left stick left";
    case PadControl::LeftStickRight: return "Left stick right";
    case PadControl::RightStickUp: return "Right stick up";
    case PadControl::RightStickDown: return "Right stick down";
    case PadControl::RightStickLeft: return "Right stick left";
    case PadControl::RightStickRight: return "Right stick right";
    }
    return "-";
}
}
