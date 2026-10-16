// VU1's translated programs and the interpreter run together (vu1translate.h). The dispatcher finds which translated program
// micro memory holds at the program counter (wherever it was uploaded), and a variant of the code there made for what the
// interpreter's state has in flight; the translated code runs until it leaves the program, jumps through a register or ends,
// and hands its state back in the interpreter's form. Where there's no translation the interpreter takes instructions one at a
// time until there is one.

#include "vu1translate.h"

#include "vu.h"
#include "vu1jit.h"

#include <algorithm>
#include <cfenv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#pragma STDC FENV_ACCESS ON

namespace Ee
{
namespace
{
using namespace Vu1Jit;

Vu1TranslationStats g_Stats;
Vu1TranslationStats g_Vu0Stats;

// A VU's statistics
Vu1TranslationStats& StatsOf(const Vu& vu)
{
    return vu.index() == 0 ? g_Vu0Stats : g_Stats;
}

// A VU's translated programs: VU1's, or VU0's, and its micro memory's size (instructions, less one)
struct Table
{
    const Program* programs;
    u32 programCount;
    const Signature* signatures;
    u32 codeMask;
};

const Table Vu1Table = {g_Programs, g_ProgramCount, g_Signatures, 0x7FF};
const Table Vu0Table = {g_ProgramsVu0, g_ProgramCountVu0, g_SignaturesVu0, 0x1FF};

// PS2_VU1_PROFILE: where the interpreter ran instead (the program and offset the dispatcher looked for, and why it found
// nothing), printed at exit
struct MissKey
{
    s32 program;
    u32 offset;
    u32 reason;
    bool operator==(const MissKey& o) const { return program == o.program && offset == o.offset && reason == o.reason; }
};

struct MissHash
{
    size_t operator()(const MissKey& k) const { return (static_cast<size_t>(k.program) * 4096 + k.offset) * 8 + k.reason; }
};

const char* const MissReasons[] = {"no program", "no variant at the offset", "no variant for the state", "state not representable"};
std::unordered_map<MissKey, u64, MissHash>* g_Misses = nullptr;
std::unordered_map<std::string, u64>* g_MissSignatures = nullptr;

struct Candidate
{
    u32 program;
    u32 offset;
};

// Where translated code continues: a program's function, the base it runs at and the variant's label
struct Target
{
    const Program* program;
    u32 base;
    u32 label;
};

class Dispatcher
{
public:
    explicit Dispatcher(const Table& table) : t_(table)
    {
        for (u32 p = 0; p < t_.programCount; p++)
        {
            const Program& program = t_.programs[p];
            std::vector<u32>& firsts = first_.emplace_back(program.size + 1, ~0u);
            for (u32 v = 0; v < program.variantCount; v++)
            {
                u32 offset = program.variants[v].offset;
                if (firsts[offset] == ~0u)
                {
                    firsts[offset] = v;
                    entries_[program.code[offset]].push_back({p, offset});
                }
            }
        }

        owner_.fill(-1);
    }

    // The variant to continue at from the interpreter's state at its program counter, with the slots made from that state.
    // False when there's none (the interpreter takes the next instruction)
    bool Enter(Vu& vu, VuMicroState& st, Slots& slots, Target& target)
    {
        s32 program;
        u32 base, offset, first;
        if (!Locate(vu, st.pc, program, base, offset, first))
        {
            return false;
        }

        const Program& code = t_.programs[program];

        // The state's signature: what's in flight relative to the next cycle, once what's due has landed
        vu.FlushPipes(st);
        Signature signature = {};
        const VuPipelines& pipes = st.pipes;
        if (pipes.fmacCount > 3)
        {
            return Miss(program, offset, 3);
        }

        // Entries that write no register and carry no new flag change nothing when they land: the translations leave them
        // out. One whose flag is the same as the one before but came from a new result (the translation keeps it) may be
        // either, so the signature is tried both ways
        const VuFmacEntry* entries[3];
        u32 optional = 0;
        u32 previousMac = vu.macFlag;
        for (u32 n = 0; n < pipes.fmacCount; n++)
        {
            const VuFmacEntry& e = pipes.fmac[(pipes.fmacRead + n) & 3];
            entries[n] = &e;
            if (e.upperReg == 0 && e.lowerReg == 0 && e.flags == 0 && e.mac == previousMac)
            {
                optional |= 1u << n;
            }

            previousMac = e.mac;
            // FSSET's entry in flight: the translations reset the sticky bits when they issue it (vutranslate.py)
            constexpr u32 FlagStatus = 2;
            if (e.flags & FlagStatus)
            {
                return Miss(program, offset, 3);
            }
        }

        if (pipes.fdiv)
        {
            signature.fdivBusy = 1;
            signature.fdivReady = static_cast<s8>(static_cast<s64>(pipes.fdivStart + pipes.fdivLatency - st.cycle));
        }

        if (pipes.efu)
        {
            signature.efuBusy = 1;
            signature.efuReady = static_cast<s8>(static_cast<s64>(pipes.efuStart + pipes.efuLatency - st.cycle));
        }

        Signature::Ialu ialu[4];
        u32 ialuCount = 0;
        for (u32 n = 0; n < 4; n++)
        {
            if (pipes.ialuReady[n] > st.cycle)
            {
                ialu[ialuCount++] = {static_cast<s8>(pipes.ialuReady[n] - st.cycle), pipes.ialuReg[n]};
            }
        }

        std::sort(ialu, ialu + ialuCount, [](const Signature::Ialu& a, const Signature::Ialu& b) {
            return a.ready != b.ready ? a.ready < b.ready : a.reg < b.reg;
        });
        signature.ialuCount = static_cast<u8>(ialuCount);
        std::copy(ialu, ialu + ialuCount, signature.ialu);
        signature.backupCycles = static_cast<u8>(st.backupCycles);
        signature.backupReg = static_cast<u8>(st.backupCycles > 0 ? st.backupReg : 0);
        signature.anyEntry = st.anyEntry ? 1 : 0;

        u32 variant = ~0u;
        const VuFmacEntry* kept[3];
        u32 keptCount = 0;
        for (u32 keep = optional;; keep = (keep - 1) & optional)
        {
            // The optional entries in keep are kept, the others left out
            keptCount = 0;
            std::memset(signature.fmac, 0, sizeof(signature.fmac));
            for (u32 n = 0; n < pipes.fmacCount; n++)
            {
                if ((optional >> n) & 1 && !((keep >> n) & 1))
                {
                    continue;
                }

                const VuFmacEntry& e = *entries[n];
                signature.fmac[keptCount] = {static_cast<s8>(static_cast<s64>(e.start - st.cycle)), e.upperReg, e.upperMask,
                                             e.lowerReg, e.lowerMask, e.flags};
                kept[keptCount++] = &e;
            }

            signature.fmacCount = static_cast<u8>(keptCount);
            for (u32 v = first; v < code.variantCount && code.variants[v].offset == offset; v++)
            {
                if (std::memcmp(&t_.signatures[code.variants[v].signature], &signature, sizeof(Signature)) == 0)
                {
                    variant = v;
                    break;
                }
            }

            if (variant != ~0u || keep == 0)
            {
                break;
            }
        }

        if (variant == ~0u)
        {
            if (g_Misses != nullptr)
            {
                ProfileSignature(program, offset, signature);
            }

            return Miss(program, offset, 2);
        }

        // The values: flags as MAC sources (vu1jit.h), the old values and results in flight
        slots = {};
        const u32 vf0[4] = {0, 0, 0, 0x3F800000};
        if (std::memcmp(vu.vf[0].u, vf0, 16) != 0 || (vu.statusFlag & 0xF) != StatusLow(vu.macFlag) ||
            (st.pending.status & 0xF) != StatusLow(st.pending.mac) || !MacSource(vu.macFlag, slots.visibleMac) ||
            !MacSource(st.pending.mac, slots.pendingMac))
        {
            return Miss(program, offset, 3);
        }

        slots.stickyBits = (vu.statusFlag >> 6) & 0xF;
        for (u32 n = 0; n < pipes.fmacCount; n++)
        {
            // Its status bits land later: the translations count every result's when it's made
            const VuFmacEntry& e = pipes.fmac[(pipes.fmacRead + n) & 3];
            slots.stickyBits |= e.status & 0xF;
            if ((e.status & 0xF) != StatusLow(e.mac))
            {
                return Miss(program, offset, 3);
            }
        }

        for (u32 n = 0; n < keptCount; n++)
        {
            if (!MacSource(kept[n]->mac, slots.entryMac[n]))
            {
                return Miss(program, offset, 3);
            }

            slots.entryClip[n] = kept[n]->clip;
        }

        slots.pendingClip = st.pending.clip;
        slots.visibleClip = vu.clipFlag;
        slots.visibleDivFlags = vu.statusFlag & 0x30;
        slots.fdivValue = pipes.fdivValue;
        slots.fdivFlags = pipes.fdivStatus & 0x30;
        slots.efuValue = pipes.efuValue;
        slots.backupValue = st.backupValue;
        target = {&code, base, code.variants[variant].label};
        Trace(program, base, offset, variant);
        return true;
    }

    // The variant to go on at from where translated code stopped (st.pc, slots.exitSignature): the slots carry over as they
    // are. False when there's none (LeaveTranslation makes the interpreter's state)
    bool Next(Vu& vu, const VuMicroState& st, const Slots& slots, Target& target)
    {
        s32 program;
        u32 base, offset, first;
        if (!Locate(vu, st.pc, program, base, offset, first))
        {
            return false;
        }

        const Program& code = t_.programs[program];
        for (u32 v = first; v < code.variantCount && code.variants[v].offset == offset; v++)
        {
            if (code.variants[v].signature == slots.exitSignature)
            {
                target = {&code, base, code.variants[v].label};
                Trace(program, base, offset, v);
                return true;
            }
        }

        if (g_Misses != nullptr)
        {
            ProfileSignature(program, offset, t_.signatures[slots.exitSignature]);
        }

        return Miss(program, offset, 2);
    }

    // (verify) the translated code entered during the run
    std::string* trace = nullptr;

    const Signature& SignatureAt(u32 id) const
    {
        return t_.signatures[id];
    }

private:
    // The translated program at pc (its number, where it starts in micro memory, pc's offset in it and its first variant
    // there)
    bool Locate(Vu& vu, u32 pc, s32& program, u32& base, u32& offset, u32& first)
    {
        if (vu.codeVersion() != version_)
        {
            // The programs whose code changed are looked for again, and so is every place no program was found at
            version_ = vu.codeVersion();
            std::bitset<2048> changes = vu.TakeCodeChanges();
            for (u32 at = 0; at <= t_.codeMask; at++)
            {
                if (owner_[at] == -2)
                {
                    owner_[at] = -1;
                }
                else if (owner_[at] >= 0 && changes.test(at))
                {
                    u32 from = base_[at];
                    u32 size = t_.programs[owner_[at]].size;
                    for (u32 n = 0; n < size; n++)
                    {
                        owner_[(from + n) & t_.codeMask] = -1;
                    }
                }
            }
        }

        program = owner_[pc];
        if (program == -2)
        {
            return Miss(-1, pc, 0);
        }

        if (program == -1)
        {
            program = Identify(vu, pc);
            if (program < 0)
            {
                owner_[pc] = -2;
                return Miss(-1, pc, 0);
            }
        }

        base = base_[pc];
        offset = (pc - base) & t_.codeMask;
        first = first_[program][offset];
        if (first == ~0u)
        {
            return Miss(program, offset, 1);
        }

        return true;
    }

    void Trace(s32 program, u32 base, u32 offset, u32 variant)
    {
        if (trace != nullptr)
        {
            char line[96];
            std::snprintf(line, sizeof(line), " [program %d at %#x: offset %#x variant %u]", program, base, offset, variant);
            *trace += line;
        }
    }

    static void ProfileSignature(s32 program, u32 offset, const Signature& signature)
    {
        // The state's signature, for the profile
        char line[160];
        int n = std::snprintf(line, sizeof(line), "    %d %#x:", program, offset);
        for (u32 k = 0; k < signature.fmacCount; k++)
        {
            const Signature::Fmac& f = signature.fmac[k];
            n += std::snprintf(line + n, sizeof(line) - n, " (%d %u %u %u %u %u)", f.start, f.upperReg, f.upperMask,
                               f.lowerReg, f.lowerMask, f.flags);
        }

        std::snprintf(line + n, sizeof(line) - n, " fdiv %d/%d efu %d/%d ialu %u backup %u/%u any %u",
                      signature.fdivBusy, signature.fdivReady, signature.efuBusy, signature.efuReady, signature.ialuCount, signature.backupCycles, signature.backupReg,
                      signature.anyEntry);
        (*g_MissSignatures)[line]++;
    }

    static bool Miss(s32 program, u32 offset, u32 reason)
    {
        if (g_Misses != nullptr)
        {
            (*g_Misses)[{program, offset, reason}]++;
        }

        return false;
    }

    // The translated program micro memory holds at pc, if any: one with a variant at its offset there whose code is all there
    s32 Identify(Vu& vu, u32 pc)
    {
        const u64* code = vu.code();
        auto found = entries_.find(code[pc]);
        if (found == entries_.end())
        {
            return -1;
        }

        for (const Candidate& candidate : found->second)
        {
            const Program& program = t_.programs[candidate.program];
            u32 base = (pc - candidate.offset) & t_.codeMask;
            bool same = true;
            for (u32 n = 0; n < program.size && same; n++)
            {
                same = code[(base + n) & t_.codeMask] == program.code[n];
            }

            if (same)
            {
                for (u32 n = 0; n < program.size; n++)
                {
                    u32 at = (base + n) & t_.codeMask;
                    owner_[at] = static_cast<s32>(candidate.program);
                    base_[at] = base;
                }

                return static_cast<s32>(candidate.program);
            }
        }

        return -1;
    }

    std::vector<std::vector<u32>> first_;
    std::unordered_map<u64, std::vector<Candidate>> entries_;
    const Table& t_;
    std::array<s32, 2048> owner_;
    std::array<u32, 2048> base_ = {};
    u64 version_ = ~0ull;
};

// PS2_VU1_PROFILE's times: in the dispatcher's entries from the interpreter's state, in translated code (less XGKICK's
// packets), in going from one translated program to the next, in leaving translated code, in the interpreter
struct Times
{
    u64 enter = 0;
    u64 code = 0;
    u64 next = 0;
    u64 leave = 0;
    u64 interpret = 0;
};
Times g_Times;

inline u64 Now()
{
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

// A program run with the translations
template <bool Profile>
void RunTranslated(Vu& vu, Dispatcher& dispatcher, u32 start)
{
    int previousRounding = std::fegetround();
    std::fesetround(FE_TOWARDZERO);
    VuMicroState st;
    Slots slots;
    vu.BeginMicro(st, start);
    u64 t = Profile ? Now() : 0;
    auto lap = [&](u64& into) {
        if constexpr (Profile)
        {
            u64 now = Now();
            into += now - t;
            t = now;
        }
    };
    for (;;)
    {
        Target target;
        bool clean = st.branch == 0 && st.ebit == 0 && !st.delayBranch;
        if (clean && dispatcher.Enter(vu, st, slots, target))
        {
            lap(g_Times.enter);
            bool ended = false;
            for (;;)
            {
                u64 executed = vu.executed;
                u64 kicks = vu.kickNanoseconds;
                ended = target.program->run(vu, st, slots, target.base, target.label) == 1;
                if constexpr (Profile)
                {
                    t += vu.kickNanoseconds - kicks;
                }

                lap(g_Times.code);
                StatsOf(vu).translatedInstructions += vu.executed - executed;
                StatsOf(vu).entries++;
                bool next = !ended && !vu.RunawayCheck() && dispatcher.Next(vu, st, slots, target);
                lap(g_Times.next);
                if (!next)
                {
                    LeaveTranslation(vu, st, slots, dispatcher.SignatureAt(slots.exitSignature));
                    lap(g_Times.leave);
                    break;
                }
            }

            if (ended || Vu::RanAway())
            {
                break;
            }

            continue;
        }

        if (clean)
        {
            lap(g_Times.enter);
        }

        StatsOf(vu).interpretedInstructions++;
        u64 kicks = vu.kickNanoseconds;
        bool going = vu.StepMicro(st);
        if constexpr (Profile)
        {
            t += vu.kickNanoseconds - kicks;
        }

        lap(g_Times.interpret);
        if (!going)
        {
            break;
        }
    }

    vu.EndMicro(st);
    lap(g_Times.leave);
    std::fesetround(previousRounding);
}

void RunTranslated(Vu& vu, Dispatcher& dispatcher, u32 start)
{
    if (g_Misses != nullptr)
    {
        RunTranslated<true>(vu, dispatcher, start);
    }
    else
    {
        RunTranslated<false>(vu, dispatcher, start);
    }
}

// PS2_VU1=verify: each run made by the interpreter on a copy first, its packets and end compared with the translated run's
class Verifier
{
public:
    struct Kick
    {
        u32 address;
        u32 kickPc;
        std::vector<u8> data;
        Vf vf[32];
        u16 vi[16];
        Vf acc;
        u32 i;
        u32 r;
        u32 q;
        u32 p;
        u32 mac;
        u32 clip;
    };

    explicit Verifier(Vu& vu) : vu_(vu), shadow_(vu.index()) {}

    void Run(Dispatcher& dispatcher, u32 start)
    {
        shadow_.CopyFrom(vu_);
        shadow_.executed = vu_.executed;
        kicks_.clear();
        shadow_.xgkick = [this](u32 address) { kicks_.push_back(Record(shadow_, address)); };
        shadow_.RunInterpreted(start);

        next_ = 0;
        start_ = start;
        trace_.clear();
        dispatcher.trace = &trace_;
        auto original = vu_.xgkick;
        vu_.xgkick = [this, &original](u32 address) {
            Compare(address);
            if (original)
            {
                original(address);
            }
        };
        RunTranslated(vu_, dispatcher, start);
        dispatcher.trace = nullptr;
        vu_.xgkick = original;
        StatsOf(vu_).verifiedRuns++;
        if (vu_.index() == 0 && g_Vu0Stats.verifiedRuns % 100000 == 0)
        {
            if (g_Misses != nullptr)
            {
                std::vector<std::pair<MissKey, u64>> misses(g_Misses->begin(), g_Misses->end());
                std::sort(misses.begin(), misses.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                for (size_t n = 0; n < misses.size() && n < 6; n++)
                {
                    std::fprintf(stderr, "vu0 miss: %llu program %d offset %#x: %s\n", static_cast<unsigned long long>(misses[n].second),
                                 misses[n].first.program, misses[n].first.offset, MissReasons[misses[n].first.reason]);
                }
            }

            std::fprintf(stderr, "vu0 verify: %llu runs, %llu instructions translated, %llu interpreted, %llu differences\n",
                         static_cast<unsigned long long>(g_Vu0Stats.verifiedRuns),
                         static_cast<unsigned long long>(g_Vu0Stats.translatedInstructions),
                         static_cast<unsigned long long>(g_Vu0Stats.interpretedInstructions),
                         static_cast<unsigned long long>(g_Vu0Stats.differences));
        }

        if (next_ != kicks_.size())
        {
            Report("%zu kicks interpreted, %u translated", kicks_.size(), next_);
        }

        const char* what = nullptr;
        if (std::memcmp(shadow_.data(), vu_.data(), vu_.dataSize()) != 0)
        {
            what = "data memory";
        }
        else if (std::memcmp(shadow_.vf, vu_.vf, sizeof(vu_.vf)) != 0)
        {
            what = "VF registers";
        }
        else if (std::memcmp(shadow_.vi, vu_.vi, sizeof(vu_.vi)) != 0)
        {
            what = "VI registers";
        }
        else if (std::memcmp(&shadow_.acc, &vu_.acc, sizeof(Vf)) != 0)
        {
            what = "ACC";
        }
        else if (shadow_.q != vu_.q || shadow_.p != vu_.p || shadow_.i != vu_.i || shadow_.r != vu_.r)
        {
            what = "Q, P, I or R";
        }
        else if (shadow_.statusFlag != vu_.statusFlag)
        {
            what = "the status flag";
        }
        else if (shadow_.macFlag != vu_.macFlag)
        {
            what = "the MAC flag";
        }
        else if (shadow_.clipFlag != vu_.clipFlag)
        {
            what = "the clip flag";
        }
        else if (shadow_.pc() != vu_.pc())
        {
            what = "where the program stopped";
        }
        else if (shadow_.executed != vu_.executed)
        {
            what = "the instructions run";
        }

        if (what != nullptr)
        {
            Report("at the end: %s differ (status %#x/%#x mac %#x/%#x clip %#x/%#x q %#x/%#x pc %#x/%#x executed %llu/%llu)",
                   what, shadow_.statusFlag, vu_.statusFlag, shadow_.macFlag, vu_.macFlag, shadow_.clipFlag, vu_.clipFlag,
                   shadow_.q, vu_.q, shadow_.pc(), vu_.pc(), static_cast<unsigned long long>(shadow_.executed),
                   static_cast<unsigned long long>(vu_.executed));
            Detail(shadow_, vu_);
            // The interpreter's results stand, so that what follows is checked from the same state
            vu_.CopyFrom(shadow_);
            vu_.executed = shadow_.executed;
        }
    }

private:
    static Kick Record(Vu& vu, u32 address)
    {
        Kick kick;
        kick.address = address;
        kick.kickPc = vu.kickPc;
        kick.data.assign(vu.data(), vu.data() + vu.dataSize());
        std::memcpy(kick.vf, vu.vf, sizeof(kick.vf));
        std::memcpy(kick.vi, vu.vi, sizeof(kick.vi));
        kick.acc = vu.acc;
        kick.i = vu.i;
        kick.r = vu.r;
        kick.q = vu.q;
        kick.p = vu.p;
        kick.mac = vu.macFlag;
        kick.clip = vu.clipFlag;
        return kick;
    }

    void Compare(u32 address)
    {
        StatsOf(vu_).verifiedKicks++;
        if (next_ >= kicks_.size())
        {
            Report("an XGKICK the interpreter didn't make (at %#x)", vu_.kickPc);
            next_++;
            return;
        }

        const Kick& expected = kicks_[next_++];
        Kick got = Record(vu_, address);
        const char* what = nullptr;
        if (expected.address != got.address || expected.kickPc != got.kickPc)
        {
            what = "the packet's address or the kick's place";
        }
        else if (expected.data != got.data)
        {
            what = "data memory";
        }
        else if (std::memcmp(expected.vf, got.vf, sizeof(got.vf)) != 0)
        {
            what = "VF registers";
        }
        else if (std::memcmp(expected.vi, got.vi, sizeof(got.vi)) != 0)
        {
            what = "VI registers";
        }
        else if (std::memcmp(&expected.acc, &got.acc, sizeof(Vf)) != 0)
        {
            what = "ACC";
        }
        else if (expected.i != got.i || expected.r != got.r || expected.q != got.q || expected.p != got.p)
        {
            what = "I, R, Q or P";
        }
        else if (expected.mac != got.mac || expected.clip != got.clip)
        {
            what = "the MAC or clip flag";
        }

        if (what != nullptr)
        {
            Report("XGKICK %u (at %#x/%#x, packet %#x/%#x): %s differ", next_ - 1, expected.kickPc, got.kickPc,
                   expected.address, got.address, what);
            for (u32 n = 0; n < got.data.size(); n += 4)
            {
                if (std::memcmp(&expected.data[n], &got.data[n], 4) != 0)
                {
                    u32 a, b;
                    std::memcpy(&a, &expected.data[n], 4);
                    std::memcpy(&b, &got.data[n], 4);
                    std::fprintf(stderr, "    data %#06x: %08x / %08x\n", n, a, b);
                    break;
                }
            }

            for (u32 r = 0; r < 32; r++)
            {
                if (std::memcmp(&expected.vf[r], &got.vf[r], 16) != 0)
                {
                    std::fprintf(stderr, "    vf%u: %08x %08x %08x %08x / %08x %08x %08x %08x\n", r, expected.vf[r].u[0],
                                 expected.vf[r].u[1], expected.vf[r].u[2], expected.vf[r].u[3], got.vf[r].u[0], got.vf[r].u[1],
                                 got.vf[r].u[2], got.vf[r].u[3]);
                }
            }

            for (u32 r = 0; r < 16; r++)
            {
                if (expected.vi[r] != got.vi[r])
                {
                    std::fprintf(stderr, "    vi%u: %04x / %04x\n", r, expected.vi[r], got.vi[r]);
                }
            }
        }
    }

    static void Detail(const Vu& expected, const Vu& got)
    {
        for (u32 n = 0; n < got.dataSize(); n += 4)
        {
            if (std::memcmp(const_cast<Vu&>(expected).data() + n, const_cast<Vu&>(got).data() + n, 4) != 0)
            {
                u32 a, b;
                std::memcpy(&a, const_cast<Vu&>(expected).data() + n, 4);
                std::memcpy(&b, const_cast<Vu&>(got).data() + n, 4);
                std::fprintf(stderr, "    data %#06x: %08x / %08x\n", n, a, b);
                break;
            }
        }

        for (u32 r = 0; r < 32; r++)
        {
            if (std::memcmp(&expected.vf[r], &got.vf[r], 16) != 0)
            {
                std::fprintf(stderr, "    vf%u: %08x %08x %08x %08x / %08x %08x %08x %08x\n", r, expected.vf[r].u[0],
                             expected.vf[r].u[1], expected.vf[r].u[2], expected.vf[r].u[3], got.vf[r].u[0], got.vf[r].u[1],
                             got.vf[r].u[2], got.vf[r].u[3]);
            }
        }

        for (u32 r = 0; r < 16; r++)
        {
            if (expected.vi[r] != got.vi[r])
            {
                std::fprintf(stderr, "    vi%u: %04x / %04x\n", r, expected.vi[r], got.vi[r]);
            }
        }
    }

    template <typename... Args>
    void Report(const char* format, Args... args)
    {
        StatsOf(vu_).differences++;
        if (StatsOf(vu_).differences > 50)
        {
            return;
        }

        std::fprintf(stderr, "vu1 verify: run from %#x: ", start_);
        std::fprintf(stderr, format, args...);
        std::fprintf(stderr, "\n    translated:%s\n", trace_.c_str());
    }

    Vu& vu_;
    Vu shadow_;
    std::vector<Kick> kicks_;
    std::string trace_;
    u32 next_ = 0;
    u32 start_ = 0;
};
}

namespace Vu1Jit
{
void LeaveTranslation(Vu& vu, VuMicroState& st, const Slots& slots, const Signature& signature)
{
    const u32 start = st.startStatus;
    st.cycle += 16;
    const u64 cycle = st.cycle;
    VuPipelines pipes;
    pipes.fmacCount = signature.fmacCount;
    for (u32 n = 0; n < signature.fmacCount; n++)
    {
        const Signature::Fmac& f = signature.fmac[n];
        VuFmacEntry& e = pipes.fmac[n];
        e.start = cycle + f.start;
        e.upperReg = f.upperReg;
        e.upperMask = f.upperMask;
        e.lowerReg = f.lowerReg;
        e.lowerMask = f.lowerMask;
        e.flags = f.flags;
        e.mac = MacOf(slots.entryMac[n]);
        e.status = (start & 0xFF0) | StatusLow(e.mac);
        e.clip = slots.entryClip[n];
    }

    if (signature.fdivBusy)
    {
        pipes.fdiv = true;
        pipes.fdivStart = cycle;
        pipes.fdivLatency = static_cast<u32>(signature.fdivReady);
        pipes.fdivValue = slots.fdivValue;
        pipes.fdivStatus = (start & 0xFC0) | slots.fdivFlags;
    }

    if (signature.efuBusy)
    {
        pipes.efu = true;
        pipes.efuStart = cycle;
        pipes.efuLatency = static_cast<u32>(signature.efuReady);
        pipes.efuValue = slots.efuValue;
    }

    for (u32 n = 0; n < signature.ialuCount; n++)
    {
        pipes.ialuReg[n] = signature.ialu[n].reg;
        pipes.ialuReady[n] = cycle + signature.ialu[n].ready;
    }

    pipes.ialuNext = signature.ialuCount & 3;
    st.pipes = pipes;
    st.pending.mac = MacOf(slots.pendingMac);
    st.pending.status = (start & 0xFF0) | StatusLow(st.pending.mac);
    st.pending.clip = slots.pendingClip;
    st.backupCycles = signature.backupCycles;
    st.backupReg = signature.backupReg;
    st.backupValue = static_cast<u16>(slots.backupValue);
    st.anyEntry = signature.anyEntry != 0;
    st.branch = 0;
    st.ebit = 0;
    st.delayBranch = false;

    // The flags as they're seen: the status flag's sticky bits have every result made so far (the interpreter adds each when
    // it lands; the end is the same)
    vu.macFlag = MacOf(slots.visibleMac);
    vu.clipFlag = slots.visibleClip;
    u32 sticky = slots.stickyBits | StickyLow(slots.sticky);
    vu.statusFlag = (start & 0xC00) | slots.visibleDivFlags | StatusLow(vu.macFlag) | (sticky << 6);
}
}

namespace
{
// The reports $PS2_VU1_PROFILE and $PS2_VU1_STATS ask for, made once: at exit, or when the host asks first (a host that ends
// with _exit runs no atexit handlers)
std::vector<void (*)()>& Vu1Reports()
{
    static std::vector<void (*)()> reports;
    return reports;
}

void AddVu1Report(void (*report)())
{
    if (Vu1Reports().empty())
    {
        std::atexit(ReportVu1);
    }

    Vu1Reports().push_back(report);
}
}

void ReportVu1()
{
    std::vector<void (*)()> reports;
    reports.swap(Vu1Reports());
    for (auto report : reports)
    {
        report();
    }
}

void AttachVu1Translation(Vu& vu1)
{
    const char* setting = std::getenv("PS2_VU1");
    std::string mode = setting != nullptr ? setting : "";

    if (std::getenv("PS2_VU1_PROFILE") != nullptr)
    {
        g_Misses = new std::unordered_map<MissKey, u64, MissHash>();
        g_MissSignatures = new std::unordered_map<std::string, u64>();
        AddVu1Report([] {
            std::vector<std::pair<MissKey, u64>> misses(g_Misses->begin(), g_Misses->end());
            std::sort(misses.begin(), misses.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            std::fprintf(stderr, "vu1 profile: %llu translated, %llu interpreted instructions; ms in entering %.1f, translated "
                                 "code %.1f, going on %.1f, leaving %.1f, interpreting %.1f\n",
                         static_cast<unsigned long long>(g_Stats.translatedInstructions),
                         static_cast<unsigned long long>(g_Stats.interpretedInstructions), g_Times.enter / 1e6,
                         g_Times.code / 1e6, g_Times.next / 1e6, g_Times.leave / 1e6, g_Times.interpret / 1e6);
            std::fprintf(stderr, "vu1 profile: the interpreter's steps by where the dispatcher found nothing:\n");
            for (size_t n = 0; n < misses.size() && n < 400; n++)
            {
                std::fprintf(stderr, "  %10llu  program %d offset %#x: %s\n", static_cast<unsigned long long>(misses[n].second),
                             misses[n].first.program, misses[n].first.offset, MissReasons[misses[n].first.reason]);
            }

            std::vector<std::pair<std::string, u64>> sigs(g_MissSignatures->begin(), g_MissSignatures->end());
            std::sort(sigs.begin(), sigs.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            for (size_t n = 0; n < sigs.size() && n < 60; n++)
            {
                std::fprintf(stderr, "  %10llu %s\n", static_cast<unsigned long long>(sigs[n].second), sigs[n].first.c_str());
            }
        });
    }

    if (std::getenv("PS2_VU1_STATS") != nullptr)
    {
        // At exit: VU1's time in its programs less XGKICK's packets (the GIF and the GS), and how much of it ran translated
        vu1.timing = true;
        static Vu* timed = &vu1;
        AddVu1Report([] {
            std::fprintf(stderr, "vu1 stats: %.2f ms in programs (less XGKICKs' %.2f ms of GIF and GS), %llu instructions "
                                 "translated, %llu interpreted, %llu entries into translated code\n",
                         (timed->runNanoseconds - timed->kickNanoseconds) / 1e6, timed->kickNanoseconds / 1e6,
                         static_cast<unsigned long long>(g_Stats.translatedInstructions),
                         static_cast<unsigned long long>(g_Stats.interpretedInstructions),
                         static_cast<unsigned long long>(g_Stats.entries));
        });
    }

    if (mode == "interpret")
    {
        std::fprintf(stderr, "graphics: VU1 interpreted (PS2_VU1=interpret)\n");
        return;
    }

    auto dispatcher = std::make_shared<Dispatcher>(Vu1Table);
    if (mode == "verify")
    {
        std::fprintf(stderr, "graphics: VU1 translated and checked against the interpreter (PS2_VU1=verify)\n");
        auto verifier = std::make_shared<Verifier>(vu1);
        vu1.runner = [dispatcher, verifier](u32 start) { verifier->Run(*dispatcher, start); };
        std::atexit([] {
            std::fprintf(stderr, "vu1 verify: %llu runs, %llu kicks compared, %llu differences\n",
                         static_cast<unsigned long long>(g_Stats.verifiedRuns),
                         static_cast<unsigned long long>(g_Stats.verifiedKicks),
                         static_cast<unsigned long long>(g_Stats.differences));
        });
        return;
    }

    Vu* vu = &vu1;
    vu1.runner = [dispatcher, vu](u32 start) { RunTranslated(*vu, *dispatcher, start); };
}

void AttachVu0Translation(Vu& vu0)
{
    const char* setting = std::getenv("PS2_VU0");
    std::string mode = setting != nullptr ? setting : "";
    if (mode == "interpret")
    {
        std::fprintf(stderr, "graphics: VU0's microprograms interpreted (PS2_VU0=interpret)\n");
        return;
    }

    auto dispatcher = std::make_shared<Dispatcher>(Vu0Table);
    if (mode == "verify")
    {
        std::fprintf(stderr, "graphics: VU0's microprograms translated and checked against the interpreter (PS2_VU0=verify)\n");
        auto verifier = std::make_shared<Verifier>(vu0);
        vu0.runner = [dispatcher, verifier](u32 start) { verifier->Run(*dispatcher, start); };
        std::atexit([] {
            std::fprintf(stderr, "vu0 verify: %llu runs, %llu instructions translated, %llu interpreted, %llu differences\n",
                         static_cast<unsigned long long>(g_Vu0Stats.verifiedRuns),
                         static_cast<unsigned long long>(g_Vu0Stats.translatedInstructions),
                         static_cast<unsigned long long>(g_Vu0Stats.interpretedInstructions),
                         static_cast<unsigned long long>(g_Vu0Stats.differences));
        });
        return;
    }

    Vu* vu = &vu0;
    vu0.runner = [dispatcher, vu](u32 start) { RunTranslated(*vu, *dispatcher, start); };
}

const Vu1TranslationStats& GetVu1TranslationStats()
{
    return g_Stats;
}
}
