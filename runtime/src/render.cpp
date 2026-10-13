// The render thread: the GS's side of the hardware (the GIF's three paths, the GS's registers, local memory and drawing, the
// PCRTC's picture) runs on a thread of its own while the game's thread runs the game, VIF1 and VU1. What reaches the GIF (VU1's
// XGKICK packets, VIF1's DIRECT data, the GIF channel's chains) is copied into a queue the render thread plays back in the same
// order, so the GS gets exactly what it got before; everything else that touches the GS's side (the privileged registers, the
// vblank's picture) is queued as work in its place among the data. The GS's interrupts (SIGNAL, FINISH) come from the render
// thread when it gets there, as the PS2's GS raises them when it's done.
//
// The queue is handed over in chunks: when one fills, at the end of each DMA transfer, and whenever the game reads CSR (it may
// be waiting for a FINISH or SIGNAL in data not handed over yet). The game's thread stays at most two pictures ahead.
//
// From the Crash Twinsanity native port's graphics/renderthread.cpp (same author). $PS2_RENDER_THREAD=0 keeps the GS on the
// game's thread.
#include "runtime.h"

#include "../../hw/ee/gif.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace Ps2
{
namespace
{
enum RecordKind : u32
{
    RecordTransfer,
    RecordKick,
    RecordCall,
};

struct Header
{
    u32 kind;
    u32 value;     // Transfer: the path; Kick: the kicking code's address; Call: the work's index
    u32 quadwords;
    u32 unused;
};
static_assert(sizeof(Header) == 16);

struct Chunk
{
    u8* bytes = nullptr;
    size_t size = 0;
    size_t capacity = 0;
    std::vector<std::function<void()>> calls;

    u8* Grow(size_t more)
    {
        if (size + more > capacity)
        {
            capacity = std::max(capacity * 2, size + more);
            bytes = static_cast<u8*>(std::realloc(bytes, capacity));
        }
        u8* at = bytes + size;
        size += more;
        return at;
    }
};

constexpr size_t ChunkBytes = 512 * 1024;
constexpr size_t KeptChunkBytes = 8 * 1024 * 1024;

class RenderThread final : public Ee::Gif::Forward
{
public:
    void Start()
    {
        thread_ = std::thread([this] { Run(); });
        Hw().gif->SetForward(this);
    }

    void Transfer(u32 path, const u8* data, u32 quadwords) override
    {
        if (quadwords != 0)
        {
            std::memcpy(Record(RecordTransfer, path, quadwords), data, static_cast<size_t>(quadwords) * 16);
        }
    }

    void Kick(const u8* vuMemory, u32 address, u32 quadwords, u32 source) override
    {
        // VU1's 16 KB ring from the address, wrapping at its end
        constexpr u32 Ring = 0x400;
        u8* to = Record(RecordKick, source, quadwords);
        u32 at = address & (Ring - 1);
        u32 first = std::min(quadwords, Ring - at);
        std::memcpy(to, vuMemory + at * 16, static_cast<size_t>(first) * 16);
        std::memcpy(to + first * 16, vuMemory, static_cast<size_t>(quadwords - first) * 16);
    }

    void Call(std::function<void()> work)
    {
        Record(RecordCall, 0, 0);
        Header* header = reinterpret_cast<Header*>(filling_->bytes + filling_->size - sizeof(Header));
        header->value = static_cast<u32>(filling_->calls.size());
        filling_->calls.push_back(std::move(work));
        Flush();
    }

    void Flush()
    {
        if (filling_ == nullptr || filling_->size == 0)
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(lock_);
            ready_.push_back(filling_);
            handed_++;
        }
        filling_ = nullptr;
        work_.notify_one();
    }

    void Sync()
    {
        Flush();
        std::unique_lock<std::mutex> lock(lock_);
        done_.wait(lock, [this] { return played_ == handed_; });
    }

    // A picture queued (its number), and waiting until the render thread is at most `behind` pictures behind
    u64 QueueFrame(std::function<void()> show)
    {
        u64 frame = ++framesQueued_;
        Call([this, frame, show = std::move(show)] {
            show();
            {
                std::lock_guard<std::mutex> lock(lock_);
                framesMade_ = frame;
            }
            done_.notify_all();
        });
        return frame;
    }

    void WaitBehind(u64 behind)
    {
        std::unique_lock<std::mutex> lock(lock_);
        done_.wait(lock, [this, behind] { return framesMade_ + behind >= framesQueued_; });
    }

private:
    Chunk* Take()
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (!spare_.empty())
        {
            Chunk* chunk = spare_.back();
            spare_.pop_back();
            return chunk;
        }
        Chunk* chunk = new Chunk();
        chunk->Grow(ChunkBytes);
        chunk->size = 0;
        return chunk;
    }

    u8* Record(u32 kind, u32 value, u32 quadwords)
    {
        size_t bytes = sizeof(Header) + static_cast<size_t>(quadwords) * 16;
        if (filling_ != nullptr && filling_->size != 0 && filling_->size + bytes > ChunkBytes)
        {
            Flush();
        }
        if (filling_ == nullptr)
        {
            filling_ = Take();
        }
        u8* at = filling_->Grow(bytes);
        Header header = {kind, value, quadwords, 0};
        std::memcpy(at, &header, sizeof(header));
        return at + sizeof(Header);
    }

    void Play(Chunk& chunk)
    {
        Ee::Gif& gif = *Hw().gif;
        const u8* at = chunk.bytes;
        const u8* end = chunk.bytes + chunk.size;
        while (at < end)
        {
            Header header;
            std::memcpy(&header, at, sizeof(header));
            at += sizeof(Header);
            switch (header.kind)
            {
            case RecordTransfer:
                gif.TransferNow(header.value, at, header.quadwords);
                break;
            case RecordKick:
                gif.KickRow(at, header.quadwords, header.value);
                break;
            case RecordCall:
                chunk.calls[header.value]();
                break;
            default:
                break;
            }
            at += static_cast<size_t>(header.quadwords) * 16;
        }
    }

    void Run()
    {
        ThreadOnPerformanceCores();
        for (;;)
        {
            Chunk* chunk;
            {
                std::unique_lock<std::mutex> lock(lock_);
                work_.wait(lock, [this] { return !ready_.empty(); });
                chunk = ready_.front();
                ready_.pop_front();
            }
            Play(*chunk);
            chunk->calls.clear();
            chunk->size = 0;
            if (chunk->capacity > KeptChunkBytes)
            {
                std::free(chunk->bytes);
                chunk->bytes = nullptr;
                chunk->capacity = 0;
            }
            {
                std::lock_guard<std::mutex> lock(lock_);
                spare_.push_back(chunk);
                played_++;
            }
            done_.notify_all();
        }
    }

    std::thread thread_;
    std::mutex lock_;
    std::condition_variable work_;
    std::condition_variable done_;
    std::deque<Chunk*> ready_;
    std::vector<Chunk*> spare_;
    u64 handed_ = 0;
    u64 played_ = 0;
    u64 framesQueued_ = 0;
    u64 framesMade_ = 0;
    Chunk* filling_ = nullptr;
};

RenderThread* g_render = nullptr;
}

void RenderStart()
{
    const char* setting = getenv("PS2_RENDER_THREAD");
    if (setting != nullptr && std::strcmp(setting, "0") == 0)
    {
        Log("graphics: the GS on the game's thread ($PS2_RENDER_THREAD=0)");
        return;
    }
    g_render = new RenderThread();
    g_render->Start();
}

void RenderCall(std::function<void()> work)
{
    if (g_render == nullptr)
    {
        work();
        return;
    }
    g_render->Call(std::move(work));
}

void RenderFlush()
{
    if (g_render != nullptr)
    {
        g_render->Flush();
    }
}

void RenderSync()
{
    if (g_render != nullptr)
    {
        g_render->Sync();
    }
}

void RenderFrame(std::function<void()> show)
{
    if (g_render == nullptr)
    {
        show();
        return;
    }
    g_render->QueueFrame(std::move(show));
    g_render->WaitBehind(2);
}
}
