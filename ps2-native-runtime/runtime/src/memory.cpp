// The EE's address space: a 4 GB reservation of the host's, with main memory mapped at its mirrors and the scratchpad at
// 0x70000000. A guest address is an offset into it, so a load is one host load. Addresses nothing is mapped at fault: the
// handler names the guest address.
#include "runtime.h"

#include <csignal>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

u8* ps2_mem = nullptr;

namespace Ps2
{
static void MapAt(int fd, u32 offset, u32 size)
{
    void* at = mmap(ps2_mem + offset, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (at == MAP_FAILED)
    {
        Fatal("couldn't map main memory at 0x%08x", offset);
    }
}

static void FaultHandler(int sig, siginfo_t* info, void*)
{
    u8* at = static_cast<u8*>(info->si_addr);
    if (ps2_mem != nullptr && at >= ps2_mem && at < ps2_mem + 0x100000000ull)
    {
        fprintf(stderr, "\n[ps2] %s: the guest touched unmapped address 0x%08llx\n", sig == SIGBUS ? "SIGBUS" : "SIGSEGV",
                static_cast<unsigned long long>(at - ps2_mem));
    }
    else
    {
        fprintf(stderr, "\n[ps2] %s at host address %p\n", sig == SIGBUS ? "SIGBUS" : "SIGSEGV", info->si_addr);
    }
    if (t_cpu != nullptr)
    {
        fprintf(stderr, "[ps2] ra=%08x sp=%08x a0=%08x a1=%08x v0=%08x\n", t_cpu->r[31].uw[0], t_cpu->r[29].uw[0],
                t_cpu->r[4].uw[0], t_cpu->r[5].uw[0], t_cpu->r[2].uw[0]);
    }
    void* frames[48];
    int n = backtrace(frames, 48);
    backtrace_symbols_fd(frames, n, 2);
    signal(sig, SIG_DFL);
    raise(sig);
}

void MemoryInit()
{
    void* base = mmap(nullptr, 0x100000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED)
    {
        Fatal("couldn't reserve the EE's address space");
    }
    ps2_mem = static_cast<u8*>(base);

    char name[64];
    snprintf(name, sizeof(name), "/ps2ram.%d", static_cast<int>(getpid()));
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
    {
        Fatal("shm_open failed");
    }
    shm_unlink(name);
    if (ftruncate(fd, RamSize) != 0)
    {
        Fatal("ftruncate failed");
    }
    for (u32 mirror : {0x00000000u, 0x20000000u, 0x30000000u, 0x80000000u, 0xA0000000u})
    {
        MapAt(fd, mirror, RamSize);
    }
    close(fd);
    // The scratchpad
    if (mmap(ps2_mem + ScratchpadBase, ScratchpadSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) ==
        MAP_FAILED)
    {
        Fatal("couldn't map the scratchpad");
    }

    struct sigaction sa = {};
    sa.sa_sigaction = FaultHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}

u8* DmaHost(u32 address)
{
    if (address & 0x80000000u)
    {
        return ps2_mem + ScratchpadBase + (address & (ScratchpadSize - 1));
    }
    return ps2_mem + (address & (RamSize - 1));
}

std::string ReadString(u32 address, u32 max)
{
    std::string out;
    for (u32 i = 0; i < max; i++)
    {
        char ch = static_cast<char>(ps2_mem[address + i]);
        if (ch == 0)
        {
            break;
        }
        out += ch;
    }
    return out;
}
}
