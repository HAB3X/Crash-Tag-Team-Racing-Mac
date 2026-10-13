// The RPC servers of the IOP modules, high level: the module loader, the IOP heap, the file I/O manager (on the disc image),
// the CD/DVD driver, and stand-ins answering zeros for the rest. Every call to a server or function not done here is logged
// once with its arguments: that log is the list of what's left.
#include "iop.h"

#include <map>

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
int g_nextModule = 1;

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

// The modules' version (their version calls answer it as 4 characters): the IOP replacement image's SDK release, from its
// name at the IOP's reset ("IOPRP250.IMG" is release 2.5.0, "2500")
char g_version[4] = {'2', '5', '0', '0'};

void Version(RpcCall& c)
{
    c.out.assign(std::max<u32>(c.recvSize, 4), 0);
    std::memcpy(c.out.data(), g_version, 4);
}

// --- the servers
void LoadFile(RpcCall& c)
{
    if (c.fno == 255)
    {
        Version(c);
        return;
    }
    // Find the module's path in the request
    std::string path;
    for (u32 off = 0; off + 8 < c.inSize && path.empty(); off += 4)
    {
        std::string s = c.InString(off, 252);
        if (s.find(':') != std::string::npos && s.size() > 4)
        {
            path = s;
        }
    }
    int id = g_nextModule++;
    Log("IOP: load module %s (function %u): id %d", path.empty() ? "(from memory)" : path.c_str(), c.fno, id);
    c.Out32(0, static_cast<u32>(id));
    c.Out32(4, 0);
}

void Heap(RpcCall& c)
{
    switch (c.fno)
    {
    case 1: // alloc
    {
        u32 size = c.In32(0);
        u32 at = IopAlloc(size);
        PS2_LOG(LogIop, "IOP heap: alloc %u at %08x", size, at);
        c.Out32(0, at);
        break;
    }
    case 2: // free
        c.Out32(0, 0);
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

void Cdvd(RpcCall& c)
{
    switch (c.sid)
    {
    case 0x80000592: // init
        c.Out32(0, 1);
        return;
    case 0x8000059A: // disk ready: complete
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
    case 0x80000593: // S commands
        switch (c.fno)
        {
        case 0x03: // disk type: a PlayStation 2 CD
            c.Out32(0, 0x12);
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
    {0x8000059C, "cdvd"},
    {0x80000701, "sdrdrv"},
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

u8* EeMemory(u32 address) { return Host(address & 0x1FFFFFFF); }

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

void IopServersInit() {}

void IopSetImage(const std::string& path)
{
    size_t at = path.find("IOPRP");
    if (at != std::string::npos && at + 8 <= path.size())
    {
        std::memcpy(g_version, path.c_str() + at + 5, 3);
        g_version[3] = '0';
    }
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
    case 0x80000592:
    case 0x80000593:
    case 0x80000595:
    case 0x80000596:
    case 0x80000597:
    case 0x8000059A:
    case 0x8000059C:
        Cdvd(c);
        break;
    default:
        Unhandled(c);
        break;
    }
}
}
