#include "runtime.h"

#include <mutex>
#include <execinfo.h>
#include <set>
#include <unistd.h>

namespace Ps2
{
u32 g_logMask = 0;
static std::mutex g_logLock;

void Log(const char* format, ...)
{
    std::lock_guard<std::mutex> lock(g_logLock);
    va_list args;
    va_start(args, format);
    fputs("[ps2] ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

void Fatal(const char* format, ...)
{
    {
        std::lock_guard<std::mutex> lock(g_logLock);
        va_list args;
        va_start(args, format);
        fputs("[ps2] fatal: ", stderr);
        vfprintf(stderr, format, args);
        fputc('\n', stderr);
        va_end(args);
        if (t_cpu != nullptr)
        {
            fprintf(stderr, "[ps2] ra=%08x sp=%08x a0=%08x a1=%08x a2=%08x v0=%08x v1=%08x\n", t_cpu->r[31].uw[0],
                    t_cpu->r[29].uw[0], t_cpu->r[4].uw[0], t_cpu->r[5].uw[0], t_cpu->r[6].uw[0], t_cpu->r[2].uw[0],
                    t_cpu->r[3].uw[0]);
        }
    }
    Backtrace();
    fflush(stderr);
    _exit(1);
}

// The host's call stack: recompiled functions are named by their guest addresses, so this is the guest's call stack too
void Backtrace()
{
    void* frames[64];
    int n = backtrace(frames, 64);
    char** names = backtrace_symbols(frames, n);
    std::string line;
    for (int i = 0; i < n; i++)
    {
        const char* f = strstr(names[i], "f_");
        if (f != nullptr && strlen(f) >= 10)
        {
            line += std::string(f, 10) + " ";
        }
    }
    fprintf(stderr, "[ps2] guest stack: %s\n", line.c_str());
    free(names);
}

bool Once(const std::string& key)
{
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> lock(g_logLock);
    return seen.insert(key).second;
}
}

#ifdef PS2_TRACE_ENTER
// The function entry counts of a PS2_TRACE_ENTER build: one counter for each word of the guest's 32 MB
static uint32_t g_enterCounts[1u << 23];

uint32_t ps2_watch_low = 0xFFFFFFFFu, ps2_watch_size = 0;
static int g_watchCount = 0;
// $PS2_TRACE_WATCH_SKIP=N: the first N stores are not logged (a start-up clearing the range would use up the 40);
// $PS2_TRACE_WATCH_MAX=N: log N stores (40)
static int g_watchSkip = -1, g_watchMax = 40;

void ps2_trace_store(uint32_t address, uint64_t value, int bytes)
{
    if (g_watchSkip < 0)
    {
        const char* skip = getenv("PS2_TRACE_WATCH_SKIP");
        g_watchSkip = skip ? atoi(skip) : 0;
        if (const char* max = getenv("PS2_TRACE_WATCH_MAX"))
        {
            g_watchMax = atoi(max);
        }
    }
    if (g_watchSkip > 0)
    {
        g_watchSkip--;
        return;
    }
    if (g_watchCount++ < g_watchMax)
    {
        Ps2::Log("store %d bytes to %08x = %llx", bytes, address, static_cast<unsigned long long>(value));
        Ps2::Backtrace();
    }
}

static void InitWatch()
{
    const char* r = getenv("PS2_TRACE_WATCH");
    if (r == nullptr)
    {
        return;
    }
    char* end;
    ps2_watch_low = static_cast<uint32_t>(strtoul(r, &end, 16));
    ps2_watch_size = *end == ':' ? static_cast<uint32_t>(strtoul(end + 1, nullptr, 16)) : 4;
}

// $PS2_TRACE_ARGS: comma separated hex addresses whose first entries (to 40) print their argument registers and RA
static uint32_t g_traceArgs[16];
static int g_traceArgCount = -1;

void ps2_trace_enter(uint32_t address)
{
    if (g_traceArgCount < 0)
    {
        g_traceArgCount = 0;
        InitWatch();
        const char* list = getenv("PS2_TRACE_ARGS");
        while (list != nullptr && *list != 0 && g_traceArgCount < 16)
        {
            char* end;
            g_traceArgs[g_traceArgCount++] = static_cast<uint32_t>(strtoul(list, &end, 16));
            list = *end == ',' ? end + 1 : end;
        }
    }
    // $PS2_TRACE_FIND=LOW-HIGH (hex): the first 80 entries with an argument register inside the range
    static uint32_t findLow = 1, findHigh = 0;
    static int findInit = 0, findCount = 0;
    if (!findInit)
    {
        findInit = 1;
        const char* r = getenv("PS2_TRACE_FIND");
        if (r != nullptr)
        {
            char* end;
            findLow = static_cast<uint32_t>(strtoul(r, &end, 16));
            findHigh = *end == '-' ? static_cast<uint32_t>(strtoul(end + 1, nullptr, 16)) : 0;
        }
    }
    if (findLow <= findHigh && findCount < 80 && Ps2::t_cpu != nullptr)
    {
        const Ps2Cpu* c = Ps2::t_cpu;
        for (int reg = 4; reg < 8; reg++)
        {
            uint32_t v = c->r[reg].uw[0];
            if (v >= findLow && v < findHigh)
            {
                findCount++;
                Ps2::Log("find %08x a%d=%08x (a0=%08x a1=%08x a2=%08x a3=%08x) ra=%08x", address, reg - 4, v, c->r[4].uw[0],
                         c->r[5].uw[0], c->r[6].uw[0], c->r[7].uw[0], c->r[31].uw[0]);
                break;
            }
        }
    }
    for (int i = 0; i < g_traceArgCount; i++)
    {
        if (g_traceArgs[i] == address && (g_enterCounts[(address & 0x1FFFFFFF) >> 2] < 40 || g_enterCounts[(address & 0x1FFFFFFF) >> 2] % 100 == 0) && Ps2::t_cpu != nullptr)
        {
            const Ps2Cpu* c = Ps2::t_cpu;
            Ps2::Log("enter %08x #%u a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x", address,
                     g_enterCounts[(address & 0x1FFFFFFF) >> 2], c->r[4].uw[0], c->r[5].uw[0], c->r[6].uw[0], c->r[7].uw[0],
                     c->r[31].uw[0]);
        }
    }
    uint32_t& n = g_enterCounts[(address & 0x1FFFFFFF) >> 2];
    if (n != 0xFFFFFFFFu)
    {
        n++;
    }
}

namespace Ps2
{
void TraceEnterDump()
{
    const char* path = getenv("PS2_TRACE_FILE");
    if (path == nullptr)
    {
        return;
    }
    FILE* f = fopen(path, "w");
    if (f == nullptr)
    {
        Log("PS2_TRACE_FILE %s: cannot write", path);
        return;
    }
    for (uint32_t i = 0; i < (1u << 23); i++)
    {
        if (g_enterCounts[i] != 0)
        {
            fprintf(f, "%08x %u\n", i << 2, g_enterCounts[i]);
        }
    }
    fclose(f);
}
}
#endif
