// What recompiled code sees of the runtime: the R5900's registers, the EE's address space, and the helpers the generated C
// calls. Generated code is C; the runtime is C++ and includes this too.
#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef unsigned __int128 u128;
typedef float f32;

#define PS2_LIKELY(x) __builtin_expect(!!(x), 1)
#define PS2_UNLIKELY(x) __builtin_expect(!!(x), 0)

typedef union R128
{
    u128 q;
    u64 ud[2];
    s64 sd[2];
    u32 uw[4];
    s32 sw[4];
    u16 uh[8];
    s16 sh[8];
    u8 ub[16];
    s8 sb[16];
} R128;

typedef union Fpr
{
    f32 f;
    u32 u;
    s32 s;
} Fpr;

// One EE thread's registers (each thread and each interrupt handler has its own)
typedef struct Ps2Cpu
{
    R128 r[32];
    R128 hi;
    R128 lo;
    u32 sa;
    u32 fcr31;
    Fpr f[32];
    Fpr acc;
    // The thread this context belongs to (the kernel's), and the call depth for diagnostics
    void* thread;
    u32 depth;
} Ps2Cpu;

typedef void (*Ps2Function)(Ps2Cpu* c);

typedef struct Ps2FunctionEntry
{
    u32 start;
    u32 end;
    Ps2Function function;
} Ps2FunctionEntry;

// The generated table (sorted by address)
extern const Ps2FunctionEntry ps2_function_table[];
extern const u32 ps2_function_count;

// ---------------------------------------------------------------------------------------------------------------------------
// Memory: the EE's 4 GB address space is a host reservation with main memory mapped at 0 and at its mirrors (0x20000000,
// 0x30000000, 0x80000000, 0xA0000000) and the scratchpad at 0x70000000. The hardware registers (0x10000000-0x1FFFFFFF, and
// their uncached mirror at 0xB0000000-0xBFFFFFFF, which some games use) go through the runtime.
extern u8* ps2_mem;

u8 ps2_io_read8(u32 a);
u16 ps2_io_read16(u32 a);
u32 ps2_io_read32(u32 a);
u64 ps2_io_read64(u32 a);
u128 ps2_io_read128(u32 a);
void ps2_io_write8(u32 a, u8 v);
void ps2_io_write16(u32 a, u16 v);
void ps2_io_write32(u32 a, u32 v);
void ps2_io_write64(u32 a, u64 v);
void ps2_io_write128(u32 a, u128 v);

// The address's top four bits are 0x1 or 0xB (bits 1 and 11 of the mask)
#define PS2_IS_IO(a) ((0x0802u >> ((u32)(a) >> 28)) & 1u)

static inline u8 rd8(u32 a)
{
    if (PS2_UNLIKELY(PS2_IS_IO(a)))
        return ps2_io_read8(a);
    return ps2_mem[a];
}
static inline u16 rd16(u32 a)
{
    if (PS2_UNLIKELY(PS2_IS_IO(a)))
        return ps2_io_read16(a);
    u16 v;
    memcpy(&v, ps2_mem + a, 2);
    return v;
}
static inline u32 rd32(u32 a)
{
    if (PS2_UNLIKELY(PS2_IS_IO(a)))
        return ps2_io_read32(a);
    u32 v;
    memcpy(&v, ps2_mem + a, 4);
    return v;
}
static inline u64 rd64(u32 a)
{
    if (PS2_UNLIKELY(PS2_IS_IO(a)))
        return ps2_io_read64(a);
    u64 v;
    memcpy(&v, ps2_mem + a, 8);
    return v;
}
static inline u128 rd128(u32 a)
{
    if (PS2_UNLIKELY(PS2_IS_IO(a)))
        return ps2_io_read128(a);
    u128 v;
    memcpy(&v, ps2_mem + a, 16);
    return v;
}
static inline void wr8(u32 a, u8 v)
{
    if (PS2_UNLIKELY(PS2_IS_IO(a)))
        ps2_io_write8(a, v);
    else
        ps2_mem[a] = v;
}
static inline void wr16(u32 a, u16 v)
{
    if (PS2_UNLIKELY(PS2_IS_IO(a)))
        ps2_io_write16(a, v);
    else
        memcpy(ps2_mem + a, &v, 2);
}
static inline void wr32(u32 a, u32 v)
{
    if (PS2_UNLIKELY(PS2_IS_IO(a)))
        ps2_io_write32(a, v);
    else
        memcpy(ps2_mem + a, &v, 4);
}
static inline void wr64(u32 a, u64 v)
{
    if (PS2_UNLIKELY(PS2_IS_IO(a)))
        ps2_io_write64(a, v);
    else
        memcpy(ps2_mem + a, &v, 8);
}
static inline void wr128(u32 a, u128 v)
{
    if (PS2_UNLIKELY(PS2_IS_IO(a)))
        ps2_io_write128(a, v);
    else
        memcpy(ps2_mem + a, &v, 16);
}

// The unaligned loads and stores (little endian)
static inline void ps2_lwl(Ps2Cpu* c, int rt, u32 a)
{
    u32 shift = a & 3;
    u32 mem = rd32(a & ~3u);
    static const u32 mask[4] = {0x00FFFFFFu, 0x0000FFFFu, 0x000000FFu, 0x00000000u};
    static const u32 sh[4] = {24, 16, 8, 0};
    c->r[rt].sd[0] = (s32)((c->r[rt].uw[0] & mask[shift]) | (mem << sh[shift]));
}
static inline void ps2_lwr(Ps2Cpu* c, int rt, u32 a)
{
    u32 shift = a & 3;
    u32 mem = rd32(a & ~3u);
    static const u32 mask[4] = {0x00000000u, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u};
    if (shift == 0)
        c->r[rt].sd[0] = (s32)mem;
    else
        c->r[rt].uw[0] = (c->r[rt].uw[0] & mask[shift]) | (mem >> (shift * 8));
}
static inline void ps2_ldl(Ps2Cpu* c, int rt, u32 a)
{
    u32 shift = a & 7;
    u64 mem = rd64(a & ~7u);
    u64 keep = shift == 7 ? 0 : (0x00FFFFFFFFFFFFFFull >> (shift * 8));
    c->r[rt].ud[0] = (c->r[rt].ud[0] & keep) | (mem << (56 - shift * 8));
}
static inline void ps2_ldr(Ps2Cpu* c, int rt, u32 a)
{
    u32 shift = a & 7;
    u64 mem = rd64(a & ~7u);
    u64 keep = shift == 0 ? 0 : (0xFFFFFFFFFFFFFFFFull << (64 - shift * 8));
    c->r[rt].ud[0] = (c->r[rt].ud[0] & keep) | (mem >> (shift * 8));
}
static inline void ps2_swl(Ps2Cpu* c, int rt, u32 a)
{
    u32 shift = a & 3;
    u32 mem = rd32(a & ~3u);
    static const u32 mask[4] = {0xFFFFFF00u, 0xFFFF0000u, 0xFF000000u, 0x00000000u};
    static const u32 sh[4] = {24, 16, 8, 0};
    wr32(a & ~3u, (c->r[rt].uw[0] >> sh[shift]) | (mem & mask[shift]));
}
static inline void ps2_swr(Ps2Cpu* c, int rt, u32 a)
{
    u32 shift = a & 3;
    u32 mem = rd32(a & ~3u);
    static const u32 mask[4] = {0x00000000u, 0x000000FFu, 0x0000FFFFu, 0x00FFFFFFu};
    wr32(a & ~3u, (c->r[rt].uw[0] << (shift * 8)) | (mem & mask[shift]));
}
static inline void ps2_sdl(Ps2Cpu* c, int rt, u32 a)
{
    u32 shift = a & 7;
    u64 mem = rd64(a & ~7u);
    u64 keep = shift == 7 ? 0 : (0xFFFFFFFFFFFFFF00ull << (shift * 8));
    wr64(a & ~7u, (c->r[rt].ud[0] >> (56 - shift * 8)) | (mem & keep));
}
static inline void ps2_sdr(Ps2Cpu* c, int rt, u32 a)
{
    u32 shift = a & 7;
    u64 mem = rd64(a & ~7u);
    u64 keep = shift == 0 ? 0 : (0xFFFFFFFFFFFFFFFFull >> (64 - shift * 8));
    wr64(a & ~7u, (c->r[rt].ud[0] << (shift * 8)) | (mem & keep));
}

// ---------------------------------------------------------------------------------------------------------------------------
// Control: indirect calls by address, the kernel, interrupts
void ps2_call(Ps2Cpu* c, u32 address);
void ps2_syscall(Ps2Cpu* c, u32 pc);
void ps2_break(Ps2Cpu* c, u32 pc);
void ps2_eret(Ps2Cpu* c, u32 pc);
u32 ps2_mfc0(Ps2Cpu* c, int reg);
void ps2_mtc0(Ps2Cpu* c, int reg, u32 value);
void ps2_ei(Ps2Cpu* c);
void ps2_di(Ps2Cpu* c);
int ps2_cpcond0(Ps2Cpu* c);
u32 ps2_cfc1(Ps2Cpu* c, int reg);
void ps2_ctc1(Ps2Cpu* c, int reg, u32 value);

// Interrupts are delivered at backward branches and in the runtime's calls
extern volatile int ps2_irq_pending;
void ps2_irq_service(Ps2Cpu* c);
#define PS2_POLL(c)                                                                                                            \
    do                                                                                                                         \
    {                                                                                                                          \
        if (PS2_UNLIKELY(ps2_irq_pending))                                                                                     \
            ps2_irq_service(c);                                                                                                \
    } while (0)

// ---------------------------------------------------------------------------------------------------------------------------
// Integer division (the R5900's results for a zero divisor and for INT_MIN / -1)
static inline void ps2_div(Ps2Cpu* c, s32 a, s32 b, int pipe)
{
    s32 q, r;
    if (b == 0)
    {
        q = a >= 0 ? -1 : 1;
        r = a;
    }
    else if (a == (s32)0x80000000 && b == -1)
    {
        q = (s32)0x80000000;
        r = 0;
    }
    else
    {
        q = a / b;
        r = a % b;
    }
    c->lo.sd[pipe] = q;
    c->hi.sd[pipe] = r;
}
static inline void ps2_divu(Ps2Cpu* c, u32 a, u32 b, int pipe)
{
    u32 q, r;
    if (b == 0)
    {
        q = 0xFFFFFFFFu;
        r = a;
    }
    else
    {
        q = a / b;
        r = a % b;
    }
    c->lo.sd[pipe] = (s32)q;
    c->hi.sd[pipe] = (s32)r;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The FPU: single precision without infinities, NaNs or denormals (results clamp to the largest float, denormals are zero)
static inline f32 fpu_fix(f32 v)
{
    u32 bits;
    memcpy(&bits, &v, 4);
    u32 e = bits & 0x7F800000u;
    if (PS2_UNLIKELY(e == 0x7F800000u))
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
    else if (PS2_UNLIKELY(e == 0))
        bits &= 0x80000000u;
    memcpy(&v, &bits, 4);
    return v;
}
static inline f32 fpu_add(f32 a, f32 b) { return fpu_fix(fpu_fix(a) + fpu_fix(b)); }
static inline f32 fpu_sub(f32 a, f32 b) { return fpu_fix(fpu_fix(a) - fpu_fix(b)); }
static inline f32 fpu_mul(f32 a, f32 b) { return fpu_fix(fpu_fix(a) * fpu_fix(b)); }
static inline f32 fpu_div(f32 a, f32 b)
{
    a = fpu_fix(a);
    b = fpu_fix(b);
    if (PS2_UNLIKELY(b == 0.0f))
    {
        u32 sa, sb, r;
        memcpy(&sa, &a, 4);
        memcpy(&sb, &b, 4);
        r = ((sa ^ sb) & 0x80000000u) | 0x7F7FFFFFu;
        f32 out;
        memcpy(&out, &r, 4);
        return out;
    }
    return fpu_fix(a / b);
}
static inline f32 fpu_sqrt(f32 a) { return sqrtf(fabsf(fpu_fix(a))); }
static inline f32 fpu_rsqrt(f32 a, f32 b)
{
    f32 s = sqrtf(fabsf(fpu_fix(b)));
    return fpu_div(a, s);
}
static inline f32 fpu_max(f32 a, f32 b) { return a > b ? a : b; }
static inline f32 fpu_min(f32 a, f32 b) { return a < b ? a : b; }
static inline s32 fpu_cvtw(f32 a)
{
    u32 bits;
    memcpy(&bits, &a, 4);
    if ((bits & 0x7F800000u) >= 0x4F000000u)
        return (bits & 0x80000000u) ? (s32)0x80000000 : 0x7FFFFFFF;
    return (s32)a;
}

// ---------------------------------------------------------------------------------------------------------------------------
// VU0 in macro mode (COP2). Its registers are the hardware layer's VU0's, so micro programs started by VCALLMS see them.
typedef union Ps2Vf
{
    f32 f[4];
    u32 u[4];
    s32 s[4];
} Ps2Vf;

typedef struct Ps2Vu0Regs
{
    Ps2Vf vf[32];
    u16 vi[16];
    Ps2Vf acc;
    u32 q, p, i, r, status, mac, clip;
} Ps2Vu0Regs;

extern Ps2Vu0Regs* ps2_vu0;
extern u8* ps2_vu0_mem;     // VU0's 4 KB of data memory
extern u32 ps2_vu0_cmsar0;
u32 vu0_cfc2(int reg);
void vu0_ctc2(int reg, u32 value);
void vu0_callms(u32 byteAddress);
static inline int vu0_busy(void) { return 0; }

#define VF(n) (ps2_vu0->vf[n])
#define VI(n) ((u32)ps2_vu0->vi[n])

static inline f32 vu_fix(f32 v) { return fpu_fix(v); }
static inline Ps2Vf VBC(int n, int bc)
{
    Ps2Vf v;
    v.f[0] = v.f[1] = v.f[2] = v.f[3] = ps2_vu0->vf[n].f[bc];
    return v;
}
static inline Ps2Vf VQ(void)
{
    Ps2Vf v;
    f32 q;
    memcpy(&q, &ps2_vu0->q, 4);
    v.f[0] = v.f[1] = v.f[2] = v.f[3] = q;
    return v;
}
static inline Ps2Vf VI_(void)
{
    Ps2Vf v;
    f32 i;
    memcpy(&i, &ps2_vu0->i, 4);
    v.f[0] = v.f[1] = v.f[2] = v.f[3] = i;
    return v;
}
// A result written to fd's (or, for -1, ACC's) components in dest (bit 0 = x)
static inline void vu0_write(int fd, int dest, Ps2Vf r)
{
    Ps2Vf* d;
    if (fd == 0)
        return;
    d = fd < 0 ? &ps2_vu0->acc : &ps2_vu0->vf[fd];
    for (int k = 0; k < 4; k++)
        if (dest & (1 << k))
            d->f[k] = r.f[k];
}
static inline void vu0_add(int dest, int fd, Ps2Vf a, Ps2Vf b)
{
    Ps2Vf r;
    for (int k = 0; k < 4; k++)
        r.f[k] = vu_fix(vu_fix(a.f[k]) + vu_fix(b.f[k]));
    vu0_write(fd, dest, r);
}
static inline void vu0_sub(int dest, int fd, Ps2Vf a, Ps2Vf b)
{
    Ps2Vf r;
    for (int k = 0; k < 4; k++)
        r.f[k] = vu_fix(vu_fix(a.f[k]) - vu_fix(b.f[k]));
    vu0_write(fd, dest, r);
}
static inline void vu0_mul(int dest, int fd, Ps2Vf a, Ps2Vf b)
{
    Ps2Vf r;
    for (int k = 0; k < 4; k++)
        r.f[k] = vu_fix(vu_fix(a.f[k]) * vu_fix(b.f[k]));
    vu0_write(fd, dest, r);
}
static inline void vu0_madd(int dest, int fd, Ps2Vf a, Ps2Vf b)
{
    Ps2Vf r;
    Ps2Vf acc = ps2_vu0->acc;
    for (int k = 0; k < 4; k++)
        r.f[k] = vu_fix(vu_fix(acc.f[k]) + vu_fix(vu_fix(a.f[k]) * vu_fix(b.f[k])));
    vu0_write(fd, dest, r);
}
static inline void vu0_msub(int dest, int fd, Ps2Vf a, Ps2Vf b)
{
    Ps2Vf r;
    Ps2Vf acc = ps2_vu0->acc;
    for (int k = 0; k < 4; k++)
        r.f[k] = vu_fix(vu_fix(acc.f[k]) - vu_fix(vu_fix(a.f[k]) * vu_fix(b.f[k])));
    vu0_write(fd, dest, r);
}
static inline void vu0_max(int dest, int fd, Ps2Vf a, Ps2Vf b)
{
    Ps2Vf r;
    for (int k = 0; k < 4; k++)
        r.f[k] = a.f[k] > b.f[k] ? a.f[k] : b.f[k];
    vu0_write(fd, dest, r);
}
static inline void vu0_mini(int dest, int fd, Ps2Vf a, Ps2Vf b)
{
    Ps2Vf r;
    for (int k = 0; k < 4; k++)
        r.f[k] = a.f[k] < b.f[k] ? a.f[k] : b.f[k];
    vu0_write(fd, dest, r);
}
static inline void vu0_opmula(Ps2Vf a, Ps2Vf b)
{
    ps2_vu0->acc.f[0] = vu_fix(a.f[1] * b.f[2]);
    ps2_vu0->acc.f[1] = vu_fix(a.f[2] * b.f[0]);
    ps2_vu0->acc.f[2] = vu_fix(a.f[0] * b.f[1]);
}
static inline void vu0_opmsub(int fd, Ps2Vf a, Ps2Vf b)
{
    Ps2Vf acc = ps2_vu0->acc, r = {{0, 0, 0, 0}};
    r.f[0] = vu_fix(acc.f[0] - a.f[1] * b.f[2]);
    r.f[1] = vu_fix(acc.f[1] - a.f[2] * b.f[0]);
    r.f[2] = vu_fix(acc.f[2] - a.f[0] * b.f[1]);
    vu0_write(fd, 7, r);
}
static inline void vu0_ftoi(int dest, int ft, Ps2Vf a, int bits)
{
    Ps2Vf r;
    f32 scale = (f32)(1 << bits);
    for (int k = 0; k < 4; k++)
    {
        f32 v = a.f[k] * scale;
        if (v >= 2147483647.0f)
            r.s[k] = 0x7FFFFFFF;
        else if (v <= -2147483648.0f)
            r.s[k] = (s32)0x80000000;
        else
            r.s[k] = (s32)v;
    }
    vu0_write(ft, dest, r);
}
static inline void vu0_itof(int dest, int ft, Ps2Vf a, int bits)
{
    Ps2Vf r;
    f32 scale = 1.0f / (f32)(1 << bits);
    for (int k = 0; k < 4; k++)
        r.f[k] = (f32)a.s[k] * scale;
    vu0_write(ft, dest, r);
}
static inline void vu0_abs(int dest, int ft, Ps2Vf a)
{
    for (int k = 0; k < 4; k++)
        a.u[k] &= 0x7FFFFFFFu;
    vu0_write(ft, dest, a);
}
static inline void vu0_move(int dest, int ft, Ps2Vf a) { vu0_write(ft, dest, a); }
static inline void vu0_mr32(int dest, int ft, Ps2Vf a)
{
    Ps2Vf r;
    r.u[0] = a.u[1];
    r.u[1] = a.u[2];
    r.u[2] = a.u[3];
    r.u[3] = a.u[0];
    vu0_write(ft, dest, r);
}
static inline void vu0_clip(Ps2Vf a, Ps2Vf b)
{
    f32 w = fabsf(b.f[3]);
    u32 flags = 0;
    if (a.f[0] > w) flags |= 0x01;
    if (a.f[0] < -w) flags |= 0x02;
    if (a.f[1] > w) flags |= 0x04;
    if (a.f[1] < -w) flags |= 0x08;
    if (a.f[2] > w) flags |= 0x10;
    if (a.f[2] < -w) flags |= 0x20;
    ps2_vu0->clip = ((ps2_vu0->clip << 6) | flags) & 0xFFFFFF;
}
static inline void vu0_setq(f32 q) { q = vu_fix(q); memcpy(&ps2_vu0->q, &q, 4); }
static inline void vu0_div(f32 a, f32 b) { vu0_setq(fpu_div(a, b)); }
static inline void vu0_sqrt(f32 a) { vu0_setq(sqrtf(fabsf(a))); }
static inline void vu0_rsqrt(f32 a, f32 b) { vu0_setq(fpu_div(a, sqrtf(fabsf(b)))); }
static inline void vu0_seti(int n, u32 v)
{
    if (n != 0)
        ps2_vu0->vi[n] = (u16)v;
}
static inline void vu0_mfir(int dest, int ft, int is)
{
    Ps2Vf r;
    s32 v = (s16)ps2_vu0->vi[is];
    r.s[0] = r.s[1] = r.s[2] = r.s[3] = v;
    vu0_write(ft, dest, r);
}
static inline void vu0_lqi(int dest, int ft, int is, int step)
{
    Ps2Vf r;
    if (step < 0)
        vu0_seti(is, VI(is) - 1);
    memcpy(&r, ps2_vu0_mem + ((VI(is) * 16) & 0xFFF), 16);
    vu0_write(ft, dest, r);
    if (step > 0)
        vu0_seti(is, VI(is) + 1);
}
static inline void vu0_sqi(int dest, int fs, int it, int step)
{
    if (step < 0)
        vu0_seti(it, VI(it) - 1);
    u8* m = ps2_vu0_mem + ((VI(it) * 16) & 0xFFF);
    for (int k = 0; k < 4; k++)
        if (dest & (1 << k))
            memcpy(m + 4 * k, &ps2_vu0->vf[fs].u[k], 4);
    if (step > 0)
        vu0_seti(it, VI(it) + 1);
}
static inline void vu0_ilwr(int dest, int it, int is)
{
    u8* m = ps2_vu0_mem + ((VI(is) * 16) & 0xFFF);
    for (int k = 0; k < 4; k++)
        if (dest & (1 << k))
        {
            u32 v;
            memcpy(&v, m + 4 * k, 4);
            vu0_seti(it, v & 0xFFFF);
            return;
        }
}
static inline void vu0_iswr(int dest, int it, int is)
{
    u8* m = ps2_vu0_mem + ((VI(is) * 16) & 0xFFF);
    u32 v = VI(it);
    for (int k = 0; k < 4; k++)
        if (dest & (1 << k))
            memcpy(m + 4 * k, &v, 4);
}
static inline void vu0_rinit(int dest, int ft, u32 v) { (void)dest; (void)ft; ps2_vu0->r = 0x3F800000u | (v & 0x007FFFFFu); }
static inline void vu0_rxor(int dest, int ft, u32 v) { (void)dest; (void)ft; ps2_vu0->r = 0x3F800000u | ((ps2_vu0->r ^ v) & 0x007FFFFFu); }
static inline void vu0_rget(int dest, int ft, u32 v)
{
    Ps2Vf r;
    (void)v;
    r.u[0] = r.u[1] = r.u[2] = r.u[3] = ps2_vu0->r;
    vu0_write(ft, dest, r);
}
static inline void vu0_rnext(int dest, int ft, u32 v)
{
    u32 x = ps2_vu0->r;
    int bit = ((x >> 4) ^ (x >> 22)) & 1;
    x = (x << 1) | (u32)bit;
    ps2_vu0->r = 0x3F800000u | (x & 0x007FFFFFu);
    vu0_rget(dest, ft, v);
}
static inline void vu0_lqc2(int ft, u128 v)
{
    if (ft != 0)
        memcpy(&ps2_vu0->vf[ft], &v, 16);
}
static inline u128 vu0_sqc2(int fs)
{
    u128 v;
    memcpy(&v, &ps2_vu0->vf[fs], 16);
    return v;
}

// ---------------------------------------------------------------------------------------------------------------------------
// The MMI instructions (mmi.c): rd, rs, rt register numbers (the shifts and PMFHL/PMTHL: rd, rt, rs, sa)
#define MMI3(name) void mmi_##name(Ps2Cpu* c, int rd, int rs, int rt);
MMI3(paddw) MMI3(psubw) MMI3(pcgtw) MMI3(pmaxw) MMI3(paddh) MMI3(psubh) MMI3(pcgth) MMI3(pmaxh) MMI3(paddb) MMI3(psubb)
MMI3(pcgtb) MMI3(paddsw) MMI3(psubsw) MMI3(pextlw) MMI3(ppacw) MMI3(paddsh) MMI3(psubsh) MMI3(pextlh) MMI3(ppach)
MMI3(paddsb) MMI3(psubsb) MMI3(pextlb) MMI3(ppacb) MMI3(pext5) MMI3(ppac5)
MMI3(pabsw) MMI3(pceqw) MMI3(pminw) MMI3(padsbh) MMI3(pabsh) MMI3(pceqh) MMI3(pminh) MMI3(pceqb) MMI3(padduw)
MMI3(psubuw) MMI3(pextuw) MMI3(padduh) MMI3(psubuh) MMI3(pextuh) MMI3(paddub) MMI3(psubub) MMI3(pextub) MMI3(qfsrv)
MMI3(pmaddw) MMI3(psllvw) MMI3(psrlvw) MMI3(pmsubw) MMI3(pmfhi) MMI3(pmflo) MMI3(pinth) MMI3(pmultw) MMI3(pdivw)
MMI3(pcpyld) MMI3(pmaddh) MMI3(phmadh) MMI3(pand) MMI3(pxor) MMI3(pmsubh) MMI3(phmsbh) MMI3(pexeh) MMI3(prevh)
MMI3(pmulth) MMI3(pdivbw) MMI3(pexew) MMI3(prot3w)
MMI3(pmadduw) MMI3(psravw) MMI3(pmthi) MMI3(pmtlo) MMI3(pinteh) MMI3(pmultuw) MMI3(pdivuw) MMI3(pcpyud) MMI3(por)
MMI3(pnor) MMI3(pexch) MMI3(pcpyh) MMI3(pexcw) MMI3(plzcw)
#undef MMI3
void mmi_psllh(Ps2Cpu* c, int rd, int rt, int rs, int sa);
void mmi_psrlh(Ps2Cpu* c, int rd, int rt, int rs, int sa);
void mmi_psrah(Ps2Cpu* c, int rd, int rt, int rs, int sa);
void mmi_psllw(Ps2Cpu* c, int rd, int rt, int rs, int sa);
void mmi_psrlw(Ps2Cpu* c, int rd, int rt, int rs, int sa);
void mmi_psraw(Ps2Cpu* c, int rd, int rt, int rs, int sa);
void mmi_pmfhl(Ps2Cpu* c, int rd, int rt, int rs, int sa);
void mmi_pmthl(Ps2Cpu* c, int rd, int rt, int rs, int sa);

#ifdef __cplusplus
}
#endif
