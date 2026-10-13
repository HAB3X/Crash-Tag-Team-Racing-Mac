// Start-up: the options, the disc, the executable SYSTEM.CNF names loaded into main memory as the BIOS's loader puts it, the
// hardware, and the main thread at the executable's entry point. The host's main thread runs the presenter.
#include "runtime.h"

#include "ps2/runtime.h"

#include <cstdlib>
#include <thread>
#include <unistd.h>

namespace Ps2
{
extern u32 g_entryPoint;

namespace
{
void Usage(const char* program)
{
    fprintf(stderr,
            "usage: %s --disc IMAGE [--headless] [--snapshots DIR] [--snapshot-every S] [--snapshot-at S,S,...]\n"
            "          [--run-seconds S] [--log kernel,syscall,io,dma,sif,iop,gs,call,thread,irq]\n",
            program);
}

u32 ParseLog(const std::string& list)
{
    static const struct
    {
        const char* name;
        u32 bit;
    } names[] = {{"kernel", LogKernel}, {"syscall", LogSyscall}, {"io", LogIo},   {"dma", LogDma},
                 {"sif", LogSif},       {"iop", LogIop},         {"gs", LogGs},   {"call", LogCall},
                 {"thread", LogThread}, {"irq", LogIrq},         {"all", 0xFFFFFFFF}};
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
    if (g_options.disc.empty())
    {
        const char* env = getenv("PS2_DISC");
        if (env != nullptr)
        {
            g_options.disc = env;
        }
    }
    if (g_options.disc.empty())
    {
        Usage(argv[0]);
        return 2;
    }
    g_logMask = g_options.logMask;

    MemoryInit();
    if (!DiscOpen(g_options.disc))
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
        return 1;
    }
    g_entryPoint = LoadElf(elf);
    if (game->init != nullptr)
    {
        game->init();
    }
    GsInit();
    IopInit();
    IoInit();
    DmacInit();
    KernelInit(g_entryPoint, 0, {std::string("cdrom0:\\") + boot + ";1"});
    Log("starting %s at %08x", game->title, g_entryPoint);

    std::thread ee([] {
        KernelRunMain();
        PresenterStop();
    });
    ee.detach();
    PresenterRun();
    fflush(stderr);
    _exit(0);
}
