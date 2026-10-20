// The memory card server (MCSERV and MCMAN on the IOP), high level, for the libmc of SDKs before 2.6 (RPC 0x80000400,
// functions 0x01-0x15 and 0xFE). Each port's card is a folder on the host (Options::memcardDir/port0, port1): the card's
// directories and files are the folder's, so saves can be looked at and copied. Port 0 has a formatted 8 MB card; port 1's
// folder makes a second card only if it exists.
//
// The protocol, as the game's libmc sends it: a 0x30 byte block for the file calls (fd at 0; port at 4, slot at 8; sizes at
// 12 and 20; the EE's buffers at 24 and 28) and a 0x414 byte block for the calls with a name (port 0, slot 4, flags 8, count 12,
// an EE buffer at 16, the name at 20). The answer is one word, the call's result; the rest goes straight into the EE's memory
// (the IOP does that with SIF DMA).
#include "iop.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace Ps2
{
namespace
{
constexpr u32 CardClusters = 8000; // an 8 MB card's free space when empty, in KB
constexpr s32 ResFull = -3;
constexpr s32 ResNoEntry = -4;
constexpr s32 ResDenied = -5;
constexpr s32 ResNotEmpty = -6;
constexpr s32 ResTooManyHandles = -7;
constexpr s32 ResNoCard = -10;

constexpr u32 OpenWrite = 0x0002;
constexpr u32 OpenMakeDir = 0x0040;
constexpr u32 OpenCreate = 0x0200;
constexpr u32 OpenTruncate = 0x0400;

struct Handle
{
    std::fstream file;
    fs::path path;
};

struct Card
{
    bool seen = false;     // GetInfo has answered "a new card" once
    std::string cwd = "/"; // the card's current directory (ChDir)
    // GetDir's listing, kept between a first call and the ones that continue it
    std::vector<std::pair<std::string, fs::path>> listing;
    size_t listed = 0;
};

std::map<int, Handle> g_handles;
Card g_cards[2];
int g_nextHandle = 1;

fs::path PortFolder(u32 port) { return fs::path(g_options.memcardDir) / ("port" + std::to_string(port)); }

bool CardIn(u32 port)
{
    if (port > 1)
    {
        return false;
    }
    if (port == 0)
    {
        std::error_code ec;
        fs::create_directories(PortFolder(0), ec);
        return true;
    }
    return fs::is_directory(PortFolder(port));
}

// A card path ("/BASLUS-20238/save", or relative to the current directory) to the host's; empty if it leaves the card
fs::path HostPath(u32 port, const std::string& name)
{
    std::string full = !name.empty() && name[0] == '/' ? name : g_cards[port].cwd + "/" + name;
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= full.size())
    {
        size_t slash = full.find('/', start);
        std::string part = full.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..")
        {
            if (!parts.empty())
            {
                parts.pop_back();
            }
        }
        else if (!part.empty() && part != ".")
        {
            parts.push_back(part);
        }
        if (slash == std::string::npos)
        {
            break;
        }
        start = slash + 1;
    }
    fs::path p = PortFolder(port);
    for (const std::string& part : parts)
    {
        p /= part;
    }
    return p;
}

u32 UsedClusters(u32 port)
{
    u64 bytes = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(PortFolder(port), ec); it != fs::recursive_directory_iterator();
         it.increment(ec))
    {
        // A file takes its size rounded to clusters, and every entry one more for its directory record
        if (it->is_regular_file(ec))
        {
            bytes += (it->file_size(ec) + 1023) & ~1023ull;
        }
        bytes += 1024;
    }
    return static_cast<u32>(bytes / 1024);
}

bool Match(const std::string& pattern, const std::string& name)
{
    size_t p = 0, n = 0, star = std::string::npos, mark = 0;
    while (n < name.size())
    {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n]))
        {
            p++;
            n++;
        }
        else if (p < pattern.size() && pattern[p] == '*')
        {
            star = p++;
            mark = n;
        }
        else if (star != std::string::npos)
        {
            p = star + 1;
            n = ++mark;
        }
        else
        {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
    {
        p++;
    }
    return p == pattern.size();
}

// sceMcStDateTime: reserved, seconds, minutes, hours, day, month, year (16 bits)
void PutTime(u8* at, fs::file_time_type when)
{
    auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(when - fs::file_time_type::clock::now() +
                                                                                  std::chrono::system_clock::now());
    time_t t = std::chrono::system_clock::to_time_t(sys);
    tm local{};
    localtime_r(&t, &local);
    at[0] = 0;
    at[1] = static_cast<u8>(local.tm_sec);
    at[2] = static_cast<u8>(local.tm_min);
    at[3] = static_cast<u8>(local.tm_hour);
    at[4] = static_cast<u8>(local.tm_mday);
    at[5] = static_cast<u8>(local.tm_mon + 1);
    u16 year = static_cast<u16>(local.tm_year + 1900);
    std::memcpy(at + 6, &year, 2);
}

// One sceMcTblGetDir (64 bytes): created, modified, size, attributes, reserved, PDA number, name
void PutEntry(u8* at, const std::string& name, const fs::path& host)
{
    std::memset(at, 0, 64);
    std::error_code ec;
    bool dir = fs::is_directory(host, ec);
    auto when = fs::last_write_time(host, ec);
    PutTime(at, when);
    PutTime(at + 8, when);
    u32 size = dir ? 0 : static_cast<u32>(fs::file_size(host, ec));
    // Exists, PS2 file, readable, writable, executable; a directory or a closed file
    u16 attr = dir ? 0x8427 : 0x8497;
    if (dir)
    {
        // A directory's size is its number of entries, "." and ".." included
        size = 2;
        for (auto it = fs::directory_iterator(host, ec); it != fs::directory_iterator(); it.increment(ec))
        {
            size++;
        }
    }
    std::memcpy(at + 16, &size, 4);
    std::memcpy(at + 20, &attr, 2);
    std::memcpy(at + 32, name.c_str(), std::min<size_t>(name.size(), 31));
}

s32 Open(u32 port, const std::string& name, u32 mode)
{
    fs::path p = HostPath(port, name);
    std::error_code ec;
    if (mode & OpenMakeDir)
    {
        if (fs::exists(p, ec))
        {
            return ResNoEntry; // libmc's "already exists"
        }
        if (!fs::is_directory(p.parent_path(), ec))
        {
            return ResNoEntry;
        }
        return fs::create_directory(p, ec) ? 0 : ResDenied;
    }
    if (!fs::is_directory(p.parent_path(), ec) || fs::is_directory(p, ec))
    {
        return ResNoEntry;
    }
    bool exists = fs::exists(p, ec);
    if (!exists && !(mode & OpenCreate))
    {
        return ResNoEntry;
    }
    if (g_handles.size() >= 32)
    {
        return ResTooManyHandles;
    }
    if (!exists || (mode & OpenTruncate))
    {
        std::ofstream(p, std::ios::binary | std::ios::trunc);
    }
    Handle h;
    h.path = p;
    std::ios::openmode m = std::ios::binary | std::ios::in;
    if (mode & (OpenWrite | OpenCreate))
    {
        m |= std::ios::out;
    }
    h.file.open(p, m);
    if (!h.file.is_open())
    {
        return ResDenied;
    }
    int fd = g_nextHandle++;
    g_handles[fd] = std::move(h);
    return fd;
}

s32 GetDir(u32 port, const std::string& name, u32 more, u32 max, u32 table)
{
    Card& card = g_cards[port];
    if (more == 0)
    {
        card.listing.clear();
        card.listed = 0;
        size_t slash = name.find_last_of('/');
        std::string folder = slash == std::string::npos ? "" : name.substr(0, slash + 1);
        std::string pattern = slash == std::string::npos ? name : name.substr(slash + 1);
        if (folder.empty() && !name.empty() && name[0] == '/')
        {
            folder = "/";
        }
        fs::path dir = HostPath(port, folder);
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
        {
            return ResNoEntry;
        }
        std::vector<std::pair<std::string, fs::path>> found;
        // A subdirectory lists "." and ".." first, as the card does
        if (dir != PortFolder(port))
        {
            if (Match(pattern, "."))
            {
                found.emplace_back(".", dir);
            }
            if (Match(pattern, ".."))
            {
                found.emplace_back("..", dir.parent_path());
            }
        }
        std::vector<std::pair<std::string, fs::path>> entries;
        for (auto it = fs::directory_iterator(dir, ec); it != fs::directory_iterator(); it.increment(ec))
        {
            std::string entry = it->path().filename().string();
            if (entry[0] != '.' && Match(pattern, entry))
            {
                entries.emplace_back(entry, it->path());
            }
        }
        std::sort(entries.begin(), entries.end());
        found.insert(found.end(), entries.begin(), entries.end());
        card.listing = std::move(found);
    }
    u32 n = 0;
    while (card.listed < card.listing.size() && n < max)
    {
        const auto& [entry, host] = card.listing[card.listed++];
        if (table != 0)
        {
            PutEntry(Host((table + n * 64) & 0x1FFFFFF), entry, host);
        }
        n++;
    }
    return static_cast<s32>(n);
}

s32 Delete(u32 port, const std::string& name)
{
    fs::path p = HostPath(port, name);
    std::error_code ec;
    if (!fs::exists(p, ec))
    {
        return ResNoEntry;
    }
    if (fs::is_directory(p, ec) && !fs::is_empty(p, ec))
    {
        return ResNotEmpty;
    }
    for (auto it = g_handles.begin(); it != g_handles.end(); ++it)
    {
        if (it->second.path == p)
        {
            return ResDenied;
        }
    }
    return fs::remove(p, ec) ? 0 : ResDenied;
}

s32 SetFileInfo(u32 port, const std::string& name, u32 flags, u32 info)
{
    fs::path p = HostPath(port, name);
    std::error_code ec;
    if (!fs::exists(p, ec))
    {
        return ResNoEntry;
    }
    // 0x10 (sceMcFileInfoName): a new name, the table's at 32; the times (0x01, 0x02) and the attributes (0x04) are the host's.
    // A game saving safely writes its icon.sys as ~icon.sys and renames it last (CTTR does): a save whose rename is lost is
    // one the game calls damaged
    if ((flags & 0x10) && info != 0)
    {
        std::string to(reinterpret_cast<const char*>(Host((info + 32) & 0x1FFFFFF)), 0, 31);
        to = to.c_str();
        if (!to.empty() && to != p.filename().string())
        {
            fs::path dest = p.parent_path() / to;
            if (fs::exists(dest, ec))
            {
                return ResDenied;
            }
            fs::rename(p, dest, ec);
            if (ec)
            {
                return ResDenied;
            }
        }
    }
    return 0;
}

void Answer(RpcCall& c, s32 result)
{
    c.out.assign(std::max<u32>(c.recvSize, 4), 0);
    std::memcpy(c.out.data(), &result, 4);
}

}

void McServ(RpcCall& c)
{
    u32 port = c.In32(0) & 1;
    switch (c.fno)
    {
    case 0xFE: // init: the result, then MCSERV's and MCMAN's versions (libmc wants at least 0x20A and 0x20E)
        c.out.assign(std::max<u32>(c.recvSize, 12), 0);
        c.Out32(0, 0);
        c.Out32(4, 0x20A);
        c.Out32(8, 0x20E);
        return;
    case 0x01: // get info: port 4, slot 8, wants format 12, free 16, type 20; the EE's buffer at 28 gets type 0, free 4,
               // format 0x90
    {
        port = c.In32(4) & 1;
        u32 to = c.In32(28) & 0x1FFFFFF;
        if (!CardIn(port))
        {
            std::memset(Host(to), 0, 0x94);
            Answer(c, ResNoCard);
            return;
        }
        u32 type = 2; // a PS2 card
        u32 used = UsedClusters(port);
        u32 free = used < CardClusters ? CardClusters - used : 0;
        u32 format = 1;
        std::memcpy(Host(to), &type, 4);
        std::memcpy(Host(to + 4), &free, 4);
        std::memcpy(Host(to + 0x90), &format, 4);
        // The first call after the card goes in says it's a new card (formatted), as the hardware does after a boot
        s32 result = g_cards[port].seen ? 0 : -1;
        g_cards[port].seen = true;
        Answer(c, result);
        return;
    }
    case 0x02: // open: port 0, slot 4, mode 8, name 20
    {
        std::string name = c.InString(20, 1024);
        s32 fd = CardIn(port) ? Open(port, name, c.In32(8)) : ResNoCard;
        PS2_LOG(LogIop, "mc open %u:%s mode %x: %d", port, name.c_str(), c.In32(8), fd);
        Answer(c, fd);
        return;
    }
    case 0x03: // close: fd 0
    {
        auto it = g_handles.find(static_cast<int>(c.In32(0)));
        Answer(c, it == g_handles.end() ? ResDenied : 0);
        if (it != g_handles.end())
        {
            g_handles.erase(it);
        }
        return;
    }
    case 0x04: // seek: fd 0, offset 16, whence 20
    {
        auto it = g_handles.find(static_cast<int>(c.In32(0)));
        if (it == g_handles.end())
        {
            Answer(c, ResDenied);
            return;
        }
        std::fstream& f = it->second.file;
        f.clear();
        u32 whence = c.In32(20);
        s32 offset = static_cast<s32>(c.In32(16));
        f.seekg(offset, whence == 0 ? std::ios::beg : whence == 1 ? std::ios::cur : std::ios::end);
        f.seekp(f.tellg());
        Answer(c, static_cast<s32>(f.tellg()));
        return;
    }
    case 0x05: // read: fd 0, size 12, the EE's buffer 24, the unaligned ends' buffer 28 (left empty: all goes straight in)
    {
        auto it = g_handles.find(static_cast<int>(c.In32(0)));
        u32 ends = c.In32(28) & 0x1FFFFFF;
        if (ends != 0)
        {
            std::memset(Host(ends), 0, 16);
        }
        if (it == g_handles.end())
        {
            Answer(c, ResDenied);
            return;
        }
        std::fstream& f = it->second.file;
        f.clear();
        u32 size = c.In32(12);
        u32 to = c.In32(24) & 0x1FFFFFF;
        f.read(reinterpret_cast<char*>(Host(to)), size);
        s32 got = static_cast<s32>(f.gcount());
        f.clear();
        f.seekp(f.tellg());
        PS2_LOG(LogIop, "mc read %u: %u bytes: %d", c.In32(0), size, got);
        Answer(c, got);
        return;
    }
    case 0x06: // write: fd 0, the first bytes (to a 16 byte boundary) at 32 with their count at 20, then size 12 from the
               // EE's buffer at 24
    {
        auto it = g_handles.find(static_cast<int>(c.In32(0)));
        if (it == g_handles.end())
        {
            Answer(c, ResDenied);
            return;
        }
        std::fstream& f = it->second.file;
        f.clear();
        u32 head = std::min<u32>(c.In32(20), 16);
        u32 size = c.In32(12);
        u32 from = c.In32(24) & 0x1FFFFFF;
        u32 used = UsedClusters(port);
        if (used + (head + size + 1023) / 1024 > CardClusters)
        {
            Answer(c, ResFull);
            return;
        }
        f.write(reinterpret_cast<const char*>(c.in + 32), head);
        if (size != 0)
        {
            f.write(reinterpret_cast<const char*>(Host(from)), size);
        }
        f.flush();
        f.seekg(f.tellp());
        PS2_LOG(LogIop, "mc write %u: %u bytes", c.In32(0), head + size);
        Answer(c, f ? static_cast<s32>(head + size) : ResDenied);
        return;
    }
    case 0x0A: // flush: fd 0
        Answer(c, g_handles.count(static_cast<int>(c.In32(0))) ? 0 : ResDenied);
        return;
    case 0x0C: // change directory: port 0, the EE's buffer for the old one at 16, name 20
    {
        std::string name = c.InString(20, 1024);
        Card& card = g_cards[port];
        u32 to = c.In32(16) & 0x1FFFFFF;
        if (to != 0)
        {
            std::memset(Host(to), 0, 1024);
            std::memcpy(Host(to), card.cwd.c_str(), std::min<size_t>(card.cwd.size(), 1023));
        }
        fs::path p = HostPath(port, name);
        std::error_code ec;
        if (!CardIn(port) || !fs::is_directory(p, ec))
        {
            Answer(c, ResNoEntry);
            return;
        }
        std::string rel = fs::relative(p, PortFolder(port), ec).generic_string();
        card.cwd = rel == "." ? "/" : "/" + rel;
        Answer(c, 0);
        return;
    }
    case 0x0D: // get directory: port 0, continue 8, max entries 12, the EE's table 16, name 20
    {
        std::string name = c.InString(20, 1024);
        s32 n = CardIn(port) ? GetDir(port, name, c.In32(8), c.In32(12), c.In32(16)) : ResNoCard;
        PS2_LOG(LogIop, "mc getdir %u:%s: %d", port, name.c_str(), n);
        Answer(c, n);
        return;
    }
    case 0x0E: // set file info: port 0, flags 8, the EE's table 16, name 20
    {
        std::string name = c.InString(20, 1024);
        s32 r = CardIn(port) ? SetFileInfo(port, name, c.In32(8), c.In32(16)) : ResNoCard;
        PS2_LOG(LogIop, "mc setfileinfo %u:%s flags %#x: %d", port, name.c_str(), c.In32(8), r);
        Answer(c, r);
        return;
    }
    case 0x0F: // delete: port 0, name 20
    {
        std::string name = c.InString(20, 1024);
        s32 r = CardIn(port) ? Delete(port, name) : ResNoCard;
        PS2_LOG(LogIop, "mc delete %u:%s: %d", port, name.c_str(), r);
        Answer(c, r);
        return;
    }
    case 0x10: // format: port 4
    {
        port = c.In32(4) & 1;
        if (!CardIn(port))
        {
            Answer(c, ResNoCard);
            return;
        }
        g_handles.clear();
        std::error_code ec;
        for (auto it = fs::directory_iterator(PortFolder(port), ec); it != fs::directory_iterator(); it.increment(ec))
        {
            fs::remove_all(it->path(), ec);
        }
        g_cards[port].cwd = "/";
        Answer(c, 0);
        return;
    }
    case 0x14: // the server thread's priority
        Answer(c, 0);
        return;
    case 0x15: // slots per port
        Answer(c, 1);
        return;
    default:
        if (Once("mcserv " + std::to_string(c.fno)))
        {
            Log("mcserv: function %#x not done: answered 0", c.fno);
        }
        Answer(c, 0);
        return;
    }
}
}
