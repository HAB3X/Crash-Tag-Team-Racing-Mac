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
