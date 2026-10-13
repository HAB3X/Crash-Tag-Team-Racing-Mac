// The EE kernel, high level: its syscalls (threads, semaphores, interrupt handlers, alarms, the SIF's DMA and registers, the
// start-up calls the C runtime makes), done on the host.
//
// Threads are host threads, one per EE thread, and only one runs guest code at a time: the one the scheduler made current,
// the highest priority ready thread (lower numbers first, first come first served within a priority), as the EE kernel picks
// it. A thread gives the processor up only in the kernel (waiting, sleeping, or a higher priority thread made ready), as on
// the EE: there's no time slicing.
//
// Interrupts (the INTC's causes and the DMAC's channels) are raised by the emulated hardware and by a timer thread for the
// vertical blank and the alarms. The handlers run on whichever host thread holds the processor: the current thread at its
// next poll point (backward branches, the runtime's own calls), or, when every thread waits, the timer thread. A handler has
// its own register context and stack.
#include "runtime.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <map>
#include <mutex>
#include <thread>
#include <unistd.h>

volatile int ps2_irq_pending = 0;

namespace Ps2
{
thread_local Ps2Cpu* t_cpu = nullptr;
u32 g_intcStat = 0;
u32 g_intcMask = 0;
u32 g_dmacStat = 0;

namespace
{
enum class State
{
    Dormant,
    Ready,
    Running,
    Waiting,
};

enum class WaitKind
{
    None,
    Sleep,
    Sema,
};

struct Thread
{
    int id = 0;
    u32 entry = 0;
    u32 stack = 0;
    u32 stackSize = 0;
    u32 gp = 0;
    int initPriority = 0;
    int priority = 0;
    u32 attr = 0;
    u32 option = 0;
    State state = State::Dormant;
    WaitKind wait = WaitKind::None;
    int waitSema = 0;
    int wakeups = 0;
    bool suspended = false;
    bool terminated = false;
    bool started = false;
    u64 readySeq = 0;
    u32 arg = 0;
    Ps2Cpu cpu = {};
    std::condition_variable cv;
    jmp_buf exitJump;
    bool interruptsEnabled = true;
};

struct Sema
{
    int id = 0;
    int count = 0;
    int max = 0;
    int init = 0;
    u32 attr = 0;
    u32 option = 0;
    std::vector<Thread*> waiters;
};

struct Handler
{
    int id;
    u32 function;
    u32 arg;
    bool enabled;
};

struct Alarm
{
    int id;
    u64 due;   // in bus cycles
    u32 ticks; // the time asked for, in horizontal syncs
    u32 function;
    u32 arg;
};

std::mutex g_lock;
Thread* g_current = nullptr;
// While the timer thread runs handlers with no thread current
bool g_idleHandling = false;
std::map<int, Thread*> g_threads;
std::map<int, Sema*> g_semas;
int g_nextThread = 1;
int g_nextSema = 1;
u64 g_readySeq = 1;
std::vector<Handler> g_intcHandlers[15];
std::vector<Handler> g_dmacHandlers[16];
int g_nextHandler = 1;
std::vector<Alarm> g_alarms;
int g_nextAlarm = 1;
bool g_interruptsEnabled = true;
int g_inInterrupt = 0;
u32 g_heapEnd = 0;
u32 g_mainStack = 0;
u32 g_mainStackSize = 0;
u32 g_argsAddress = 0;
std::vector<std::string> g_args;
std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();
std::condition_variable g_idleCv;
// Pending events the processor holder deals with: vblank start and end
std::atomic<int> g_vblankStart{0};
std::atomic<int> g_vblankEnd{0};

// The kernel's syscall table, where libkernel looks for the handlers it installed with SetSyscall (it scans kernel memory for
// them to find the table): user handlers by number, also written into the table in low memory
constexpr u32 SyscallTable = 0x00012000;
std::map<int, u32> g_userSyscalls;
// SetVSyncFlag's addresses
u32 g_vsyncFlag = 0;
u32 g_vsyncCsr = 0;

// The handlers' contexts and stack (below the game's executable)
constexpr u32 InterruptStackTop = 0x000F0000;
Ps2Cpu g_interruptCpu[4];

constexpr int KE_OK = 0;
constexpr int KE_ERROR = -1;
constexpr int KE_SEMA_ZERO = -419;
constexpr int KE_UNKNOWN_THID = -406;
constexpr int KE_UNKNOWN_SEMID = -408;

u32 Arg(Ps2Cpu* c, int n)
{
    static const int regs[] = {4, 5, 6, 7, 8, 9, 10, 11};
    return c->r[regs[n]].uw[0];
}

void Ret(Ps2Cpu* c, s64 value) { c->r[2].sd[0] = value; }

void UpdatePending()
{
    bool any = (g_intcStat & g_intcMask) != 0 || (g_dmacStat & (g_dmacStat >> 16) & 0x3FF) != 0 ||
               g_vblankStart.load() != 0 || g_vblankEnd.load() != 0;
    if (!any)
    {
        u64 now = BusCycles();
        for (const Alarm& a : g_alarms)
        {
            if (a.due <= now)
            {
                any = true;
                break;
            }
        }
    }
    ps2_irq_pending = any ? 1 : 0;
}

Thread* BestReady()
{
    Thread* best = nullptr;
    for (auto& [id, t] : g_threads)
    {
        if (t->state != State::Ready || t->suspended)
        {
            continue;
        }
        if (best == nullptr || t->priority < best->priority ||
            (t->priority == best->priority && t->readySeq < best->readySeq))
        {
            best = t;
        }
    }
    return best;
}

void MakeReady(Thread* t, bool front = false)
{
    t->state = State::Ready;
    t->wait = WaitKind::None;
    t->readySeq = front ? 0 : g_readySeq++;
}

// The processor handed to the best ready thread (or to none: idle). Called with the lock held
void Dispatch()
{
    Thread* next = BestReady();
    g_current = next;
    if (next != nullptr)
    {
        next->state = State::Running;
        PS2_LOG(LogThread, "thread %d runs (priority %d)", next->id, next->priority);
        next->cv.notify_one();
    }
    else
    {
        PS2_LOG(LogThread, "idle");
        g_idleCv.notify_all();
    }
}

void LeaveThread(std::unique_lock<std::mutex>& lock, Thread* self);

// Waits until the thread is current again (the lock held)
void WaitTurn(std::unique_lock<std::mutex>& lock, Thread* self)
{
    while (g_current != self && !self->terminated)
    {
        self->cv.wait(lock);
    }
    if (self->terminated)
    {
        LeaveThread(lock, self);
    }
    g_interruptsEnabled = self->interruptsEnabled;
}

// The current thread stops running (it's waiting): the next one runs
void Block(std::unique_lock<std::mutex>& lock, Thread* self)
{
    self->interruptsEnabled = g_interruptsEnabled;
    Dispatch();
    WaitTurn(lock, self);
}

// A thread made ready may be better than the current one: the current one gives way (it stays ready, first of its priority)
void Preempt(std::unique_lock<std::mutex>& lock)
{
    Thread* self = g_current;
    if (self == nullptr || g_inInterrupt > 0)
    {
        return;
    }
    Thread* best = BestReady();
    if (best != nullptr && best->priority < self->priority)
    {
        self->interruptsEnabled = g_interruptsEnabled;
        MakeReady(self, true);
        Dispatch();
        WaitTurn(lock, self);
    }
}

void ThreadMain(Thread* t);

// The thread's host thread ends: back to its start (ThreadMain) with nothing of the guest's on the host stack
void LeaveThread(std::unique_lock<std::mutex>& lock, Thread* self)
{
    lock.unlock();
    longjmp(self->exitJump, 1);
}

void StartHostThread(Thread* t)
{
    std::thread([t] { ThreadMain(t); }).detach();
}

void ThreadMain(Thread* t)
{
    {
        std::unique_lock<std::mutex> lock(g_lock);
        WaitTurn(lock, t);
    }
    if (setjmp(t->exitJump) == 0)
    {
        t_cpu = &t->cpu;
        PS2_LOG(LogThread, "thread %d starts at %08x", t->id, t->entry);
        ps2_call(&t->cpu, t->entry);
        // Returned from its entry: ExitThread
        std::unique_lock<std::mutex> lock(g_lock);
        PS2_LOG(LogThread, "thread %d returned", t->id);
        t->state = State::Dormant;
        if (g_current == t)
        {
            Dispatch();
        }
    }
    t_cpu = nullptr;
}

// --- interrupts
void CallHandler(u32 function, u32 a0, u32 a1, u32 a2, u32 gp)
{
    Ps2Cpu* saved = t_cpu;
    Ps2Cpu* c = &g_interruptCpu[std::min(g_inInterrupt, 3)];
    std::memset(c, 0, sizeof(*c));
    c->r[4].sd[0] = static_cast<s32>(a0);
    c->r[5].sd[0] = static_cast<s32>(a1);
    c->r[6].sd[0] = static_cast<s32>(a2);
    c->r[28].sd[0] = static_cast<s32>(gp);
    c->r[29].sd[0] = static_cast<s32>(InterruptStackTop - 0x4000 * std::min(g_inInterrupt, 3) - 0x40);
    c->r[31].sd[0] = 0;
    t_cpu = c;
    ps2_call(c, function);
    t_cpu = saved;
}

u32 g_gp = 0;

// Runs what's pending (the lock held, released around the guest's handlers)
void ServiceLocked(std::unique_lock<std::mutex>& lock)
{
    if (g_vblankStart.exchange(0))
    {
        lock.unlock();
        GsVblank(true);
        lock.lock();
        g_intcStat |= 1u << 2;
        if (g_vsyncFlag != 0)
        {
            Write32(g_vsyncFlag, 1);
            u64 csr = GsCsr();
            std::memcpy(Host(g_vsyncCsr & 0x1FFFFFF8), &csr, 8);
            g_vsyncFlag = 0;
        }
    }
    if (g_vblankEnd.exchange(0))
    {
        lock.unlock();
        GsVblank(false);
        lock.lock();
        g_intcStat |= 1u << 3;
    }
    if (!g_interruptsEnabled || g_inInterrupt > 0)
    {
        UpdatePending();
        return;
    }
    g_inInterrupt++;
    for (int round = 0; round < 8; round++)
    {
        bool any = false;
        // The INTC's causes
        for (u32 cause = 0; cause < 15; cause++)
        {
            if (!(g_intcStat & g_intcMask & (1u << cause)))
            {
                continue;
            }
            g_intcStat &= ~(1u << cause);
            any = true;
            std::vector<Handler> handlers = g_intcHandlers[cause];
            for (const Handler& h : handlers)
            {
                if (!h.enabled)
                {
                    continue;
                }
                PS2_LOG(LogIrq, "intc %u handler %08x", cause, h.function);
                lock.unlock();
                CallHandler(h.function, cause, h.arg, 0, g_gp);
                s32 result = g_interruptCpu[std::min(g_inInterrupt - 1, 3)].r[2].sw[0];
                lock.lock();
                if (result == 0 && false)
                {
                    break;
                }
            }
        }
        // The DMAC's channels
        u32 dmaPending = g_dmacStat & (g_dmacStat >> 16) & 0x3FF;
        for (u32 ch = 0; ch < 10; ch++)
        {
            if (!(dmaPending & (1u << ch)))
            {
                continue;
            }
            g_dmacStat &= ~(1u << ch);
            any = true;
            std::vector<Handler> handlers = g_dmacHandlers[ch];
            for (const Handler& h : handlers)
            {
                if (!h.enabled)
                {
                    continue;
                }
                PS2_LOG(LogIrq, "dmac %u handler %08x", ch, h.function);
                lock.unlock();
                CallHandler(h.function, ch, h.arg, 0, g_gp);
                lock.lock();
            }
            if (ch == 5)
            {
                lock.unlock();
                SifReplyTaken();
                lock.lock();
            }
        }
        // Alarms due
        u64 now = BusCycles();
        for (size_t n = 0; n < g_alarms.size();)
        {
            if (g_alarms[n].due <= now)
            {
                Alarm a = g_alarms[n];
                g_alarms.erase(g_alarms.begin() + static_cast<long>(n));
                any = true;
                PS2_LOG(LogIrq, "alarm %d handler %08x", a.id, a.function);
                lock.unlock();
                CallHandler(a.function, static_cast<u32>(a.id), a.ticks, a.arg, g_gp);
                lock.lock();
            }
            else
            {
                n++;
            }
        }
        if (!any)
        {
            break;
        }
    }
    g_inInterrupt--;
    UpdatePending();
}

// The timer thread: vertical blanks at the TV's rate, alarms, and the handlers while every thread waits
double g_vblankRate = 59.94;

void TimerThread()
{
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    for (;;)
    {
        next += std::chrono::nanoseconds(static_cast<long long>(1e9 / g_vblankRate));
        // Sleep until the vblank, waking for alarms on the way
        std::this_thread::sleep_until(next);
        {
            std::unique_lock<std::mutex> lock(g_lock);
            g_vblankStart = 1;
            UpdatePending();
        }
        // The blank lasts about 22 lines of 525
        std::this_thread::sleep_for(std::chrono::microseconds(700));
        {
            std::unique_lock<std::mutex> lock(g_lock);
            g_vblankEnd = 1;
            UpdatePending();
            if (g_current == nullptr && !g_idleHandling)
            {
                // Nobody holds the processor: the handlers run here
                g_idleHandling = true;
                ServiceLocked(lock);
                g_idleHandling = false;
                if (g_current == nullptr)
                {
                    Dispatch();
                }
            }
        }
    }
}

Thread* Current() { return g_current; }

// --- the syscalls
Thread* FindThread(int id)
{
    if (id == 0)
    {
        return g_current;
    }
    auto it = g_threads.find(id);
    return it == g_threads.end() ? nullptr : it->second;
}

void SysCreateThread(Ps2Cpu* c)
{
    u32 p = Arg(c, 0);
    Thread* t = new Thread();
    t->id = g_nextThread++;
    t->entry = Read32(p + 4);
    t->stack = Read32(p + 8);
    t->stackSize = Read32(p + 12);
    t->gp = Read32(p + 16);
    t->initPriority = static_cast<s32>(Read32(p + 20));
    t->priority = t->initPriority;
    t->attr = Read32(p + 28);
    t->option = Read32(p + 32);
    g_threads[t->id] = t;
    PS2_LOG(LogThread, "CreateThread %d entry %08x stack %08x+%x priority %d", t->id, t->entry, t->stack, t->stackSize,
            t->priority);
    Ret(c, t->id);
}

void SysStartThread(std::unique_lock<std::mutex>& lock, Ps2Cpu* c)
{
    int id = static_cast<s32>(Arg(c, 0));
    Thread* old = FindThread(id);
    if (old == nullptr || old->state != State::Dormant)
    {
        Ret(c, KE_UNKNOWN_THID);
        return;
    }
    // A fresh host thread each start (a thread that ended left its own behind)
    Thread* t = old;
    if (old->started)
    {
        t = new Thread();
        t->id = old->id;
        t->entry = old->entry;
        t->stack = old->stack;
        t->stackSize = old->stackSize;
        t->gp = old->gp;
        t->initPriority = old->initPriority;
        t->attr = old->attr;
        t->option = old->option;
        g_threads[id] = t;
    }
    t->priority = t->initPriority;
    t->arg = Arg(c, 1);
    std::memset(&t->cpu, 0, sizeof(t->cpu));
    t->cpu.r[4].sd[0] = static_cast<s32>(t->arg);
    t->cpu.r[28].sd[0] = static_cast<s32>(t->gp);
    t->cpu.r[29].sd[0] = static_cast<s32>(t->stack + t->stackSize - 0x40);
    t->cpu.fcr31 = 0x01000001;
    t->cpu.thread = t;
    t->started = true;
    MakeReady(t);
    StartHostThread(t);
    PS2_LOG(LogThread, "StartThread %d arg %08x", id, t->arg);
    Ret(c, KE_OK);
    Preempt(lock);
}

void SysExitThread(std::unique_lock<std::mutex>& lock, Ps2Cpu* c, bool deleteIt)
{
    (void)c;
    Thread* self = g_current;
    PS2_LOG(LogThread, "ExitThread %d", self->id);
    self->state = State::Dormant;
    self->terminated = true;
    if (deleteIt)
    {
        g_threads.erase(self->id);
    }
    Dispatch();
    LeaveThread(lock, self);
}

void SysTerminateThread(Ps2Cpu* c)
{
    Thread* t = FindThread(static_cast<s32>(Arg(c, 0)));
    if (t == nullptr || t == g_current)
    {
        Ret(c, KE_UNKNOWN_THID);
        return;
    }
    PS2_LOG(LogThread, "TerminateThread %d", t->id);
    for (auto& [id, s] : g_semas)
    {
        s->waiters.erase(std::remove(s->waiters.begin(), s->waiters.end(), t), s->waiters.end());
    }
    t->state = State::Dormant;
    t->terminated = true;
    t->cv.notify_one();
    Ret(c, KE_OK);
}

void SysDeleteThread(Ps2Cpu* c)
{
    int id = static_cast<s32>(Arg(c, 0));
    Thread* t = FindThread(id);
    if (t == nullptr || t->state != State::Dormant)
    {
        Ret(c, KE_UNKNOWN_THID);
        return;
    }
    g_threads.erase(id);
    Ret(c, KE_OK);
}

void SysChangePriority(std::unique_lock<std::mutex>& lock, Ps2Cpu* c, bool fromInterrupt)
{
    Thread* t = FindThread(static_cast<s32>(Arg(c, 0)));
    if (t == nullptr)
    {
        Ret(c, KE_UNKNOWN_THID);
        return;
    }
    int old = t->priority;
    t->priority = static_cast<s32>(Arg(c, 1));
    PS2_LOG(LogThread, "ChangeThreadPriority %d: %d to %d", t->id, old, t->priority);
    if (t->state == State::Ready)
    {
        t->readySeq = g_readySeq++;
    }
    Ret(c, old);
    if (!fromInterrupt)
    {
        Preempt(lock);
    }
}

void SysRotate(std::unique_lock<std::mutex>& lock, Ps2Cpu* c, bool fromInterrupt)
{
    int priority = static_cast<s32>(Arg(c, 0));
    Thread* self = g_current;
    // The first ready thread of the priority goes to the back; the current thread of that priority gives way
    if (self != nullptr && self->priority == priority && !fromInterrupt)
    {
        Thread* other = nullptr;
        for (auto& [id, t] : g_threads)
        {
            if (t->state == State::Ready && !t->suspended && t->priority == priority)
            {
                other = t;
                break;
            }
        }
        if (other != nullptr)
        {
            self->interruptsEnabled = g_interruptsEnabled;
            MakeReady(self);
            Ret(c, KE_OK);
            Dispatch();
            WaitTurn(lock, self);
            return;
        }
    }
    else
    {
        Thread* first = nullptr;
        for (auto& [id, t] : g_threads)
        {
            if (t->state == State::Ready && t->priority == priority && (first == nullptr || t->readySeq < first->readySeq))
            {
                first = t;
            }
        }
        if (first != nullptr)
        {
            first->readySeq = g_readySeq++;
        }
    }
    Ret(c, KE_OK);
}

void SysSleepThread(std::unique_lock<std::mutex>& lock, Ps2Cpu* c)
{
    Thread* self = g_current;
    if (self->wakeups > 0)
    {
        self->wakeups--;
        Ret(c, KE_OK);
        return;
    }
    self->state = State::Waiting;
    self->wait = WaitKind::Sleep;
    PS2_LOG(LogThread, "SleepThread %d", self->id);
    Ret(c, KE_OK);
    Block(lock, self);
}

void SysWakeupThread(std::unique_lock<std::mutex>& lock, Ps2Cpu* c, bool fromInterrupt)
{
    Thread* t = FindThread(static_cast<s32>(Arg(c, 0)));
    if (t == nullptr)
    {
        Ret(c, KE_UNKNOWN_THID);
        return;
    }
    if (t->state == State::Waiting && t->wait == WaitKind::Sleep)
    {
        MakeReady(t);
    }
    else
    {
        t->wakeups++;
    }
    Ret(c, KE_OK);
    if (!fromInterrupt)
    {
        Preempt(lock);
    }
}

void SysReferThreadStatus(Ps2Cpu* c)
{
    Thread* t = FindThread(static_cast<s32>(Arg(c, 0)));
    u32 p = Arg(c, 1);
    if (t == nullptr)
    {
        Ret(c, KE_UNKNOWN_THID);
        return;
    }
    if (p != 0)
    {
        u32 status = t->state == State::Running ? 1 : t->state == State::Ready ? 2 : t->state == State::Waiting ? 4 : 16;
        if (t->suspended)
        {
            status |= 8;
        }
        u32 words[12] = {status, t->entry, t->stack, t->stackSize, t->gp, static_cast<u32>(t->initPriority),
                         static_cast<u32>(t->priority), t->attr, t->option,
                         t->wait == WaitKind::Sleep ? 1u : t->wait == WaitKind::Sema ? 2u : 0u,
                         static_cast<u32>(t->waitSema), static_cast<u32>(t->wakeups)};
        std::memcpy(Host(p), words, sizeof(words));
    }
    u32 status = t->state == State::Running ? 1 : t->state == State::Ready ? 2 : t->state == State::Waiting ? 4 : 16;
    Ret(c, status);
}

void SysCreateSema(Ps2Cpu* c)
{
    u32 p = Arg(c, 0);
    Sema* s = new Sema();
    s->id = g_nextSema++;
    s->count = static_cast<s32>(Read32(p + 8));
    s->max = static_cast<s32>(Read32(p + 4));
    s->init = s->count;
    s->attr = Read32(p + 16);
    s->option = Read32(p + 20);
    g_semas[s->id] = s;
    PS2_LOG(LogThread, "CreateSema %d count %d max %d", s->id, s->count, s->max);
    Ret(c, s->id);
}

Sema* FindSema(int id)
{
    auto it = g_semas.find(id);
    return it == g_semas.end() ? nullptr : it->second;
}

void SysSignalSema(std::unique_lock<std::mutex>& lock, Ps2Cpu* c, bool fromInterrupt)
{
    Sema* s = FindSema(static_cast<s32>(Arg(c, 0)));
    if (s == nullptr)
    {
        Ret(c, KE_UNKNOWN_SEMID);
        return;
    }
    if (!s->waiters.empty())
    {
        Thread* t = s->waiters.front();
        s->waiters.erase(s->waiters.begin());
        MakeReady(t);
    }
    else if (s->count < s->max || s->max <= 0)
    {
        s->count++;
    }
    Ret(c, s->id);
    if (!fromInterrupt)
    {
        Preempt(lock);
    }
}

void SysWaitSema(std::unique_lock<std::mutex>& lock, Ps2Cpu* c)
{
    Sema* s = FindSema(static_cast<s32>(Arg(c, 0)));
    if (s == nullptr)
    {
        Ret(c, KE_UNKNOWN_SEMID);
        return;
    }
    Ret(c, s->id);
    if (s->count > 0)
    {
        s->count--;
        return;
    }
    Thread* self = g_current;
    if (self == nullptr)
    {
        Fatal("WaitSema %d with no thread (in a handler?)", s->id);
    }
    self->state = State::Waiting;
    self->wait = WaitKind::Sema;
    self->waitSema = s->id;
    s->waiters.push_back(self);
    Block(lock, self);
}

void SysPollSema(Ps2Cpu* c)
{
    Sema* s = FindSema(static_cast<s32>(Arg(c, 0)));
    if (s == nullptr)
    {
        Ret(c, KE_UNKNOWN_SEMID);
        return;
    }
    if (s->count > 0)
    {
        s->count--;
        Ret(c, s->id);
    }
    else
    {
        Ret(c, KE_SEMA_ZERO);
    }
}

void SysReferSema(Ps2Cpu* c)
{
    Sema* s = FindSema(static_cast<s32>(Arg(c, 0)));
    u32 p = Arg(c, 1);
    if (s == nullptr)
    {
        Ret(c, KE_UNKNOWN_SEMID);
        return;
    }
    if (p != 0)
    {
        u32 words[6] = {static_cast<u32>(s->count), static_cast<u32>(s->max), static_cast<u32>(s->init),
                        static_cast<u32>(s->waiters.size()), s->attr, s->option};
        std::memcpy(Host(p), words, sizeof(words));
    }
    Ret(c, s->id);
}

int AddHandler(std::vector<Handler>& list, u32 function, int next, u32 arg)
{
    Handler h = {g_nextHandler++, function, arg, true};
    if (next < 0)
    {
        list.push_back(h);
    }
    else
    {
        list.insert(list.begin(), h);
    }
    return h.id;
}

bool RemoveHandler(std::vector<Handler>* lists, int count, int cause, int id)
{
    if (cause < 0 || cause >= count)
    {
        return false;
    }
    auto& list = lists[cause];
    for (auto it = list.begin(); it != list.end(); ++it)
    {
        if (it->id == id)
        {
            list.erase(it);
            return true;
        }
    }
    return false;
}

void SetHandlerEnabled(std::vector<Handler>* lists, int count, int cause, int id, bool enabled)
{
    if (cause < 0 || cause >= count)
    {
        return;
    }
    for (Handler& h : lists[cause])
    {
        if (h.id == id)
        {
            h.enabled = enabled;
        }
    }
}

const char* SyscallName(int n)
{
    switch (n)
    {
    case 0x02: return "SetGsCrt";
    case 0x04: return "Exit";
    case 0x10: return "AddIntcHandler";
    case 0x11: return "RemoveIntcHandler";
    case 0x12: return "AddDmacHandler";
    case 0x13: return "RemoveDmacHandler";
    case 0x14: return "EnableIntc";
    case 0x15: return "DisableIntc";
    case 0x16: return "EnableDmac";
    case 0x17: return "DisableDmac";
    case 0x18: return "SetAlarm";
    case 0x19: return "ReleaseAlarm";
    case 0x20: return "CreateThread";
    case 0x22: return "StartThread";
    case 0x29: return "ChangeThreadPriority";
    case 0x2F: return "GetThreadId";
    case 0x3C: return "SetupThread";
    case 0x3D: return "SetupHeap";
    case 0x40: return "CreateSema";
    case 0x64: return "FlushCache";
    case 0x71: return "GsPutIMR";
    case 0x77: return "SifSetDma";
    case 0x78: return "SifSetDChain";
    case 0x79: return "SifSetReg";
    case 0x7A: return "SifGetReg";
    default: return "";
    }
}
}

// ----------------------------------------------------------------------------------------------------------------------------
u64 BusCycles()
{
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_start).count();
    return static_cast<u64>(static_cast<double>(ns) * 0.147456);
}

bool InInterrupt() { return g_inInterrupt > 0; }

bool InterruptsEnabled() { return g_interruptsEnabled; }

void KernelDumpThreads()
{
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto& [id, t] : g_threads)
    {
        const char* st = t->state == State::Running ? "running" : t->state == State::Ready ? "ready"
                         : t->state == State::Waiting ? "waiting" : "dormant";
        Log("thread %d: %s%s priority %d entry %08x wait %s sema %d wakeups %d", id, st, t->suspended ? " suspended" : "",
            t->priority, t->entry, t->wait == WaitKind::Sleep ? "sleep" : t->wait == WaitKind::Sema ? "sema" : "-",
            t->waitSema, t->wakeups);
    }
    for (auto& [id, s] : g_semas)
    {
        if (!s->waiters.empty())
        {
            Log("sema %d: count %d, %zu waiting", id, s->count, s->waiters.size());
        }
    }
}

void KernelSignalSema(int id)
{
    std::lock_guard<std::mutex> lock(g_lock);
    Sema* s = FindSema(id);
    if (s == nullptr)
    {
        Log("the IOP signalled unknown semaphore %d", id);
        return;
    }
    if (!s->waiters.empty())
    {
        Thread* t = s->waiters.front();
        s->waiters.erase(s->waiters.begin());
        MakeReady(t);
    }
    else if (s->count < s->max || s->max <= 0)
    {
        s->count++;
    }
}

void RaiseIntc(u32 cause)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_intcStat |= 1u << cause;
    UpdatePending();
}

void RaiseDmac(u32 channel)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_dmacStat |= 1u << channel;
    UpdatePending();
}

void KernelInit(u32 entry, u32 gp, const std::vector<std::string>& args)
{
    (void)entry;
    g_gp = gp;
    g_args = args;
}

void KernelRunMain()
{
    Thread* t = new Thread();
    t->id = g_nextThread++;
    t->priority = 0;
    t->initPriority = 0;
    t->state = State::Running;
    t->cpu.thread = t;
    t->cpu.fcr31 = 0x01000001;
    g_threads[t->id] = t;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_current = t;
    }
    std::thread(TimerThread).detach();
    t_cpu = &t->cpu;
    if (setjmp(t->exitJump) == 0)
    {
        extern u32 g_entryPoint;
        ps2_call(&t->cpu, g_entryPoint);
        Log("the main thread returned from the entry point");
    }
}

u32 g_entryPoint = 0;
}

using namespace Ps2;

extern "C" void ps2_irq_service(Ps2Cpu* c)
{
    (void)c;
    std::unique_lock<std::mutex> lock(g_lock);
    if (g_inInterrupt > 0)
    {
        return;
    }
    ServiceLocked(lock);
    Preempt(lock);
}

extern "C" void ps2_ei(Ps2Cpu* c)
{
    g_interruptsEnabled = true;
    if (ps2_irq_pending)
    {
        ps2_irq_service(c);
    }
}

extern "C" void ps2_di(Ps2Cpu* c)
{
    (void)c;
    g_interruptsEnabled = false;
}

extern "C" void ps2_syscall(Ps2Cpu* c, u32 pc)
{
    s32 number = c->r[3].sw[0];
    bool fromInterrupt = number < 0;
    int n = number < 0 ? -number : number;
    PS2_LOG(LogSyscall, "syscall %s(0x%x) a0=%08x a1=%08x a2=%08x a3=%08x at %08x", SyscallName(n), number, Arg(c, 0),
            Arg(c, 1), Arg(c, 2), Arg(c, 3), pc);
    auto user = g_userSyscalls.find(n);
    if (user != g_userSyscalls.end())
    {
        // A handler the game installed: it runs as the kernel would run it, on the caller's registers
        ps2_call(c, user->second);
        return;
    }
    std::unique_lock<std::mutex> lock(g_lock);
    switch (n)
    {
    case 0x01: // ResetEE
        Ret(c, 0);
        break;
    case 0x02: // SetGsCrt
        lock.unlock();
        GsSetCrt(Arg(c, 0), Arg(c, 1), Arg(c, 2));
        lock.lock();
        Ret(c, 0);
        break;
    case 0x04: // Exit
        Log("the game called Exit(%d)", static_cast<s32>(Arg(c, 0)));
        Backtrace();
        fflush(stderr);
        _exit(0);
    case 0x10: // AddIntcHandler(cause, handler, next, arg)
    {
        u32 cause = Arg(c, 0);
        if (cause >= 15)
        {
            Ret(c, KE_ERROR);
            break;
        }
        int id = AddHandler(g_intcHandlers[cause], Arg(c, 1), static_cast<s32>(Arg(c, 2)), Arg(c, 3));
        PS2_LOG(LogIrq, "AddIntcHandler cause %u function %08x id %d", cause, Arg(c, 1), id);
        Ret(c, id);
        break;
    }
    case 0x11:
        Ret(c, RemoveHandler(g_intcHandlers, 15, static_cast<s32>(Arg(c, 0)), static_cast<s32>(Arg(c, 1))) ? 0 : -1);
        break;
    case 0x12: // AddDmacHandler(channel, handler, next, arg)
    {
        u32 ch = Arg(c, 0);
        if (ch >= 16)
        {
            Ret(c, KE_ERROR);
            break;
        }
        int id = AddHandler(g_dmacHandlers[ch], Arg(c, 1), static_cast<s32>(Arg(c, 2)), Arg(c, 3));
        PS2_LOG(LogIrq, "AddDmacHandler channel %u function %08x id %d", ch, Arg(c, 1), id);
        Ret(c, id);
        break;
    }
    case 0x13:
        Ret(c, RemoveHandler(g_dmacHandlers, 16, static_cast<s32>(Arg(c, 0)), static_cast<s32>(Arg(c, 1))) ? 0 : -1);
        break;
    case 0x14: // EnableIntc
    case 0x1A:
    {
        u32 bit = 1u << (Arg(c, 0) & 15);
        bool was = (g_intcMask & bit) != 0;
        g_intcMask |= bit;
        UpdatePending();
        Ret(c, was ? 0 : 1);
        break;
    }
    case 0x15: // DisableIntc
    case 0x1B:
    {
        u32 bit = 1u << (Arg(c, 0) & 15);
        bool was = (g_intcMask & bit) != 0;
        g_intcMask &= ~bit;
        Ret(c, was ? 1 : 0);
        break;
    }
    case 0x16: // EnableDmac
    case 0x1C:
    {
        u32 bit = 1u << (16 + (Arg(c, 0) & 15));
        bool was = (g_dmacStat & bit) != 0;
        g_dmacStat |= bit;
        UpdatePending();
        Ret(c, was ? 0 : 1);
        break;
    }
    case 0x17: // DisableDmac
    case 0x1D:
    {
        u32 bit = 1u << (16 + (Arg(c, 0) & 15));
        bool was = (g_dmacStat & bit) != 0;
        g_dmacStat &= ~bit;
        Ret(c, was ? 1 : 0);
        break;
    }
    case 0x18: // SetAlarm(ticks in hsyncs, handler, arg)
    case 0x1E:
    {
        Alarm a;
        a.id = g_nextAlarm++;
        a.ticks = Arg(c, 0) & 0xFFFF;
        // NTSC: 15734 lines a second; 147456000 / 15734 = 9372 bus cycles a line
        a.due = BusCycles() + static_cast<u64>(a.ticks) * 9372;
        a.function = Arg(c, 1);
        a.arg = Arg(c, 2);
        g_alarms.push_back(a);
        Ret(c, a.id);
        break;
    }
    case 0x19: // ReleaseAlarm
    case 0x1F:
    {
        int id = static_cast<s32>(Arg(c, 0));
        auto it = std::find_if(g_alarms.begin(), g_alarms.end(), [&](const Alarm& a) { return a.id == id; });
        if (it != g_alarms.end())
        {
            g_alarms.erase(it);
            Ret(c, 0);
        }
        else
        {
            Ret(c, -1);
        }
        break;
    }
    case 0x20:
        SysCreateThread(c);
        break;
    case 0x21:
        SysDeleteThread(c);
        break;
    case 0x22:
        SysStartThread(lock, c);
        break;
    case 0x23:
        SysExitThread(lock, c, false);
        break;
    case 0x24:
        SysExitThread(lock, c, true);
        break;
    case 0x25:
    case 0x26:
        SysTerminateThread(c);
        break;
    case 0x29:
    case 0x2A:
        SysChangePriority(lock, c, n == 0x2A || fromInterrupt);
        break;
    case 0x2B:
    case 0x2C:
        SysRotate(lock, c, n == 0x2C || fromInterrupt || g_inInterrupt > 0);
        break;
    case 0x2D: // ReleaseWaitThread
    case 0x2E:
    {
        Thread* t = FindThread(static_cast<s32>(Arg(c, 0)));
        if (t != nullptr && t->state == State::Waiting)
        {
            for (auto& [id, s] : g_semas)
            {
                s->waiters.erase(std::remove(s->waiters.begin(), s->waiters.end(), t), s->waiters.end());
            }
            MakeReady(t);
        }
        Ret(c, 0);
        if (n == 0x2D && !fromInterrupt)
        {
            Preempt(lock);
        }
        break;
    }
    case 0x2F: // GetThreadId
        Ret(c, g_current != nullptr ? g_current->id : 0);
        break;
    case 0x30:
    case 0x31:
        SysReferThreadStatus(c);
        break;
    case 0x32:
        SysSleepThread(lock, c);
        break;
    case 0x33:
    case 0x34:
        SysWakeupThread(lock, c, n == 0x34 || fromInterrupt || g_inInterrupt > 0);
        break;
    case 0x35: // CancelWakeupThread
    case 0x36:
    {
        Thread* t = FindThread(static_cast<s32>(Arg(c, 0)));
        int w = t != nullptr ? t->wakeups : 0;
        if (t != nullptr)
        {
            t->wakeups = 0;
        }
        Ret(c, w);
        break;
    }
    case 0x37: // SuspendThread
    case 0x38:
    {
        Thread* t = FindThread(static_cast<s32>(Arg(c, 0)));
        if (t != nullptr)
        {
            t->suspended = true;
        }
        Ret(c, 0);
        if (t == g_current && g_inInterrupt == 0)
        {
            Block(lock, t);
        }
        break;
    }
    case 0x39: // ResumeThread
    case 0x3A:
    {
        Thread* t = FindThread(static_cast<s32>(Arg(c, 0)));
        if (t != nullptr && t->suspended)
        {
            t->suspended = false;
            if (n == 0x39 && !fromInterrupt)
            {
                Ret(c, 0);
                Preempt(lock);
                break;
            }
        }
        Ret(c, 0);
        break;
    }
    case 0x3C: // SetupThread(gp, stack, stack size, args, root)
    {
        u32 stack = Arg(c, 1);
        u32 size = Arg(c, 2);
        if (stack == 0xFFFFFFFFu)
        {
            stack = RamSize - size;
        }
        g_mainStack = stack;
        g_mainStackSize = size;
        g_gp = Arg(c, 0);
        g_argsAddress = Arg(c, 3);
        if (g_argsAddress != 0)
        {
            // struct { int argc; char* argv[16]; char text[256]; }
            u32 text = g_argsAddress + 4 + 16 * 4;
            u32 at = text;
            Write32(g_argsAddress, static_cast<u32>(g_args.size()));
            for (size_t i = 0; i < g_args.size() && i < 16; i++)
            {
                Write32(g_argsAddress + 4 + 4 * static_cast<u32>(i), at);
                std::memcpy(Host(at), g_args[i].c_str(), g_args[i].size() + 1);
                at += static_cast<u32>(g_args[i].size()) + 1;
            }
        }
        PS2_LOG(LogKernel, "SetupThread gp %08x stack %08x+%x", g_gp, stack, size);
        Ret(c, stack + size);
        break;
    }
    case 0x3D: // SetupHeap(start, size)
    {
        u32 start = Arg(c, 0);
        u32 size = Arg(c, 1);
        g_heapEnd = size == 0xFFFFFFFFu ? g_mainStack : start + size;
        PS2_LOG(LogKernel, "SetupHeap %08x to %08x", start, g_heapEnd);
        Ret(c, g_heapEnd);
        break;
    }
    case 0x3E: // EndOfHeap
        Ret(c, g_heapEnd);
        break;
    case 0x40:
        SysCreateSema(c);
        break;
    case 0x41: // DeleteSema
    {
        int id = static_cast<s32>(Arg(c, 0));
        Sema* s = FindSema(id);
        if (s != nullptr)
        {
            for (Thread* t : s->waiters)
            {
                MakeReady(t);
            }
            g_semas.erase(id);
        }
        Ret(c, s != nullptr ? id : KE_UNKNOWN_SEMID);
        break;
    }
    case 0x42:
    case 0x43:
        SysSignalSema(lock, c, n == 0x43 || fromInterrupt || g_inInterrupt > 0);
        break;
    case 0x44:
        SysWaitSema(lock, c);
        break;
    case 0x45:
    case 0x46:
        SysPollSema(c);
        break;
    case 0x47:
    case 0x48:
        SysReferSema(c);
        break;
    case 0x4B: // GetOsdConfigParam: English, 4:3, ...
        if (Arg(c, 0) != 0)
        {
            Write32(Arg(c, 0), 1u << 16);
        }
        Ret(c, 0);
        break;
    case 0x4A:
    case 0x6E:
        Ret(c, 0);
        break;
    case 0x6F: // GetOsdConfigParam2
        Ret(c, 0);
        break;
    case 0x5C: // EnableIntcHandler(cause, id)
    case 0x5D:
        SetHandlerEnabled(g_intcHandlers, 15, static_cast<s32>(Arg(c, 0)), static_cast<s32>(Arg(c, 1)), n == 0x5C);
        Ret(c, 0);
        break;
    case 0x5E:
    case 0x5F:
        SetHandlerEnabled(g_dmacHandlers, 16, static_cast<s32>(Arg(c, 0)), static_cast<s32>(Arg(c, 1)), n == 0x5E);
        Ret(c, 0);
        break;
    case 0x60: // KSeg0
    case 0x61: // EnableCache
    case 0x62: // DisableCache
    case 0x64: // FlushCache
    case 0x68:
    case 0x66: // CpuConfig
    case 0x6A:
        Ret(c, 0);
        break;
    case 0x63: // GetCop0
    case 0x67:
        Ret(c, static_cast<s32>(ps2_mfc0(c, static_cast<s32>(Arg(c, 0)))));
        break;
    case 0x70: // GsGetIMR
        lock.unlock();
        Ret(c, static_cast<s64>(GsPrivRead(0x12001010)));
        lock.lock();
        break;
    case 0x71: // GsPutIMR
        lock.unlock();
        GsPrivWrite(0x12001010, Arg(c, 0));
        lock.lock();
        Ret(c, 0);
        break;
    case 0x73: // SetVSyncFlag(flag, csr): at the next vblank the kernel writes 1 to the flag and the GS's CSR to csr
        g_vsyncFlag = Arg(c, 0);
        g_vsyncCsr = Arg(c, 1);
        Ret(c, 0);
        break;
    case 0x74: // SetSyscall(number, handler)
    {
        int number = static_cast<s32>(Arg(c, 0)) & 0xFF;
        u32 handler = Arg(c, 1);
        Write32(SyscallTable + number * 4, handler);
        // Only handlers in the game's own code are run: libkernel's patches of the kernel's syscalls (code copied into
        // kernel memory, or the table entries it reads back through those) are for the real kernel, which this replaces
        if (handler >= 0x00100000 && handler < RamSize)
        {
            g_userSyscalls[number] = handler;
        }
        else
        {
            g_userSyscalls.erase(number);
        }
        PS2_LOG(LogKernel, "SetSyscall %x: %08x", number, handler);
        Ret(c, 0);
        break;
    }
    case 0x75: // _print
        Log("print: %s", ReadString(Arg(c, 0)).c_str());
        Ret(c, 0);
        break;
    case 0x76: // SifDmaStat: done
        Ret(c, -1);
        break;
    case 0x77: // SifSetDma(transfers, count)
    {
        u32 list = Arg(c, 0);
        u32 count = Arg(c, 1);
        lock.unlock();
        for (u32 i = 0; i < count; i++)
        {
            u32 base = list + i * 16;
            SifSetDma(Read32(base), Read32(base + 4), Read32(base + 8), Read32(base + 12));
        }
        lock.lock();
        static u32 id = 1;
        Ret(c, static_cast<s32>(id++ & 0xFFFF));
        break;
    }
    case 0x78: // SifSetDChain: the EE's command handler is ready for the IOP's next reply
        lock.unlock();
        SifReplyTaken();
        lock.lock();
        Ret(c, 0);
        break;
    case 0x79: // SifSetReg
        lock.unlock();
        SifSetReg(Arg(c, 0), Arg(c, 1));
        lock.lock();
        Ret(c, 0);
        break;
    case 0x7A: // SifGetReg
        lock.unlock();
        Ret(c, static_cast<s32>(SifGetReg(Arg(c, 0))));
        lock.lock();
        break;
    case 0x7C: // Deci2Call: no debugger attached (its open fails, so the libraries' DECI2 output stays off)
        Ret(c, -1);
        break;
    case 0x7D: // PSMode
    case 0x7E: // MachineType
        Ret(c, 0);
        break;
    case 0x7F: // GetMemorySize
        Ret(c, RamSize);
        break;
    case 0x5B:
        // libkernel's kernel patch entry (its code copied into kernel memory): not needed here
        if (Once("syscall5b"))
        {
            Log("syscall 0x5b (libkernel's kernel patch) skipped");
        }
        Ret(c, 0);
        break;
    default:
        if (Once("syscall" + std::to_string(number)))
        {
            Log("syscall 0x%x (%d) not done (a0=%08x at %08x)", number, number, Arg(c, 0), pc);
        }
        Ret(c, 0);
        break;
    }
    // The IOP's replies to what the syscall sent
    if (ps2_irq_pending && g_inInterrupt == 0 && g_current != nullptr)
    {
        ServiceLocked(lock);
        Preempt(lock);
    }
}
