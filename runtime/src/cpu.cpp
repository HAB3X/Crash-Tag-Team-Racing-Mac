// Calls by address (the function table), COP0 and the FPU's control registers, and the instructions that stop the program
#include "runtime.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <sys/mman.h>

namespace Ps2
{
const Ps2FunctionEntry* FindFunction(u32 address)
{
    const Ps2FunctionEntry* begin = ps2_function_table;
    const Ps2FunctionEntry* end = ps2_function_table + ps2_function_count;
    const Ps2FunctionEntry* it =
        std::upper_bound(begin, end, address, [](u32 a, const Ps2FunctionEntry& e) { return a < e.start; });
    if (it == begin)
    {
        return nullptr;
    }
    --it;
    return address < it->end ? it : nullptr;
}

void Call(Ps2Cpu* c, u32 address) { ps2_call(c, address); }

namespace
{
bool MemoryHolds(u32 address, u32 word) { return *reinterpret_cast<const u32*>(ps2_mem + address) == word; }

bool HeadInMemory(const Ps2OverlayFunction& f)
{
    for (u32 n = 0; n < 4; n++)
    {
        if (!MemoryHolds(f.start + 4 * n, f.head[n]))
        {
            return false;
        }
    }
    return true;
}

bool OverlayInMemory(const Ps2Overlay& o)
{
    for (u32 n = 0; n < o.checkCount; n++)
    {
        if (!MemoryHolds(o.check[n].address, o.check[n].word))
        {
            return false;
        }
    }
    return true;
}
}

// The overlay function starting at an address whose overlay is in memory (recomp.h), or a stop that says what's there
const Ps2OverlayFunction* FindOverlayFunction(u32 address)
{
    const Ps2OverlayFunction* begin = ps2_overlay_function_table;
    const Ps2OverlayFunction* end = begin + ps2_overlay_function_count;
    const Ps2OverlayFunction* first =
        std::lower_bound(begin, end, address, [](const Ps2OverlayFunction& f, u32 a) { return f.start < a; });
    // The candidates whose first words are in memory; when several are (the same code at the same address in two
    // overlays), the one whose overlay's check words are
    const Ps2OverlayFunction* head = nullptr;
    const Ps2OverlayFunction* found = nullptr;
    int heads = 0;
    int overlays = 0;
    for (const Ps2OverlayFunction* f = first; f != end && f->start == address; ++f)
    {
        if (HeadInMemory(*f))
        {
            head = f;
            heads++;
            if (OverlayInMemory(ps2_overlays[f->overlay]))
            {
                found = f;
                overlays++;
            }
        }
    }
    if (heads <= 1)
    {
        return head;
    }
    if (overlays != 1)
    {
        Fatal("call to %08x: %d overlays have this function's first words and %d of them are in memory", address, heads,
              overlays);
    }
    return found;
}
}

using namespace Ps2;

extern "C" void ps2_call(Ps2Cpu* c, u32 address)
{
    const Ps2FunctionEntry* f = FindFunction(address & 0x1FFFFFFF);
    if (f == nullptr)
    {
        if (const Ps2OverlayFunction* o = FindOverlayFunction(address & 0x1FFFFFFF))
        {
            PS2_LOG(LogCall, "call %08x (%s)", address, ps2_overlays[o->overlay].file);
            c->depth++;
            o->function(c);
            c->depth--;
            return;
        }
        for (u32 n = 0; n < ps2_overlay_count; n++)
        {
            const Ps2Overlay& o = ps2_overlays[n];
            if ((address & 0x1FFFFFFF) >= o.base && (address & 0x1FFFFFFF) < o.end && OverlayInMemory(o))
            {
                Fatal("call to %08x: not a function start of %s, the overlay in memory there (add it to its extra functions)",
                      address, o.file);
            }
        }
    }
    if (f == nullptr || f->start != (address & 0x1FFFFFFF))
    {
        if (f != nullptr)
        {
            Fatal("call to %08x: inside the function at %08x, not a start (add it to the config's extra functions)", address,
                  f->start);
        }
        Fatal("call to %08x: no code there", address);
    }
    PS2_LOG(LogCall, "call %08x", address);
    c->depth++;
    f->function(c);
    c->depth--;
}

namespace
{
// The host's places kept for the guest's setjmp buffers, by their addresses
std::mutex g_jumpLock;
struct Place
{
    jmp_buf buf;
};
std::map<u32, std::unique_ptr<Place>> g_jumps;

// The host stacks of the guest's own stacks (by the guest SP they started at): a game's thread restarted on the same stack
// gets the same host stack, as the old one's frames are gone in the guest too
constexpr size_t HostStackSize = 2 << 20;
std::map<u32, u8*> g_stacks;

struct StackStart
{
    Ps2Cpu* c;
};

__attribute__((noreturn)) void RunOnStack(void* arg)
{
    Ps2Cpu* c = static_cast<StackStart*>(arg)->c;
    // JR RA at the bottom of the guest's stack: on at whatever RA says, each time the code returns here
    for (;;)
    {
        ps2_call(c, c->r[31].uw[0]);
    }
}

__attribute__((noreturn)) void CallOnHostStack(u8* top, void (*function)(void*), void* arg)
{
#if defined(__aarch64__)
    asm volatile("mov sp, %0\n\tmov x0, %2\n\tblr %1\n\tbrk #1" : : "r"(top), "r"(function), "r"(arg) : "memory");
#elif defined(__x86_64__)
    asm volatile("mov %0, %%rsp\n\tmov %2, %%rdi\n\tcall *%1\n\tud2" : : "r"(top), "r"(function), "r"(arg) : "memory");
#else
#error "a host stack switch for this processor"
#endif
    __builtin_unreachable();
}
}

extern "C" jmp_buf* ps2_setjmp_buffer(Ps2Cpu* c, u32 buffer)
{
    (void)c;
    std::lock_guard<std::mutex> lock(g_jumpLock);
    std::unique_ptr<Place>& place = g_jumps[buffer & 0x1FFFFFFF];
    if (!place)
    {
        place = std::make_unique<Place>();
    }
    return &place->buf;
}

extern "C" void ps2_longjmp(Ps2Cpu* c, u32 buffer)
{
    (void)c;
    jmp_buf* place = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_jumpLock);
        auto it = g_jumps.find(buffer & 0x1FFFFFFF);
        if (it != g_jumps.end())
        {
            place = &it->second->buf;
        }
    }
    if (place == nullptr)
    {
        Fatal("longjmp to %08x: no setjmp kept a place there (a setjmp reached by an indirect call?)", buffer);
    }
    _longjmp(*place, 1);
}

extern "C" void ps2_switch_stack(Ps2Cpu* c, u32 sp, u32 pc)
{
    u8* stack;
    {
        std::lock_guard<std::mutex> lock(g_jumpLock);
        u8*& slot = g_stacks[sp & 0x1FFFFFFF];
        if (slot == nullptr)
        {
            void* p = mmap(nullptr, HostStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (p == MAP_FAILED)
            {
                Fatal("no memory for a host stack for the guest's stack at %08x", sp);
            }
            // The lowest page left unmapped: an overflow faults instead of running into other memory
            mprotect(p, 16384, PROT_NONE);
            slot = static_cast<u8*>(p);
            PS2_LOG(LogThread, "a host stack for the guest's stack at %08x", sp);
        }
        stack = slot;
    }
    c->r[29].sd[0] = static_cast<s32>(sp);
    c->r[31].sd[0] = static_cast<s32>(pc);
    static thread_local StackStart start;
    start.c = c;
    CallOnHostStack(stack + HostStackSize, RunOnStack, &start);
}

extern "C" void ps2_undecodable(Ps2Cpu* c, u32 pc, u32 word)
{
    (void)c;
    Fatal("%08x: %08x isn't an instruction: this overlay's code decrypts itself as it runs, and the recompiled code is what's "
          "on the disc, still encrypted here",
          pc, word);
}

extern "C" void ps2_break(Ps2Cpu* c, u32 pc)
{
    (void)c;
    // A game's assert or its debugger stop: the program goes on, as it would with no debugger to stop it. Logged the first
    // time with where it was called from (a timeout's report may break on every turn of its loop)
    if (Once("break " + std::to_string(pc)))
    {
        Log("break at %08x (logged once)", pc);
        Backtrace();
    }
}

namespace
{
// COP0's EPC and ErrorEPC: only a game's own kernel-mode code uses them (the HLE kernel takes exceptions itself)
u32 g_epc = 0;
u32 g_errorEpc = 0;
}

extern "C" u32 ps2_eret(Ps2Cpu* c, u32 pc)
{
    u32 status = KernelStatus();
    bool error = (status & 4) != 0;
    u32 target = error ? g_errorEpc : g_epc;
    KernelSetStatus(c, status & ~(error ? 4u : 2u));
    PS2_LOG(LogKernel, "eret at %08x to %08x (%s)", pc, target, error ? "ErrorEPC" : "EPC");
    return target;
}

extern "C" u32 ps2_mfc0(Ps2Cpu* c, int reg)
{
    (void)c;
    switch (reg)
    {
    case 9: // Count: the core's clock, twice the bus's
        return static_cast<u32>(BusCycles() * 2);
    case 12:
        return KernelStatus();
    case 14:
        return g_epc;
    case 15: // PRId
        return 0x2E20;
    case 16: // Config
        return 0x440;
    case 30:
        return g_errorEpc;
    default:
        return 0;
    }
}

extern "C" void ps2_mtc0(Ps2Cpu* c, int reg, u32 value)
{
    switch (reg)
    {
    case 12:
        KernelSetStatus(c, value);
        return;
    case 14:
        g_epc = value;
        return;
    case 30:
        g_errorEpc = value;
        return;
    default:
        if (Once("mtc0 " + std::to_string(reg)))
        {
            PS2_LOG(LogKernel, "mtc0 $%d = %08x", reg, value);
        }
    }
}

extern "C" int ps2_cpcond0(Ps2Cpu* c)
{
    (void)c;
    // The DMAC's channels in D_PCR's CPC bits are all done: transfers finish when they're started
    return 1;
}

extern "C" u32 ps2_cfc1(Ps2Cpu* c, int reg)
{
    if (reg == 0)
    {
        return 0x2E30;
    }
    if (reg == 31)
    {
        return c->fcr31;
    }
    return 0;
}

extern "C" void ps2_ctc1(Ps2Cpu* c, int reg, u32 value)
{
    if (reg == 31)
    {
        c->fcr31 = value;
    }
}
