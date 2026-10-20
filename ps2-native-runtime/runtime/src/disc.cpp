// The disc: an ISO 9660 image read in place, from a 2048 byte .iso or a raw 2352 byte .bin (MODE1 or MODE2 form 1, named by
// itself or by its .cue). A patch file (--patch) replaces byte ranges of the disc's data as the sectors are read, so a game's
// files can be changed without a modified copy of the image: "DPAT", u32 version, u32 count, then count times u64 offset (in
// the image's 2048 byte sectors' data, sector * 2048 + byte), u32 length, in version 2 u32 variant, then the bytes. A variant
// says when a range is laid: 0 always, 1 while the keyboard and mouse are the device used last, 2 while a gamepad is (a game's
// pictures and font with its buttons' prompts drawn for each: what's read follows the device as it's read)
#include "runtime.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>

namespace Ps2
{
namespace
{
std::ifstream g_file;
std::mutex g_discLock;
u32 g_sectorSize = 2048;
u32 g_dataOffset = 0;
u32 g_rootLsn = 0;
u32 g_rootSize = 0;
bool g_dvd = false;

struct Patch
{
    u64 offset;
    u32 variant; // 0 always, 1 the keyboard and mouse, 2 a gamepad
    std::vector<u8> bytes;
};
std::vector<Patch> g_patches;

struct Entry
{
    u32 lsn;
    u32 size;
    bool dir;
    std::string name;
};

bool ReadRaw(u32 lsn, u32 count, u8* out)
{
    for (u32 i = 0; i < count; i++)
    {
        g_file.clear();
        g_file.seekg(static_cast<std::streamoff>(lsn + i) * g_sectorSize + g_dataOffset);
        g_file.read(reinterpret_cast<char*>(out + i * 2048), 2048);
        if (!g_file)
        {
            return false;
        }
    }
    return true;
}

std::vector<Entry> ListDir(u32 lsn, u32 size)
{
    std::vector<u8> data((size + 2047) & ~2047u);
    std::vector<Entry> out;
    if (!ReadRaw(lsn, static_cast<u32>(data.size() / 2048), data.data()))
    {
        return out;
    }
    u32 pos = 0;
    while (pos < size)
    {
        u8 len = data[pos];
        if (len == 0)
        {
            pos = (pos / 2048 + 1) * 2048;
            continue;
        }
        Entry e;
        std::memcpy(&e.lsn, &data[pos + 2], 4);
        std::memcpy(&e.size, &data[pos + 10], 4);
        e.dir = (data[pos + 25] & 2) != 0;
        u8 nameLen = data[pos + 32];
        e.name.assign(reinterpret_cast<const char*>(&data[pos + 33]), nameLen);
        size_t semi = e.name.find(';');
        if (semi != std::string::npos)
        {
            e.name.resize(semi);
        }
        if (!(nameLen == 1 && (e.name[0] == 0 || e.name[0] == 1)))
        {
            out.push_back(e);
        }
        pos += len;
    }
    return out;
}

std::string Upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return s;
}
}

bool DiscOpen(const std::string& path)
{
    std::string bin = path;
    if (bin.size() > 4 && Upper(bin.substr(bin.size() - 4)) == ".CUE")
    {
        std::ifstream cue(bin);
        std::string line;
        std::string folder = bin.substr(0, bin.find_last_of('/') + 1);
        while (std::getline(cue, line))
        {
            size_t q1 = line.find('"');
            size_t q2 = line.rfind('"');
            if (line.find("FILE") != std::string::npos && q1 != std::string::npos && q2 > q1)
            {
                bin = folder + line.substr(q1 + 1, q2 - q1 - 1);
                break;
            }
        }
    }
    g_file.open(bin, std::ios::binary);
    if (!g_file)
    {
        Log("can't open the disc image %s", bin.c_str());
        return false;
    }
    u8 head[16];
    g_file.read(reinterpret_cast<char*>(head), 16);
    static const u8 sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    if (std::memcmp(head, sync, 12) == 0)
    {
        g_sectorSize = 2352;
        g_dataOffset = head[15] == 2 ? 24 : 16;
    }
    u8 pvd[2048];
    if (!ReadRaw(16, 1, pvd) || std::memcmp(pvd + 1, "CD001", 5) != 0)
    {
        Log("%s: no ISO 9660 volume", bin.c_str());
        return false;
    }
    std::memcpy(&g_rootLsn, pvd + 156 + 2, 4);
    std::memcpy(&g_rootSize, pvd + 156 + 10, 4);
    // A PlayStation 2 DVD is an ISO 9660 and UDF bridge: UDF's volume recognition sequence (BEA01, NSR02 or NSR03, TEA01)
    // follows the ISO descriptors; a PlayStation 2 CD has none
    for (u32 lsn = 17; lsn < 32 && !g_dvd; lsn++)
    {
        u8 descriptor[2048];
        if (!ReadRaw(lsn, 1, descriptor))
        {
            break;
        }
        g_dvd = std::memcmp(descriptor + 1, "BEA01", 5) == 0;
    }
    Log("disc %s (%u byte sectors, a %s)", bin.c_str(), g_sectorSize, g_dvd ? "DVD" : "CD");
    return true;
}

bool DiscLoadPatch(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    u32 head[3];
    if (!f.read(reinterpret_cast<char*>(head), 12) || head[0] != 0x54415044 /* "DPAT" */ || (head[1] != 1 && head[1] != 2))
    {
        Log("--patch %s: not a disc patch file (version 1 or 2)", path.c_str());
        return false;
    }
    u64 bytes = 0;
    u32 variants[3] = {};
    for (u32 i = 0; i < head[2]; i++)
    {
        Patch p;
        u32 length;
        p.variant = 0;
        if (!f.read(reinterpret_cast<char*>(&p.offset), 8) || !f.read(reinterpret_cast<char*>(&length), 4) ||
            (head[1] == 2 && !f.read(reinterpret_cast<char*>(&p.variant), 4)))
        {
            Log("--patch %s: cut short at entry %u", path.c_str(), i);
            return false;
        }
        if (p.variant > 2)
        {
            Log("--patch %s: entry %u has variant %u (0 always, 1 keyboard, 2 gamepad)", path.c_str(), i, p.variant);
            return false;
        }
        variants[p.variant]++;
        p.bytes.resize(length);
        if (!f.read(reinterpret_cast<char*>(p.bytes.data()), length))
        {
            Log("--patch %s: cut short at entry %u", path.c_str(), i);
            return false;
        }
        bytes += length;
        g_patches.push_back(std::move(p));
    }
    Log("patch %s: %u ranges (%u for the keyboard, %u for a gamepad), %llu bytes", path.c_str(), head[2], variants[1],
        variants[2], static_cast<unsigned long long>(bytes));
    return true;
}

bool DiscIsDvd() { return g_dvd; }

bool DiscFind(const std::string& path, u32& lsn, u32& size)
{
    std::lock_guard<std::mutex> lock(g_discLock);
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    size_t semi = p.find(';');
    if (semi != std::string::npos)
    {
        p.resize(semi);
    }
    u32 curLsn = g_rootLsn;
    u32 curSize = g_rootSize;
    bool isDir = true;
    size_t start = 0;
    while (start < p.size())
    {
        size_t slash = p.find('/', start);
        std::string part = Upper(p.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        start = slash == std::string::npos ? p.size() : slash + 1;
        if (part.empty())
        {
            continue;
        }
        if (!isDir)
        {
            return false;
        }
        bool found = false;
        for (const Entry& e : ListDir(curLsn, curSize))
        {
            if (Upper(e.name) == part)
            {
                curLsn = e.lsn;
                curSize = e.size;
                isDir = e.dir;
                found = true;
                break;
            }
        }
        if (!found)
        {
            return false;
        }
    }
    lsn = curLsn;
    size = curSize;
    return true;
}

bool DiscReadSectors(u32 lsn, u32 count, u8* out)
{
    std::lock_guard<std::mutex> lock(g_discLock);
    if (!ReadRaw(lsn, count, out))
    {
        return false;
    }
    const u64 start = static_cast<u64>(lsn) * 2048, end = start + static_cast<u64>(count) * 2048;
    const u32 device = InputLastKeyboard() ? 1 : 2;
    for (const Patch& p : g_patches)
    {
        if (p.variant != 0 && p.variant != device)
        {
            continue;
        }
        const u64 from = std::max(start, p.offset), to = std::min(end, p.offset + p.bytes.size());
        if (from < to)
        {
            std::memcpy(out + (from - start), p.bytes.data() + (from - p.offset), to - from);
        }
    }
    return true;
}

bool DiscReadFile(const std::string& path, std::vector<u8>& out)
{
    u32 lsn, size;
    if (!DiscFind(path, lsn, size))
    {
        return false;
    }
    std::vector<u8> buf(((size + 2047) & ~2047u) + 2048);
    if (!DiscReadSectors(lsn, (size + 2047) / 2048, buf.data()))
    {
        return false;
    }
    out.assign(buf.begin(), buf.begin() + size);
    return true;
}
}
