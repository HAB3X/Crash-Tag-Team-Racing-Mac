// The disc: an ISO 9660 image read in place, from a 2048 byte .iso or a raw 2352 byte .bin (MODE1 or MODE2 form 1, named by
// itself or by its .cue)
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
    Log("disc %s (%u byte sectors)", bin.c_str(), g_sectorSize);
    return true;
}

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
    return ReadRaw(lsn, count, out);
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
