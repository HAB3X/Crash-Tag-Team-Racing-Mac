// Start-up: the options, the disc, the executable SYSTEM.CNF names loaded into main memory as the BIOS's loader puts it, the
// hardware, and the main thread at the executable's entry point. The host's main thread runs the presenter.
#include "runtime.h"

#include "ps2/runtime.h"
#include "ui/menus.h"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

namespace Ps2
{
extern u32 g_entryPoint;
const Ps2GameInfo* g_game = nullptr;

std::string WindowTitle()
{
    std::string title = g_game != nullptr ? g_game->title : "PS2 native runtime";
    size_t bracket = title.find(" (");
    return bracket == std::string::npos ? title : title.substr(0, bracket);
}

namespace
{
void Usage(const char* program)
{
    fprintf(stderr,
            "usage: %s --disc IMAGE [--headless] [--snapshots DIR] [--snapshot-every S] [--snapshot-at S,S,...]\n"
            "          [--run-seconds S] [--log kernel,syscall,io,dma,sif,iop,gs,call,thread,irq,ipu]\n"
            "          [--watch ADDR[:WORDS],... [--watch-every S]] [--memcards DIR] [--settings DIR]\n"
            "          [--press S:BUTTON[+BUTTON][:HOLD],...] [--key S:KEY[+KEY][:HOLD],...]\n"
            "          [--control FIFO] [--patch FILE]\n"
            "          [--type S:TEXT] [--click S:X,Y] [--ui S:KEY,KEY,...]\n"
            "          [--fps] [--gs hardware|software] [--scale 1-8] [--texture-upscale 0|2|4] [--mute] [--audio-dump FILE.wav]\n"
            "          [--gs-record S:VBLANKS:FILE] [--gs-replay FILE[:PASSES]]\n",
            program);
}

// The disc image asked for with the system's file dialog (the first run of an app); empty if none was chosen
std::string AskForDisc(const Ps2GameInfo* game)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        return "";
    }
    std::string message = std::string("Choose your disc image of ") + game->title + " (" + game->executable +
                          "): an .iso, or a .cue/.bin.";
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, WindowTitle().c_str(), message.c_str(), nullptr);
    struct Pick
    {
        bool done = false;
        std::string path;
    } pick;
    static const SDL_DialogFileFilter filters[] = {{"PS2 disc image", "iso;cue;bin;img"}, {"Any file", "*"}};
    SDL_ShowOpenFileDialog(
        [](void* user, const char* const* files, int) {
            auto* result = static_cast<Pick*>(user);
            if (files != nullptr && files[0] != nullptr)
            {
                result->path = files[0];
            }
            result->done = true;
        },
        &pick, nullptr, filters, 2, nullptr, false);
    while (!pick.done)
    {
        SDL_Event event;
        SDL_WaitEventTimeout(&event, 50);
    }
    return pick.path;
}

// A disc that isn't the game's: said in a window when it was just chosen (and forgotten, to be asked for again)
void WrongDisc(const Ps2GameInfo* game, bool asked, const std::string& discFile)
{
    if (!asked)
    {
        return;
    }
    std::error_code ec;
    std::filesystem::remove(discFile, ec);
    std::string message = std::string("That isn't ") + game->title + " (" + game->executable +
                          "). Open the app again to choose your disc image.";
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, WindowTitle().c_str(), message.c_str(), nullptr);
}

u32 ParseLog(const std::string& list)
{
    static const struct
    {
        const char* name;
        u32 bit;
    } names[] = {{"kernel", LogKernel}, {"syscall", LogSyscall}, {"io", LogIo},   {"dma", LogDma},
                 {"sif", LogSif},       {"iop", LogIop},         {"gs", LogGs},   {"call", LogCall},
                 {"thread", LogThread}, {"irq", LogIrq},         {"ipu", LogIpu}, {"input", LogInput}, {"all", 0xFFFFFFFF}};
    u32 mask = 0;
    size_t start = 0;
    while (start <= list.size())
    {
        size_t comma = list.find(',', start);
        std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        for (const auto& n : names)
        {
            if (item == n.name)
            {
                mask |= n.bit;
            }
        }
        if (comma == std::string::npos)
        {
            break;
        }
        start = comma + 1;
    }
    return mask;
}

// The ELF's loadable segments into main memory; returns the entry point
u32 LoadElf(const std::vector<u8>& elf)
{
    if (elf.size() < 52 || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0)
    {
        Fatal("the executable isn't an ELF file");
    }
    u32 entry, phoff;
    u16 phnum;
    std::memcpy(&entry, &elf[24], 4);
    std::memcpy(&phoff, &elf[28], 4);
    std::memcpy(&phnum, &elf[44], 2);
    for (u32 i = 0; i < phnum; i++)
    {
        const u8* ph = &elf[phoff + i * 32];
        u32 type, offset, vaddr, filesz, memsz;
        std::memcpy(&type, ph, 4);
        std::memcpy(&offset, ph + 4, 4);
        std::memcpy(&vaddr, ph + 8, 4);
        std::memcpy(&filesz, ph + 16, 4);
        std::memcpy(&memsz, ph + 20, 4);
        if (type != 1)
        {
            continue;
        }
        std::memcpy(Host(vaddr), &elf[offset], filesz);
        std::memset(Host(vaddr + filesz), 0, memsz - filesz);
        Log("loaded %08x-%08x", vaddr, vaddr + memsz);
    }
    return entry;
}
}
}

using namespace Ps2;

int ps2_main(int argc, char** argv, const Ps2GameInfo* game)
{
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc)
            {
                Usage(argv[0]);
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--disc")
        {
            g_options.disc = next();
        }
        else if (a == "--patch")
        {
            g_options.patch = next();
        }
        else if (a == "--headless")
        {
            g_options.headless = true;
        }
        else if (a == "--snapshots")
        {
            g_options.snapshotDir = next();
        }
        else if (a == "--snapshot-every")
        {
            g_options.snapshotEvery = atof(next().c_str());
        }
        else if (a == "--snapshot-at")
        {
            std::string list = next();
            size_t start = 0;
            while (start < list.size())
            {
                size_t comma = list.find(',', start);
                g_options.snapshotAt.push_back(atof(list.substr(start, comma - start).c_str()));
                if (comma == std::string::npos)
                {
                    break;
                }
                start = comma + 1;
            }
        }
        else if (a == "--run-seconds")
        {
            g_options.runSeconds = atof(next().c_str());
        }
        else if (a == "--watch")
        {
            std::string list = next();
            size_t start = 0;
            while (start < list.size())
            {
                size_t comma = list.find(',', start);
                std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                size_t colon = item.find(':');
                u32 address = static_cast<u32>(strtoul(item.c_str(), nullptr, 16));
                u32 count = colon == std::string::npos ? 1 : static_cast<u32>(strtoul(item.c_str() + colon + 1, nullptr, 0));
                g_options.watch.emplace_back(address, count);
                if (comma == std::string::npos)
                {
                    break;
                }
                start = comma + 1;
            }
        }
        else if (a == "--gs")
        {
            std::string which = next();
            g_options.gs = which == "software" ? 0 : 1;
        }
        else if (a == "--scale")
        {
            g_options.gsScale = atoi(next().c_str());
        }
        else if (a == "--texture-upscale")
        {
            g_options.textureUpscale = atoi(next().c_str());
        }
        else if (a == "--gs-record")
        {
            // 95:120:/tmp/park.gsrec: from the game's 95th second, 120 vblanks
            std::string item = next();
            size_t c1 = item.find(':');
            size_t c2 = c1 == std::string::npos ? std::string::npos : item.find(':', c1 + 1);
            if (c2 == std::string::npos)
            {
                Usage(argv[0]);
                return 2;
            }
            g_options.gsRecordAt = atof(item.substr(0, c1).c_str());
            g_options.gsRecordVblanks = strtoull(item.c_str() + c1 + 1, nullptr, 10);
            g_options.gsRecordFile = item.substr(c2 + 1);
        }
        else if (a == "--gs-replay")
        {
            std::string item = next();
            size_t colon = item.rfind(':');
            if (colon != std::string::npos && colon + 1 < item.size() &&
                item.find_first_not_of("0123456789", colon + 1) == std::string::npos)
            {
                g_options.gsReplayPasses = static_cast<u32>(std::max(1, atoi(item.c_str() + colon + 1)));
                item.resize(colon);
            }
            g_options.gsReplayFile = item;
        }
        else if (a == "--fps")
        {
            g_options.fps = true;
        }
        else if (a == "--control")
        {
            g_options.controlPath = next();
        }
        else if (a == "--watch-every")
        {
            g_options.watchEvery = atof(next().c_str());
        }
        else if (a == "--key")
        {
            // 30:Space, 31.5:W+Left Shift:0.5 (held 0.15 s unless said): bindings.txt's names
            std::string list = next();
            size_t start = 0;
            while (start < list.size())
            {
                size_t comma = list.find(',', start);
                std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                size_t c1 = item.find(':');
                size_t c2 = c1 == std::string::npos ? std::string::npos : item.find(':', c1 + 1);
                if (c1 != std::string::npos)
                {
                    Options::KeyPress k;
                    k.at = atof(item.substr(0, c1).c_str());
                    std::string names = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
                    size_t from = 0;
                    while (from <= names.size())
                    {
                        size_t plus = names.find('+', from);
                        k.keys.push_back(names.substr(from, plus == std::string::npos ? std::string::npos : plus - from));
                        if (plus == std::string::npos)
                        {
                            break;
                        }
                        from = plus + 1;
                    }
                    k.hold = c2 == std::string::npos ? 0.15 : atof(item.c_str() + c2 + 1);
                    g_options.keys.push_back(k);
                }
                if (comma == std::string::npos)
                {
                    break;
                }
                start = comma + 1;
            }
        }
        else if (a == "--press")
        {
            // 30:start, 31.5:cross+up:0.5 (held 0.15 s unless said)
            std::string list = next();
            size_t start = 0;
            while (start < list.size())
            {
                size_t comma = list.find(',', start);
                std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                size_t c1 = item.find(':');
                size_t c2 = c1 == std::string::npos ? std::string::npos : item.find(':', c1 + 1);
                if (c1 != std::string::npos)
                {
                    Options::Press p;
                    p.at = atof(item.substr(0, c1).c_str());
                    p.buttons = ButtonsFromNames(item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1));
                    p.hold = c2 == std::string::npos ? 0.15 : atof(item.c_str() + c2 + 1);
                    g_options.presses.push_back(p);
                }
                if (comma == std::string::npos)
                {
                    break;
                }
                start = comma + 1;
            }
        }
        else if (a == "--click")
        {
            // 41:0.5,0.83 (a click at game time 41 s, the middle of the picture across, 83% down)
            std::string item = next();
            Options::Click click = {};
            if (sscanf(item.c_str(), "%lf:%f,%f", &click.at, &click.x, &click.y) == 3)
            {
                g_options.clicks.push_back(click);
            }
        }
        else if (a == "--type")
        {
            // 41:CRASH<enter> (<enter> and <back> for Return and Backspace)
            std::string item = next();
            size_t colon = item.find(':');
            if (colon != std::string::npos)
            {
                std::string text = item.substr(colon + 1);
                for (auto [token, ch] : {std::pair<std::string, char>{"<enter>", '\r'}, {"<back>", '\b'}})
                {
                    for (size_t at; (at = text.find(token)) != std::string::npos;)
                    {
                        text.replace(at, token.size(), 1, ch);
                    }
                }
                g_options.typed.emplace_back(atof(item.substr(0, colon).c_str()), text);
            }
        }
        else if (a == "--ui")
        {
            // 50:esc,shot,down,enter,shot (the PC menus' keys from game time 50 s, a third of a second apart: menus.cpp)
            std::string item = next();
            size_t colon = item.find(':');
            if (colon != std::string::npos)
            {
                g_options.ui.emplace_back(atof(item.substr(0, colon).c_str()), item.substr(colon + 1));
            }
        }
        else if (a == "--audio-dump")
        {
            g_options.audioDump = next();
        }
        else if (a == "--mute")
        {
            g_options.mute = true;
        }
        else if (a == "--memcards")
        {
            g_options.memcardDir = next();
        }
        else if (a == "--settings")
        {
            g_options.settingsDir = next();
        }
        else if (a == "--log")
        {
            g_options.logMask = ParseLog(next());
        }
        else
        {
            Usage(argv[0]);
            return 2;
        }
    }
    g_logMask = g_options.logMask;
    g_game = game;
    // ~/Library/Application Support/<the game's folder>/: the settings, the disc's path, and the memory cards in memcards/.
    // A game that names its folder takes over the runtime's default one (PS2 Native Runtime/<executable>) if it's there
    const char* home = getenv("HOME");
    std::string support = std::string(home != nullptr ? home : ".") + "/Library/Application Support/";
    std::string fallback = support + "PS2 Native Runtime/" + game->executable;
    bool chosen = !g_options.settingsDir.empty(); // --settings
    if (!chosen)
    {
        g_options.settingsDir = game->folder != nullptr ? support + game->folder : fallback;
    }
    {
        std::error_code ec;
        if (!chosen && game->folder != nullptr && !std::filesystem::exists(g_options.settingsDir, ec) &&
            std::filesystem::is_directory(fallback, ec))
        {
            std::filesystem::rename(fallback, g_options.settingsDir, ec);
            Log("settings moved to %s", g_options.settingsDir.c_str());
        }
        std::filesystem::create_directories(g_options.settingsDir, ec);
    }
    // The disc: --disc, $PS2_DISC, the one chosen before (disc_image.txt), or (with a window) asked for
    const std::string discFile = g_options.settingsDir + "/disc_image.txt";
    if (g_options.disc.empty() && getenv("PS2_DISC") != nullptr)
    {
        g_options.disc = getenv("PS2_DISC");
    }
    if (g_options.disc.empty())
    {
        std::ifstream remembered(discFile);
        std::getline(remembered, g_options.disc);
        std::error_code ec;
        if (!g_options.disc.empty() && !std::filesystem::exists(g_options.disc, ec))
        {
            Log("the disc image %s isn't there any more", g_options.disc.c_str());
            g_options.disc.clear();
        }
    }
    bool asked = false;
    if (g_options.disc.empty() && !g_options.headless)
    {
        g_options.disc = AskForDisc(game);
        asked = true;
    }
    if (g_options.disc.empty())
    {
        Usage(argv[0]);
        return 2;
    }
    if (g_options.memcardDir.empty())
    {
        g_options.memcardDir = g_options.settingsDir + "/memcards";
    }

    // The PC menus' settings (options.txt), before what they set starts
    Ui::MenusInit();
    MemoryInit();
    if (!DiscOpen(g_options.disc))
    {
        WrongDisc(game, asked, discFile);
        return 1;
    }
    if (!g_options.patch.empty() && !DiscLoadPatch(g_options.patch))
    {
        return 1;
    }
    std::vector<u8> cnf;
    std::string boot = game->executable;
    if (DiscReadFile("SYSTEM.CNF", cnf))
    {
        std::string text(cnf.begin(), cnf.end());
        size_t b = text.find("BOOT2");
        if (b != std::string::npos)
        {
            size_t colon = text.find(':', b);
            size_t end = text.find_first_of(";\r\n", colon);
            std::string path = text.substr(colon + 1, end - colon - 1);
            while (!path.empty() && (path[0] == '\\' || path[0] == '/'))
            {
                path.erase(0, 1);
            }
            if (path != game->executable)
            {
                Log("this disc boots %s; this build is for %s (%s)", path.c_str(), game->executable, game->title);
                WrongDisc(game, asked, discFile);
                return 1;
            }
        }
    }
    std::vector<u8> elf;
    if (!DiscReadFile(boot, elf))
    {
        Log("%s isn't on the disc", boot.c_str());
        return 1;
    }
    if (game->executableSize != 0 && elf.size() != game->executableSize)
    {
        Log("%s is %zu bytes; this build is for the %u byte one (%s)", boot.c_str(), elf.size(), game->executableSize,
            game->title);
        WrongDisc(game, asked, discFile);
        return 1;
    }
    if (asked)
    {
        std::ofstream(discFile) << g_options.disc << '\n';
    }
    g_entryPoint = LoadElf(elf);
    if (game->init != nullptr)
    {
        game->init();
    }
    GsInit();
    if (!g_options.gsReplayFile.empty())
    {
        // A recording of the GS's side played back in place of the game (gsrecord.cpp)
        std::thread replay([] {
            ThreadOnPerformanceCores();
            std::vector<u8> file;
            if (FILE* f = fopen(g_options.gsReplayFile.c_str(), "rb"))
            {
                fseek(f, 0, SEEK_END);
                file.resize(static_cast<size_t>(ftell(f)));
                fseek(f, 0, SEEK_SET);
                file.resize(fread(file.data(), 1, file.size(), f));
                fclose(f);
            }
            else
            {
                Log("gs replay: can't read %s", g_options.gsReplayFile.c_str());
            }
            bool ok = !file.empty();
            for (u32 pass = 0; ok && pass < g_options.gsReplayPasses; pass++)
            {
                RenderCall([&file, &ok, pass] { ok = GsReplay(*Hw().gs, *Hw().gif, file, pass); });
                RenderSync();
            }
            PresenterStop();
        });
        replay.detach();
        PresenterRun();
        fflush(stderr);
        _exit(0);
    }
    IopInit();
    SoundInit();
    IoInit();
    if (!g_options.controlPath.empty())
    {
        ControlStart(g_options.controlPath);
    }
    DmacInit();
    KernelInit(g_entryPoint, 0, {std::string("cdrom0:\\") + boot + ";1"});
    Log("starting %s at %08x", game->title, g_entryPoint);

    std::thread ee([] {
        ThreadOnPerformanceCores();
        KernelRunMain();
        PresenterStop();
    });
    ee.detach();
    PresenterRun();
    SoundStop();
    fflush(stderr);
    _exit(0);
}
