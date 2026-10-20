// The RPC servers of the IOP modules, high level: the module loader, the IOP heap, the file I/O manager (on the disc image),
// the CD/DVD driver, the memory card (mcserv.cpp), the pads (padman.cpp), the Dbc_Manager (dbcman.cpp), and stand-ins answering
// zeros for the rest. Every call to a server or function not done here is logged
// once with its arguments: that log is the list of what's left.
#include "iop.h"
#include "cri_adx.h"
#include "ps2/sound.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <vector>

namespace Ps2
{
namespace
{
struct OpenFile
{
    u32 lsn;
    u32 size;
    u32 pos;
    std::string path;
};
std::map<int, OpenFile> g_files;
int g_nextFd = 3;

// The modules loaded (their IDs and names, the modules' own) and the game's own modules (IopRegisterModule), by name
int g_nextModule = 1;
std::map<int, std::string> g_modules;
std::map<std::string, IopModuleStart>& GameModules()
{
    static std::map<std::string, IopModuleStart> m;
    return m;
}
// The game's modules with no name of their own (IopRegisterUnnamedModule), by a string in their image
std::vector<std::pair<std::string, IopModuleStart>>& UnnamedModules()
{
    static std::vector<std::pair<std::string, IopModuleStart>> m;
    return m;
}
// The modules' vertical blank handlers (IopOnVblank), in the order they were added
std::vector<std::function<void()>> g_vblankHandlers;

void Dump(const RpcCall& c)
{
    std::string hex;
    u32 n = std::min<u32>(c.inSize, 64);
    for (u32 i = 0; i < n; i += 4)
    {
        char b[12];
        snprintf(b, sizeof(b), "%08x ", c.In32(i));
        hex += b;
    }
    Log("  in (%u bytes): %s", c.inSize, hex.c_str());
}

void Unhandled(const RpcCall& c)
{
    char key[64];
    snprintf(key, sizeof(key), "rpc %08x %u", c.sid, c.fno);
    if (Once(key))
    {
        Log("RPC %08x (%s) function %u not done: answered with zeros (recv %u bytes)", c.sid, IopServerName(c.sid), c.fno,
            c.recvSize);
        Dump(c);
    }
}

// "cdrom0:\\PATH\\FILE.EXT;1" to the disc's path
std::string DiscPath(std::string path)
{
    size_t colon = path.find(':');
    if (colon != std::string::npos)
    {
        path = path.substr(colon + 1);
    }
    return path;
}

int OpenOnDisc(const std::string& name)
{
    std::string device = name.substr(0, name.find(':'));
    if (device != "cdrom0" && device != "cdrom")
    {
        return -1;
    }
    OpenFile f;
    if (!DiscFind(DiscPath(name), f.lsn, f.size))
    {
        return -1;
    }
    f.pos = 0;
    f.path = name;
    int fd = g_nextFd++;
    g_files[fd] = f;
    return fd;
}

int ReadFile(int fd, u32 dest, u32 size)
{
    auto it = g_files.find(fd);
    if (it == g_files.end())
    {
        return -1;
    }
    OpenFile& f = it->second;
    if (f.pos >= f.size)
    {
        return 0;
    }
    size = std::min(size, f.size - f.pos);
    u32 first = f.pos / 2048;
    u32 last = (f.pos + size + 2047) / 2048;
    std::vector<u8> buf((last - first) * 2048);
    DiscReadSectors(f.lsn + first, last - first, buf.data());
    std::memcpy(Host(dest & 0x1FFFFFF), buf.data() + (f.pos - first * 2048), size);
    f.pos += size;
    return static_cast<int>(size);
}

// The modules' version (their version calls answer it as 4 characters): the IOP replacement image's, as its FILEIO module
// names itself ("PsIIfileio  2800"); the boot ROM's until the game resets the IOP with an image
char g_version[4] = {'2', '5', '0', '0'};

void Version(RpcCall& c)
{
    c.out.assign(std::max<u32>(c.recvSize, 4), 0);
    std::memcpy(c.out.data(), g_version, 4);
}

// An IOP module's name, as its .iopmod section (an ELF section of type 0x70000080) has it after six words and the version
std::string ModuleName(const u8* elf, size_t size)
{
    auto r32 = [&](size_t off) { u32 v = 0; if (off + 4 <= size) std::memcpy(&v, elf + off, 4); return v; };
    auto r16 = [&](size_t off) { u16 v = 0; if (off + 2 <= size) std::memcpy(&v, elf + off, 2); return v; };
    if (size < 52 || std::memcmp(elf, "\x7f" "ELF", 4) != 0)
    {
        return "";
    }
    u32 shoff = r32(32);
    u32 count = r16(48);
    for (u32 n = 0; n < count; n++)
    {
        size_t h = shoff + n * 40;
        if (r32(h + 4) == 0x70000080)
        {
            size_t at = r32(h + 16) + 26;
            std::string name;
            while (at < size && elf[at] != 0 && name.size() < 64)
            {
                name += static_cast<char>(elf[at++]);
            }
            return name;
        }
    }
    return "";
}

// An IOP module's bytes: to the end of its section headers (the last thing an IRX's file has), within size
size_t ModuleExtent(const u8* elf, size_t size)
{
    if (size < 52 || std::memcmp(elf, "\x7f" "ELF", 4) != 0)
    {
        return 0;
    }
    u32 shoff;
    u16 shentsize, shnum;
    std::memcpy(&shoff, elf + 32, 4);
    std::memcpy(&shentsize, elf + 46, 2);
    std::memcpy(&shnum, elf + 48, 2);
    return std::min<size_t>(size, static_cast<size_t>(shoff) + static_cast<size_t>(shentsize) * shnum);
}

// A module's image seen as it loads (its file, or its copy in the IOP's memory), for the stand-ins that take something from it:
// LIBSD.IRX's reverb presets are libsd's (sound/libsd.cpp), the Dbc_Manager's drivers start (dbcman.cpp)
void ModuleImageLoaded(const std::string& name, const u8* elf, size_t size)
{
    if (name == "Sound_Device_Library")
    {
        SdModuleLoaded(elf, size);
    }
    DbcModuleLoaded(name, elf, size);
}

// A module loaded: its ID, and the game's own module of that name (or, with none, known by a string in its image) started
// with the arguments (arg_len bytes, each ending with a zero)
int ModuleLoaded(const std::string& name, const RpcCall& c, u32 argLength, const u8* image, size_t imageSize)
{
    std::vector<std::string> args;
    std::string arg;
    for (u32 i = 0; i < std::min<u32>(argLength, 252); i++)
    {
        char ch = static_cast<char>(c.in[260 + i]);
        if (ch == 0)
        {
            args.push_back(arg);
            arg.clear();
        }
        else
        {
            arg += ch;
        }
    }
    int id = g_nextModule++;
    g_modules[id] = name;
    const IopModuleStart* start = nullptr;
    std::string known = name;
    if (auto it = GameModules().find(name); !name.empty() && it != GameModules().end())
    {
        start = &it->second;
    }
    else if (name.empty())
    {
        size_t extent = ModuleExtent(image, imageSize);
        for (const auto& [text, unnamedStart] : UnnamedModules())
        {
            if (std::search(image, image + extent, text.begin(), text.end()) != image + extent)
            {
                start = &unnamedStart;
                known = "\"" + text + "\"";
                break;
            }
        }
    }
    Log("IOP: module %s loaded: id %d%s", known.empty() ? "(unnamed)" : known.c_str(), id, start != nullptr ? ", the game's" : "");
    if (start != nullptr)
    {
        (*start)(args);
    }
    return id;
}

// --- the servers
// The module loader (loadfile): 0 loads a module from a path (arg_len at 0, the path at 8, the arguments at 260), 6 from a
// buffer in the IOP's memory (its address at 0, arg_len at 4), 9 finds a loaded module by name (at 8). The modules
// themselves never run here: a module of the game's own (IopRegisterModule) is started by name
void LoadFile(RpcCall& c)
{
    switch (c.fno)
    {
    case 255:
        Version(c);
        return;
    case 0:
    {
        std::string path = c.InString(8, 252);
        std::string name;
        u32 lsn, size;
        if (path.rfind("rom0:", 0) == 0)
        {
            // The console's own modules: named by their file
            name = path.substr(5);
            c.Out32(0, static_cast<u32>(ModuleLoaded(name, c, c.In32(0), nullptr, 0)));
            c.Out32(4, 0);
            return;
        }
        else if (DiscFind(DiscPath(path), lsn, size))
        {
            std::vector<u8> elf((size + 2047) / 2048 * 2048);
            DiscReadSectors(lsn, static_cast<u32>(elf.size() / 2048), elf.data());
            name = ModuleName(elf.data(), size);
            ModuleImageLoaded(name, elf.data(), size);
            PS2_LOG(LogIop, "IOP: load module %s", path.c_str());
            c.Out32(0, static_cast<u32>(ModuleLoaded(name, c, c.In32(0), elf.data(), size)));
            c.Out32(4, 0);
            return;
        }
        Log("IOP: load module %s: no such file", path.c_str());
        c.Out32(0, static_cast<u32>(-203)); // KE_NOFILE
        return;
    }
    case 6:
    {
        u32 at = c.In32(0) & (IopRamSize - 1);
        std::string name = ModuleName(IopMem() + at, IopRamSize - at);
        ModuleImageLoaded(name, IopMem() + at, IopRamSize - at);
        c.Out32(0, static_cast<u32>(ModuleLoaded(name, c, c.In32(4), IopMem() + at, IopRamSize - at)));
        c.Out32(4, 0);
        return;
    }
    case 9:
    {
        std::string name = c.InString(8, 56);
        for (const auto& [id, loaded] : g_modules)
        {
            if (loaded == name)
            {
                c.Out32(0, static_cast<u32>(id));
                return;
            }
        }
        c.Out32(0, static_cast<u32>(-202)); // KE_UNKNOWN_MODULE
        return;
    }
    default:
        Unhandled(c);
        return;
    }
}

void Heap(RpcCall& c)
{
    switch (c.fno)
    {
    case 1: // alloc: 0 when there's no room, as AllocSysMemory answers
    {
        u32 size = c.In32(0);
        u32 at = IopAllocMode(0, size, 0);
        PS2_LOG(LogIop, "IOP heap: alloc %u at %08x", size, at);
        if (at == 0)
        {
            Log("IOP heap: no room for %u bytes (%u free, the largest block %u)", size, IopFreeTotal(false),
                IopFreeTotal(true));
        }
        c.Out32(0, at);
        break;
    }
    case 2: // free
    {
        u32 at = c.In32(0);
        bool freed = IopFree(at);
        PS2_LOG(LogIop, "IOP heap: free %08x%s", at, freed ? "" : " (not allocated)");
        c.Out32(0, freed ? 0 : static_cast<u32>(-1));
        break;
    }
    case 3: // a file loaded into the IOP's memory: its address at 0, its name at 4
    {
        std::vector<u8> data;
        std::string name = c.InString(4, 252);
        if (!DiscReadFile(DiscPath(name), data))
        {
            Log("IOP heap: load %s: no such file", name.c_str());
            c.Out32(0, static_cast<u32>(-1));
            break;
        }
        u32 at = c.In32(0) & (IopRamSize - 1);
        std::memcpy(IopMem() + at, data.data(), std::min<size_t>(data.size(), IopRamSize - at));
        c.Out32(0, 0);
        break;
    }
    case 4: // allocate as AllocSysMemory: size at 0, mode at 4, address at 8
        c.Out32(0, IopAllocMode(c.In32(4), c.In32(0), c.In32(8)));
        break;
    case 5: // QueryMemSize
        c.Out32(0, IopRamSize);
        break;
    case 6: // QueryMaxFreeMemSize
        c.Out32(0, IopFreeTotal(true));
        break;
    case 7: // QueryTotalFreeMemSize
        c.Out32(0, IopFreeTotal(false));
        break;
    case 8: // QueryBlockTopAddress
        c.Out32(0, IopBlockOf(c.In32(0), false));
        break;
    case 9: // QueryBlockSize
        c.Out32(0, IopBlockOf(c.In32(0), true));
        break;
    default:
        Unhandled(c);
        break;
    }
}

// The file I/O manager's protocol (SDK 2.5's filestub): each request starts with a semaphore, the address of the EE's result
// and its size; the call's own answer only says the request was taken (non-zero), and the result comes after it: written to
// that address, then the semaphore signalled (the IOP does both with a command the EE's handler takes; here they're done
// directly)
void FileIoComplete(const RpcCall& c, const void* result, u32 size)
{
    u32 sema = c.In32(0);
    u32 at = c.In32(4);
    u32 n = std::min(size, c.In32(8));
    if (at != 0 && n != 0)
    {
        std::memcpy(Host(at & 0x1FFFFFF), result, n);
    }
    KernelSignalSema(static_cast<int>(sema));
}

void FileIoResult(RpcCall& c, s32 value)
{
    c.Out32(0, 1);
    FileIoComplete(c, &value, 4);
}

void FileIo(RpcCall& c)
{
    switch (c.fno)
    {
    case 255:
        Version(c);
        break;
    case 0: // open: mode at 12, name at 20
    {
        std::string name = c.InString(20, 1024);
        int fd = OpenOnDisc(name);
        PS2_LOG(LogIop, "fileio open %s mode %x: %d", name.c_str(), c.In32(12), fd);
        if (fd < 0)
        {
            Log("fileio: %s not opened (mode %x)", name.c_str(), c.In32(12));
        }
        FileIoResult(c, fd);
        break;
    }
    case 1: // close: fd at 12
        g_files.erase(static_cast<int>(c.In32(12)));
        FileIoResult(c, 0);
        break;
    case 2: // read: fd at 12, buffer at 16, size at 20
    {
        if (Once("fileio read layout"))
        {
            Dump(c);
        }
        int fd = static_cast<int>(c.In32(12));
        u32 dest = c.In32(16);
        u32 size = c.In32(20);
        int got = ReadFile(fd, dest, size);
        PS2_LOG(LogIop, "fileio read %d: %u bytes to %08x: %d", fd, size, dest, got);
        FileIoResult(c, got);
        break;
    }
    case 4: // lseek: fd at 12, offset at 16, whence at 20
    {
        auto it = g_files.find(static_cast<int>(c.In32(12)));
        if (it == g_files.end())
        {
            FileIoResult(c, -9);
            break;
        }
        s32 off = static_cast<s32>(c.In32(16));
        u32 whence = c.In32(20);
        s64 pos = whence == 0 ? off : whence == 1 ? static_cast<s64>(it->second.pos) + off
                                                  : static_cast<s64>(it->second.size) + off;
        it->second.pos = static_cast<u32>(std::max<s64>(0, pos));
        PS2_LOG(LogIop, "fileio lseek %d: %d from %u: %u", it->first, off, whence, it->second.pos);
        FileIoResult(c, static_cast<s32>(it->second.pos));
        break;
    }
    default:
        Unhandled(c);
        FileIoResult(c, -1);
        break;
    }
}

// SDRDRV, libsdr's server (sceSdRemote): libsd's calls made on the EE's behalf (read from SDRDRV.IRX 4.0.1): the command
// (the function number, a core in its low bits where it takes one) and its arguments as words from 4 on; the answer's word 0
// the call's result
void Sdr(RpcCall& c)
{
    u32 command = c.fno & 0xFFF0;
    u32 core = c.fno & 0xF;
    u32 result = 0;
    auto lock = SoundLock();
    switch (command)
    {
    case 0x8000: // sceSdInit
        if (c.In32(4) == 0)
        {
            SdInit();
        }
        break;
    case 0x8010:
        SdSetParam(c.In16(4), c.In16(8));
        break;
    case 0x8020:
        result = SdGetParam(c.In16(4));
        break;
    case 0x8030:
        SdSetSwitch(c.In16(4), c.In32(8));
        break;
    case 0x8040:
        result = SdGetSwitch(c.In16(4));
        break;
    case 0x8050:
        SdSetAddr(c.In16(4), c.In32(8));
        break;
    case 0x8060:
        result = SdGetAddr(c.In16(4));
        break;
    case 0x8070:
        SdSetCoreAttr(c.In16(4), c.In16(8));
        break;
    case 0x8130: // sceSdSetEffectAttr(core, the sceSdEffectAttr at 0: core, mode, depth left and right (16 bits each), delay,
                 // feedback); its own core word is libsd's to ignore
        result = static_cast<u32>(SdSetEffectAttr(static_cast<s32>(core), c.In32(4), c.In16(8), c.In16(10), c.In32(12),
                                                  c.In32(16)));
        PS2_LOG(LogIop, "sdrdrv: core %u's reverb mode %#x, depths %04x %04x, delay %u, feedback %u: %d", core, c.In32(4),
                c.In16(8), c.In16(10), c.In32(12), c.In32(16), static_cast<s32>(result));
        break;
    case 0x8150: // sceSdClearEffectWorkArea(core, channel, mode)
        result = static_cast<u32>(SdClearEffectWorkArea(static_cast<s32>(c.In32(4)), static_cast<s32>(c.In32(8)), c.In32(12)));
        break;
    default:
        Unhandled(c);
        break;
    }
    c.Out32(0, result);
}

void Cdvd(RpcCall& c)
{
    switch (c.sid)
    {
    case 0x80000592: // init
        c.Out32(0, 1);
        return;
    case 0x8000059A: // disk ready: complete (SCECdComplete)
    case 0x8000059C: // the same, in libcdvd 2.6 on
        c.Out32(0, 2);
        return;
    case 0x80000597: // search file: { sceCdlFILE (32 bytes), ?, name[256], the EE's sceCdlFILE }
    {
        std::string name = c.InString(36, 256);
        u32 dest = c.In32(292);
        if (Once("cdsearch layout"))
        {
            Log("cdvd search file %s: words at 288: %08x %08x %08x", name.c_str(), c.In32(288), c.In32(292),
                c.In32(296));
        }
        u32 lsn, size;
        if (!DiscFind(name, lsn, size))
        {
            Log("cdvd search file: %s not found", name.c_str());
            c.Out32(0, 0);
            return;
        }
        u8 file[32] = {};
        std::memcpy(file, &lsn, 4);
        std::memcpy(file + 4, &size, 4);
        std::string base = name.substr(name.find_last_of('\\') + 1);
        std::memcpy(file + 8, base.c_str(), std::min<size_t>(base.size(), 15));
        if (dest != 0)
        {
            std::memcpy(Host(dest & 0x1FFFFFF), file, 32);
        }
        PS2_LOG(LogIop, "cdvd search file %s: lsn %u size %u", name.c_str(), lsn, size);
        c.Out32(0, 1);
        return;
    }
    case 0x80000595: // N commands
        switch (c.fno)
        {
        case 0x01: // read: lsn, sectors, the EE's buffer, the mode (tries, spindle, sector size), the unaligned ends' buffer
                   // (left empty: the sectors go straight in), a progress word
        {
            u32 lsn = c.In32(0);
            u32 sectors = c.In32(4);
            u32 dest = c.In32(8) & 0x1FFFFFF;
            u32 pattern = (c.In32(12) >> 16) & 0xFF;
            u32 ends = c.In32(16) & 0x1FFFFFF;
            if (pattern != 0 && Once("cd read pattern"))
            {
                Log("cdvd read: sector size pattern %u read as 2048 byte sectors", pattern);
            }
            if (ends != 0)
            {
                std::memset(Host(ends), 0, 16);
            }
            if (dest + sectors * 2048 <= RamSize)
            {
                DiscReadSectors(lsn, sectors, Host(dest));
            }
            PS2_LOG(LogIop, "cdvd read %u sectors from %u to %08x", sectors, lsn, dest);
            c.Out32(0, 1);
            return;
        }
        case 0x0D: // read into the IOP's memory (libcdvd 2.6 on): lsn, sectors, the IOP's buffer, the mode, as a read's
        {
            u32 lsn = c.In32(0);
            u32 sectors = c.In32(4);
            u32 dest = c.In32(8) & (IopRamSize - 1);
            if (dest + sectors * 2048 <= IopRamSize)
            {
                DiscReadSectors(lsn, sectors, IopMem() + dest);
            }
            PS2_LOG(LogIop, "cdvd read %u sectors from %u to IOP %06x", sectors, lsn, dest);
            c.Out32(0, 1);
            return;
        }
        case 0x0E: // the disc's type again, before a read (6 would stop it): as the S command's
            c.Out32(0, DiscIsDvd() ? 0x14 : 0x12);
            return;
        default:
            break;
        }
        break;
    case 0x80000593: // S commands
        switch (c.fno)
        {
        case 0x01: // ReadClock: the result, then the clock (status, seconds, minutes, hours, a pad, day, month, year: BCD) as
                   // a console's keeps it, in Japan's time (GMT+9); games make it local with the time zone GetOsdConfigParam
                   // gives (the Mac's)
        {
            time_t japan = time(nullptr) + 9 * 3600;
            struct tm t;
            gmtime_r(&japan, &t);
            auto bcd = [](int v) { return static_cast<u8>(((v / 10) << 4) | (v % 10)); };
            u8 clock[8] = {0, bcd(t.tm_sec), bcd(t.tm_min), bcd(t.tm_hour), 0, bcd(t.tm_mday), bcd(t.tm_mon + 1),
                           bcd(t.tm_year % 100)};
            c.Out32(0, 1);
            c.Out32(4, 0);
            c.Out32(8, 0);
            std::memcpy(&c.out[4], clock, 8);
            return;
        }
        case 0x03: // disk type: a PlayStation 2 DVD (SCECdPS2DVD) or CD (SCECdPS2CD)
            c.Out32(0, DiscIsDvd() ? 0x14 : 0x12);
            return;
        case 0x04: // the last command's error: none
            c.Out32(0, 0);
            return;
        case 0x05: // the tray (its mode in): done, never opened since the last ask
            c.Out32(0, 1);
            c.Out32(4, 0);
            return;
        case 0x22: // media mode
            c.Out32(0, 1);
            return;
        default:
            break;
        }
        break;
    default:
        break;
    }
    Unhandled(c);
    // Commands answer "done, no error"
    c.Out32(0, 1);
}

struct ServerName
{
    u32 sid;
    const char* name;
};
const ServerName g_names[] = {
    {0x80000001, "fileio"},
    {0x80000003, "iop heap"},
    {0x80000006, "loadfile"},
    {0x80000100, "padman"},
    {0x80000101, "padman ext"},
    {0x8000010F, "padman"},
    {0x80000110, "padman ext"},
    {0x80000400, "mcserv"},
    {0x80000592, "cdvd init"},
    {0x80000593, "cdvd s-command"},
    {0x80000595, "cdvd n-command"},
    {0x80000596, "cdvd"},
    {0x80000597, "cdvd search file"},
    {0x8000059A, "cdvd disk ready"},
    {0x8000059C, "cdvd disk ready"},
    {0x80000701, "sdrdrv"},
    {0x80001300, "dbcman"},
    {0x8000131B, "dbcman send_data2"},
    {0x8000131C, "dbcman send_data3"},
};
}

struct Registered
{
    std::string name;
    RpcServer server;
};
std::map<u32, Registered>& Registry()
{
    static std::map<u32, Registered> r;
    return r;
}

void IopRegisterServer(u32 sid, const char* name, RpcServer server) { Registry()[sid] = {name, std::move(server)}; }

void IopRegisterModule(const char* name, IopModuleStart start) { GameModules()[name] = std::move(start); }

void IopRegisterUnnamedModule(const char* text, IopModuleStart start) { UnnamedModules().emplace_back(text, std::move(start)); }

void IopOnVblank(std::function<void()> handler) { g_vblankHandlers.push_back(std::move(handler)); }

void IopVblank()
{
    for (const auto& handler : g_vblankHandlers)
    {
        handler();
    }
}

u8* IopMemory(u32 address) { return IopMem() + (address & (IopRamSize - 1)); }

u8* EeMemory(u32 address) { return Host(address & 0x1FFFFFFF); }

bool IopLogging() { return (g_logMask & LogIop) != 0; }

const char* IopServerName(u32 sid)
{
    auto it = Registry().find(sid);
    if (it != Registry().end())
    {
        return it->second.name.c_str();
    }
    for (const ServerName& n : g_names)
    {
        if (n.sid == sid)
        {
            return n.name;
        }
    }
    return "?";
}

void IopServersInit() { CriAdx::Register(); }

void IopSetImage(const std::string& reset)
{
    // "rom0:UDNL cdrom0:\IRX\IOPRP280.IMG;1": the image on the disc after the device. Its name doesn't always say the
    // release (Biohazard Outbreak's is DNAS280.IMG), so it's read: its modules carry their ID strings
    size_t at = reset.find("cdrom0:");
    if (at == std::string::npos)
    {
        Log("IOP reset with no image on the disc: the modules' version stays %.4s", g_version);
        return;
    }
    std::string path = reset.substr(at + 7);
    std::vector<u8> image;
    if (!DiscReadFile(path, image))
    {
        Fatal("IOP reset: the image %s isn't on the disc", path.c_str());
    }
    static const char id[] = "PsIIfileio  ";
    auto found = std::search(image.begin(), image.end(), id, id + sizeof(id) - 1);
    if (found == image.end() || image.end() - found < static_cast<long>(sizeof(id) - 1 + 4))
    {
        Fatal("IOP reset: the image %s has no FILEIO module's ID (\"%s\" and its version)", path.c_str(), id);
    }
    std::memcpy(g_version, &*found + sizeof(id) - 1, 4);
    Log("IOP image %s: modules' version %.4s", path.c_str(), g_version);
}

void IopServe(RpcCall& c)
{
    PS2_LOG(LogIop, "RPC %08x (%s) function %u, %u bytes in, %u out", c.sid, IopServerName(c.sid), c.fno, c.inSize,
            c.recvSize);
    auto reg = Registry().find(c.sid);
    if (reg != Registry().end())
    {
        reg->second.server(c);
        return;
    }
    switch (c.sid)
    {
    case 0x80000006:
        LoadFile(c);
        break;
    case 0x80000003:
        Heap(c);
        break;
    case 0x80000001:
        FileIo(c);
        break;
    case 0x80000400:
        McServ(c);
        break;
    case 0x80000100:
    case 0x80000101:
        PadMan(c);
        break;
    case 0x80000592:
    case 0x80000593:
    case 0x80000595:
    case 0x80000596:
    case 0x80000597:
    case 0x8000059A:
    case 0x8000059C:
        Cdvd(c);
        break;
    case 0x80000701:
        Sdr(c);
        break;
    case 0x80001300:
    case 0x8000131B:
    case 0x8000131C:
        DbcServe(c);
        break;
    default:
        Unhandled(c);
        break;
    }
}
}
