// CRI's stream joints on the IOP (SJ/PS2IOP Ver.6.18 in the ADX driver's module): SJUNI, a chain of chunks, and SJRBF, a ring
// buffer, as the module's SJUNI_* (0x5A00-0x61C0) and SJRBF_* (0x5030-0x59A4) keep them
#include "cri_adx.h"

#include <algorithm>
#include <cstring>

namespace Ps2::CriAdx
{
namespace
{
constexpr u32 Lanes = 4;

// The library's error function (SJERR_CallErr with "SJUNI Error" or "SJRBF Error", code -3): the call is otherwise ignored
void Misuse(const char* kind, const char* call, u32 lane)
{
    char key[64];
    snprintf(key, sizeof(key), "cri sj %s %s %u", kind, call, lane);
    if (Once(key))
    {
        Log("CRI ADX: %s Error (-3): %s on lane %u, ignored as the driver does", kind, call, lane);
    }
}

// IOP memory, checked: a chunk the EE gave that runs past the IOP's 2 MB is the game's corruption, not something to wrap
u8* IopBytes(u32 address, u32 size)
{
    if (address > IopRamSize || size > IopRamSize - address)
    {
        Fatal("CRI ADX: a chunk at %08x (%u bytes) runs past the IOP's memory", address, size);
    }
    return IopMem() + address;
}

class SjUni final : public Sj
{
public:
    SjUni(u32 mode, s32 links) : m_mode(mode), m_links(links) {}

    void Reset() override
    {
        for (auto& lane : m_lanes)
        {
            lane.clear();
        }
        m_used = 0;
    }

    Chunk Get(u32 lane, s32 size) override
    {
        if (lane >= Lanes)
        {
            Misuse("SJUNI", "get", lane);
            return {};
        }
        auto& chunks = m_lanes[lane];
        if (chunks.empty())
        {
            return {};
        }
        Chunk& first = chunks.front();
        if (size >= first.size)
        {
            Chunk whole = first;
            chunks.pop_front();
            m_used--;
            return whole;
        }
        if (m_mode != 1)
        {
            return {};
        }
        // SJ_SplitChunk: the part asked for, the rest stays first
        Chunk part = {first.data, size};
        first = {first.data + static_cast<u32>(size), first.size - size};
        return part;
    }

    void Unget(u32 lane, Chunk chunk) override
    {
        if (lane >= Lanes)
        {
            Misuse("SJUNI", "unget", lane);
            return;
        }
        if (chunk.size <= 0 || chunk.data == 0)
        {
            return;
        }
        auto& chunks = m_lanes[lane];
        if (m_mode == 1 && !chunks.empty() && chunk.data + static_cast<u32>(chunk.size) == chunks.front().data)
        {
            chunks.front() = {chunk.data, chunks.front().size + chunk.size};
            return;
        }
        if (!TakeLink("unget", lane))
        {
            return;
        }
        chunks.push_front(chunk);
    }

    void Put(u32 lane, Chunk chunk) override
    {
        if (lane >= Lanes)
        {
            Misuse("SJUNI", "put", lane);
            return;
        }
        if (chunk.size <= 0 || chunk.data == 0)
        {
            return;
        }
        auto& chunks = m_lanes[lane];
        if (m_mode == 1 && !chunks.empty() && chunks.back().data + static_cast<u32>(chunks.back().size) == chunk.data)
        {
            chunks.back().size += chunk.size;
            return;
        }
        if (!TakeLink("put", lane))
        {
            return;
        }
        chunks.push_back(chunk);
    }

    s32 Data(u32 lane) const override
    {
        if (lane >= Lanes)
        {
            Misuse("SJUNI", "data", lane);
            return 0;
        }
        s32 total = 0;
        for (const Chunk& c : m_lanes[lane])
        {
            total += c.size;
        }
        return total;
    }

    bool IsGet(u32 lane, s32 size, s32& room) const override
    {
        room = 0;
        if (lane >= Lanes)
        {
            Misuse("SJUNI", "is-get", lane);
            return false;
        }
        if (m_lanes[lane].empty())
        {
            return false;
        }
        room = m_lanes[lane].front().size;
        return m_mode == 1 ? room >= size : room == size;
    }

private:
    // A link from the work area's chain for a new chunk: none left is the library's error
    bool TakeLink(const char* call, u32 lane)
    {
        if (m_used >= m_links)
        {
            Misuse("SJUNI", call, lane);
            return false;
        }
        m_used++;
        return true;
    }

    u32 m_mode;
    s32 m_links;
    s32 m_used = 0;
    std::array<std::deque<Chunk>, Lanes> m_lanes;
};

class SjRbf final : public Sj
{
public:
    SjRbf(u32 buffer, s32 size, s32 extra) : m_buffer(buffer), m_size(size), m_extra(extra) { Reset(); }

    void Reset() override
    {
        m_data = 0;
        m_room = m_size;
        m_roomAt = 0;
        m_dataAt = 0;
    }

    Chunk Get(u32 lane, s32 size) override
    {
        if (lane > 1)
        {
            Misuse("SJRBF", "get", lane);
            return {};
        }
        s32& count = lane == 0 ? m_room : m_data;
        s32& at = lane == 0 ? m_roomAt : m_dataAt;
        // As far as the buffer's end and the extra room after it
        s32 length = std::min({size, count, m_size - at + m_extra});
        Chunk chunk = {m_buffer + static_cast<u32>(at), length};
        at = (at + length) % m_size;
        count -= length;
        return chunk;
    }

    void Unget(u32 lane, Chunk chunk) override
    {
        if (chunk.size <= 0 || chunk.data == 0)
        {
            return;
        }
        if (lane > 1)
        {
            Misuse("SJRBF", "unget", lane);
            return;
        }
        s32& count = lane == 0 ? m_room : m_data;
        s32& at = lane == 0 ? m_roomAt : m_dataAt;
        s32 back = (at + m_size - chunk.size) % m_size;
        // Only the chunk got last, back where it was
        if (back != static_cast<s32>(chunk.data - m_buffer) % m_size)
        {
            Misuse("SJRBF", "unget", lane);
            return;
        }
        at = back;
        count += chunk.size;
    }

    void Put(u32 lane, Chunk chunk) override
    {
        if (chunk.size <= 0 || chunk.data == 0)
        {
            return;
        }
        if (lane == 0)
        {
            m_room += chunk.size;
            return;
        }
        if (lane != 1)
        {
            Misuse("SJRBF", "put", lane);
            return;
        }
        m_data += chunk.size;
        // Data written at the start is copied after the end (the extra room), and data written past the end to the start,
        // so a chunk got across the end reads straight on
        s32 offset = static_cast<s32>(chunk.data - m_buffer);
        if (offset < m_extra)
        {
            s32 n = std::min(m_extra - offset, chunk.size);
            std::memmove(IopBytes(m_buffer + static_cast<u32>(m_size + offset), n), IopBytes(chunk.data, n), n);
        }
        s32 end = offset + chunk.size;
        if (end > m_size)
        {
            s32 n = std::min(chunk.size, end - m_size);
            std::memmove(IopBytes(m_buffer, n), IopBytes(m_buffer + static_cast<u32>(end - n), n), n);
        }
    }

    s32 Data(u32 lane) const override
    {
        if (lane > 1)
        {
            Misuse("SJRBF", "data", lane);
            return 0;
        }
        return lane == 0 ? m_room : m_data;
    }

    bool IsGet(u32 lane, s32 size, s32& room) const override
    {
        room = 0;
        if (lane > 1)
        {
            Misuse("SJRBF", "is-get", lane);
            return false;
        }
        s32 count = lane == 0 ? m_room : m_data;
        s32 at = lane == 0 ? m_roomAt : m_dataAt;
        room = std::min({count, m_size - at + m_extra, size});
        return room == size;
    }

private:
    u32 m_buffer;
    s32 m_size;
    s32 m_extra;
    s32 m_data = 0;
    s32 m_room = 0;
    s32 m_roomAt = 0;
    s32 m_dataAt = 0;
};
}

std::unique_ptr<Sj> MakeSjUni(u32 mode, u32 work, s32 workSize)
{
    // SJUNI_Reset chains workSize / 16 links (one when that's none: the first is always set up)
    IopBytes(work, static_cast<u32>(std::max(workSize, 16)));
    return std::make_unique<SjUni>(mode, std::max(workSize / 16, 1));
}

std::unique_ptr<Sj> MakeSjRbf(u32 buffer, s32 size, s32 extra)
{
    if (size <= 0 || extra < 0)
    {
        Fatal("CRI ADX: a ring buffer SJ of %d bytes (%d extra)", size, extra);
    }
    IopBytes(buffer, static_cast<u32>(size + extra));
    return std::make_unique<SjRbf>(buffer, size, extra);
}
}
