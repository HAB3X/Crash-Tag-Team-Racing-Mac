#include "sfd.h"

namespace IpuTest
{
namespace
{
uint32_t Be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }

// A PES packet's header (MPEG-1's or MPEG-2's): where its payload starts
bool PayloadStart(const uint8_t* p, size_t length, size_t& start)
{
    size_t i = 0;
    if (length > 0 && (p[0] & 0xC0) == 0x80)
    {
        // MPEG-2: two flag bytes and the header data's length
        if (length < 3)
        {
            return false;
        }
        start = 3 + p[2];
        return start <= length;
    }
    while (i < length && p[i] == 0xFF)
    {
        i++; // stuffing
    }
    if (i < length && (p[i] & 0xC0) == 0x40)
    {
        i += 2; // STD buffer
    }
    if (i >= length)
    {
        return false;
    }
    if ((p[i] & 0xF0) == 0x20)
    {
        i += 5; // PTS
    }
    else if ((p[i] & 0xF0) == 0x30)
    {
        i += 10; // PTS and DTS
    }
    else if (p[i] == 0x0F)
    {
        i += 1;
    }
    else
    {
        return false;
    }
    start = i;
    return start <= length;
}
}

bool DemuxVideo(const std::vector<uint8_t>& file, std::vector<uint8_t>& video, std::string& error)
{
    size_t at = 0;
    size_t packs = 0;
    while (at + 4 <= file.size())
    {
        const uint8_t* p = file.data() + at;
        if (p[0] != 0 || p[1] != 0 || p[2] != 1)
        {
            error = "no start code at " + std::to_string(at);
            return false;
        }
        uint8_t id = p[3];
        if (id == 0xB9)
        {
            break; // the program's end
        }
        if (id == 0xBA)
        {
            packs++;
            if ((p[4] & 0xF0) == 0x20)
            {
                at += 12; // MPEG-1's pack header
            }
            else if ((p[4] & 0xC0) == 0x40)
            {
                at += 14 + (p[13] & 7); // MPEG-2's, with its stuffing
            }
            else
            {
                error = "a pack header of neither MPEG-1 nor MPEG-2 at " + std::to_string(at);
                return false;
            }
            continue;
        }
        if (id < 0xBB || at + 6 > file.size())
        {
            error = "stream id " + std::to_string(id) + " at " + std::to_string(at) + " isn't a packet's";
            return false;
        }
        size_t length = Be16(p + 4);
        if (at + 6 + length > file.size())
        {
            error = "a packet past the file's end at " + std::to_string(at);
            return false;
        }
        if (id == 0xE0)
        {
            size_t start;
            if (!PayloadStart(p + 6, length, start))
            {
                error = "a video packet's header at " + std::to_string(at);
                return false;
            }
            video.insert(video.end(), p + 6 + start, p + 6 + length);
        }
        at += 6 + length;
    }
    if (packs == 0 || video.empty())
    {
        error = "no MPEG program stream video (stream 0xE0) in it";
        return false;
    }
    return true;
}
}
